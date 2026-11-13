/* SPDX-License-Identifier: MIT */
/* Local Vulkan memory wrapper over the mode-2 GPU arena. The
 * native_backing, native_map and native_arena_bind files create the GPU
 * mapping and its ownership. This file never creates a VA without an owner.
 * It never claims that a remote slice is retired.
 */
#include "nvkmd_nvgpu_native_mem.h"
#include "nvkmd_nvgpu_native_device_owner.h"
#include "nvkmd_nvgpu_native_cache.h"

#include "util/log.h"
#include "util/macros.h"

#include <stdlib.h>

enum {
   NATIVE_MEM_PAGE_B = 4096,
   /* The first uncompressed Turing/Ampere 2D profile. The map owner accepts
    * kind 6. */
   NATIVE_MEM_UNCOMPRESSED_PTE_KIND = 6,
};

struct nvkmd_nvgpu_native_mem {
   struct nvkmd_mem base;
   struct nvkmd_va va;
   struct nvkmd_nvgpu_native_arena *arena;
   struct nvkmd_nvgpu_native_slice slice;
   uint8_t pte_kind;
   uint16_t tile_mode;
};

static VkResult
native_mem_map(struct nvkmd_mem *base,
               struct vk_object_base *log_obj,
               enum nvkmd_mem_map_flags flags,
               void *fixed_addr,
               void **map_out)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   if (map_out == NULL || fixed_addr != NULL ||
       (flags & NVKMD_MEM_MAP_FIXED) ||
       (flags & NVKMD_MEM_MAP_RDWR) == 0 ||
       mem->arena == NULL || mem->arena->self != mem->arena ||
       mem->slice.generation != mem->arena->generation)
      return VK_ERROR_MEMORY_MAP_FAILED;
   *map_out = (void *)(uintptr_t)mem->slice.cpu;
   return VK_SUCCESS;
}

static void
native_mem_unmap(struct nvkmd_mem *base,
                 enum nvkmd_mem_map_flags flags,
                 void *map)
{
   (void)base;
   (void)flags;
   (void)map;
   /* The underlying mode-2 backing stays mapped until the process exits. */
}

static void
native_mem_sync(struct nvkmd_mem *base,
                uint64_t offset_B,
                uint64_t range_B)
{
   const struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   /* CPU-cached readback memory (mode 1). Flush and invalidate both run
    * dc civac over the lines that cover the range, then dsb sy. See
    * nvkmd_nvgpu_native_cache.h. */
   if (mem->arena && mem->arena->cpu_cached && offset_B < mem->slice.length) {
      const uint64_t range = MIN2(range_B, mem->slice.length - offset_B);
      nvkmd_nvgpu_native_dcache_clean_invalidate(mem->slice.cpu + offset_B,
                                                 range);
      return;
   }
   /* Mode-2 memory uses a full-system barrier. This is a backing policy. It
    * is not derived from a CPU atom size and does not claim HOST_COHERENT. */
#if defined(__aarch64__)
   __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static VkResult
native_mem_export_dma_buf(struct nvkmd_mem *base,
                          struct vk_object_base *log_obj,
                          int *fd_out)
{
   (void)base;
   (void)log_obj;
   (void)fd_out;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static uint32_t
native_mem_log_handle(struct nvkmd_mem *base)
{
   (void)base;
   return 0;
}

static bool
native_mem_can_bind_image_direct(struct nvkmd_mem *base,
                                 uint8_t pte_kind, uint16_t tile_mode,
                                 uint64_t offset_B, uint64_t size_B)
{
   const struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   (void)tile_mode;
   return mem->arena && mem->arena->self == mem->arena &&
          mem->slice.generation == mem->arena->generation &&
          pte_kind == NATIVE_MEM_UNCOMPRESSED_PTE_KIND &&
          /* Tile mode is part of the image layout. The whole-arena map is
           * always kind 6, whatever the metadata of this allocation. */
          size_B && offset_B < mem->slice.length &&
          size_B <= mem->slice.length - offset_B &&
          base->va == &mem->va && base->va->addr == mem->slice.gpu;
}

/* Depth/stencil PTE kinds (Z16, S8, S8Z24, ZF32_X24S8, Z24S8 = 1-5, as in
 * NIL tu102_choose_pte_kind) go in the kind alias of the cacheable pool.
 * The device owner holds that alias. */
static VkResult
native_mem_bind_image_kind(struct nvkmd_mem *base,
                           uint8_t pte_kind, uint16_t tile_mode,
                           uint64_t offset_B, uint64_t size_B,
                           uint64_t *addr_out)
{
   struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   (void)tile_mode;
   uint64_t addr = 0;
   if (!mem->arena || mem->arena->self != mem->arena || !mem->arena->owner ||
       mem->slice.generation != mem->arena->generation ||
       !size_B || offset_B >= mem->slice.length ||
       size_B > mem->slice.length - offset_B ||
       !nvkmd_nvgpu_native_owner_kind_alias(mem->arena->owner, mem->arena,
                                            mem->slice.gpu + offset_B, size_B,
                                            pte_kind, &addr))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   *addr_out = addr;
   return VK_SUCCESS;
}

static void
native_mem_free(struct nvkmd_mem *base)
{
   struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   if (mem->arena && mem->slice.serial) {
      /* Any thread can free. The submit path also uses the arena. */
      struct nvkmd_nvgpu_native_device_owner *owner = mem->arena->owner;
      const bool locked = owner && nvkmd_nvgpu_native_owner_lock(owner);
      nvkmd_nvgpu_native_arena_release(mem->arena, &mem->slice);
      if (locked) {
         nvkmd_nvgpu_native_owner_segment_emptied(owner, mem->arena,
                                                  base->dev);
         nvkmd_nvgpu_native_owner_unlock(owner);
      }
   }
   simple_mtx_destroy(&base->map_mutex);
   free(mem);
}

static const struct nvkmd_mem_ops native_mem_ops = {
   .free = native_mem_free,
   .map = native_mem_map,
   .unmap = native_mem_unmap,
   .sync_to_gpu = native_mem_sync,
   .sync_from_gpu = native_mem_sync,
   .export_dma_buf = native_mem_export_dma_buf,
   .log_handle = native_mem_log_handle,
   .can_bind_image_direct = native_mem_can_bind_image_direct,
   .bind_image_kind = native_mem_bind_image_kind,
};

/* Pin and unpin change the arena, so they take the owner lock, as
 * allocation and free do. The lock is recursive. */
bool
nvkmd_nvgpu_native_mem_pin_scanout(struct nvkmd_mem *base)
{
   if (!base || base->ops != &native_mem_ops)
      return false;
   struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   if (!mem->arena || !mem->slice.serial ||
       !nvkmd_nvgpu_native_owner_lock(mem->arena->owner))
      return false;
   const bool pinned =
      nvkmd_nvgpu_native_arena_pin(mem->arena, &mem->slice) ==
      NVKMD_NVGPU_NATIVE_ARENA_OK;
   nvkmd_nvgpu_native_owner_unlock(mem->arena->owner);
   return pinned;
}

bool
nvkmd_nvgpu_native_mem_unpin_scanout(struct nvkmd_mem *base)
{
   if (!base || base->ops != &native_mem_ops)
      return false;
   struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   if (!mem->arena || !mem->slice.serial ||
       !nvkmd_nvgpu_native_owner_lock(mem->arena->owner))
      return false;
   const bool unpinned =
      nvkmd_nvgpu_native_arena_unpin(mem->arena, &mem->slice) ==
      NVKMD_NVGPU_NATIVE_ARENA_OK;
   /* The unpin can recycle the last allocation, if it was already freed. */
   if (unpinned)
      nvkmd_nvgpu_native_owner_segment_emptied(mem->arena->owner, mem->arena,
                                               base->dev);
   nvkmd_nvgpu_native_owner_unlock(mem->arena->owner);
   return unpinned;
}

VkResult
nvkmd_nvgpu_native_mem_map_view(const struct nvkmd_mem *base,
                                uint64_t offset_B, uint64_t size_B,
                                uint64_t view_addr, uint8_t kind,
                                struct nvkmd_nvgpu_native_view_source *source_out)
{
   if (!base || base->ops != &native_mem_ops)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, const struct nvkmd_nvgpu_native_mem, base);
   if (!mem->arena || mem->arena->self != mem->arena || !mem->arena->owner ||
       mem->slice.generation != mem->arena->generation || !size_B ||
       offset_B >= mem->slice.length || size_B > mem->slice.length - offset_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   return nvkmd_nvgpu_native_owner_map_view(mem->arena->owner, mem->arena,
                                            mem->slice.gpu + offset_B, size_B,
                                            view_addr, kind, source_out);
}

bool
nvkmd_nvgpu_native_mem_matches_slice(
   const struct nvkmd_mem *base,
   const struct nvkmd_nvgpu_native_slice *slice)
{
   if (!base || base->ops != &native_mem_ops || !slice || !slice->serial)
      return false;
   const struct nvkmd_nvgpu_native_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_mem, base);
   return mem->arena && mem->arena->self == mem->arena &&
          mem->slice.generation == slice->generation &&
          mem->slice.serial == slice->serial &&
          mem->slice.offset == slice->offset &&
          mem->slice.length == slice->length;
}

VkResult
nvkmd_nvgpu_native_mem_create(struct nvkmd_dev *dev,
                               struct nvkmd_nvgpu_native_arena *arena,
                               uint64_t size_B,
                               uint64_t align_B,
                               enum nvkmd_mem_flags flags,
                               uint8_t pte_kind,
                               uint16_t tile_mode,
                               const struct nvkmd_nvgpu_native_slice *reserved,
                               struct nvkmd_mem **mem_out)
{
   if (mem_out == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;
   *mem_out = NULL;
   if (dev == NULL || arena == NULL || arena->self != arena ||
       arena->retiring ||
       size_B == 0 || align_B > UINT32_MAX ||
       (flags & (NVKMD_MEM_SHARED | NVKMD_MEM_COMPRESSED)) ||
       ((flags & (NVKMD_MEM_COHERENT | NVKMD_MEM_CPU_UNCACHED)) &&
        !arena->coherent_eligible) ||
       ((flags & (NVKMD_MEM_GPU_UNCACHED | NVKMD_MEM_CAN_MAP)) &&
        !arena->host_mappable) ||
       ((flags & NVKMD_MEM_CPU_CACHED) && !arena->cpu_cached) ||
       /* With kind 6, the tile mode is image metadata only: every block
        * height maps as kind 6. */
       !((pte_kind == 0 && tile_mode == 0) ||
         pte_kind == NATIVE_MEM_UNCOMPRESSED_PTE_KIND))
      return VK_ERROR_FEATURE_NOT_PRESENT;

   if (align_B == 0 || align_B < NATIVE_MEM_PAGE_B)
      align_B = NATIVE_MEM_PAGE_B;
   if (flags & NVKMD_MEM_COHERENT)
      flags |= NVKMD_MEM_CPU_UNCACHED | NVKMD_MEM_GPU_UNCACHED;
   struct nvkmd_nvgpu_native_mem *mem = calloc(1, sizeof(*mem));
   if (mem == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (reserved) {
      if (!reserved->serial || reserved->generation != arena->generation ||
          reserved->cpu != arena->cpu_base + reserved->offset ||
          reserved->gpu != arena->gpu_base + reserved->offset ||
          reserved->length < size_B || reserved->gpu % align_B ||
          reserved->cpu % align_B) {
         free(mem);
         return VK_ERROR_FEATURE_NOT_PRESENT;
      }
      mem->slice = *reserved;
   } else {
      const enum nvkmd_nvgpu_native_arena_status status =
         nvkmd_nvgpu_native_arena_alloc_reclaimable(
            arena, size_B, align_B, &mem->slice);
      if (status != NVKMD_NVGPU_NATIVE_ARENA_OK) {
         free(mem);
         if (status == NVKMD_NVGPU_NATIVE_ARENA_EXHAUSTED)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         if (status == NVKMD_NVGPU_NATIVE_ARENA_HOST_MEMORY)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         mesa_loge("native memory: arena status %d, size 0x%llx, "
                   "cursor 0x%llx of 0x%llx",
                   (int)status, (unsigned long long)size_B,
                   (unsigned long long)arena->cursor,
                   (unsigned long long)arena->length);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
   }

   /* A GPU-cacheable arena is never CPU mapped. CPU writes would bypass
    * lines that the GPU L2 can hold. */
   nvkmd_mem_init(dev, &mem->base, &native_mem_ops,
                  flags | (arena->host_mappable ? NVKMD_MEM_CAN_MAP : 0),
                  mem->slice.length, NATIVE_MEM_PAGE_B);
   mem->arena = arena;
   mem->pte_kind = pte_kind;
   mem->tile_mode = tile_mode;
   mem->va = (struct nvkmd_va) {
      .dev = dev,
      .addr = mem->slice.gpu,
      .size_B = mem->slice.length,
   };
   mem->base.va = &mem->va;
   *mem_out = &mem->base;
   return VK_SUCCESS;
}
