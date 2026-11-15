/* SPDX-License-Identifier: MIT
 * Original OunceVK GPU map owner. libounce-nx sends the worker requests
 * (gsvGpuMap, gsvGpuChangeKind, gsvGpuUnmap).
 */
#include "nvkmd_nvgpu_native_map.h"
#include "nvkmd_nvgpu_native_backing.h"

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

void
nvkmd_nvgpu_native_slot_close(struct nvkmd_nvgpu_native_slot *slot)
{
   if (!slot || !slot->ready)
      return;
   eventClose(&slot->event);
   memset(slot, 0, sizeof(*slot));
}

/* Waits for the completion of the request that the worker accepted on the
 * slot, without a limit: a timeout says nothing about the request, which
 * the service still runs, and the slot stays busy until it completes. A
 * caller that gave up could neither send another request on the slot nor
 * tell what the first one did. Returns a failure only for a wait error
 * other than a timeout.
 */
Result
nvkmd_nvgpu_native_slot_wait(struct nvkmd_nvgpu_native_slot *slot)
{
   for (;;) {
      const Result rc = eventWait(&slot->event, MAP_WAIT_SLICE_NS);
      if (R_VALUE(rc) != KERNELRESULT(TimedOut))
         return rc;
   }
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
   Result rc = gsvGpuMap(&runtime->gpu, &mapping, slot->index, &selected_kind);
   if (R_FAILED(rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   /* The worker accepted the request, so the mapping exists or will: it
    * counts from here, also when the wait fails or the kind is another. */
   map->mapped = true;
   backing->maps++;
   rc = nvkmd_nvgpu_native_slot_wait(slot);
   if (R_FAILED(rc))
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

   map->modify_failed = true; /* cleared only by a verified completion */
   u8 selected_kind = 0;
   Result rc = gsvGpuChangeKind(&runtime->gpu, map->vm_id,
                                map->gpu_va + offset_B, size_B, kind,
                                slot->index, &selected_kind);
   if (R_FAILED(rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   /* Success needs both a success reply and the completion of the slot. */
   rc = nvkmd_nvgpu_native_slot_wait(slot);
   if (R_FAILED(rc))
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
   if (!map->mapped || !slot->ready || !backing->maps ||
       map->range_id != backing->range_id ||
       map->generation != runtime->generation)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;

   /* No more use of the map. It stays mapped until the unmap completes: a
    * refused unmap can be sent again. */
   map->ready = false;
   Result rc = gsvGpuUnmap(&runtime->gpu, map->vm_id, map->gpu_va,
                           map->length, slot->index);
   if (R_FAILED(rc))
      return map_fail(map, NVKMD_NVGPU_NATIVE_MAP_REPLY, rc);
   rc = nvkmd_nvgpu_native_slot_wait(slot);
   if (R_FAILED(rc))
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
   Result rc = gsvGpuMap(&runtime->gpu, &mapping, slot->index, &selected_kind);
   *result_out = rc;
   if (R_FAILED(rc))
      return NVKMD_NVGPU_NATIVE_MAP_REPLY;
   /* The worker accepted the request, so the mapping exists or will: it
    * counts from here, also when the wait fails or the kind is not the
    * requested one. A backing with a mapping cannot be released. */
   backing->maps++;
   rc = nvkmd_nvgpu_native_slot_wait(slot);
   *result_out = rc;
   if (R_FAILED(rc))
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
   if (!runtime || !backing || !slot || !result_out || !backing->maps)
      return NVKMD_NVGPU_NATIVE_MAP_ARGUMENT;
   if (!nvkmd_nvgpu_native_runtime_ready(runtime) || !slot->ready)
      return NVKMD_NVGPU_NATIVE_MAP_STATE;
   Result rc = gsvGpuUnmap(&runtime->gpu, runtime->vm.id, gpu_va, size_B,
                           slot->index);
   *result_out = rc;
   if (R_FAILED(rc))
      return NVKMD_NVGPU_NATIVE_MAP_REPLY;
   rc = nvkmd_nvgpu_native_slot_wait(slot);
   *result_out = rc;
   if (R_FAILED(rc))
      return NVKMD_NVGPU_NATIVE_MAP_WAIT;
   backing->maps--;
   return NVKMD_NVGPU_NATIVE_MAP_OK;
}
