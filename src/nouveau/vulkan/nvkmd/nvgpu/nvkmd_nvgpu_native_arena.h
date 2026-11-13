/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_ARENA_H
#define NVKMD_NVGPU_NATIVE_ARENA_H

#include <stdint.h>
#include <stdbool.h>

enum nvkmd_nvgpu_native_arena_status {
   NVKMD_NVGPU_NATIVE_ARENA_OK,
   NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT,
   NVKMD_NVGPU_NATIVE_ARENA_STATE,
   NVKMD_NVGPU_NATIVE_ARENA_EXHAUSTED,
   NVKMD_NVGPU_NATIVE_ARENA_HOST_MEMORY,
};

struct nvkmd_nvgpu_native_arena_record;

struct nvkmd_nvgpu_native_device_owner;

struct nvkmd_nvgpu_native_arena {
   const struct nvkmd_nvgpu_native_arena *self;
   /* The device owner whose lock serializes this arena. It is NULL for an
    * owner that uses only one thread. Allocation entry points outside the
    * owner's context operations take that lock. */
   struct nvkmd_nvgpu_native_device_owner *owner;
   uint64_t generation;
   uint64_t cpu_base, gpu_base, length, cursor;
   bool coherent_eligible;
   /* host_mappable: the CPU can map the arena because the GPU map is
    * uncached. cpu_cached: the CPU side is cached and not coherent. CPU maps
    * then need dc civac maintenance. */
   bool host_mappable, cpu_cached;
   bool recycle_poisoned;
   /* Set at shutdown, under serialization. After the unmap (raw 0xe5)
    * starts, the arena issues no new slice. */
   bool retiring;
   uint64_t next_serial;
   uint64_t reused_bytes;
   struct nvkmd_nvgpu_native_arena_record *live_records;
   struct nvkmd_nvgpu_native_arena_record *free_ranges;
};

struct nvkmd_nvgpu_native_slice {
   uint64_t generation;
   uint64_t cpu, gpu, offset, length;
   /* Zero marks an infrastructure slice. The arena never recycles it. */
   uint64_t serial;
};

struct nvkmd_nvgpu_native_runtime;
struct nvkmd_nvgpu_native_backing;
struct nvkmd_nvgpu_native_map_owner;
struct nvkmd_nvgpu_native_va_lease;

/* Joins the retained backing, the completed ordinary map and the whole-VA
 * lease into one local arena. This makes no service call. It does not
 * transfer remote ownership and makes no claim about caches. The external
 * objects and their backing must stay alive while any arena slice exists. */
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_bind(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_runtime *runtime,
   const struct nvkmd_nvgpu_native_backing *backing,
   const struct nvkmd_nvgpu_native_map_owner *map,
   const struct nvkmd_nvgpu_native_va_lease *lease);

/* Sets the local geometry only. The caller must already own and retain a
 * CPU backing that the service accepted, a GPU registration, a VA lease and
 * a completed remote map. Each of them must have exactly this extent. This
 * function does not call a service and does not make caches visible. */
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_init(
   struct nvkmd_nvgpu_native_arena *arena, uint64_t generation,
   uint64_t cpu_base, uint64_t gpu_base, uint64_t length);

/* Allocates locally at increasing offsets, rounded to whole pages. These
 * slices cannot be released or reused. That needs a protocol for completed
 * submits and remote-map retirement first. An invalid or exhausted request
 * does not change the cursor. The caller serializes access on the original
 * runtime thread. */
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_alloc(
   struct nvkmd_nvgpu_native_arena *arena, uint64_t length,
   uint64_t alignment, struct nvkmd_nvgpu_native_slice *slice);

/* For ordinary Vulkan allocations only. Each returned slice has a unique
 * serial. After the GPU completes its use of a slice, the final backend unref
 * can return the slice to the arena. The caller must keep the whole backing,
 * map and lease. The caller must serialize calls on the original runtime
 * thread. */
enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_alloc_reclaimable(
   struct nvkmd_nvgpu_native_arena *arena, uint64_t length,
   uint64_t alignment, struct nvkmd_nvgpu_native_slice *slice);

/* An import pin keeps a range out of the recycle pool. The owner can unpin a
 * range only after it retires the external display alias, image, reservation
 * and release dependency of that range. Unpinning a range that was released
 * before recycles it. A poisoned arena recycles no later release. */
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_pin(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_slice *slice);
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_unpin(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_slice *slice);
enum nvkmd_nvgpu_native_arena_status nvkmd_nvgpu_native_arena_release(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_slice *slice);
void nvkmd_nvgpu_native_arena_poison(
   struct nvkmd_nvgpu_native_arena *arena);

/* Frees every record and clears the arena. Used for a released segment. */
void nvkmd_nvgpu_native_arena_finish(struct nvkmd_nvgpu_native_arena *arena);

/* Diagnostic. Returns the bytes in live records that are not released
 * (allocated and not yet returned). Does not change state. */
uint64_t nvkmd_nvgpu_native_arena_live_bytes(
   const struct nvkmd_nvgpu_native_arena *arena);

#endif
