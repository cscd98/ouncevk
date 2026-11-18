/* SPDX-License-Identifier: MIT
 * Page-granular GPU-only memory.
 *
 * A large GPU-only allocation does not need one CPU-contiguous span. It is
 * a VA range from the owner's VA heap. Fixed-size chunks of the process
 * heap, each registered with GSV once, are mapped at consecutive offsets in
 * that range (worker e3 with the chunk's backing). A free unmaps
 * the chunks (e5) and keeps them registered in a pool for the next
 * allocation. So the process heap does not fragment for these allocations.
 */
#ifndef NVKMD_NVGPU_NATIVE_CHUNKED_H
#define NVKMD_NVGPU_NATIVE_CHUNKED_H

#include "nvkmd/nvkmd.h"
#include "nvkmd_nvgpu_native_device_owner.h"

struct nvkmd_nvgpu_native_va_hold;

#define NVKMD_NVGPU_NATIVE_CHUNK_B (UINT64_C(4) * 1024 * 1024)

/* One registered chunk of the process heap. */
struct nvkmd_nvgpu_native_chunk {
   struct nvkmd_nvgpu_native_chunk *next;
   void *allocation;
   struct nvkmd_nvgpu_native_backing backing;
   /* For a chunk that is out of the pool: the GPU VA where its own
    * mapping may remain, or 0 when that mapping is gone. */
   uint64_t stranded_va;
   /* With stranded_va: the hold of the VA of the chunk's allocation. */
   struct nvkmd_nvgpu_native_va_hold *va_hold;
   /* The chunk was mapped with a compressible kind: its backing has
    * compression tags until it is released. */
   bool compressible;
};

/* A part of a memory range that lies in one backing, for a view. */
struct nvkmd_nvgpu_native_view_part {
   uint64_t offset_B; /* from the start of the requested range */
   uint64_t size_B;
   struct nvkmd_nvgpu_native_view_source source;
};

/* True for memory of nvkmd_nvgpu_native_chunked_mem_create. */
bool nvkmd_nvgpu_native_mem_is_chunked(const struct nvkmd_mem *mem);

/* GPU-only memory of size_B (rounded up to whole chunks), at a VA aligned
 * to align_B, with the GPU cache on. pte_kind is 6, or a compressible kind
 * for compressed memory (flags has NVKMD_MEM_COMPRESSED). Compressed memory
 * is mapped with that kind in every chunk or not made:
 * VK_ERROR_FORMAT_NOT_SUPPORTED when the service mapped the ordinary
 * fallback kind (no compression tags were free). */
VkResult nvkmd_nvgpu_native_chunked_mem_create(
   struct nvkmd_dev *dev, struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t size_B, uint64_t align_B, enum nvkmd_mem_flags flags,
   uint8_t pte_kind, struct nvkmd_mem **mem_out);

/* The parts of [offset_B, +size_B) of chunked memory, one for each chunk
 * that the range touches. Returns the number of parts, or 0 when the range
 * is not in the memory or max is too small. Compressed memory has no parts:
 * a view of it with another kind would read the compressed bytes. */
uint32_t nvkmd_nvgpu_native_chunked_mem_parts(
   const struct nvkmd_mem *mem, uint64_t offset_B, uint64_t size_B,
   struct nvkmd_nvgpu_native_view_part *parts, uint32_t max);

/* The steps of the chunk reserve (nvkmd_nvgpu_native_chunk_reserve.h),
 * which registers free chunks ahead of the allocations that need them. */

/* The number of chunks that the reserve lacks, or 0 when it is full or the
 * device can take no more. Call with the owner lock held. */
uint32_t nvkmd_nvgpu_native_chunk_reserve_missing(
   const struct nvkmd_nvgpu_native_device_owner *owner);

/* A chunk with cleared memory from the heap, not registered yet. NULL when
 * there is no memory. Call without the owner lock: the clearing is the long
 * step, and it must not hold up the other threads. */
struct nvkmd_nvgpu_native_chunk *
nvkmd_nvgpu_native_chunk_reserve_prepare(void);

/* Registers a prepared chunk and puts it in the pool. Returns false when
 * the registration failed; the chunk is then gone (freed, or set aside for
 * device close). Call with the owner lock held. */
bool nvkmd_nvgpu_native_chunk_reserve_add(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_chunk *chunk);

/* Frees a prepared chunk that was not added. */
void nvkmd_nvgpu_native_chunk_reserve_discard(
   struct nvkmd_nvgpu_native_chunk *chunk);

/* Returns to the pool every set-aside chunk that only waited for its
 * views: nothing maps it any more, and it did not fail. Call with the owner
 * lock held, after a view is unmapped. */
void nvkmd_nvgpu_native_owner_reclaim_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* When the process heap has no memory for the driver: returns every free
 * chunk of both pools to the heap, after a finish flush, and returns their
 * number. The reserve then waits for an allocation that registers a chunk
 * before it fills the pool again. Call with the owner lock held; the lock
 * is released during the wait for the flush. */
uint32_t nvkmd_nvgpu_native_owner_release_free_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner, struct nvkmd_dev *dev);

/* The bytes of free chunks in both pools. Call with the owner lock held. */
uint64_t nvkmd_nvgpu_native_owner_free_chunk_bytes(
   const struct nvkmd_nvgpu_native_device_owner *owner);

/* At device close: unmaps the chunks of memory that the program did not
 * free and releases every chunk, the set-aside ones included. Returns false
 * when a chunk is still mapped or registered afterwards. */
bool nvkmd_nvgpu_native_owner_release_chunks(
   struct nvkmd_nvgpu_native_device_owner *owner);

#endif
