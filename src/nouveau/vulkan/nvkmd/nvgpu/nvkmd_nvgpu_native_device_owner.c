/* SPDX-License-Identifier: MIT */
/* OunceVK creates the native GPU device on the libounce-nx runtime. The order
 * is: CPU backing, TSG, VA, map, arena, notifier page and channel.
 */
#include "nvkmd_nvgpu_native_device_owner.h"
#include "nvkmd_nvgpu_native_budget.h"

#include "nvkmd_nvgpu_native_batch.h"
#include "nvkmd_nvgpu_native_ctx.h"
#include "nvkmd_nvgpu_native_chunked.h"
#include "nvkmd_nvgpu_native_host_mem.h"
#include "nvkmd_nvgpu_native_progress.h"
#include "nvkmd_nvgpu_native_views.h"

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <stdlib.h>
#include <string.h>
#include <switch.h>

/* The local handle of the notifier page. It is published after the page is
 * registered, and passed to the notifier ioctl of the channel. OunceVK
 * registers one notifier page. */
#define NOTIFIER_LOCAL_HANDLE 1u

_Static_assert(NVKMD_NVGPU_NATIVE_PRIMARY_B +
               NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B +
               NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B <=
               NVKMD_NVGPU_NATIVE_CPU_EXTENT_B,
               "aligned backing and notifier must fit borrowed CPU extent");
_Static_assert(NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B %
               NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B == 0,
               "pooled segments are whole alignment units");

/* The heap that libounce's malloc manages: for an NRO, the OverrideHeap of
 * the loader; otherwise, the heap from svcSetHeapSize. */
void
nvkmd_nvgpu_native_heap_query(uint64_t *size_B, uint64_t *available_B)
{
   if (size_B)
      *size_B = heapGetTotalSize();
   if (available_B)
      *available_B = heapGetFreeSize();
}

static void
observe(const struct nvkmd_nvgpu_native_device_io *io,
        struct nvkmd_nvgpu_native_device_owner *owner,
        enum nvkmd_nvgpu_native_device_stage stage,
        uint32_t result)
{
   owner->stage = stage;
   owner->last_result = result;
   if (io->observe)
      io->observe(io->context, stage, result);
}

bool
nvkmd_nvgpu_native_device_prepare(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_runtime *runtime,
   const struct nvkmd_nvgpu_native_device_io *io)
{
   if (!owner || !runtime || !io || !io->borrow_cpu ||
       owner->attempted || owner->self)
      return false;
   if (!nvkmd_nvgpu_native_runtime_ready(runtime) ||
       runtime->stage != NVKMD_NVGPU_RUNTIME_GPU_FILE_OPEN_RECEIVED ||
       !runtime->delegate.characteristics_decoded ||
       nvkmd_nvgpu_native_profile_screen(&runtime->delegate.characteristics) !=
          NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED)
      return false;

   owner->attempted = true;
   owner->self = owner;
   owner->runtime = runtime;
   if (!nvkmd_nvgpu_native_owner_lock_init(owner))
      return false;
   uint32_t result = io->borrow_cpu(io->context, &owner->cpu_base,
                                    &owner->cpu_extent_B);
   observe(io, owner, NVKMD_NVGPU_DEVICE_CPU_RESERVED, result);
   struct nvkmd_nvgpu_native_budget_layout layout;
   if (result || !nvkmd_nvgpu_native_budget_layout(
          owner->cpu_base, owner->cpu_extent_B, &layout))
      return false;

   /* The primary segment (uncached). Other segments are created on demand
    * (nvkmd_nvgpu_native_owner_grow). */
   result = nvkmd_nvgpu_native_backing_prepare(
      &owner->backing, runtime, layout.backing_base,
      NVKMD_NVGPU_NATIVE_PRIMARY_B, false);
   observe(io, owner, NVKMD_NVGPU_DEVICE_BACKING,
           result ? owner->backing.result : 0);
   if (result != NVKMD_NVGPU_BACKING_OK)
      return false;

   result = nvkmd_nvgpu_native_runtime_open_tsg(runtime);
   observe(io, owner, NVKMD_NVGPU_DEVICE_TSG, result ? runtime->result : 0);
   if (result != NVKMD_NVGPU_RUNTIME_OK)
      return false;

   result = nvkmd_nvgpu_native_va_admit(&owner->va, runtime);
   if (result == NVKMD_NVGPU_NATIVE_VA_OK) {
      struct nvkmd_nvgpu_native_va_result va =
         nvkmd_nvgpu_native_va_lease_whole(&owner->va,
                                           NVKMD_NVGPU_NATIVE_GPU_VA_LEASE_B,
                                           NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B);
      if (va.status == NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED)
         va = nvkmd_nvgpu_native_va_lease_whole(&owner->va,
                                                NVKMD_NVGPU_NATIVE_GPU_VA_LEASE_MIN_B,
                                                NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B);
      result = va.status;
      owner->lease = va.lease;
   }
   observe(io, owner, NVKMD_NVGPU_DEVICE_VA, result);
   if (result != NVKMD_NVGPU_NATIVE_VA_OK)
      return false;

   const Result slot_rc = nvkmd_nvgpu_native_slot_open(&owner->slot, runtime);
   if (R_FAILED(slot_rc)) {
      observe(io, owner, NVKMD_NVGPU_DEVICE_MAP, slot_rc);
      return false;
   }
   /* The primary segment maps at the start of the lease. Every other map
    * takes its own interval from the VA heap. The service needs a
    * permitted interval that does not overlap another, for each mapping. */
   util_vma_heap_init(&owner->va_heap,
                      owner->lease.start + NVKMD_NVGPU_NATIVE_PRIMARY_B,
                      owner->lease.length - NVKMD_NVGPU_NATIVE_PRIMARY_B);
   owner->va_heap.alloc_high = false;
   owner->va_heap_ready = true;
   owner->kind_alias_ready = true;
   nvkmd_nvgpu_native_heap_query(&owner->heap_size_B, NULL);
   owner->map_lease = owner->lease;
   owner->map_lease.length = NVKMD_NVGPU_NATIVE_PRIMARY_B;
   result = nvkmd_nvgpu_native_map_once(&owner->map, runtime,
                                        &owner->map_lease, &owner->backing,
                                        false, &owner->slot);
   observe(io, owner, NVKMD_NVGPU_DEVICE_MAP, result ? owner->map.result : 0);
   if (result != NVKMD_NVGPU_NATIVE_MAP_OK)
      return false;

   result = nvkmd_nvgpu_native_arena_bind(
      &owner->arena, runtime, &owner->backing, &owner->map, &owner->map_lease);
   observe(io, owner, NVKMD_NVGPU_DEVICE_ARENA, result);
   if (result != NVKMD_NVGPU_NATIVE_ARENA_OK)
      return false;

   /* The notifier page uses mode 1: clear, SVC 0x80, then worker raw 0x75
    * without the mode-2 attribute walk. */
   result = nvkmd_nvgpu_native_backing_prepare(
      &owner->notifier, runtime, layout.notifier_base,
      NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B, true);
   observe(io, owner, NVKMD_NVGPU_DEVICE_NOTIFIER,
           result ? owner->notifier.result : 0);
   if (result != NVKMD_NVGPU_BACKING_OK)
      return false;

   result = nvkmd_nvgpu_native_channel_open(&owner->channel, runtime);
   observe(io, owner, NVKMD_NVGPU_DEVICE_CHANNEL,
           result ? owner->channel.result : 0);
   if (result != NVKMD_NVGPU_CHANNEL_OK)
      return false;
   result = nvkmd_nvgpu_native_channel_configure(
      &owner->setup, &owner->channel, NOTIFIER_LOCAL_HANDLE);
   observe(io, owner, NVKMD_NVGPU_DEVICE_CONFIGURED,
           result ? owner->setup.result : 0);
   if (result != NVKMD_NVGPU_CHANNEL_SETUP_OK)
      return false;

   owner->arena_ref = &owner->arena;
   owner->arena.owner = owner;
   owner->segments_enabled = true;
   owner->tls = runtime->tls;
   owner->mapped_gpu_base = owner->map.gpu_va;
   owner->mapped_length = owner->map.length;
   owner->mapped_ready = owner->map.ready;
   owner->endpoint = (struct nvkmd_nvgpu_gpu_endpoint) {
      .generation = runtime->generation,
      .gpu_fd = runtime->gpu_file.fd,
      .producer_id = nvkmd_nvgpu_native_channel_producer_id(&owner->setup),
      .sync = &runtime->sync,
      .gpu_fd_ready = runtime->gpu_file.open,
   };
   owner->ready = true;
   const bool valid = nvkmd_nvgpu_native_device_common_valid(owner);
   owner->ready = valid;
   return valid;
}

void
nvkmd_nvgpu_native_owner_log_refusal(
   const struct nvkmd_nvgpu_native_device_owner *owner, const char *where)
{
   if (!owner) {
      mesa_loge("native %s: no owner", where);
      return;
   }
   mesa_loge("native %s: self %d ready %d retiring %d runtime %d arena %d "
             "mapped %d endpoint %d tls %d map %d setup %d endpoint-valid %d",
             where, owner->self == owner, owner->ready, owner->retiring,
             owner->runtime != NULL,
             owner->arena_ref == &owner->arena &&
                owner->arena.self == &owner->arena,
             owner->mapped_ready &&
                owner->mapped_gpu_base == owner->arena.gpu_base &&
                owner->mapped_length == owner->arena.length,
             owner->endpoint.generation == owner->arena.generation,
             owner->runtime && owner->tls && owner->tls == owner->runtime->tls,
             owner->map.ready, owner->setup.configured,
             nvkmd_nvgpu_gpu_endpoint_valid(&owner->endpoint));
}

bool
nvkmd_nvgpu_native_device_common_valid(
   const struct nvkmd_nvgpu_native_device_owner *owner)
{
   return owner && owner->self == owner && owner->ready &&
          owner->runtime && owner->arena_ref == &owner->arena &&
          owner->arena.self == &owner->arena && owner->mapped_ready &&
          owner->mapped_gpu_base == owner->arena.gpu_base &&
          owner->mapped_length == owner->arena.length &&
          owner->endpoint.generation == owner->arena.generation &&
          owner->tls && owner->tls == owner->runtime->tls &&
          owner->map.ready && owner->setup.configured &&
          nvkmd_nvgpu_gpu_endpoint_valid(&owner->endpoint);
}

bool
nvkmd_nvgpu_native_owner_kind_alias(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_arena *arena,
   uint64_t gpu_addr, uint64_t size_B, uint8_t kind, uint64_t *alias_addr)
{
   if (!owner || !arena || !alias_addr || !owner->kind_alias_ready ||
       kind < 1 || kind > 5)
      return false;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return false;
   struct nvkmd_nvgpu_native_segment *segment = NULL;
   for (uint32_t i = 0; i < owner->segment_count && !segment; i++) {
      if (&owner->segments[i].arena == arena && owner->segments[i].ready &&
          owner->segments[i].kind == NVKMD_NVGPU_SEGMENT_CACHED)
         segment = &owner->segments[i];
   }
   const struct nvkmd_nvgpu_native_map_owner *pool =
      segment ? &segment->map : NULL;
   if (!pool || gpu_addr < pool->gpu_va || !size_B || size_B > pool->length ||
       gpu_addr - pool->gpu_va > pool->length - size_B) {
      nvkmd_nvgpu_native_owner_unlock(owner);
      return false;
   }
   const uint64_t offset_B = gpu_addr - pool->gpu_va;
   /* The kind alias of the segment maps its backing again, at a VA of its
    * own. It uses the VA alignment of the segment, kind 6 and the same
    * cacheability. OunceVK makes it once, on first use. A failed map is not
    * retried because its remote state is unknown. Its VA is not reused. */
   if (!segment->alias_attempted) {
      segment->alias_attempted = true;
      segment->alias_lease = segment->lease;
      segment->alias_lease.start =
         util_vma_heap_alloc(&owner->va_heap, segment->length_B,
                             segment->va_align_B);
      if (segment->alias_lease.start &&
          nvkmd_nvgpu_native_map_once(
             &segment->alias_map, owner->runtime, &segment->alias_lease,
             &segment->backing, true,
             &owner->slot) == NVKMD_NVGPU_NATIVE_MAP_OK)
         segment->alias_ready = true;
   }
   if (!segment->alias_ready) {
      owner->stats.kind_failures++;
      nvkmd_nvgpu_native_owner_unlock(owner);
      return false;
   }
   const enum nvkmd_nvgpu_native_map_status status =
      nvkmd_nvgpu_native_map_modify_kind(&segment->alias_map, owner->runtime,
                                         offset_B, size_B, kind,
                                         &owner->slot);
   if (status == NVKMD_NVGPU_NATIVE_MAP_OK)
      owner->stats.kind_changes++;
   else
      owner->stats.kind_failures++;
   const uint64_t alias_base = segment->alias_map.gpu_va;
   nvkmd_nvgpu_native_owner_unlock(owner);
   if (status != NVKMD_NVGPU_NATIVE_MAP_OK)
      return false;
   *alias_addr = alias_base + offset_B;
   return true;
}

/* Completes all earlier work on the channel and writes back dirty GPU L2
 * lines. Use it before segments are released when no flushing submission
 * exists to wait for (growth without room). It submits the QueueFinish
 * prefix (nvkmd_nvgpu_native_batch_dirty_flush) as a separate batch. Then it
 * waits for the completion marker of that batch. */
static VkResult
native_owner_finish_flush(struct nvkmd_nvgpu_native_device_owner *owner,
                          struct nvkmd_dev *dev,
                          struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_native_slice words = {0};
   if (nvkmd_nvgpu_native_arena_alloc_reclaimable(
          owner->arena_ref, sizeof(nvkmd_nvgpu_native_batch_dirty_flush),
          4096, &words) != NVKMD_NVGPU_NATIVE_ARENA_OK)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   memcpy((void *)(uintptr_t)words.cpu, nvkmd_nvgpu_native_batch_dirty_flush,
          sizeof(nvkmd_nvgpu_native_batch_dirty_flush));
   __asm__ volatile("dsb sy" ::: "memory");
   uint32_t contexts = 1;
   struct nvkmd_ctx *ctx = NULL;
   /* The finish flush runs on the primary channel: the segments' earlier
    * work is there or on the copy channel, whose batches always end with
    * the dirty flush. */
   VkResult result = nvkmd_nvgpu_native_ctx_create(
      dev, owner, NVKMD_ENGINE_COPY, &owner->endpoint, &contexts, &ctx);
   if (result == VK_SUCCESS) {
      const struct nvkmd_ctx_exec exec = {
         .addr = words.gpu,
         .size_B = sizeof(nvkmd_nvgpu_native_batch_dirty_flush),
      };
      result = nvkmd_ctx_exec(ctx, log_obj, 1, &exec);
      if (result == VK_SUCCESS)
         result = nvkmd_ctx_sync(ctx, log_obj);
      nvkmd_ctx_destroy(ctx);
   }
   /* Keep the words if the batch can still run. */
   if (result == VK_SUCCESS)
      nvkmd_nvgpu_native_arena_release(owner->arena_ref, &words);
   return result;
}

/* Releases one empty segment. It unmaps the kind alias first, if there is
 * one. Then it unmaps the map of the segment itself. Each unmap is raw 0xe5
 * followed by a wait. It then releases
 * the backing (see nvkmd_nvgpu_native_backing_release). Last, it returns
 * the VA and memory. The caller has joined every earlier use with
 * native_owner_finish_flush. A failed step marks the segment failed and
 * keeps its entry, VA and memory; device close calls this again. */
static bool
native_owner_release_segment(struct nvkmd_nvgpu_native_device_owner *owner,
                             struct nvkmd_nvgpu_native_segment *segment)
{
   if (segment->release_pending && !segment->release_flush)
      owner->releases_waiting--;
   segment->release_pending = false;
   segment->ready = false;
   segment->failed = true;
   /* Each step is skipped once it is done, so a call after a failed step
    * continues with that step. */
   if (segment->alias_map.mapped) {
      if (nvkmd_nvgpu_native_map_unmap(&segment->alias_map, owner->runtime,
                                       &segment->backing, &owner->slot) !=
          NVKMD_NVGPU_NATIVE_MAP_OK)
         return false;
      segment->alias_ready = false;
      util_vma_heap_free(&owner->va_heap, segment->alias_lease.start,
                         segment->length_B);
   }
   if (segment->map.mapped &&
       nvkmd_nvgpu_native_map_unmap(&segment->map, owner->runtime,
                                    &segment->backing, &owner->slot) !=
          NVKMD_NVGPU_NATIVE_MAP_OK)
      return false;
   if (nvkmd_nvgpu_native_backing_held(&segment->backing) &&
       nvkmd_nvgpu_native_backing_release(&segment->backing, owner->runtime,
                                          &owner->slot) !=
          NVKMD_NVGPU_BACKING_OK)
      return false;
   util_vma_heap_free(&owner->va_heap, segment->lease.start,
                      segment->length_B);
   free(segment->cpu_allocation);
   nvkmd_nvgpu_native_arena_finish(&segment->arena);
   memset(segment, 0, sizeof(*segment));
   owner->stats.segments_released++;
   return true;
}

/* A segment is empty when no slice is live and no VA view maps it (the
 * segment's own map and its kind alias are the only maps). */
static bool
native_segment_empty(const struct nvkmd_nvgpu_native_segment *segment)
{
   return segment->ready && !segment->arena.live_records &&
          segment->backing.maps == 1u + (segment->alias_map.mapped ? 1u : 0u);
}

/* Releases every empty segment (no live slice) after one finish flush.
 * The caller holds the owner lock. */
static uint32_t
native_owner_reclaim(struct nvkmd_nvgpu_native_device_owner *owner,
                     struct nvkmd_dev *dev, struct vk_object_base *log_obj)
{
   if (!dev)
      return 0;
   /* Mark the candidates first. The join of the flush releases the owner
    * lock, and no other thread can allocate in them during that time. */
   bool any = false;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (native_segment_empty(segment)) {
         segment->arena.retiring = true;
         any = true;
      }
   }
   if (!any)
      return 0;
   if (native_owner_finish_flush(owner, dev, log_obj) != VK_SUCCESS) {
      for (uint32_t i = 0; i < owner->segment_count; i++)
         if (owner->segments[i].ready && !owner->segments[i].release_pending)
            owner->segments[i].arena.retiring = false;
      return 0;
   }
   uint32_t released = 0;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (!segment->ready || !segment->arena.retiring)
         continue;
      if (!native_owner_release_segment(owner, segment)) {
         owner->stats.segment_release_failures++;
         break;
      }
      released++;
   }
   return released;
}

/* Bytes in empty segments kept for reuse (not marked for release). */
static uint64_t
native_owner_cached_bytes(const struct nvkmd_nvgpu_native_device_owner *owner)
{
   uint64_t bytes = 0;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      const struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (native_segment_empty(segment) && !segment->release_pending)
         bytes += segment->length_B;
   }
   return bytes;
}

void
nvkmd_nvgpu_native_owner_segment_emptied(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_arena *arena, struct nvkmd_dev *dev)
{
   if (!owner || !arena || arena == owner->arena_ref ||
       !nvkmd_nvgpu_native_owner_lock(owner))
      return;
   struct nvkmd_nvgpu_native_segment *segment = NULL;
   for (uint32_t i = 0; i < owner->segment_count && !segment; i++)
      if (&owner->segments[i].arena == arena)
         segment = &owner->segments[i];
   if (segment && native_segment_empty(segment) && !segment->release_pending &&
       !segment->arena.retiring) {
      /* The cache, without this segment, plus this segment. */
      const uint64_t cached =
         native_owner_cached_bytes(owner) - segment->length_B;
      if (cached + segment->length_B > NVKMD_NVGPU_NATIVE_SEGMENT_CACHE_B) {
         segment->release_pending = true;
         segment->arena.retiring = true;
         owner->releases_waiting++;
         /* With no submission in flight, no later batch may come to carry
          * the dirty flush (for example a program that only maps memory).
          * Submit the finish flush now; its completion releases the
          * segment. */
         if (dev && owner->inflight_submissions == 0)
            (void)native_owner_finish_flush(owner, dev, NULL);
      }
   }
   nvkmd_nvgpu_native_owner_unlock(owner);
}

uint64_t
nvkmd_nvgpu_native_owner_claim_release_flush(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   const uint64_t serial = ++owner->flush_serial;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (segment->ready && segment->release_pending &&
          !segment->release_flush)
         segment->release_flush = serial;
   }
   owner->releases_waiting = 0;
   owner->stats.release_flushes++;
   return serial;
}

void
nvkmd_nvgpu_native_owner_release_flushed(
   struct nvkmd_nvgpu_native_device_owner *owner, uint64_t flush_serial)
{
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (!segment->ready || !segment->release_pending ||
          !segment->release_flush || segment->release_flush > flush_serial)
         continue;
      if (native_owner_release_segment(owner, segment))
         owner->stats.segments_released_on_retire++;
      else
         owner->stats.segment_release_failures++;
   }
}

static struct nvkmd_nvgpu_native_segment *
native_owner_free_entry(struct nvkmd_nvgpu_native_device_owner *owner)
{
   for (uint32_t i = 0; i < owner->segment_count; i++)
      if (!owner->segments[i].ready && !owner->segments[i].failed)
         return &owner->segments[i];
   if (owner->segment_count >= NVKMD_NVGPU_NATIVE_SEGMENT_MAX)
      return NULL;
   return &owner->segments[owner->segment_count++];
}

/* Takes room for a segment: a table entry, its heap block and its VA. On
 * success, returns all three through the outputs. Otherwise, returns the
 * missing one and takes nothing. */
static enum nvkmd_nvgpu_native_segment_failure
native_owner_take_room(struct nvkmd_nvgpu_native_device_owner *owner,
                       uint64_t length_B, uint64_t cpu_align_B,
                       uint64_t va_align_B,
                       struct nvkmd_nvgpu_native_segment **entry_out,
                       void **allocation_out, void **cpu_out,
                       uint64_t *va_out)
{
   struct nvkmd_nvgpu_native_segment *entry = native_owner_free_entry(owner);
   if (!entry)
      return NVKMD_NVGPU_SEGMENT_FAILURE_TABLE;
   /* The alignment is done here, not with aligned_alloc: the allocator
    * places a block with an alignment above 64 KiB only through OS memory,
    * which libounce does not use. */
   if (length_B > SIZE_MAX - cpu_align_B)
      return NVKMD_NVGPU_SEGMENT_FAILURE_HEAP;
   void *allocation = malloc(length_B + cpu_align_B);
   if (!allocation)
      return NVKMD_NVGPU_SEGMENT_FAILURE_HEAP;
   void *cpu = (void *)(uintptr_t)align64((uintptr_t)allocation, cpu_align_B);
   const uint64_t va = util_vma_heap_alloc(&owner->va_heap, length_B,
                                           va_align_B);
   if (!va) {
      free(allocation);
      return NVKMD_NVGPU_SEGMENT_FAILURE_VA;
   }
   *entry_out = entry;
   *allocation_out = allocation;
   *cpu_out = cpu;
   *va_out = va;
   return NVKMD_NVGPU_SEGMENT_FAILURE_NONE;
}

static struct nvkmd_nvgpu_native_arena *
native_owner_grow_failed(struct nvkmd_nvgpu_native_device_owner *owner,
                         enum nvkmd_nvgpu_native_segment_failure step,
                         uint32_t result, uint64_t length_B)
{
   owner->stats.segment_create_failures++;
   owner->stats.last_segment_failure =
      (uint32_t)step << 24 | (result & 0xffffffu);
   uint64_t ready_B = 0, failed_B = 0, empty_B = 0, pending_B = 0;
   uint32_t ready = 0, failed = 0, with_views = 0;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      const struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (segment->ready) {
         ready++;
         ready_B += segment->length_B;
         if (!segment->arena.live_records)
            empty_B += segment->length_B;
         if (segment->release_pending)
            pending_B += segment->length_B;
         if (segment->backing.maps > 1u + (segment->alias_map.mapped ? 1u : 0u))
            with_views++;
      } else if (segment->failed) {
         failed++;
         failed_B += segment->length_B;
      }
   }
   mesa_loge("native segment: %u ready (0x%llx bytes, 0x%llx empty, "
             "0x%llx pending release, %u with views), %u failed "
             "(0x%llx bytes)",
             ready, (unsigned long long)ready_B, (unsigned long long)empty_B,
             (unsigned long long)pending_B, with_views, failed,
             (unsigned long long)failed_B);
   mesa_loge("native segment: growth of 0x%llx bytes failed at step %u, "
             "result 0x%x; %u segments; heap 0x%llx, free 0x%llx",
             (unsigned long long)length_B, (unsigned)step, result,
             owner->segment_count,
             (unsigned long long)heapGetTotalSize(),
             (unsigned long long)heapGetFreeSize());
   return NULL;
}

static bool
native_segment_shaped(enum nvkmd_nvgpu_native_segment_kind kind,
                      const struct nvkmd_nvgpu_native_arena *arena)
{
   switch (kind) {
   case NVKMD_NVGPU_SEGMENT_UNCACHED:
      return arena->coherent_eligible && arena->host_mappable;
   case NVKMD_NVGPU_SEGMENT_CACHED:
      return !arena->host_mappable && !arena->coherent_eligible;
   case NVKMD_NVGPU_SEGMENT_READBACK:
      return arena->host_mappable && arena->cpu_cached &&
             !arena->coherent_eligible;
   }
   return false;
}

struct nvkmd_nvgpu_native_arena *
nvkmd_nvgpu_native_owner_grow(
   struct nvkmd_nvgpu_native_device_owner *owner,
   enum nvkmd_nvgpu_native_segment_kind kind,
   uint64_t size_B, uint64_t align_B,
   struct nvkmd_dev *dev, struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_native_runtime *runtime = owner ? owner->runtime : NULL;
   if (!owner || !runtime || !owner->segments_enabled ||
       !owner->va_heap_ready || !size_B ||
       !util_is_power_of_two_or_zero64(align_B) ||
       align_B > NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B)
      return NULL;
   const bool dedicated = nvkmd_nvgpu_native_segment_dedicated(size_B);
   /* The segment starts aligned, so the allocation needs no room beyond
    * size_B. */
   const uint64_t cpu_align_B =
      MAX2(align_B, NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B);
   const uint64_t va_align_B =
      MAX2(align_B, NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B);
   if (size_B > UINT64_MAX - va_align_B)
      return NULL;
   const uint64_t length_B = dedicated ?
      align64(size_B, NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B) :
      NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B;

   /* Without room, every empty segment goes back to the heap first, after
    * an explicit finish flush. */
   struct nvkmd_nvgpu_native_segment *segment = NULL;
   void *allocation = NULL;
   void *cpu = NULL;
   uint64_t va = 0;
   enum nvkmd_nvgpu_native_segment_failure room =
      native_owner_take_room(owner, length_B, cpu_align_B, va_align_B,
                             &segment, &allocation, &cpu, &va);
   /* Try again after the reclaim also when it released no candidate: its
    * finish flush completes pending releases, which also free memory. */
   if (room != NVKMD_NVGPU_SEGMENT_FAILURE_NONE) {
      (void)native_owner_reclaim(owner, dev, log_obj);
      room = native_owner_take_room(owner, length_B, cpu_align_B,
                                    va_align_B, &segment, &allocation, &cpu,
                                    &va);
   }
   if (room != NVKMD_NVGPU_SEGMENT_FAILURE_NONE)
      return native_owner_grow_failed(owner, room, 0, length_B);

   /* From here, the segment owns the entry, the memory and the VA, also
    * when a step below fails: device close releases what the failed
    * segment still has. */
   memset(segment, 0, sizeof(*segment));
   segment->kind = kind;
   segment->failed = true;
   segment->dedicated = dedicated;
   segment->cpu_allocation = allocation;
   segment->length_B = length_B;
   segment->va_align_B = va_align_B;
   segment->lease = owner->lease;
   segment->lease.start = va;
   segment->lease.length = length_B;
   if (nvkmd_nvgpu_native_backing_prepare(
          &segment->backing, runtime, (uintptr_t)cpu, length_B,
          kind == NVKMD_NVGPU_SEGMENT_READBACK) !=
       NVKMD_NVGPU_BACKING_OK)
      return native_owner_grow_failed(owner,
                                      NVKMD_NVGPU_SEGMENT_FAILURE_BACKING,
                                      segment->backing.result, length_B);
   if (nvkmd_nvgpu_native_map_once(
          &segment->map, runtime, &segment->lease, &segment->backing,
          /* Readback segments are GPU-cached too. Batches then
           * end with the dirty flush and start with the L2 invalidate. */
          kind == NVKMD_NVGPU_SEGMENT_CACHED ||
             kind == NVKMD_NVGPU_SEGMENT_READBACK,
          &owner->slot) != NVKMD_NVGPU_NATIVE_MAP_OK)
      return native_owner_grow_failed(owner, NVKMD_NVGPU_SEGMENT_FAILURE_MAP,
                                      segment->map.result, length_B);
   if (nvkmd_nvgpu_native_arena_bind(
          &segment->arena, runtime, &segment->backing, &segment->map,
          &segment->lease) != NVKMD_NVGPU_NATIVE_ARENA_OK ||
       !native_segment_shaped(kind, &segment->arena))
      return native_owner_grow_failed(owner,
                                      NVKMD_NVGPU_SEGMENT_FAILURE_ARENA, 0,
                                      length_B);
   segment->arena.owner = owner;
   segment->failed = false;
   segment->ready = true;
   return &segment->arena;
}

static bool
native_owner_wait_terminal(void *context, uint32_t producer_id, uint32_t target)
{
   return nvkmd_nvgpu_native_wait_terminal_target(context, producer_id, target);
}

static enum nvkmd_nvgpu_native_device_close_status
native_device_close(struct nvkmd_nvgpu_native_device_owner *owner,
                    bool at_exit)
{
   if (!owner || owner->self != owner)
      return NVKMD_NVGPU_DEVICE_CLOSE_ARGUMENT;
   struct nvkmd_nvgpu_native_runtime *runtime = owner->runtime;
   owner->retiring = true;

   /* Complete all earlier work on the channel, with a new marker. */
   if (owner->ready && !owner->terminal.complete) {
      const struct nvkmd_nvgpu_native_terminal_io io = {
         .context = owner,
         .wait_target = native_owner_wait_terminal,
      };
      const enum nvkmd_nvgpu_native_terminal_status drained = at_exit ?
         nvkmd_nvgpu_native_terminal_drain_at_exit(
            &owner->terminal, &owner->arena, &owner->endpoint, &io) :
         nvkmd_nvgpu_native_terminal_drain(
            &owner->terminal, &owner->arena, &owner->endpoint, &io,
            !owner->inflight_first);
      if (drained != NVKMD_NVGPU_TERMINAL_OK)
         return NVKMD_NVGPU_DEVICE_CLOSE_TERMINAL;
   }
   owner->ready = false;

   if (owner->setup.attempted &&
       R_FAILED(nvkmd_nvgpu_native_channel_close(&owner->setup,
                                                 &owner->channel)))
      return NVKMD_NVGPU_DEVICE_CLOSE_CHANNEL;
   /* The copy channel shares the notifier page, so it closes
    * before that page is released. The close order is notifier disable and
    * event close, then the channel file, then the TSG file. */
   struct nvkmd_nvgpu_native_copy_channel *copy = &owner->copy;
   if (copy->setup.attempted &&
       R_FAILED(nvkmd_nvgpu_native_channel_close(&copy->setup,
                                                 &copy->channel)))
      return NVKMD_NVGPU_DEVICE_CLOSE_CHANNEL;
   if (copy->gpu_open) {
      nvClose(copy->gpu_fd);
      copy->gpu_open = false;
   }
   if (copy->tsg_open) {
      nvClose(copy->tsg_fd);
      copy->tsg_open = false;
   }
   copy->ready = false;
   /* Freeing the slot disarms it, so the next owner of the slot does not see
    * this owner's target. */
   if (owner->gsv_event_slot_allocated) {
      gsvSyncFreeSlot(&runtime->sync, owner->gsv_event_slot);
      owner->gsv_event_slot_allocated = false;
   }
   owner->gsv_event_armed = false;
   if (owner->gsv_event_open) {
      eventClose(&owner->gsv_event);
      owner->gsv_event_open = false;
   }

   /* The views first: a chunk or a segment that a view maps cannot be
    * released. */
   if (!nvkmd_nvgpu_native_owner_release_retired_views(owner))
      return NVKMD_NVGPU_DEVICE_CLOSE_SEGMENT;
   if (!nvkmd_nvgpu_native_owner_release_host_mems(owner))
      return NVKMD_NVGPU_DEVICE_CLOSE_SEGMENT;
   if (!nvkmd_nvgpu_native_owner_release_chunks(owner))
      return NVKMD_NVGPU_DEVICE_CLOSE_SEGMENT;
   /* Every segment that has its memory, the failed ones included: one
    * whose creation or release failed earlier continues here with what it
    * still has (a mapping, a held backing, or only its memory and VA). */
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (segment->cpu_allocation &&
          !native_owner_release_segment(owner, segment))
         return NVKMD_NVGPU_DEVICE_CLOSE_SEGMENT;
   }
   if (owner->map.mapped &&
       nvkmd_nvgpu_native_map_unmap(&owner->map, runtime, &owner->backing,
                                    &owner->slot) != NVKMD_NVGPU_NATIVE_MAP_OK)
      return NVKMD_NVGPU_DEVICE_CLOSE_MAP;
   if (nvkmd_nvgpu_native_backing_held(&owner->backing) &&
       nvkmd_nvgpu_native_backing_release(&owner->backing, runtime,
                                          &owner->slot) != NVKMD_NVGPU_BACKING_OK)
      return NVKMD_NVGPU_DEVICE_CLOSE_BACKING;
   if (nvkmd_nvgpu_native_backing_held(&owner->notifier) &&
       nvkmd_nvgpu_native_backing_release(&owner->notifier, runtime,
                                          &owner->slot) != NVKMD_NVGPU_BACKING_OK)
      return NVKMD_NVGPU_DEVICE_CLOSE_NOTIFIER;
   if (!nvkmd_nvgpu_native_slot_close(&owner->slot))
      return NVKMD_NVGPU_DEVICE_CLOSE_SLOT;
   return NVKMD_NVGPU_DEVICE_CLOSE_OK;
}

enum nvkmd_nvgpu_native_device_close_status
nvkmd_nvgpu_native_device_close(struct nvkmd_nvgpu_native_device_owner *owner)
{
   return native_device_close(owner, false);
}

enum nvkmd_nvgpu_native_device_close_status
nvkmd_nvgpu_native_device_close_at_exit(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   return native_device_close(owner, true);
}

/* Detaches the progress domain from the owner. Any later use of the domain
 * fails as device lost. Drops the reference of the owner. The last point
 * frees the domain. */
static void
native_owner_detach_progress(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_progress *progress = owner->progress;
   owner->progress = NULL;
   if (progress) {
      mtx_lock(&progress->mutex);
      progress->owner = NULL;
      progress->lost = VK_ERROR_DEVICE_LOST;
      mtx_unlock(&progress->mutex);
   }
   nvkmd_nvgpu_native_progress_unref(progress);
}

void
nvkmd_nvgpu_native_device_abandon(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || owner->self != owner)
      return;
   owner->ready = false;
   owner->retiring = true;
   native_owner_detach_progress(owner);
}

bool
nvkmd_nvgpu_native_device_finish(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || owner->self != owner)
      return false;
   if (owner->ready || owner->inflight_first || owner->active_pdevs ||
       owner->active_devices || owner->active_vas || owner->kept_contexts) {
      mesa_loge("Native GPU device still in use: ready %d, in flight %d, "
                "pdevs %u, devices %u, VAs %u, kept contexts %u",
                owner->ready, owner->inflight_first != NULL, owner->active_pdevs,
                owner->active_devices, owner->active_vas, owner->kept_contexts);
      return false;
   }

   /* Every device is gone and every submission is retired. The remaining
    * holders of the progress domain are completed points in sync objects.
    * The application did not destroy them before vkDestroyDevice. Vulkan
    * allows no use of them after that, so they must not keep the GPU.
    * Detach the domain (any later use of it fails as device lost). The
    * last point frees it. */
   native_owner_detach_progress(owner);
   nvkmd_nvgpu_native_ctx_free_submissions(owner);
   for (uint32_t i = 0; i < NVKMD_NVGPU_NATIVE_SEGMENT_MAX; i++)
      nvkmd_nvgpu_native_arena_finish(&owner->segments[i].arena);
   nvkmd_nvgpu_native_arena_finish(&owner->arena);
   if (owner->va_heap_ready)
      util_vma_heap_finish(&owner->va_heap);
   nvkmd_nvgpu_native_owner_lock_finish(owner);
   memset(owner, 0, sizeof(*owner));
   return true;
}

uint64_t
nvkmd_nvgpu_native_owner_reserve_va(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t size_B, uint64_t align_B)
{
   if (!owner || !owner->va_heap_ready || !size_B ||
       !util_is_power_of_two_or_zero64(align_B))
      return 0;
   return util_vma_heap_alloc(&owner->va_heap, size_B, MAX2(align_B, 4096));
}

void
nvkmd_nvgpu_native_owner_release_va(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t addr, uint64_t size_B)
{
   if (owner && owner->va_heap_ready && addr && size_B)
      util_vma_heap_free(&owner->va_heap, addr, size_B);
}

/* Finds the backing and the map of an arena: the primary segment or one of
 * the other segments. */
static bool
native_owner_arena_backing(struct nvkmd_nvgpu_native_device_owner *owner,
                           const struct nvkmd_nvgpu_native_arena *arena,
                           struct nvkmd_nvgpu_native_backing **backing_out,
                           const struct nvkmd_nvgpu_native_map_owner **map_out)
{
   if (arena == owner->arena_ref) {
      *backing_out = &owner->backing;
      *map_out = &owner->map;
      return owner->map.ready;
   }
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      struct nvkmd_nvgpu_native_segment *segment = &owner->segments[i];
      if (&segment->arena == arena && segment->ready) {
         *backing_out = &segment->backing;
         *map_out = &segment->map;
         return segment->map.ready;
      }
   }
   return false;
}

VkResult
nvkmd_nvgpu_native_owner_map_view(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_arena *arena,
   uint64_t gpu_addr, uint64_t size_B,
   uint64_t view_addr, uint8_t kind,
   struct nvkmd_nvgpu_native_view_source *source_out,
   bool *left_mapped_out)
{
   if (!owner || !arena || !source_out || !left_mapped_out || !size_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   *left_mapped_out = false;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   struct nvkmd_nvgpu_native_backing *backing = NULL;
   const struct nvkmd_nvgpu_native_map_owner *map = NULL;
   VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
   if (native_owner_arena_backing(owner, arena, &backing, &map) &&
       gpu_addr >= map->gpu_va && size_B <= map->length &&
       gpu_addr - map->gpu_va <= map->length - size_B) {
      *source_out = (struct nvkmd_nvgpu_native_view_source) {
         .backing = backing,
         .backing_offset_B = gpu_addr - map->gpu_va,
         .gpu_cacheable = !map->gpu_uncached,
      };
      if (nvkmd_nvgpu_native_owner_map_view_range(owner, backing,
                                      source_out->backing_offset_B, size_B,
                                      view_addr, kind,
                                      source_out->gpu_cacheable,
                                      left_mapped_out))
         result = VK_SUCCESS;
   }
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

/* The engine catalog record of a copy engine with a selector of its own:
 * exported type 2 is a copy or compute engine whose selector differs from
 * the primary (type-zero) selector. A record is {type, instance, selector,
 * 0}, each u32. */
static bool
native_copy_selector(const struct nvkmd_nvgpu_native_runtime *runtime,
                     uint32_t *selector_out)
{
   const struct nvkmd_nvgpu_native_type0_candidate type0 =
      nvkmd_nvgpu_native_runtime_type0_candidate(runtime);
   for (uint32_t i = 0; i < runtime->engine_catalog.record_count &&
                        i < NVKMD_NVGPU_RUNTIME_ENGINE_RECORDS; i++) {
      uint32_t record[4];
      memcpy(record, runtime->engine_catalog.records[i], sizeof(record));
      if (record[0] == 2 && (!type0.found || record[2] != type0.selector)) {
         *selector_out = record[2];
         return true;
      }
   }
   return false;
}

#define NATIVE_COPY_CLASS 0xc7b5u

bool
nvkmd_nvgpu_native_owner_open_copy_channel(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || !nvkmd_nvgpu_native_owner_lock(owner))
      return false;
   struct nvkmd_nvgpu_native_copy_channel *copy = &owner->copy;
   if (copy->attempted) {
      const bool ready = copy->ready;
      nvkmd_nvgpu_native_owner_unlock(owner);
      return ready;
   }
   copy->attempted = true;
   struct nvkmd_nvgpu_native_runtime *runtime = owner->runtime;

   copy->step = NVKMD_NVGPU_COPY_CHANNEL_CATALOG;
   if (!native_copy_selector(runtime, &copy->selector))
      goto done;

   copy->step = NVKMD_NVGPU_COPY_CHANNEL_GPU_FILE;
   copy->result = nvOpen(&copy->gpu_fd, "/dev/nvhost-gpu");
   if (R_FAILED(copy->result))
      goto done;
   copy->gpu_open = true;

   copy->step = NVKMD_NVGPU_COPY_CHANNEL_TSG_FILE;
   copy->result = nvOpen(&copy->tsg_fd, "/dev/nvhost-tsg-gpu");
   if (R_FAILED(copy->result))
      goto done;
   copy->tsg_open = true;

   copy->step = NVKMD_NVGPU_COPY_CHANNEL_OPEN;
   copy->channel_status = nvkmd_nvgpu_native_channel_open_on(
      &copy->channel, runtime, copy->selector, copy->gpu_fd, copy->tsg_fd);
   copy->result = copy->channel.result;
   if (copy->channel_status != NVKMD_NVGPU_CHANNEL_OK)
      goto done;

   /* The copy channel shares the notifier page of the primary channel. */
   copy->step = NVKMD_NVGPU_COPY_CHANNEL_CONFIGURE;
   copy->channel_status = nvkmd_nvgpu_native_channel_configure_class(
      &copy->setup, &copy->channel, NOTIFIER_LOCAL_HANDLE, NATIVE_COPY_CLASS);
   copy->result = copy->setup.result;
   if (copy->channel_status != NVKMD_NVGPU_CHANNEL_SETUP_OK)
      goto done;

   copy->endpoint = (struct nvkmd_nvgpu_gpu_endpoint) {
      .generation = runtime->generation,
      .gpu_fd = copy->gpu_fd,
      .producer_id = nvkmd_nvgpu_native_channel_producer_id(&copy->setup),
      .sync = &runtime->sync,
      .gpu_fd_ready = true,
   };
   copy->step = NVKMD_NVGPU_COPY_CHANNEL_READY;
   copy->ready = true;

done:
   mesa_logi("native copy channel: step %u, status %u, result 0x%x, "
             "selector %u", (unsigned)copy->step,
             (unsigned)copy->channel_status, copy->result, copy->selector);
   const bool ready = copy->ready;
   nvkmd_nvgpu_native_owner_unlock(owner);
   return ready;
}

VkResult
nvkmd_nvgpu_native_owner_finish_flush(
   struct nvkmd_nvgpu_native_device_owner *owner, struct nvkmd_dev *dev)
{
   return native_owner_finish_flush(owner, dev, NULL);
}
