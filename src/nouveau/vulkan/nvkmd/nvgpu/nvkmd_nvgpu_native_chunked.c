/* SPDX-License-Identifier: MIT
 * Page-granular GPU-only memory. See
 * nvkmd_nvgpu_native_chunked.h.
 */
#include "nvkmd_nvgpu_native_chunked.h"
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

/* A chunk from the pool, or a new registered chunk. Call with the owner
 * lock held. */

static struct nvkmd_nvgpu_native_chunk *
native_chunk_take(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_chunk *chunk = owner->free_chunks;
   if (chunk) {
      owner->free_chunks = chunk->next;
      owner->free_chunk_count--;
      chunk->next = NULL;
      return chunk;
   }
   chunk = calloc(1, sizeof(*chunk));
   if (!chunk)
      return NULL;
   chunk->allocation = malloc(NVKMD_NVGPU_NATIVE_CHUNK_B + NATIVE_CHUNK_ALIGN_B);
   if (!chunk->allocation) {
      free(chunk);
      return NULL;
   }
   const uintptr_t cpu = align64((uintptr_t)chunk->allocation,
                                 NATIVE_CHUNK_ALIGN_B);
   /* Mode 2 (CPU uncached), as for the GPU-only segments. */
   if (nvkmd_nvgpu_native_backing_prepare(&chunk->backing, owner->runtime,
                                          cpu, NVKMD_NVGPU_NATIVE_CHUNK_B,
                                          false) != NVKMD_NVGPU_BACKING_OK) {
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
      return NULL;
   }
   owner->stats.chunks_registered++;
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

static void
native_chunk_give_back(struct nvkmd_nvgpu_native_device_owner *owner,
                       struct nvkmd_nvgpu_native_chunk *chunk)
{
   chunk->next = owner->free_chunks;
   owner->free_chunks = chunk;
   owner->free_chunk_count++;
}

/* A hold with one reference, the allocation's. NULL when there is no
 * memory. */
static struct nvkmd_nvgpu_native_va_hold *
native_va_hold_create(uint64_t addr, uint64_t size_B)
{
   struct nvkmd_nvgpu_native_va_hold *hold = calloc(1, sizeof(*hold));
   if (!hold)
      return NULL;
   hold->addr = addr;
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

/* Returns pool chunks beyond NATIVE_CHUNK_POOL_B to the process heap. */
static void
native_chunk_pool_trim(struct nvkmd_nvgpu_native_device_owner *owner,
                       struct nvkmd_dev *dev)
{
   const uint32_t keep =
      (uint32_t)(NATIVE_CHUNK_POOL_B / NVKMD_NVGPU_NATIVE_CHUNK_B);
   if (owner->free_chunk_count <= keep)
      return;
   /* The GPU L2 can hold lines of these chunks: complete earlier work and
    * write back dirty lines first. */
   if (nvkmd_nvgpu_native_owner_finish_flush(owner, dev) != VK_SUCCESS)
      return;
   while (owner->free_chunk_count > keep) {
      struct nvkmd_nvgpu_native_chunk *chunk = owner->free_chunks;
      owner->free_chunks = chunk->next;
      owner->free_chunk_count--;
      if (!native_chunk_release(owner, chunk))
         break; /* keep the rest in the pool */
   }
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

VkResult
nvkmd_nvgpu_native_chunked_mem_create(
   struct nvkmd_dev *dev, struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t size_B, uint64_t align_B, enum nvkmd_mem_flags flags,
   struct nvkmd_mem **mem_out)
{
   if (!dev || !owner || !mem_out || !size_B ||
       size_B > UINT64_MAX - NVKMD_NVGPU_NATIVE_CHUNK_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const uint64_t length_B = align64(size_B, NVKMD_NVGPU_NATIVE_CHUNK_B);
   const uint64_t count64 = length_B / NVKMD_NVGPU_NATIVE_CHUNK_B;
   if (count64 > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   const uint32_t count = (uint32_t)count64;
   struct nvkmd_nvgpu_native_chunked_mem *mem =
      calloc(1, sizeof(*mem) + count * sizeof(mem->chunks[0]));
   if (!mem)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!nvkmd_nvgpu_native_owner_lock(owner)) {
      free(mem);
      return VK_ERROR_DEVICE_LOST;
   }
   native_chunks_init(owner);
   mem->owner = owner;
   const uint64_t va = nvkmd_nvgpu_native_owner_reserve_va(
      owner, length_B, MAX2(align_B, NATIVE_CHUNK_ALIGN_B));
   VkResult result = va ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct nvkmd_nvgpu_native_va_hold *hold =
      va ? native_va_hold_create(va, length_B) : NULL;
   if (va && !hold) {
      nvkmd_nvgpu_native_owner_release_va(owner, va, length_B);
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   uint32_t mapped = 0;
   for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
      struct nvkmd_nvgpu_native_chunk *chunk = native_chunk_take(owner);
      if (!chunk) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         break;
      }
      Result rc = 0;
      const enum nvkmd_nvgpu_native_map_status status =
         nvkmd_nvgpu_native_map_range(
            owner->runtime, &chunk->backing,
            va + (uint64_t)i * NVKMD_NVGPU_NATIVE_CHUNK_B, 0,
            NVKMD_NVGPU_NATIVE_CHUNK_B, NATIVE_CHUNK_KIND, true,
            &owner->slot, &rc);
      if (status != NVKMD_NVGPU_NATIVE_MAP_OK) {
         mesa_loge("native chunk: map failed: status %u, result 0x%x",
                   (unsigned)status, rc);
         const uint64_t chunk_va =
            va + (uint64_t)i * NVKMD_NVGPU_NATIVE_CHUNK_B;
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
   }
   if (result != VK_SUCCESS) {
      if (hold)
         native_chunks_unmap_release_va(owner, mem->chunks, mapped, hold);
      nvkmd_nvgpu_native_owner_unlock(owner);
      free(mem);
      return result;
   }
   mem->chunk_count = count;
   mem->va_hold = hold;
   list_addtail(&mem->link, &owner->chunked_mems);
   owner->stats.chunked_mems++;
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
   /* Try every stranded chunk once more, those of the pool included. The
    * result is whether any chunk remains. */
   return native_chunks_release_stranded(owner);
}
