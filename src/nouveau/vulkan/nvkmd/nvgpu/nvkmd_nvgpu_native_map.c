/* SPDX-License-Identifier: MIT
 * Original OunceVK GPU map owner. libounce-nx sends the worker requests
 * (gsvGpuMap, gsvGpuChangeKind, gsvGpuUnmap).
 */
#include "nvkmd_nvgpu_native_map.h"
#include "nvkmd_nvgpu_native_backing.h"

#include "util/log.h"
#include <ouncevk/profile.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define MAP_PAGE_B UINT64_C(0x1000)
#define MAP_SLOT_LIMIT 32
#define MAP_KIND_ORDINARY 6
/* One wait for a slot's completion. A wait that times out is repeated. */
#define MAP_WAIT_SLICE_NS UINT64_C(1000000000)

Result
nvkmd_nvgpu_native_slot_open(struct nvkmd_nvgpu_native_slot *slot,
                             struct nvkmd_nvgpu_native_runtime *runtime)
{
   if (!slot || !nvkmd_nvgpu_native_runtime_ready(runtime) || slot->ready)
      return MAKERESULT(Module_Libnx, LibnxError_BadInput);

   uint32_t index = runtime->worker.lower;
   while (index <= runtime->worker.upper && index == runtime->worker.slot)
      index++;
   if (index > runtime->worker.upper || index >= MAP_SLOT_LIMIT)
      return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

   const Result rc = gsvGpuCreateSlotEvent(&runtime->gpu, index,
                                           &slot->event, false);
   if (R_SUCCEEDED(rc)) {
      slot->index = index;
      slot->ready = true;
   }
   return rc;
}

bool
nvkmd_nvgpu_native_slot_close(struct nvkmd_nvgpu_native_slot *slot)
{
   if (!slot || !slot->ready)
      return true;
   Result rc = 0;
   if (!nvkmd_nvgpu_native_slot_settle(slot, &rc))
      return false;
   /* Every mapping's record was retired before the slot is closed, so a
    * record that is left here belongs to an object that was never
    * destroyed. */
   if (slot->unmapped_count)
      mesa_logw("native slot: %u late unmaps were not asked for again",
                slot->unmapped_count);
   free(slot->unmapped);
   eventClose(&slot->event);
   memset(slot, 0, sizeof(*slot));
   return true;
}

/* Waits for the completion of the request that the worker accepted on the
 * slot, without a limit: a timeout says nothing about the request, which
 * the service still runs, and the slot stays busy until it completes. A
 * caller that gave up could neither send another request on the slot nor
 * tell what the first one did. Returns a failure only for a wait error
 * other than a timeout.
 */
static Result
slot_wait(struct nvkmd_nvgpu_native_slot *slot)
{
   for (;;) {
      const Result rc = eventWait(&slot->event, MAP_WAIT_SLICE_NS);
      if (R_VALUE(rc) != KERNELRESULT(TimedOut))
         return rc;
   }
}

/* A send that fails with a kernel Result did not get the worker's reply:
 * the request may have arrived all the same. Any other failure is the
 * worker's reply, which refuses the request. */
static bool
send_failed_in_transport(Result rc)
{
   return R_FAILED(rc) && R_MODULE(rc) == Module_Kernel;
}

/* Makes the change of the pending request, whose completion was seen. */
static void
slot_apply_pending(struct nvkmd_nvgpu_native_slot *slot)
{
   const struct nvkmd_nvgpu_native_slot_pending pending = slot->pending;
   memset(&slot->pending, 0, sizeof(slot->pending));
   switch (pending.request) {
   case NVKMD_NVGPU_NATIVE_SLOT_UNMAP:
      pending.backing->maps--;
      if (pending.map) {
         pending.map->mapped = false;
      } else {
         /* The room was made before the unmap was sent. */
         assert(slot->unmapped_count < slot->unmapped_capacity);
         slot->unmapped[slot->unmapped_count++] =
            (struct nvkmd_nvgpu_native_slot_unmapped) {
               .backing = pending.backing,
               .gpu_va = pending.gpu_va,
               .size_B = pending.size_B,
            };
      }
      break;
   case NVKMD_NVGPU_NATIVE_SLOT_UNREGISTER:
      pending.backing->registered = false;
      break;
   case NVKMD_NVGPU_NATIVE_SLOT_MAP:
      /* The mapping counted from the send; its caller unmaps it. */
   case NVKMD_NVGPU_NATIVE_SLOT_CHANGE_KIND:
      /* The map stays marked: no later kind change is attempted. */
   case NVKMD_NVGPU_NATIVE_SLOT_IDLE:
      break;
   }
}

bool
nvkmd_nvgpu_native_slot_settle(struct nvkmd_nvgpu_native_slot *slot,
                               Result *result_out)
{
   struct nvkmd_nvgpu_native_slot_pending *pending = &slot->pending;
   if (pending->request == NVKMD_NVGPU_NATIVE_SLOT_IDLE)
      return true;
   if (!pending->acceptance_unknown) {
      pending->result = slot_wait(slot);
      if (R_SUCCEEDED(pending->result)) {
         slot_apply_pending(slot);
         return true;
      }
   }
   *result_out = pending->result;
   return false;
}

/* The profile timer of a slot request. */
static enum ouncevk_profile_timer
slot_request_timer(enum nvkmd_nvgpu_native_slot_request request)
{
   switch (request) {
   case NVKMD_NVGPU_NATIVE_SLOT_MAP:
      return OUNCEVK_PROFILE_SERVICE_MAP;
   case NVKMD_NVGPU_NATIVE_SLOT_CHANGE_KIND:
      return OUNCEVK_PROFILE_SERVICE_KIND;
   case NVKMD_NVGPU_NATIVE_SLOT_UNREGISTER:
      return OUNCEVK_PROFILE_SERVICE_UNREGISTER;
   case NVKMD_NVGPU_NATIVE_SLOT_UNMAP:
   case NVKMD_NVGPU_NATIVE_SLOT_IDLE:
      break;
   }
   return OUNCEVK_PROFILE_SERVICE_UNMAP;
}

enum nvkmd_nvgpu_native_slot_outcome
nvkmd_nvgpu_native_slot_finish(
   struct nvkmd_nvgpu_native_slot *slot, uint64_t sent_at,
   Result send_result,
   const struct nvkmd_nvgpu_native_slot_pending *request,
   Result *result_out)
{
   *result_out = send_result;
   if (R_SUCCEEDED(send_result))
      *result_out = slot_wait(slot);
   ouncevk_profile_record(slot_request_timer(request->request),
                          armGetSystemTick() - sent_at);
   if (R_FAILED(send_result) && !send_failed_in_transport(send_result))
      return NVKMD_NVGPU_NATIVE_SLOT_REFUSED;
   if (R_SUCCEEDED(send_result) && R_SUCCEEDED(*result_out))
      return NVKMD_NVGPU_NATIVE_SLOT_COMPLETED;
   slot->pending = *request;
   slot->pending.acceptance_unknown = R_FAILED(send_result);
   slot->pending.result = *result_out;
   return NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED;
}

/* Makes room for the record of one more range unmap that completes late. */
static bool
slot_reserve_unmapped(struct nvkmd_nvgpu_native_slot *slot)
{
   if (slot->unmapped_count < slot->unmapped_capacity)
      return true;
   const uint32_t capacity = slot->unmapped_capacity + 8;
   struct nvkmd_nvgpu_native_slot_unmapped *unmapped =
      realloc(slot->unmapped, capacity * sizeof(*unmapped));
   if (!unmapped)
      return false;
   slot->unmapped = unmapped;
   slot->unmapped_capacity = capacity;
   return true;
}

/* Takes the record of a range unmap that completed late. */
static bool
slot_claim_unmapped(struct nvkmd_nvgpu_native_slot *slot,
                    const struct nvkmd_nvgpu_native_backing *backing,
                    uint64_t gpu_va, uint64_t size_B)
{
   for (uint32_t i = 0; i < slot->unmapped_count; i++) {
      const struct nvkmd_nvgpu_native_slot_unmapped *unmapped =
         &slot->unmapped[i];
      if (unmapped->backing == backing && unmapped->gpu_va == gpu_va &&
          unmapped->size_B == size_B) {
         slot->unmapped[i] = slot->unmapped[--slot->unmapped_count];
         return true;
      }
   }
   return false;
}

static enum nvkmd_nvgpu_native_map_status
map_fail(struct nvkmd_nvgpu_native_map_owner *map,
         enum nvkmd_nvgpu_native_map_status status, Result rc)
{
   map->result = rc;
   return status;
}

enum nvkmd_nvgpu_native_map_status
nvkmd_nvgpu_native_map_once(struct nvkmd_nvgpu_native_map_owner *map,
                            struct nvkmd_nvgpu_native_runtime *runtime,
                            const struct nvkmd_nvgpu_native_va_lease *lease,
                            struct nvkmd_nvgpu_native_backing *backing,
                            bool gpu_cacheable,
                            struct nvkmd_nvgpu_native_slot *slot)
{
   if (!map || !runtime || !lease || !backing || !slot ||
       !nvkmd_nvgpu_native_runtime_ready(runtime) ||
       lease->vm_id != runtime->vm.id || lease->as_fd != runtime->as.fd ||
       lease->runtime_generation != runtime->generation || !lease->length ||
       backing->self != backing || !backing->ready ||
       backing->generation != runtime->generation ||
       backing->length != lease->length)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   if (map->self || map->attempted || !slot->ready)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;
   Result rc = 0;
   if (!nvkmd_nvgpu_native_slot_settle(slot, &rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_SLOT, rc);

   map->self = map;
   map->attempted = true;
   map->generation = runtime->generation;
   map->gpu_va = lease->start;
   map->length = lease->length;
   map->vm_id = runtime->vm.id;
   map->range_id = backing->range_id;

   /* The mapping's GPU cacheability: 1 for GPU-cached memory, 0 for
    * uncached. The service accepts 0 or 1. */
   const GsvGpuMapping mapping = {
      .vm_id = map->vm_id,
      .va = map->gpu_va,
      .size = map->length,
      .range_id = map->range_id,
      .range_offset = 0,
      .kind = MAP_KIND_ORDINARY,
      .fallback_kind = MAP_KIND_ORDINARY,
      .gpu_cacheable = gpu_cacheable,
      .client = runtime->vm.client,
   };
   u8 selected_kind = 0;
   const uint64_t sent_at = armGetSystemTick();
   rc = gsvGpuMap(&runtime->gpu, &mapping, slot->index, &selected_kind);
   const struct nvkmd_nvgpu_native_slot_pending request = {
      .request = NVKMD_NVGPU_NATIVE_SLOT_MAP,
   };
   const enum nvkmd_nvgpu_native_slot_outcome outcome =
      nvkmd_nvgpu_native_slot_finish(slot, sent_at, rc, &request, &rc);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   /* The worker accepted the request, or may have: the mapping exists, will
    * exist or may exist. It counts, also when the completion was not seen
    * or the kind is another. */
   map->mapped = true;
   backing->maps++;
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_WAIT, rc);
   if (selected_kind != MAP_KIND_ORDINARY)
      return NVKMD_NVGPU_NATIVE_MAP_KIND;

   map->gpu_uncached = !gpu_cacheable;
   map->ready = true;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}

/* Worker raw 0xe4 changes the kind of part of an existing map. The service
 * checks owner permission and a 4 KiB interval. The interval must be inside
 * one existing map. Only uncompressed kinds 1-6 are accepted. Completion
 * signals the slot event. */
enum nvkmd_nvgpu_native_map_status
nvkmd_nvgpu_native_map_modify_kind(struct nvkmd_nvgpu_native_map_owner *map,
                                   struct nvkmd_nvgpu_native_runtime *runtime,
                                   uint64_t offset_B, uint64_t size_B,
                                   uint8_t kind,
                                   struct nvkmd_nvgpu_native_slot *slot)
{
   if (!map || map->self != map || !runtime || !slot || !size_B ||
       kind < 1 || kind > MAP_KIND_ORDINARY ||
       ((offset_B | size_B) & (MAP_PAGE_B - 1)) ||
       offset_B >= map->length || size_B > map->length - offset_B)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   if (!map->ready || !slot->ready || map->generation != runtime->generation)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;
   if (map->modify_failed)
      return NVKMD_NVGPU_NATIVE_MAP_FAILED;
   Result rc = 0;
   if (!nvkmd_nvgpu_native_slot_settle(slot, &rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_SLOT, rc);

   map->modify_failed = true; /* cleared only by a verified completion */
   u8 selected_kind = 0;
   const uint64_t sent_at = armGetSystemTick();
   rc = gsvGpuChangeKind(&runtime->gpu, map->vm_id, map->gpu_va + offset_B,
                         size_B, kind, slot->index, &selected_kind);
   const struct nvkmd_nvgpu_native_slot_pending request = {
      .request = NVKMD_NVGPU_NATIVE_SLOT_CHANGE_KIND,
   };
   /* Success needs both a success reply and the completion of the slot. */
   const enum nvkmd_nvgpu_native_slot_outcome outcome =
      nvkmd_nvgpu_native_slot_finish(slot, sent_at, rc, &request, &rc);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_WAIT, rc);
   if (selected_kind != kind)
      return NVKMD_NVGPU_NATIVE_MAP_KIND;
   map->modify_failed = false;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}

enum nvkmd_nvgpu_native_map_status
nvkmd_nvgpu_native_map_unmap(struct nvkmd_nvgpu_native_map_owner *map,
                             struct nvkmd_nvgpu_native_runtime *runtime,
                             struct nvkmd_nvgpu_native_backing *backing,
                             struct nvkmd_nvgpu_native_slot *slot)
{
   if (!map || map->self != map || !runtime || !backing || !slot)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   const bool was_mapped = map->mapped;
   Result rc = 0;
   if (!nvkmd_nvgpu_native_slot_settle(slot, &rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_SLOT, rc);
   /* This map's own unmap was pending, and its completion was just seen. */
   if (was_mapped && !map->mapped)
      return NVKMD_NVGPU_NATIVE_MAP_OK;
   if (!map->mapped || !slot->ready || !backing->maps ||
       map->range_id != backing->range_id ||
       map->generation != runtime->generation)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;

   /* No more use of the map. It stays mapped until the unmap completes: a
    * refused unmap can be sent again. */
   map->ready = false;
   const uint64_t sent_at = armGetSystemTick();
   rc = gsvGpuUnmap(&runtime->gpu, map->vm_id, map->gpu_va, map->length,
                    slot->index);
   const struct nvkmd_nvgpu_native_slot_pending request = {
      .request = NVKMD_NVGPU_NATIVE_SLOT_UNMAP,
      .backing = backing,
      .map = map,
      .gpu_va = map->gpu_va,
      .size_B = map->length,
   };
   const enum nvkmd_nvgpu_native_slot_outcome outcome =
      nvkmd_nvgpu_native_slot_finish(slot, sent_at, rc, &request, &rc);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_WAIT, rc);
   map->mapped = false;
   backing->maps--;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}

/* Maps part of a registered backing at a GPU VA that the caller reserved
 * (worker raw 0xe3 with a backing offset). Kinds 1-6 are accepted. Waits
 * for the slot. */
enum nvkmd_nvgpu_native_map_status
nvkmd_nvgpu_native_map_range(struct nvkmd_nvgpu_native_runtime *runtime,
                             struct nvkmd_nvgpu_native_backing *backing,
                             uint64_t gpu_va, uint64_t backing_offset_B,
                             uint64_t size_B, uint8_t kind,
                             bool gpu_cacheable,
                             struct nvkmd_nvgpu_native_slot *slot,
                             Result *result_out)
{
   if (!runtime || !backing || !slot || !result_out || !gpu_va || !size_B ||
       kind < 1 || kind > MAP_KIND_ORDINARY ||
       ((gpu_va | backing_offset_B | size_B) & (MAP_PAGE_B - 1)) ||
       backing_offset_B >= backing->length ||
       size_B > backing->length - backing_offset_B)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   if (!nvkmd_nvgpu_native_runtime_ready(runtime) || !slot->ready ||
       backing->self != backing || !backing->ready ||
       backing->generation != runtime->generation)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;
   if (!nvkmd_nvgpu_native_slot_settle(slot, result_out))
      return NVKMD_NVGPU_NATIVE_MAP_SLOT;

   const GsvGpuMapping mapping = {
      .vm_id = runtime->vm.id,
      .va = gpu_va,
      .size = size_B,
      .range_id = backing->range_id,
      .range_offset = backing_offset_B,
      .kind = kind,
      .fallback_kind = kind,
      .gpu_cacheable = gpu_cacheable,
      .client = runtime->vm.client,
   };
   u8 selected_kind = 0;
   const uint64_t sent_at = armGetSystemTick();
   const Result rc =
      gsvGpuMap(&runtime->gpu, &mapping, slot->index, &selected_kind);
   const struct nvkmd_nvgpu_native_slot_pending request = {
      .request = NVKMD_NVGPU_NATIVE_SLOT_MAP,
   };
   const enum nvkmd_nvgpu_native_slot_outcome outcome =
      nvkmd_nvgpu_native_slot_finish(slot, sent_at, rc, &request, result_out);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
      return NVKMD_NVGPU_NATIVE_MAP_REPLY;
   /* The worker accepted the request, or may have: the mapping exists, will
    * exist or may exist. It counts, also when the completion was not seen
    * or the kind is not the requested one. A backing with a mapping cannot
    * be released. */
   backing->maps++;
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
      return NVKMD_NVGPU_NATIVE_MAP_WAIT;
   if (selected_kind != kind)
      return NVKMD_NVGPU_NATIVE_MAP_KIND;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}

/* Removes one mapping that nvkmd_nvgpu_native_map_range made (worker raw
 * 0xe5 over its whole interval). Waits for the slot. */
enum nvkmd_nvgpu_native_map_status
nvkmd_nvgpu_native_unmap_range(struct nvkmd_nvgpu_native_runtime *runtime,
                               struct nvkmd_nvgpu_native_backing *backing,
                               uint64_t gpu_va, uint64_t size_B,
                               struct nvkmd_nvgpu_native_slot *slot,
                               Result *result_out)
{
   if (!runtime || !backing || !slot || !result_out)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   if (!nvkmd_nvgpu_native_runtime_ready(runtime) || !slot->ready)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;
   if (!nvkmd_nvgpu_native_slot_settle(slot, result_out))
      return NVKMD_NVGPU_NATIVE_MAP_SLOT;
   /* An earlier unmap of this interval was accepted and has completed,
    * now or before another request. */
   if (slot_claim_unmapped(slot, backing, gpu_va, size_B))
      return NVKMD_NVGPU_NATIVE_MAP_OK;
   if (!backing->maps)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   /* Without room for its record, the unmap is not sent: its completion
    * could not be handed to this caller if it came late. */
   if (!slot_reserve_unmapped(slot)) {
      *result_out = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
      return NVKMD_NVGPU_NATIVE_MAP_SLOT;
   }
   const uint64_t sent_at = armGetSystemTick();
   const Result rc = gsvGpuUnmap(&runtime->gpu, runtime->vm.id, gpu_va,
                                 size_B, slot->index);
   const struct nvkmd_nvgpu_native_slot_pending request = {
      .request = NVKMD_NVGPU_NATIVE_SLOT_UNMAP,
      .backing = backing,
      .gpu_va = gpu_va,
      .size_B = size_B,
   };
   const enum nvkmd_nvgpu_native_slot_outcome outcome =
      nvkmd_nvgpu_native_slot_finish(slot, sent_at, rc, &request, result_out);
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
      return NVKMD_NVGPU_NATIVE_MAP_REPLY;
   if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
      return NVKMD_NVGPU_NATIVE_MAP_WAIT;
   backing->maps--;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}
