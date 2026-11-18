/* SPDX-License-Identifier: MIT
 * Page-granular GPU-only memory. See
 * nvkmd_nvgpu_native_chunked.h.
 */
#include "nvkmd_nvgpu_native_chunked.h"
#include "nvkmd_nvgpu_native_chunk_reserve.h"
#include "nvkmd_nvgpu_native_map.h"

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <stdlib.h>
#include <string.h>

/* Free chunks that stay registered for later allocations. Beyond this, a
 * free returns chunks to the process heap (after a finish flush, because
 * the GPU L2 can still hold their lines). */
#define NATIVE_CHUNK_POOL_B (UINT64_C(128) * 1024 * 1024)
/* The most free chunks that the reserve keeps ready (see
 * nvkmd_nvgpu_native_chunk_reserve.h). */
#define NATIVE_CHUNK_RESERVE_MAX_B (UINT64_C(256) * 1024 * 1024)
#define NATIVE_CHUNK_ALIGN_B (UINT64_C(64) * 1024)
#define NATIVE_CHUNK_KIND 6

/* The VA of one chunked allocation. It is made with the allocation, so
 * that a free needs no memory to keep the VA of a chunk that may still be
 * mapped. The allocation has one reference, and so has each of its chunks
 * whose own mapping may remain after the free; the VA returns to the heap
 * with the last. */
struct nvkmd_nvgpu_native_va_hold {
   uint64_t addr, size_B;
   uint32_t references;
};

struct nvkmd_nvgpu_native_chunked_mem {
   struct nvkmd_mem base;
   struct nvkmd_va va;
   struct nvkmd_nvgpu_native_va_hold *va_hold;
   struct nvkmd_nvgpu_native_device_owner *owner;
   /* Every chunk is mapped with a compressible kind. */
   bool compressed;
   struct list_head link; /* in owner->chunked_mems */
   uint32_t chunk_count;
   struct nvkmd_nvgpu_native_chunk *chunks[];
};

static const struct nvkmd_mem_ops native_chunked_mem_ops;

bool
nvkmd_nvgpu_native_mem_is_chunked(const struct nvkmd_mem *mem)
{
   return mem && mem->ops == &native_chunked_mem_ops;
}

static void
native_chunks_init(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner->chunked_ready) {
      list_inithead(&owner->chunked_mems);
      owner->chunked_ready = true;
   }
}

/* Puts a chunk on the owner's list of chunks that are out of the pool. It
 * stays registered, with its memory. chunk_va is where its own mapping may
 * remain, or 0. */
static void
native_chunk_set_aside(struct nvkmd_nvgpu_native_device_owner *owner,
                       struct nvkmd_nvgpu_native_chunk *chunk,
                       uint64_t chunk_va)
{
   chunk->stranded_va = chunk_va;
   chunk->next = owner->stranded_chunks;
   owner->stranded_chunks = chunk;
}

/* Sets a chunk aside after a failure: a mapping of it may remain, or its
 * registration or release failed. It is not reused, and device close tries
 * it again. */
static void
native_chunk_strand(struct nvkmd_nvgpu_native_device_owner *owner,
                    struct nvkmd_nvgpu_native_chunk *chunk, uint64_t chunk_va,
                    const char *what, Result rc)
{
   owner->stats.chunk_failures++;
   mesa_loge("native chunk: %s: 0x%x; the chunk is not reused", what, rc);
   native_chunk_set_aside(owner, chunk, chunk_va);
}

/* The start of the chunk's memory in its heap block. */
static uintptr_t
native_chunk_cpu(const struct nvkmd_nvgpu_native_chunk *chunk)
{
   return align64((uintptr_t)chunk->allocation, NATIVE_CHUNK_ALIGN_B);
}

/* A chunk record with its heap block, not registered yet. NULL when there
 * is no memory. Needs no lock. */
static struct nvkmd_nvgpu_native_chunk *
native_chunk_allocate(void)
{
   struct nvkmd_nvgpu_native_chunk *chunk = calloc(1, sizeof(*chunk));
   if (!chunk)
      return NULL;
   const uint64_t heap_start = armGetSystemTick();
   chunk->allocation = malloc(NVKMD_NVGPU_NATIVE_CHUNK_B + NATIVE_CHUNK_ALIGN_B);
   ouncevk_profile_record(OUNCEVK_PROFILE_BACKING_HEAP,
                          armGetSystemTick() - heap_start);
   if (!chunk->allocation) {
      free(chunk);
      return NULL;
   }
   return chunk;
}

/* Registers an allocated chunk. With cleared, the caller has cleared its
 * memory. Returns false when the registration failed: the chunk is then
 * freed, or stranded when a part of the registration remains. Call with
 * the owner lock held. */
static bool
native_chunk_register(struct nvkmd_nvgpu_native_device_owner *owner,
                      struct nvkmd_nvgpu_native_chunk *chunk, bool cleared)
{
   const uintptr_t cpu = native_chunk_cpu(chunk);
   /* Mode 2 (CPU uncached), as for the GPU-only segments. */
   const enum nvkmd_nvgpu_native_backing_status status = cleared ?
      nvkmd_nvgpu_native_backing_prepare_cleared(
         &chunk->backing, owner->runtime, cpu, NVKMD_NVGPU_NATIVE_CHUNK_B) :
      nvkmd_nvgpu_native_backing_prepare(
         &chunk->backing, owner->runtime, cpu, NVKMD_NVGPU_NATIVE_CHUNK_B,
         false);
   if (status != NVKMD_NVGPU_BACKING_OK) {
      if (nvkmd_nvgpu_native_backing_held(&chunk->backing)) {
         /* Part of the registration remains (a prepared span, a registered
          * range): the chunk keeps it, and device close releases it. */
         native_chunk_strand(owner, chunk, 0, "registration failed",
                             chunk->backing.result);
      } else {
         mesa_loge("native chunk: registration failed: 0x%x",
                   chunk->backing.result);
         owner->stats.chunk_failures++;
         free(chunk->allocation);
         free(chunk);
      }
      return false;
   }
   owner->stats.chunks_registered++;
   return true;
}

/* A chunk from the pool, or a new registered chunk (*created is then
 * incremented). Compressed memory takes a chunk that has compression tags
 * first. Without heap memory for a new chunk, the free chunks that this
 * memory cannot use (the compressible pool, for ordinary memory) return to
 * the heap first. Call with the owner lock held; that release lets go of
 * the lock during its flush. */
static struct nvkmd_nvgpu_native_chunk *
native_chunk_take(struct nvkmd_nvgpu_native_device_owner *owner,
                  struct nvkmd_dev *dev, bool compressed, uint32_t *created)
{
   struct nvkmd_nvgpu_native_chunk *chunk = owner->free_compressible_chunks;
   if (compressed && chunk) {
      owner->free_compressible_chunks = chunk->next;
      owner->free_compressible_chunk_count--;
      chunk->next = NULL;
      return chunk;
   }
   chunk = owner->free_chunks;
   if (chunk) {
      owner->free_chunks = chunk->next;
      owner->free_chunk_count--;
      chunk->next = NULL;
      return chunk;
   }
   chunk = native_chunk_allocate();
   if (!chunk && nvkmd_nvgpu_native_owner_release_free_chunks(owner, dev))
      chunk = native_chunk_allocate();
   if (!chunk || !native_chunk_register(owner, chunk, false))
      return NULL;
   (*created)++;
   return chunk;
}

/* Unregisters a chunk and frees its memory. The caller has completed and
 * flushed every GPU use of it. Returns false, with the chunk stranded,
 * when the release failed. */
static bool
native_chunk_release(struct nvkmd_nvgpu_native_device_owner *owner,
                     struct nvkmd_nvgpu_native_chunk *chunk)
{
   if (nvkmd_nvgpu_native_backing_release(&chunk->backing, owner->runtime,
                                          &owner->slot) !=
       NVKMD_NVGPU_BACKING_OK) {
      native_chunk_strand(owner, chunk, 0, "release failed",
                          chunk->backing.result);
      return false;
   }
   free(chunk->allocation);
   free(chunk);
   owner->stats.chunks_released++;
   return true;
}

/* Returns a chunk to its pool. A chunk that was mapped compressed goes to
 * the compressible pool: ordinary memory never maps it again, so the GPU
 * never reads its bytes with and without compression. Its tags stay with
 * its backing until the release. */
static void
native_chunk_give_back(struct nvkmd_nvgpu_native_device_owner *owner,
                       struct nvkmd_nvgpu_native_chunk *chunk)
{
   if (chunk->compressible) {
      chunk->next = owner->free_compressible_chunks;
      owner->free_compressible_chunks = chunk;
      owner->free_compressible_chunk_count++;
      return;
   }
   chunk->next = owner->free_chunks;
   owner->free_chunks = chunk;
   owner->free_chunk_count++;
}

/* A hold with one reference, the allocation's, for a VA that the caller
 * reserves next. NULL when there is no memory. */
static struct nvkmd_nvgpu_native_va_hold *
native_va_hold_create(uint64_t size_B)
{
   struct nvkmd_nvgpu_native_va_hold *hold = calloc(1, sizeof(*hold));
   if (!hold)
      return NULL;
   hold->size_B = size_B;
   hold->references = 1;
   return hold;
}

/* Drops one reference; the last returns the VA to the heap. */
static void
native_va_hold_drop(struct nvkmd_nvgpu_native_device_owner *owner,
                    struct nvkmd_nvgpu_native_va_hold *hold)
{
   if (--hold->references)
      return;
   nvkmd_nvgpu_native_owner_release_va(owner, hold->addr, hold->size_B);
   free(hold);
}

/* Strands a chunk whose own mapping at chunk_va may remain. It takes a
 * reference to the VA of its allocation, until device close has unmapped
 * it. */
static void
native_chunk_strand_mapped(struct nvkmd_nvgpu_native_device_owner *owner,
                           struct nvkmd_nvgpu_native_chunk *chunk,
                           uint64_t chunk_va,
                           struct nvkmd_nvgpu_native_va_hold *hold,
                           const char *what, Result rc)
{
   hold->references++;
   chunk->va_hold = hold;
   native_chunk_strand(owner, chunk, chunk_va, what, rc);
}

/* Unmaps the chunk mapped at chunk_va and returns it to the pool. A chunk
 * whose unmap failed is stranded, with a hold on the VA.
 *
 * A chunk that a view still maps is set aside, though its VA is free: a
 * program may free memory before the sparse resource that is bound to it.
 * The chunk returns to the pool when its last view is unmapped
 * (nvkmd_nvgpu_native_owner_reclaim_chunks). */
static void
native_chunk_unmap_give_back(struct nvkmd_nvgpu_native_device_owner *owner,
                             struct nvkmd_nvgpu_native_chunk *chunk,
                             uint64_t chunk_va,
                             struct nvkmd_nvgpu_native_va_hold *hold)
{
   Result rc = 0;
   if (nvkmd_nvgpu_native_unmap_range(owner->runtime, &chunk->backing,
                                      chunk_va, NVKMD_NVGPU_NATIVE_CHUNK_B,
                                      &owner->slot, &rc) !=
       NVKMD_NVGPU_NATIVE_MAP_OK)
      native_chunk_strand_mapped(owner, chunk, chunk_va, hold,
                                 "unmap failed", rc);
   else if (chunk->backing.maps)
      native_chunk_set_aside(owner, chunk, 0);
   else
      native_chunk_give_back(owner, chunk);
}

/* Unmaps the first chunk_count chunks of the memory and returns them to
 * the pool, and drops the allocation's reference to its VA: the VA returns
 * to the heap unless a chunk may still be mapped in it. */
static void
native_chunks_unmap_release_va(struct nvkmd_nvgpu_native_device_owner *owner,
                               struct nvkmd_nvgpu_native_chunk **chunks,
                               uint32_t chunk_count,
                               struct nvkmd_nvgpu_native_va_hold *hold)
{
   for (uint32_t i = 0; i < chunk_count; i++)
      native_chunk_unmap_give_back(
         owner, chunks[i],
         hold->addr + (uint64_t)i * NVKMD_NVGPU_NATIVE_CHUNK_B, hold);
   native_va_hold_drop(owner, hold);
}

/* Unmaps every chunk, returns them to the pool and the VA to the heap.
 * Call with the owner lock held. */
static void
native_chunked_mem_retire(struct nvkmd_nvgpu_native_chunked_mem *mem)
{
   native_chunks_unmap_release_va(mem->owner, mem->chunks, mem->chunk_count,
                                  mem->va_hold);
   mem->va_hold = NULL;
   list_del(&mem->link);
}

/* Takes the chunks of one pool beyond keep off it, as a list. They are the
 * most recently freed ones. */
static struct nvkmd_nvgpu_native_chunk *
native_chunk_pool_detach(struct nvkmd_nvgpu_native_chunk **pool,
                         uint32_t *count, uint32_t keep)
{
   struct nvkmd_nvgpu_native_chunk *detached = NULL;
   while (*count > keep) {
      struct nvkmd_nvgpu_native_chunk *chunk = *pool;
      *pool = chunk->next;
      (*count)--;
      chunk->next = detached;
      detached = chunk;
   }
   return detached;
}

/* Returns detached chunks to their pools. */
static void
native_chunks_give_back(struct nvkmd_nvgpu_native_device_owner *owner,
                        struct nvkmd_nvgpu_native_chunk *chunk)
{
   while (chunk) {
      struct nvkmd_nvgpu_native_chunk *next = chunk->next;
      native_chunk_give_back(owner, chunk);
      chunk = next;
   }
}

/* Releases detached chunks and returns their number. After a failed
 * release, which strands that chunk, the rest return to their pools. */
static uint32_t
native_chunks_release(struct nvkmd_nvgpu_native_device_owner *owner,
                      struct nvkmd_nvgpu_native_chunk *chunk)
{
   uint32_t released = 0;
   while (chunk) {
      struct nvkmd_nvgpu_native_chunk *next = chunk->next;
      if (!native_chunk_release(owner, chunk)) {
         native_chunks_give_back(owner, next);
         break;
      }
      released++;
      chunk = next;
   }
   return released;
}

/* Returns free chunks to the process heap until the ordinary pool has keep
 * chunks and the compressible pool keep_compressible, and returns their
 * number. The release of a chunk also frees its compression tags.
 *
 * The GPU L2 can hold lines of these chunks, so a finish flush completes
 * earlier work and writes back dirty lines first. The chunks leave their
 * pools before the flush: its wait releases the owner lock, and the flush
 * does not cover a chunk that another thread frees during that time. */
static uint32_t
native_chunk_pools_shrink(struct nvkmd_nvgpu_native_device_owner *owner,
                          struct nvkmd_dev *dev, uint32_t keep,
                          uint32_t keep_compressible)
{
   if (owner->free_chunk_count <= keep &&
       owner->free_compressible_chunk_count <= keep_compressible)
      return 0;
   struct nvkmd_nvgpu_native_chunk *ordinary = native_chunk_pool_detach(
      &owner->free_chunks, &owner->free_chunk_count, keep);
   struct nvkmd_nvgpu_native_chunk *compressible = native_chunk_pool_detach(
      &owner->free_compressible_chunks,
      &owner->free_compressible_chunk_count, keep_compressible);
   if (nvkmd_nvgpu_native_owner_finish_flush(owner, dev) != VK_SUCCESS) {
      native_chunks_give_back(owner, ordinary);
      native_chunks_give_back(owner, compressible);
      return 0;
   }
   return native_chunks_release(owner, ordinary) +
          native_chunks_release(owner, compressible);
}

/* After a free: returns to the process heap the pool chunks beyond
 * NATIVE_CHUNK_POOL_B above the reserve. The reserve fills the pool after
 * an allocation, and the free that often follows (a program that replaces
 * a buffer with a larger one) adds its chunks on top. With a limit at the
 * reserve, each such free released the chunks that the reserve had just
 * registered: 197 of 246 in one game session.
 *
 * While the process heap is below the reserve's floor, the two pools keep
 * NATIVE_CHUNK_POOL_B together and nothing for the reserve: the program
 * needs the heap more. */
static void
native_chunk_pool_trim(struct nvkmd_nvgpu_native_device_owner *owner,
                       struct nvkmd_dev *dev)
{
   const uint32_t pool_chunks =
      (uint32_t)(NATIVE_CHUNK_POOL_B / NVKMD_NVGPU_NATIVE_CHUNK_B);
   if (nvkmd_nvgpu_native_chunk_reserve_heap_allows()) {
      (void)native_chunk_pools_shrink(
         owner, dev, pool_chunks + owner->chunk_reserve_target, pool_chunks);
   } else {
      (void)native_chunk_pools_shrink(owner, dev, pool_chunks / 2,
                                      pool_chunks / 2);
   }
}

uint32_t
nvkmd_nvgpu_native_owner_release_free_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner, struct nvkmd_dev *dev)
{
   if (!owner->chunked_ready || !dev)
      return 0;
   /* The reserve does not fill the pools again until an allocation has to
    * register a chunk. */
   owner->chunk_reserve_target = 0;
   return native_chunk_pools_shrink(owner, dev, 0, 0);
}

static void
native_chunked_mem_free(struct nvkmd_mem *base)
{
   struct nvkmd_nvgpu_native_chunked_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_chunked_mem, base);
   struct nvkmd_nvgpu_native_device_owner *owner = mem->owner;
   if (nvkmd_nvgpu_native_owner_lock(owner)) {
      native_chunked_mem_retire(mem);
      native_chunk_pool_trim(owner, base->dev);
      nvkmd_nvgpu_native_owner_unlock(owner);
   }
   simple_mtx_destroy(&base->map_mutex);
   free(mem);
}

static VkResult
native_chunked_mem_map(struct nvkmd_mem *base, struct vk_object_base *log_obj,
                       enum nvkmd_mem_map_flags flags, void *fixed_addr,
                       void **map_out)
{
   (void)base;
   (void)log_obj;
   (void)flags;
   (void)fixed_addr;
   (void)map_out;
   /* GPU-only memory has no contiguous CPU view. */
   return VK_ERROR_MEMORY_MAP_FAILED;
}

static void
native_chunked_mem_unmap(struct nvkmd_mem *base,
                         enum nvkmd_mem_map_flags flags, void *map)
{
   (void)base;
   (void)flags;
   (void)map;
}

static const struct nvkmd_mem_ops native_chunked_mem_ops = {
   .free = native_chunked_mem_free,
   .map = native_chunked_mem_map,
   .unmap = native_chunked_mem_unmap,
};

/* After an allocation of count chunks, of which created were registered
 * during the call: the reserve keeps as many chunks ready as the largest
 * allocation that had to register one, and fills up again. */
static void
native_chunk_reserve_note(struct nvkmd_nvgpu_native_device_owner *owner,
                          uint32_t count, uint32_t created)
{
   const uint32_t most =
      (uint32_t)(NATIVE_CHUNK_RESERVE_MAX_B / NVKMD_NVGPU_NATIVE_CHUNK_B);
   if (created)
      owner->chunk_reserve_target =
         MAX2(owner->chunk_reserve_target, MIN2(count, most));
   if (owner->free_chunk_count < owner->chunk_reserve_target)
      nvkmd_nvgpu_native_chunk_reserve_wake();
}

uint64_t
nvkmd_nvgpu_native_owner_free_chunk_bytes(
   const struct nvkmd_nvgpu_native_device_owner *owner)
{
   return ((uint64_t)owner->free_chunk_count +
           owner->free_compressible_chunk_count) *
          NVKMD_NVGPU_NATIVE_CHUNK_B;
}

uint32_t
nvkmd_nvgpu_native_chunk_reserve_missing(
   const struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner->chunked_ready || owner->submission_failed || owner->retiring ||
       owner->free_chunk_count >= owner->chunk_reserve_target)
      return 0;
   return owner->chunk_reserve_target - owner->free_chunk_count;
}

struct nvkmd_nvgpu_native_chunk *
nvkmd_nvgpu_native_chunk_reserve_prepare(void)
{
   struct nvkmd_nvgpu_native_chunk *chunk = native_chunk_allocate();
   if (!chunk)
      return NULL;
   const uint64_t clear_start = armGetSystemTick();
   memset((void *)native_chunk_cpu(chunk), 0, NVKMD_NVGPU_NATIVE_CHUNK_B);
   ouncevk_profile_record(OUNCEVK_PROFILE_BACKING_CLEAR,
                          armGetSystemTick() - clear_start);
   return chunk;
}

void
nvkmd_nvgpu_native_chunk_reserve_discard(
   struct nvkmd_nvgpu_native_chunk *chunk)
{
   free(chunk->allocation);
   free(chunk);
}

bool
nvkmd_nvgpu_native_chunk_reserve_add(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_chunk *chunk)
{
   if (!native_chunk_register(owner, chunk, true))
      return false;
   owner->stats.chunks_prepared++;
   native_chunk_give_back(owner, chunk);
   return true;
}

VkResult
nvkmd_nvgpu_native_chunked_mem_create(
   struct nvkmd_dev *dev, struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t size_B, uint64_t align_B, enum nvkmd_mem_flags flags,
   uint8_t pte_kind, struct nvkmd_mem **mem_out)
{
   const bool compressed = flags & NVKMD_MEM_COMPRESSED;
   if (!dev || !owner || !mem_out || !size_B ||
       size_B > UINT64_MAX - NVKMD_NVGPU_NATIVE_CHUNK_B ||
       (compressed ? !nvkmd_nvgpu_native_kind_is_compressible(pte_kind)
                   : pte_kind != NATIVE_CHUNK_KIND))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const uint64_t length_B = align64(size_B, NVKMD_NVGPU_NATIVE_CHUNK_B);
   const uint64_t count64 = length_B / NVKMD_NVGPU_NATIVE_CHUNK_B;
   if (count64 > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   const uint32_t count = (uint32_t)count64;
   struct nvkmd_nvgpu_native_chunked_mem *mem =
      calloc(1, sizeof(*mem) + count * sizeof(mem->chunks[0]));
   /* The hold is made before its VA is reserved, so that a VA is never
    * reserved without the record that returns it. */
   struct nvkmd_nvgpu_native_va_hold *hold =
      mem ? native_va_hold_create(length_B) : NULL;
   if (!mem || !hold) {
      free(mem);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (!nvkmd_nvgpu_native_owner_lock(owner)) {
      free(hold);
      free(mem);
      return VK_ERROR_DEVICE_LOST;
   }
   native_chunks_init(owner);
   mem->owner = owner;
   const uint64_t va = nvkmd_nvgpu_native_owner_reserve_va(
      owner, length_B, MAX2(align_B, NATIVE_CHUNK_ALIGN_B));
   hold->addr = va;
   VkResult result = va ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
   uint32_t mapped = 0, created = 0;
   for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
      struct nvkmd_nvgpu_native_chunk *chunk =
         native_chunk_take(owner, dev, compressed, &created);
      if (!chunk) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         break;
      }
      Result rc = 0;
      uint8_t selected_kind = pte_kind;
      const uint64_t chunk_va = va + (uint64_t)i * NVKMD_NVGPU_NATIVE_CHUNK_B;
      const enum nvkmd_nvgpu_native_map_status status =
         compressed ?
            nvkmd_nvgpu_native_map_range_compressible(
               owner->runtime, &chunk->backing, chunk_va, 0,
               NVKMD_NVGPU_NATIVE_CHUNK_B, pte_kind, true, &owner->slot,
               &selected_kind, &rc) :
            nvkmd_nvgpu_native_map_range(
               owner->runtime, &chunk->backing, chunk_va, 0,
               NVKMD_NVGPU_NATIVE_CHUNK_B, NATIVE_CHUNK_KIND, true,
               &owner->slot, &rc);
      if (status != NVKMD_NVGPU_NATIVE_MAP_OK) {
         mesa_loge("native chunk: map failed: status %u, result 0x%x",
                   (unsigned)status, rc);
         if (status == NVKMD_NVGPU_NATIVE_MAP_KIND) {
            /* Mapped with another kind. */
            native_chunk_unmap_give_back(owner, chunk, chunk_va, hold);
         } else if (status == NVKMD_NVGPU_NATIVE_MAP_WAIT) {
            /* Accepted, and the wait failed: it may be mapped. */
            native_chunk_strand_mapped(owner, chunk, chunk_va, hold,
                                       "map wait failed", rc);
         } else {
            /* Refused: nothing was mapped. */
            native_chunk_give_back(owner, chunk);
         }
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         break;
      }
      mem->chunks[i] = chunk;
      mapped++;
      if (selected_kind != pte_kind) {
         /* The ordinary fallback: no compression tags were free. The memory
          * is compressed in every chunk or not at all. The chunks mapped so
          * far are unmapped below. */
         owner->stats.compression_fallbacks++;
         result = VK_ERROR_FORMAT_NOT_SUPPORTED;
         break;
      }
      if (compressed)
         chunk->compressible = true;
   }
   if (result != VK_SUCCESS) {
      if (va) {
         native_chunks_unmap_release_va(owner, mem->chunks, mapped, hold);
         native_chunk_pool_trim(owner, dev);
      } else {
         free(hold);
      }
      nvkmd_nvgpu_native_owner_unlock(owner);
      free(mem);
      return result;
   }
   mem->chunk_count = count;
   mem->va_hold = hold;
   mem->compressed = compressed;
   list_addtail(&mem->link, &owner->chunked_mems);
   owner->stats.chunked_mems++;
   if (compressed)
      owner->stats.compressed_mems++;
   native_chunk_reserve_note(owner, count, created);
   nvkmd_nvgpu_native_owner_unlock(owner);

   nvkmd_mem_init(dev, &mem->base, &native_chunked_mem_ops,
                  flags & ~NVKMD_MEM_CAN_MAP, length_B, 4096);
   mem->va = (struct nvkmd_va) {
      .dev = dev,
      .addr = va,
      .size_B = length_B,
   };
   mem->base.va = &mem->va;
   *mem_out = &mem->base;
   return VK_SUCCESS;
}

uint32_t
nvkmd_nvgpu_native_chunked_mem_parts(
   const struct nvkmd_mem *base, uint64_t offset_B, uint64_t size_B,
   struct nvkmd_nvgpu_native_view_part *parts, uint32_t max)
{
   if (!nvkmd_nvgpu_native_mem_is_chunked(base) || !parts || !size_B ||
       offset_B >= base->size_B || size_B > base->size_B - offset_B)
      return 0;
   const struct nvkmd_nvgpu_native_chunked_mem *mem =
      container_of(base, const struct nvkmd_nvgpu_native_chunked_mem, base);
   if (mem->compressed)
      return 0;
   uint32_t count = 0;
   uint64_t done_B = 0;
   while (done_B < size_B) {
      if (count == max)
         return 0;
      const uint64_t at = offset_B + done_B;
      const uint32_t index = (uint32_t)(at / NVKMD_NVGPU_NATIVE_CHUNK_B);
      const uint64_t in_chunk = at % NVKMD_NVGPU_NATIVE_CHUNK_B;
      const uint64_t part_B = MIN2(NVKMD_NVGPU_NATIVE_CHUNK_B - in_chunk,
                                   size_B - done_B);
      parts[count++] = (struct nvkmd_nvgpu_native_view_part) {
         .offset_B = done_B,
         .size_B = part_B,
         .source = {
            .backing = &mem->chunks[index]->backing,
            .backing_offset_B = in_chunk,
            .gpu_cacheable = true,
         },
      };
      done_B += part_B;
   }
   return count;
}

void
nvkmd_nvgpu_native_owner_reclaim_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_chunk *chunk = owner->stranded_chunks;
   owner->stranded_chunks = NULL;
   while (chunk) {
      struct nvkmd_nvgpu_native_chunk *next = chunk->next;
      /* A chunk that failed keeps its place: its own mapping may remain,
       * or its backing is no longer ready. */
      if (!chunk->stranded_va && !chunk->backing.maps &&
          chunk->backing.ready) {
         native_chunk_give_back(owner, chunk);
      } else {
         chunk->next = owner->stranded_chunks;
         owner->stranded_chunks = chunk;
      }
      chunk = next;
   }
}

/* Tries the set-aside chunks again: unmaps one whose own mapping may
 * remain, and releases one that nothing maps. Returns false when a chunk
 * stays on the list. */
static bool
native_chunks_release_stranded(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_chunk *chunk = owner->stranded_chunks;
   owner->stranded_chunks = NULL;
   bool all_released = true;
   while (chunk) {
      struct nvkmd_nvgpu_native_chunk *next = chunk->next;
      Result rc = 0;
      if (chunk->stranded_va &&
          nvkmd_nvgpu_native_unmap_range(owner->runtime, &chunk->backing,
                                         chunk->stranded_va,
                                         NVKMD_NVGPU_NATIVE_CHUNK_B,
                                         &owner->slot, &rc) ==
             NVKMD_NVGPU_NATIVE_MAP_OK) {
         chunk->stranded_va = 0;
         /* Its allocation's VA returns to the heap with the last of them. */
         native_va_hold_drop(owner, chunk->va_hold);
         chunk->va_hold = NULL;
      }
      /* native_chunk_release strands the chunk again when it fails. */
      if (chunk->stranded_va || chunk->backing.maps) {
         chunk->next = owner->stranded_chunks;
         owner->stranded_chunks = chunk;
         all_released = false;
      } else if (!native_chunk_release(owner, chunk)) {
         all_released = false;
      }
      chunk = next;
   }
   return all_released;
}

bool
nvkmd_nvgpu_native_owner_release_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner->chunked_ready)
      return true;
   list_for_each_entry_safe(struct nvkmd_nvgpu_native_chunked_mem, mem,
                            &owner->chunked_mems, link)
      native_chunked_mem_retire(mem);
   while (owner->free_chunks) {
      struct nvkmd_nvgpu_native_chunk *chunk = owner->free_chunks;
      owner->free_chunks = chunk->next;
      owner->free_chunk_count--;
      /* A failed release strands the chunk. */
      native_chunk_release(owner, chunk);
   }
   while (owner->free_compressible_chunks) {
      struct nvkmd_nvgpu_native_chunk *chunk =
         owner->free_compressible_chunks;
      owner->free_compressible_chunks = chunk->next;
      owner->free_compressible_chunk_count--;
      native_chunk_release(owner, chunk);
   }
   /* Try every stranded chunk once more, those of the pool included. The
    * result is whether any chunk remains. */
   return native_chunks_release_stranded(owner);
}
