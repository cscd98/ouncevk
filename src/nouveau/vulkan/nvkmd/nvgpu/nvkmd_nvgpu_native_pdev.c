/* SPDX-License-Identifier: MIT */
/* OunceVK-native backend objects on one accepted owner that lives as long as
 * the process. These objects do not close the service-side backing, map or
 * channel. No safe remote retirement contract is established.
 */
#include "nvkmd_nvgpu_native_pdev.h"

#include "nvkmd_nvgpu_native_mem.h"
#include "nvkmd_nvgpu_native_chunked.h"
#include "nvkmd_nvgpu_native_host_mem.h"
#include "nvkmd_nvgpu_native_ctx.h"
#include "nvkmd_nvgpu_native_progress.h"
#include "nvkmd_nvgpu_native_runtime.h"
#include "nvkmd_nvgpu_native_syncobj.h"

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_sync_timeline.h"

#include <assert.h>
#include <stdlib.h>

/* NIL's ordinary 2D profile uses a maximum of 16 GOB rows of 512 bytes.
 * Generic allocations need this alignment. They can later back non-dedicated
 * depth or texture images that have no image metadata at allocation time. */
enum { NATIVE_BIND_ALIGN_B = 8192 };

struct nvkmd_nvgpu_native_pdev {
   struct nvkmd_pdev base;
   struct nvkmd_nvgpu_native_device_owner *owner;
   uint32_t device_count;
   bool destroy_requested;
   struct vk_sync_timeline_type timeline_type;
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_nvgpu_native_dev {
   struct nvkmd_dev base;
   struct nvkmd_nvgpu_native_device_owner *owner;
   uint32_t context_count;
   uint32_t va_count;
};

/* One bound range of a reserved VA: memory mapped again at the VA. */
struct nvkmd_nvgpu_native_va_view {
   uint64_t offset_B, size_B;
   struct nvkmd_nvgpu_native_view_source source;
};

/* A VA from the owner's VA heap with no memory of its own (an image plane
 * or a sparse-binding buffer). Each bind maps a range of a memory object at
 * the VA with the PTE kind. */
struct nvkmd_nvgpu_native_va {
   struct nvkmd_va base;
   struct nvkmd_nvgpu_native_dev *dev;
   uint8_t pte_kind;
   struct nvkmd_nvgpu_native_va_view *views;
   uint32_t view_count, view_capacity;
};

enum {
   /* Software contexts: the application queue (COMPUTE|3D|COPY) on the
    * primary channel, and the upload context and the transfer queues (COPY)
    * on the copy channel when it is ready (else on the primary channel).
    * Contexts on one channel are serialized. */
   NATIVE_MAX_SOFTWARE_CONTEXTS = 8,
};

static void
native_pdev_free(struct nvkmd_nvgpu_native_pdev *pdev)
{
   assert(pdev->owner && pdev->owner->active_pdevs);
   pdev->owner->active_pdevs--;
   free(pdev);
}

static void
native_pdev_destroy(struct nvkmd_pdev *base)
{
   struct nvkmd_nvgpu_native_pdev *pdev =
      container_of(base, struct nvkmd_nvgpu_native_pdev, base);
   /* Logical devices that still exist keep a pointer in base.pdev. The last
    * child completes this deferred request when its own references are
    * gone. */
   if (pdev->device_count) {
      pdev->destroy_requested = true;
      return;
   }
   native_pdev_free(pdev);
}

static VkResult
native_timeline_register(struct vk_device *device,
                         struct vk_sync_timeline_state *state,
                         void *data)
{
   return nvkmd_nvgpu_native_progress_register_timeline(data, device, state);
}

static void
native_timeline_unregister(struct vk_device *device,
                           struct vk_sync_timeline_state *state,
                           void *data)
{
   nvkmd_nvgpu_native_progress_unregister_timeline(data, device, state);
}

static uint64_t
native_pdev_vram_used(struct nvkmd_pdev *base)
{
   (void)base;
   return 0;
}

static void
native_dev_destroy(struct nvkmd_dev *base)
{
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   /* A context or VA that still exists can refer to this device and its
    * arena. In that case, do not destroy the remaining memory wrappers or
    * the mutex. */
   if (dev->context_count || dev->va_count)
      return;
   simple_mtx_lock(&base->mems_mutex);
   list_for_each_entry_safe(struct nvkmd_mem, mem, &base->mems, link) {
      list_del(&mem->link);
      mem->ops->free(mem);
   }
   simple_mtx_unlock(&base->mems_mutex);
   simple_mtx_destroy(&base->mems_mutex);
   struct nvkmd_nvgpu_native_pdev *pdev =
      container_of(base->pdev, struct nvkmd_nvgpu_native_pdev, base);
   assert(pdev->device_count);
   pdev->device_count--;
   assert(dev->owner && dev->owner->active_devices);
   dev->owner->active_devices--;
   free(dev);
   if (pdev->destroy_requested && !pdev->device_count)
      native_pdev_free(pdev);
}

/* Reads the GPU PTIMER in ns through the delegated nvservices session. See
 * nvkmd_nvgpu_native_runtime_read_gpu_time.
 */
static VkResult
native_dev_read_timestamp(struct nvkmd_dev *base, uint64_t *timestamp)
{
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   const struct nvkmd_nvgpu_native_runtime *runtime = dev->owner->runtime;
   *timestamp = 0;
   if (!runtime)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   return nvkmd_nvgpu_native_runtime_read_gpu_time(runtime, timestamp) ?
      VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static uint64_t
native_dev_timestamp(struct nvkmd_dev *base)
{
   uint64_t timestamp = 0;
   (void)native_dev_read_timestamp(base, &timestamp);
   return timestamp;
}

/* Game role:
 * - GPU-only memory (no host mapping and no coherence request) comes from
 *   GPU-cacheable segments.
 * - Application HOST_CACHED memory comes from readback segments. Only that
 *   flag selects them, because NVK's internal mapped allocations get no
 *   cache maintenance.
 * - All other memory comes from the primary segment and further uncached
 *   segments.
 * A pending VA reservation is a slice of the primary arena. */
extern uint32_t ouncevk_debug_sync_flags;
void ouncevk_debug_sync_flags_load(void);

static enum nvkmd_nvgpu_native_segment_kind
native_segment_kind(enum nvkmd_mem_flags flags)
{
   if (flags & NVKMD_MEM_CPU_CACHED)
      return NVKMD_NVGPU_SEGMENT_READBACK;
   /* Debug switch 16 puts GPU-only memory in GPU-uncached segments. GPU-
    * cached memory (the default) is faster: quick vkmark
    * 5672 against 5538, all in the phong scene, and it allows chunked
    * memory. But fragment-stage shaders that loop over a storage image in
    * GPU-cached memory can leave stale texels: about 20 of 125 CTS
    * synchronization.op R8 image cases fail, 0 with uncached memory
    * (the cause is not known yet). */
   if (ouncevk_debug_sync_flags & 16)
      return NVKMD_NVGPU_SEGMENT_UNCACHED;
   if (!(flags & (NVKMD_MEM_CAN_MAP | NVKMD_MEM_COHERENT |
                  NVKMD_MEM_CPU_UNCACHED | NVKMD_MEM_GPU_UNCACHED)))
      return NVKMD_NVGPU_SEGMENT_CACHED;
   return NVKMD_NVGPU_SEGMENT_UNCACHED;
}

/* Returns true when an existing segment can take this allocation. The
 * segment must be of the same kind. A small allocation needs a pooled
 * segment. A large allocation needs a dedicated segment that wastes less
 * than the size of a pooled segment. */
static bool
native_segment_fits(const struct nvkmd_nvgpu_native_segment *segment,
                    enum nvkmd_nvgpu_native_segment_kind kind, uint64_t size_B)
{
   if (!segment->ready || segment->kind != kind || segment->arena.retiring)
      return false;
   if (!nvkmd_nvgpu_native_segment_dedicated(size_B))
      return !segment->dedicated;
   return segment->dedicated && segment->length_B >= size_B &&
          segment->length_B - size_B < NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B;
}

/* Tries the kind's arenas in creation order. When none has room, it creates
 * a segment for this allocation. Large allocations never use the primary
 * segment, which holds the submission chunks.
 */
static VkResult
native_dev_mem_create(struct nvkmd_nvgpu_native_dev *dev,
                      struct vk_object_base *log_obj,
                      uint64_t size_B, uint64_t align_B,
                      enum nvkmd_mem_flags flags,
                      uint8_t pte_kind, uint16_t tile_mode,
                      struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvgpu_native_device_owner *owner = dev->owner;
   if (!owner->segments_enabled)
      return nvkmd_nvgpu_native_mem_create(
         &dev->base, owner->arena_ref, size_B, align_B, flags, pte_kind,
         tile_mode, NULL, mem_out);
   const enum nvkmd_nvgpu_native_segment_kind kind =
      native_segment_kind(flags);
   /* Large GPU-only memory is page-granular: registered chunks mapped at
    * consecutive VA offsets, so it needs no CPU-contiguous span. */
   if (kind == NVKMD_NVGPU_SEGMENT_CACHED &&
       nvkmd_nvgpu_native_segment_dedicated(size_B) &&
       (pte_kind == 0 || pte_kind == 6))
      return nvkmd_nvgpu_native_chunked_mem_create(&dev->base, owner, size_B,
                                                   align_B, flags, mem_out);
   VkResult result;
   if (kind == NVKMD_NVGPU_SEGMENT_UNCACHED &&
       !nvkmd_nvgpu_native_segment_dedicated(size_B)) {
      result = nvkmd_nvgpu_native_mem_create(
         &dev->base, owner->arena_ref, size_B, align_B, flags, pte_kind,
         tile_mode, NULL, mem_out);
      if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
         return result;
   }
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (!native_segment_fits(segment, kind, size_B))
         continue;
      result = nvkmd_nvgpu_native_mem_create(
         &dev->base, &segment->arena, size_B, align_B, flags, pte_kind,
         tile_mode, NULL, mem_out);
      if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
         return result;
   }
   struct nvkmd_nvgpu_native_arena *arena =
      nvkmd_nvgpu_native_owner_grow(owner, kind, size_B, align_B,
                                    &dev->base, log_obj);
   if (!arena)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   return nvkmd_nvgpu_native_mem_create(
      &dev->base, arena, size_B, align_B, flags, pte_kind, tile_mode, NULL,
      mem_out);
}

static VkResult
native_dev_alloc_mem_locked(struct nvkmd_dev *base,
                            struct vk_object_base *log_obj,
                            uint64_t size_B,
                            uint64_t align_B,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   /* The kind-6 mapping of the arena is the backing for ordinary memory. The
    * per-allocation kind and tile fields below are only requested metadata. */
   if (!dev->owner->mapped_ready ||
       dev->owner->mapped_gpu_base != dev->owner->arena_ref->gpu_base ||
       dev->owner->mapped_length != dev->owner->arena_ref->length)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   dev->owner->last_mem_request_B = size_B;
   VkResult result = native_dev_mem_create(
      dev, log_obj, size_B, MAX2(align_B, NATIVE_BIND_ALIGN_B), flags, 0, 0,
      mem_out);
   dev->owner->last_mem_result = result;
   return result;
}

static VkResult
native_dev_alloc_mem(struct nvkmd_dev *base,
                     struct vk_object_base *log_obj,
                     uint64_t size_B,
                     uint64_t align_B,
                     enum nvkmd_mem_flags flags,
                     struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_dev, base)->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_dev_alloc_mem_locked(
      base, log_obj, size_B, align_B, flags, mem_out);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

static VkResult
native_dev_alloc_tiled_locked(struct nvkmd_dev *base,
                              struct vk_object_base *log_obj,
                              uint64_t size_B,
                              uint64_t align_B,
                              uint8_t pte_kind,
                              uint16_t tile_mode,
                              enum nvkmd_mem_flags flags,
                              struct nvkmd_mem **mem_out)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   /* The whole-backing map, which stays mapped, was accepted as uncompressed
    * kind 6. Tile mode is only image metadata. This function does not create
    * a second GPU VA mapping. */
   if (!dev->owner->mapped_ready ||
       dev->owner->mapped_gpu_base != dev->owner->arena_ref->gpu_base ||
       dev->owner->mapped_length != dev->owner->arena_ref->length)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   dev->owner->last_mem_request_B = size_B;
   VkResult result = native_dev_mem_create(
      dev, log_obj, size_B, align_B, flags, pte_kind, tile_mode, mem_out);
   dev->owner->last_mem_result = result;
   return result;
}

static VkResult
native_dev_alloc_tiled(struct nvkmd_dev *base,
                       struct vk_object_base *log_obj,
                       uint64_t size_B,
                       uint64_t align_B,
                       uint8_t pte_kind,
                       uint16_t tile_mode,
                       enum nvkmd_mem_flags flags,
                       struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_dev, base)->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_dev_alloc_tiled_locked(
      base, log_obj, size_B, align_B, pte_kind, tile_mode, flags, mem_out);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

/* Makes room for one more view in the list. A mapping must not exist
 * without its view, so this comes before the map request. */
static bool
native_va_reserve_view(struct nvkmd_nvgpu_native_va *va)
{
   if (va->view_count < va->view_capacity)
      return true;
   const uint32_t capacity = MAX2(4, va->view_capacity * 2);
   struct nvkmd_nvgpu_native_va_view *views =
      realloc(va->views, capacity * sizeof(*views));
   if (!views)
      return false;
   va->views = views;
   va->view_capacity = capacity;
   return true;
}

/* Unmaps the views that are inside [offset_B, +size_B) and removes them
 * from the list. A view whose unmap failed stays in the list: its mapping
 * may remain. Returns false when one stays. */
static bool
native_va_unmap_views(struct nvkmd_nvgpu_native_va *va,
                      uint64_t offset_B, uint64_t size_B)
{
   bool all_unmapped = true;
   uint32_t kept = 0;
   for (uint32_t i = 0; i < va->view_count; i++) {
      const struct nvkmd_nvgpu_native_va_view view = va->views[i];
      const bool inside = view.offset_B >= offset_B &&
                          view.offset_B + view.size_B <= offset_B + size_B;
      if (inside &&
          nvkmd_nvgpu_native_owner_unmap_view(va->dev->owner,
                                              view.source.backing,
                                              va->base.addr + view.offset_B,
                                              view.size_B))
         continue;
      if (inside)
         all_unmapped = false;
      va->views[kept++] = view;
   }
   va->view_count = kept;
   return all_unmapped;
}

static void
native_va_free_locked(struct nvkmd_va *base)
{
   struct nvkmd_nvgpu_native_va *va =
      container_of(base, struct nvkmd_nvgpu_native_va, base);
   /* A VA with a view that may still be mapped stays reserved. */
   if (native_va_unmap_views(va, 0, va->base.size_B))
      nvkmd_nvgpu_native_owner_release_va(va->dev->owner, va->base.addr,
                                          va->base.size_B);
   else
      mesa_loge("native view: a VA of 0x%llx bytes is not reused: %u of its "
                "views may still be mapped",
                (unsigned long long)va->base.size_B, va->view_count);
   free(va->views);
   assert(va->dev->va_count && va->dev->owner->active_vas);
   va->dev->va_count--;
   va->dev->owner->active_vas--;
   free(va);
}

static void
native_va_free(struct nvkmd_va *base)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_va, base)->dev->owner;
   const bool locked = nvkmd_nvgpu_native_owner_lock(owner);
   native_va_free_locked(base);
   if (locked)
      nvkmd_nvgpu_native_owner_unlock(owner);
}

/* Maps the view's backing bytes at its place in the VA and adds it to the
 * list. A view that failed but may be mapped all the same is added too, so
 * that it is unmapped with the others and its VA is not reused. */
static VkResult
native_va_map_view(struct nvkmd_nvgpu_native_va *va,
                   const struct nvkmd_nvgpu_native_va_view *view)
{
   if (!native_va_reserve_view(va))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bool left_mapped = false;
   const VkResult result = nvkmd_nvgpu_native_owner_map_backing(
      va->dev->owner, &view->source, view->size_B,
      va->base.addr + view->offset_B, va->pte_kind, &left_mapped);
   if (result == VK_SUCCESS || left_mapped)
      va->views[va->view_count++] = *view;
   return result;
}

/* Binds chunked memory: one view for each chunk part of the range. */
static VkResult
native_va_bind_chunked_locked(struct nvkmd_nvgpu_native_va *va,
                              uint64_t va_offset_B, struct nvkmd_mem *mem,
                              uint64_t mem_offset_B, uint64_t range_B)
{
   const uint64_t max64 = range_B / NVKMD_NVGPU_NATIVE_CHUNK_B + 2;
   if (max64 > UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const uint32_t max = (uint32_t)max64;
   struct nvkmd_nvgpu_native_view_part *parts = calloc(max, sizeof(*parts));
   if (!parts)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   const uint32_t count = nvkmd_nvgpu_native_chunked_mem_parts(
      mem, mem_offset_B, range_B, parts, max);
   VkResult result = count ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
   for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
      const struct nvkmd_nvgpu_native_va_view view = {
         .offset_B = va_offset_B + parts[i].offset_B,
         .size_B = parts[i].size_B,
         .source = parts[i].source,
      };
      result = native_va_map_view(va, &view);
   }
   free(parts);
   return result;
}

static VkResult native_va_unbind_views_locked(struct nvkmd_nvgpu_native_va *va,
                                              uint64_t offset_B,
                                              uint64_t size_B);

/* Binds memory in a reserved VA. Views do not overlap: a range that is
 * already bound must be unbound first. */
static VkResult
native_va_bind_view_locked(struct nvkmd_nvgpu_native_va *va,
                           uint64_t va_offset_B,
                           struct nvkmd_mem *mem,
                           uint64_t mem_offset_B,
                           uint64_t range_B)
{
   if (!mem || !range_B || va_offset_B >= va->base.size_B ||
       range_B > va->base.size_B - va_offset_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   /* A bind replaces what is bound in its range (Vulkan sparse binding):
    * unbind the range first. */
   for (uint32_t i = 0; i < va->view_count; i++) {
      const struct nvkmd_nvgpu_native_va_view *view = &va->views[i];
      if (va_offset_B < view->offset_B + view->size_B &&
          view->offset_B < va_offset_B + range_B) {
         const VkResult unbound =
            native_va_unbind_views_locked(va, va_offset_B, range_B);
         if (unbound != VK_SUCCESS)
            return unbound;
         break;
      }
   }
   if (nvkmd_nvgpu_native_mem_is_chunked(mem))
      return native_va_bind_chunked_locked(va, va_offset_B, mem,
                                           mem_offset_B, range_B);
   if (!native_va_reserve_view(va))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct nvkmd_nvgpu_native_va_view view = {
      .offset_B = va_offset_B,
      .size_B = range_B,
   };
   bool left_mapped = false;
   const VkResult result = nvkmd_nvgpu_native_mem_map_view(
      mem, mem_offset_B, range_B, va->base.addr + va_offset_B, va->pte_kind,
      &view.source, &left_mapped);
   if (result == VK_SUCCESS || left_mapped)
      va->views[va->view_count++] = view;
   return result;
}

/* Unbinds [offset_B, +size_B) of a reserved VA. e5 removes whole mappings
 * only, so a view that the range covers in part is removed and
 * its remaining head and tail are mapped again from the same backing
 * (remove, then a distinct e3: no atomic replacement). */
static VkResult
native_va_unbind_views_locked(struct nvkmd_nvgpu_native_va *va,
                              uint64_t offset_B, uint64_t size_B)
{
   if (size_B > UINT64_MAX - offset_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const uint64_t end_B = offset_B + size_B;
   /* The parts to keep, at most two for each view, and the view that each
    * comes from. */
   struct nvkmd_nvgpu_native_va_view *keep =
      calloc(2 * (size_t)va->view_count + 1, sizeof(*keep));
   uint32_t *keep_from = calloc(2 * (size_t)va->view_count + 1,
                                sizeof(*keep_from));
   bool *removed = calloc((size_t)va->view_count + 1, sizeof(*removed));
   if (!keep || !keep_from || !removed) {
      free(keep);
      free(keep_from);
      free(removed);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   uint32_t keep_count = 0;
   for (uint32_t i = 0; i < va->view_count; i++) {
      const struct nvkmd_nvgpu_native_va_view *view = &va->views[i];
      const uint64_t view_end = view->offset_B + view->size_B;
      if (view_end <= offset_B || view->offset_B >= end_B)
         continue;
      if (view->offset_B < offset_B) {
         keep_from[keep_count] = i;
         keep[keep_count++] = (struct nvkmd_nvgpu_native_va_view) {
            .offset_B = view->offset_B,
            .size_B = offset_B - view->offset_B,
            .source = view->source,
         };
      }
      if (view_end > end_B) {
         struct nvkmd_nvgpu_native_view_source tail = view->source;
         tail.backing_offset_B += end_B - view->offset_B;
         keep_from[keep_count] = i;
         keep[keep_count++] = (struct nvkmd_nvgpu_native_va_view) {
            .offset_B = end_B,
            .size_B = view_end - end_B,
            .source = tail,
         };
      }
   }
   /* Remove every view that the range touches, as whole records. A view
    * whose unmap failed stays whole: its mapping may remain, so nothing is
    * mapped in its place. */
   VkResult result = VK_SUCCESS;
   uint32_t kept = 0;
   for (uint32_t i = 0; i < va->view_count; i++) {
      const struct nvkmd_nvgpu_native_va_view view = va->views[i];
      const bool touched = view.offset_B + view.size_B > offset_B &&
                           view.offset_B < end_B;
      if (touched &&
          nvkmd_nvgpu_native_owner_unmap_view(va->dev->owner,
                                              view.source.backing,
                                              va->base.addr + view.offset_B,
                                              view.size_B)) {
         removed[i] = true;
         continue;
      }
      if (touched)
         result = VK_ERROR_DEVICE_LOST;
      va->views[kept++] = view;
   }
   va->view_count = kept;
   /* Map the kept parts of the removed views again. */
   for (uint32_t i = 0; i < keep_count; i++) {
      if (!removed[keep_from[i]])
         continue;
      const VkResult mapped = native_va_map_view(va, &keep[i]);
      if (mapped != VK_SUCCESS) {
         result = mapped;
         break;
      }
   }
   free(keep);
   free(keep_from);
   free(removed);
   return result;
}

static VkResult
native_va_bind_mem_locked(struct nvkmd_va *base,
                          struct vk_object_base *log_obj,
                          uint64_t va_offset_B,
                          struct nvkmd_mem *mem,
                          uint64_t mem_offset_B,
                          uint64_t range_B)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_va *va =
      container_of(base, struct nvkmd_nvgpu_native_va, base);
   return native_va_bind_view_locked(va, va_offset_B, mem, mem_offset_B,
                                     range_B);
}

static VkResult
native_va_bind_mem(struct nvkmd_va *base,
                   struct vk_object_base *log_obj,
                   uint64_t va_offset_B,
                   struct nvkmd_mem *mem,
                   uint64_t mem_offset_B,
                   uint64_t range_B)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_va, base)->dev->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_va_bind_mem_locked(
      base, log_obj, va_offset_B, mem, mem_offset_B, range_B);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

static VkResult
native_va_unbind_locked(struct nvkmd_va *base,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        uint64_t range_B)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_va *va =
      container_of(base, struct nvkmd_nvgpu_native_va, base);
   return native_va_unbind_views_locked(va, va_offset_B, range_B);
}

static VkResult
native_va_unbind(struct nvkmd_va *base,
                 struct vk_object_base *log_obj,
                 uint64_t va_offset_B,
                 uint64_t range_B)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_va, base)->dev->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result =
      native_va_unbind_locked(base, log_obj, va_offset_B, range_B);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

static const struct nvkmd_va_ops native_va_ops = {
   .free = native_va_free,
   .bind_mem = native_va_bind_mem,
   .unbind = native_va_unbind,
};

/* A VA with a PTE kind and no memory (an image plane). */
static VkResult
native_dev_reserve_va_locked(struct nvkmd_nvgpu_native_dev *dev,
                             uint8_t pte_kind, uint64_t size_B,
                             uint64_t align_B, struct nvkmd_va **va_out)
{
   if (!size_B || (align_B & (align_B - 1)) ||
       dev->va_count == UINT32_MAX || dev->owner->active_vas == UINT32_MAX) {
      dev->owner->last_va_result = VK_ERROR_FEATURE_NOT_PRESENT;
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   const uint64_t reserved_B = align64(size_B, 4096);
   struct nvkmd_nvgpu_native_va *va = calloc(1, sizeof(*va));
   if (va == NULL) {
      dev->owner->last_va_result = VK_ERROR_OUT_OF_HOST_MEMORY;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   const uint64_t addr =
      nvkmd_nvgpu_native_owner_reserve_va(dev->owner, reserved_B, align_B);
   if (!addr) {
      free(va);
      dev->owner->last_va_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   va->base.ops = &native_va_ops;
   va->base.dev = &dev->base;
   va->base.addr = addr;
   va->base.size_B = reserved_B;
   va->dev = dev;
   va->pte_kind = pte_kind;
   dev->va_count++;
   dev->owner->active_vas++;
   *va_out = &va->base;
   dev->owner->last_va_result = VK_SUCCESS;
   return VK_SUCCESS;
}

static VkResult
native_dev_alloc_va_locked(struct nvkmd_dev *base,
                           struct vk_object_base *log_obj,
                           enum nvkmd_va_flags flags,
                           uint8_t pte_kind,
                           uint64_t size_B,
                           uint64_t align_B,
                           uint64_t fixed_addr,
                           struct nvkmd_va **va_out)
{
   (void)log_obj;
   if (va_out == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   *va_out = NULL;
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   dev->owner->last_va_request_B = size_B;
   const uint64_t bind_align_B = MAX2(align_B, NATIVE_BIND_ALIGN_B);
   /* Image planes (a PTE kind) and sparse-binding buffers (kind 0, mapped
    * as generic kind 6). Sparse residency (NVKMD_VA_SPARSE) needs zero
    * pages for unbound ranges and is not supported. */
   if (flags || fixed_addr) {
      dev->owner->last_va_result = VK_ERROR_FEATURE_NOT_PRESENT;
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   return native_dev_reserve_va_locked(dev, pte_kind ? pte_kind : 6, size_B,
                                       bind_align_B, va_out);
}

static VkResult
native_dev_alloc_va(struct nvkmd_dev *base,
                    struct vk_object_base *log_obj,
                    enum nvkmd_va_flags flags,
                    uint8_t pte_kind,
                    uint64_t size_B,
                    uint64_t align_B,
                    uint64_t fixed_addr,
                    struct nvkmd_va **va_out)
{
   struct nvkmd_nvgpu_native_device_owner *owner =
      container_of(base, struct nvkmd_nvgpu_native_dev, base)->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_dev_alloc_va_locked(
      base, log_obj, flags, pte_kind, size_B, align_B, fixed_addr, va_out);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

static VkResult
native_dev_create_ctx(struct nvkmd_dev *base,
                      struct vk_object_base *log_obj,
                      enum nvkmd_engines engines,
                      struct nvkmd_ctx **ctx_out)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   if (dev->context_count == NATIVE_MAX_SOFTWARE_CONTEXTS)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   /* Copy-only contexts (NVK's transfer queue and its upload context) use
    * the copy channel when it is ready. */
   struct nvkmd_nvgpu_native_device_owner *owner = dev->owner;
   struct nvkmd_nvgpu_gpu_endpoint *endpoint =
      engines == NVKMD_ENGINE_COPY && owner->copy.ready &&
      (ouncevk_debug_sync_flags_load(),
       !(ouncevk_debug_sync_flags & 256)) ?
      &owner->copy.endpoint : &owner->endpoint;
   VkResult result = nvkmd_nvgpu_native_ctx_create(
      base, owner, engines, endpoint, &dev->context_count, ctx_out);
   if (result == VK_SUCCESS)
      dev->context_count++;
   return result;
}

static VkResult
native_dev_import_host_ptr(struct nvkmd_dev *base,
                           struct vk_object_base *log_obj,
                           void *host_ptr, uint64_t size_B,
                           enum nvkmd_mem_flags flags,
                           struct nvkmd_mem **mem_out)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_dev *dev =
      container_of(base, struct nvkmd_nvgpu_native_dev, base);
   if (!dev->owner->segments_enabled)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   return nvkmd_nvgpu_native_host_mem_create(base, dev->owner, host_ptr,
                                              size_B, flags, mem_out);
}

static bool
native_dev_host_ptr_importable(struct nvkmd_dev *base, const void *host_ptr)
{
   (void)base;
   return nvkmd_nvgpu_native_host_ptr_importable(host_ptr);
}

static const struct nvkmd_dev_ops native_dev_ops = {
   .destroy = native_dev_destroy,
   .get_gpu_timestamp = native_dev_timestamp,
   .read_gpu_timestamp = native_dev_read_timestamp,
   .alloc_mem = native_dev_alloc_mem,
   .alloc_tiled_mem = native_dev_alloc_tiled,
   .alloc_va = native_dev_alloc_va,
   .create_ctx = native_dev_create_ctx,
   .import_host_ptr = native_dev_import_host_ptr,
   .host_ptr_importable = native_dev_host_ptr_importable,
};

static VkResult
native_pdev_create_dev(struct nvkmd_pdev *base,
                       struct vk_object_base *log_obj,
                       struct nvkmd_dev **dev_out)
{
   (void)log_obj;
   if (dev_out == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   *dev_out = NULL;
   struct nvkmd_nvgpu_native_pdev *pdev =
      container_of(base, struct nvkmd_nvgpu_native_pdev, base);
   if (!nvkmd_nvgpu_native_device_common_valid(pdev->owner) ||
       pdev->owner->retiring ||
       pdev->owner->active_devices == UINT32_MAX ||
       pdev->device_count == UINT32_MAX) {
      nvkmd_nvgpu_native_owner_log_refusal(pdev->owner, "create device");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   struct nvkmd_nvgpu_native_dev *dev = calloc(1, sizeof(*dev));
   if (dev == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   dev->base.ops = &native_dev_ops;
   dev->base.pdev = base;
   /* The VA range is the whole lease: the map and kind alias of every
    * segment. */
   dev->base.va_start = pdev->owner->arena_ref->gpu_base;
   dev->base.va_end = dev->base.va_start +
      (pdev->owner->segments_enabled ? pdev->owner->lease.length :
                                       pdev->owner->arena_ref->length);
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   dev->owner = pdev->owner;
   pdev->device_count++;
   dev->owner->active_devices++;
   *dev_out = &dev->base;
   return VK_SUCCESS;
}

static uint64_t
native_arena_unused_bytes(const struct nvkmd_nvgpu_native_arena *arena)
{
   const uint64_t live = nvkmd_nvgpu_native_arena_live_bytes(arena);
   return live < arena->length ? arena->length - live : 0;
}

/* Game role: further allocations can use the free bytes of the heap and the
 * empty space in segments. */
static uint64_t
native_pdev_sysmem_available(struct nvkmd_pdev *base)
{
   struct nvkmd_nvgpu_native_pdev *pdev =
      container_of(base, struct nvkmd_nvgpu_native_pdev, base);
   struct nvkmd_nvgpu_native_device_owner *owner = pdev->owner;
   if (!owner->segments_enabled)
      return base->bounded_sysmem_size_B;
   uint64_t available = 0;
   nvkmd_nvgpu_native_heap_query(NULL, &available);
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return available;
   available += native_arena_unused_bytes(owner->arena_ref);
   for (uint32_t i = 0; i < owner->segment_count; i++)
      if (owner->segments[i].ready)
         available += native_arena_unused_bytes(&owner->segments[i].arena);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return MIN2(available, base->bounded_sysmem_size_B);
}

static const struct nvkmd_pdev_ops native_pdev_ops = {
   .destroy = native_pdev_destroy,
   .get_vram_used = native_pdev_vram_used,
   .get_sysmem_available = native_pdev_sysmem_available,
   .create_dev = native_pdev_create_dev,
};

VkResult
nvkmd_nvgpu_native_create_pdev(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nv_device_info *info,
   enum nvk_debug debug_flags,
   struct nvkmd_pdev **pdev_out)
{
   if (pdev_out == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   *pdev_out = NULL;
   if (!nvkmd_nvgpu_native_device_common_valid(owner) ||
       owner->retiring || !info ||
       owner->active_pdevs == UINT32_MAX ||
       !owner->arena_ref->length)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (!owner->progress) {
      VkResult result = nvkmd_nvgpu_native_progress_create(
         owner, &owner->progress);
      if (result != VK_SUCCESS)
         return result;
   }
   struct nvkmd_nvgpu_native_pdev *pdev = calloc(1, sizeof(*pdev));
   if (pdev == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   pdev->base.ops = &native_pdev_ops;
   pdev->base.debug_flags = debug_flags;
   pdev->base.dev_info = *info;
   /* Game role: segments of every kind come from the process heap, which
    * the CPU shares. */
   pdev->base.bounded_sysmem_size_B = owner->segments_enabled ?
      owner->heap_size_B : owner->arena_ref->length;
   pdev->base.has_cached_memory = owner->segments_enabled;
   pdev->base.has_readback_memory = owner->segments_enabled;
   /* Imports take the readback memory type (HOST_CACHED, non-coherent). */
   pdev->base.has_host_ptr_import = owner->segments_enabled;
   pdev->base.host_ptr_align_B = NVKMD_NVGPU_NATIVE_HOST_ALIGN_B;
   pdev->base.bind_align_B = NATIVE_BIND_ALIGN_B;
   /* Sparse binding: reserved VAs, views bound and unbound by
    * the bind context. Sparse residency is not reported (no zero pages
    * for unbound ranges). */
   pdev->base.kmd_info.has_sparse = true;
   pdev->owner = owner;
   pdev->timeline_type =
      vk_sync_timeline_get_type(&nvkmd_nvgpu_native_syncobj_type);
   pdev->timeline_type.register_state = native_timeline_register;
   pdev->timeline_type.unregister_state = native_timeline_unregister;
   pdev->timeline_type.state_data = owner->progress;
   pdev->sync_types[0] = &pdev->timeline_type.sync;
   pdev->sync_types[1] = &nvkmd_nvgpu_native_syncobj_type;
   pdev->base.sync_types = pdev->sync_types;
   owner->active_pdevs++;
   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}
