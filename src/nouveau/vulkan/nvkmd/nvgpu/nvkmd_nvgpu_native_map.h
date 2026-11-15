/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_MAP_H
#define NVKMD_NVGPU_NATIVE_MAP_H

#include "nvkmd_nvgpu_native_va.h"

#include <stdbool.h>
#include <stdint.h>

struct nvkmd_nvgpu_native_backing;
struct nvkmd_nvgpu_native_map_owner;

enum nvkmd_nvgpu_native_map_status {
   NVKMD_NVGPU_NATIVE_MAP_OK,
   NVKMD_NVGPU_NATIVE_MAP_ARGUMENT,
   NVKMD_NVGPU_NATIVE_MAP_STATE,
   NVKMD_NVGPU_NATIVE_MAP_REPLY,
   NVKMD_NVGPU_NATIVE_MAP_WAIT,
   NVKMD_NVGPU_NATIVE_MAP_KIND,
   NVKMD_NVGPU_NATIVE_MAP_FAILED, /* an earlier kind change failed */
   /* An earlier request on the slot is unresolved: nothing was sent. */
   NVKMD_NVGPU_NATIVE_MAP_SLOT,
};

/* One main-session completion slot for worker requests (map, kind change,
 * unmap, range unregister). A completion slot serves one request at a time.
 * It is not tied to the lifetime of a map. The service resets the event
 * when it accepts a request. It signals the event on completion. The
 * caller serializes every request that uses the slot.
 */
enum nvkmd_nvgpu_native_slot_request {
   NVKMD_NVGPU_NATIVE_SLOT_IDLE,
   NVKMD_NVGPU_NATIVE_SLOT_MAP,         /* worker raw 0xe3 */
   NVKMD_NVGPU_NATIVE_SLOT_CHANGE_KIND, /* worker raw 0xe4 */
   NVKMD_NVGPU_NATIVE_SLOT_UNMAP,       /* worker raw 0xe5 */
   NVKMD_NVGPU_NATIVE_SLOT_UNREGISTER,  /* worker raw 0x76 */
};

/* A request whose completion the driver has not seen: the worker accepted
 * it and the wait for it failed with an error, or its send failed in the
 * transport. It names what the completion changes, so that the change is
 * made once, when the completion is seen, and before the next request. */
struct nvkmd_nvgpu_native_slot_pending {
   enum nvkmd_nvgpu_native_slot_request request;
   /* The send failed in the transport: whether the worker has the request
    * is not known, and there is no completion to wait for. The slot is not
    * used again. */
   bool acceptance_unknown;
   Result result; /* of the failed wait or send */
   /* UNMAP: the backing loses one map. UNREGISTER: the backing's range is
    * no longer registered. The backing stays where it is until then: its
    * owner keeps a backing that is mapped or registered. */
   struct nvkmd_nvgpu_native_backing *backing;
   /* UNMAP of a whole map (nvkmd_nvgpu_native_map_unmap), else NULL. */
   struct nvkmd_nvgpu_native_map_owner *map;
   /* UNMAP: the interval. */
   uint64_t gpu_va, size_B;
};

/* A range unmap that completed while another request was being prepared.
 * Its caller still holds the mapping's record and asks again later. */
struct nvkmd_nvgpu_native_slot_unmapped {
   const struct nvkmd_nvgpu_native_backing *backing;
   uint64_t gpu_va, size_B;
};

#define NVKMD_NVGPU_NATIVE_SLOT_UNMAPPED_MAX 8

struct nvkmd_nvgpu_native_slot {
   uint32_t index;
   Event event;
   bool ready;
   struct nvkmd_nvgpu_native_slot_pending pending;
   struct nvkmd_nvgpu_native_slot_unmapped
      unmapped[NVKMD_NVGPU_NATIVE_SLOT_UNMAPPED_MAX];
   uint32_t unmapped_count;
};

/* What became of a request that was sent on the slot. */
enum nvkmd_nvgpu_native_slot_outcome {
   /* The worker replied with a failure: nothing changed. */
   NVKMD_NVGPU_NATIVE_SLOT_REFUSED,
   /* Accepted, or possibly accepted, and the completion was not seen. The
    * request is the slot's pending request now. */
   NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED,
   NVKMD_NVGPU_NATIVE_SLOT_COMPLETED,
};

/* Takes the first slot in the range of the runtime that is not the worker
 * slot. Gets the event of that slot (main raw 0x1c). */
Result nvkmd_nvgpu_native_slot_open(struct nvkmd_nvgpu_native_slot *slot,
                                    struct nvkmd_nvgpu_native_runtime *runtime);
/* Returns false, with the slot open, when its pending request cannot be
 * resolved. */
bool nvkmd_nvgpu_native_slot_close(struct nvkmd_nvgpu_native_slot *slot);

/* Call before every request on the slot. Resolves the pending request, if
 * there is one: waits for its completion again and makes its change.
 * Returns false, with the wait's Result, when it is still unresolved; then
 * no request may be sent. */
bool nvkmd_nvgpu_native_slot_settle(struct nvkmd_nvgpu_native_slot *slot,
                                    Result *result_out);

/* Call after every send on the slot, with the send's Result and what the
 * request's completion changes. Waits, without a limit, for the completion
 * of an accepted request; a wait that times out is repeated. *result_out
 * is the Result of the send, or of the wait when that failed. The caller
 * makes the change itself when the outcome is COMPLETED. */
enum nvkmd_nvgpu_native_slot_outcome nvkmd_nvgpu_native_slot_finish(
   struct nvkmd_nvgpu_native_slot *slot, Result send_result,
   const struct nvkmd_nvgpu_native_slot_pending *request,
   Result *result_out);

/* One GPU map of a whole registered range. Keep it at a stable address. */
struct nvkmd_nvgpu_native_map_owner {
   const struct nvkmd_nvgpu_native_map_owner *self;
   uint64_t generation;
   uint64_t gpu_va, length;
   uint32_t vm_id, range_id;
   Result result;
   bool attempted, ready, gpu_uncached;
   /* The worker accepted the map request, and no unmap has completed: the
    * mapping exists or may exist, whether or not the map is ready for use. */
   bool mapped;
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

/* Unmaps a map that is or may be mapped (worker raw 0xe5) and waits for
 * the slot. The map is not ready afterwards. After a failure it is still
 * mapped, and this can be called again: an unmap that the worker accepted
 * earlier is then completed, not sent a second time. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_unmap(
   struct nvkmd_nvgpu_native_map_owner *map,
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_slot *slot);

/* Maps backing bytes [backing_offset_B, +size_B) at gpu_va with the kind
 * (1-6), worker raw 0xe3, and waits for the slot. All values are 4 KiB
 * aligned. The VA must be free. On NVKMD_NVGPU_NATIVE_MAP_KIND the mapping
 * exists with another kind; unmap it. On NVKMD_NVGPU_NATIVE_MAP_WAIT the
 * worker accepted the request but the wait failed with an error (a timeout
 * is waited out): the mapping may exist, so do not use the VA or the
 * backing bytes for anything else. *result_out is the service Result. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_map_range(
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t gpu_va, uint64_t backing_offset_B, uint64_t size_B,
   uint8_t kind, bool gpu_cacheable,
   struct nvkmd_nvgpu_native_slot *slot, Result *result_out);

/* Removes a mapping of nvkmd_nvgpu_native_map_range (worker raw 0xe5 over
 * the same interval) and waits for the slot. After a failure the mapping
 * may remain; call this again with the same interval. An unmap that the
 * worker accepted earlier is then completed, not sent a second time. */
enum nvkmd_nvgpu_native_map_status nvkmd_nvgpu_native_unmap_range(
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t gpu_va, uint64_t size_B,
   struct nvkmd_nvgpu_native_slot *slot, Result *result_out);

#endif
