/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_MAP_H
#define NVKMD_NVGPU_NATIVE_MAP_H

#include "nvkmd_nvgpu_native_va.h"

#include <stdbool.h>
#include <stdint.h>

struct nvkmd_nvgpu_native_backing;

enum nvkmd_nvgpu_native_map_status {
   NVKMD_NVGPU_NATIVE_MAP_OK,
   NVKMD_NVGPU_NATIVE_MAP_ARGUMENT,
   NVKMD_NVGPU_NATIVE_MAP_STATE,
   NVKMD_NVGPU_NATIVE_MAP_REPLY,
   NVKMD_NVGPU_NATIVE_MAP_WAIT,
   NVKMD_NVGPU_NATIVE_MAP_KIND,
   NVKMD_NVGPU_NATIVE_MAP_FAILED, /* an earlier kind change failed */
};

/* One main-session completion slot for worker requests (map, kind change,
 * unmap, range unregister). A completion slot serves one request at a time.
 * It is not tied to the lifetime of a map. The service resets the event
 * when it accepts a request. It signals the event on completion. The
 * caller serializes every request that uses the slot.
 */
struct nvkmd_nvgpu_native_slot {
   uint32_t index;
   Event event;
   bool ready;
};

/* Takes the first slot in the range of the runtime that is not the worker
 * slot. Gets the event of that slot (main raw 0x1c). */
Result nvkmd_nvgpu_native_slot_open(struct nvkmd_nvgpu_native_slot *slot,
                                    struct nvkmd_nvgpu_native_runtime *runtime);
void nvkmd_nvgpu_native_slot_close(struct nvkmd_nvgpu_native_slot *slot);
/* Waits up to 5 s for the slot to complete the request just sent. */
Result nvkmd_nvgpu_native_slot_wait(struct nvkmd_nvgpu_native_slot *slot);

/* One GPU map of a whole registered range. Keep it at a stable address. */
struct nvkmd_nvgpu_native_map_owner {
   const struct nvkmd_nvgpu_native_map_owner *self;
   uint64_t generation;
   uint64_t gpu_va, length;
   uint32_t vm_id, range_id;
   Result result;
   bool attempted, ready, gpu_uncached;
   /* A raw 0xe4 kind change failed or has an unknown result. No more kind
    * changes are attempted. */
   bool modify_failed;
};

/* Maps backing at the lease (worker raw 0xe3, kind 6). Waits for the slot
 * to complete. gpu_cacheable selects byte +0x2c of raw 0xe3 (the
 * GpuCacheability argument). A range can be mapped more than once, at
 * disjoint VA intervals. A failure after the send makes the map unusable. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_once(
   struct nvkmd_nvgpu_native_map_owner *map,
   struct nvkmd_nvgpu_native_runtime *runtime,
   const struct nvkmd_nvgpu_native_va_lease *lease,
   struct nvkmd_nvgpu_native_backing *backing,
   bool gpu_cacheable,
   struct nvkmd_nvgpu_native_slot *slot);

/* Changes the PTE kind of [offset_B, offset_B + size_B) of a ready map in
 * place, with worker raw 0xe4. The new kind is an uncompressed kind (1-6).
 * Then waits for the slot. Offset and size are 4 KiB multiples within the
 * map. The caller must first retire all GPU use of the range under its old
 * kind. Any failure marks the map, and no later change is attempted. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_modify_kind(
   struct nvkmd_nvgpu_native_map_owner *map,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t offset_B, uint64_t size_B, uint8_t kind,
   struct nvkmd_nvgpu_native_slot *slot);

/* Unmaps a ready map (worker raw 0xe5) and waits for the slot. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_unmap(
   struct nvkmd_nvgpu_native_map_owner *map,
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_slot *slot);

/* Maps backing bytes [backing_offset_B, +size_B) at gpu_va with the kind
 * (1-6), worker raw 0xe3, and waits for the slot. All values are 4 KiB
 * aligned. The VA must be free. On NVKMD_NVGPU_NATIVE_MAP_KIND the mapping
 * exists with another kind; unmap it. *result_out is the service Result. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_range(
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t gpu_va, uint64_t backing_offset_B, uint64_t size_B,
   uint8_t kind, bool gpu_cacheable,
   struct nvkmd_nvgpu_native_slot *slot, Result *result_out);

/* Removes a mapping of nvkmd_nvgpu_native_map_range (worker raw 0xe5 over
 * the same interval) and waits for the slot. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_unmap_range(
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t gpu_va, uint64_t size_B,
   struct nvkmd_nvgpu_native_slot *slot, Result *result_out);

#endif
