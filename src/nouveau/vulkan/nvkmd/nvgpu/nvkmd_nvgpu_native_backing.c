/* SPDX-License-Identifier: MIT
 * Original OunceVK owner of one GPU-registered CPU span. libounce-nx sends the
 * worker requests (gsvGpuRegisterMemory, gsvGpuUnregisterMemory) and
 * SVC 0x80.
 */
#include "nvkmd_nvgpu_native_backing.h"
#include "nvkmd_nvgpu_native_cache.h"
#include "nvkmd_nvgpu_native_map.h"

#include "util/log.h"
#include "util/simple_mtx.h"

#include <inttypes.h>
#include <string.h>

#define BACKING_PAGE_B UINT64_C(0x1000)
#define BACKING_CPU_ADDRESS_LIMIT (UINT64_C(1) << 39)
#define BACKING_CLIENT_LIMIT 8
/* The QueryMemory check of an ordinary heap span: MemType_Heap, no
 * attributes, read/write. */
#define BACKING_HEAP_PERMISSIONS (Perm_R | Perm_W)
/* The SVC 0x80 attribute to set before worker raw 0x75. */
#define BACKING_PREPARE_BIT 0x20
#define BACKING_UNCACHED_ATTRIBUTE MemAttr_IsUncached
/* The Result of an SVC 0x80 clear while the GPU service still holds the
 * span (2001-0106). After the program's sessions close, the service releases
 * the registrations asynchronously; until it has, the clear returns this.
 * Keep the allocation and retry the clear later. */
#define BACKING_CLEAR_NOT_YET KERNELRESULT(InvalidMemoryState)
/* How long the exit hook retries the clears, in all. */
#define BACKING_CLEAR_RETRY_TIMEOUT_NS UINT64_C(2000000000)
#define BACKING_CLEAR_RETRY_INTERVAL_NS UINT64_C(1000000)

static bool
query(uint64_t address, MemoryInfo *info)
{
   u32 page_info = 0;
   memset(info, 0, sizeof(*info));
   return R_SUCCEEDED(svcQueryMemory(info, &page_info, address));
}

static bool
heap_span(const MemoryInfo *info, uint64_t base, uint64_t length)
{
   return info->addr <= base && info->size >= length &&
          base - info->addr <= info->size - length &&
          (info->type & MemState_Type) == MemType_Heap && !info->attr &&
          info->perm == BACKING_HEAP_PERMISSIONS &&
          !info->ipc_refcount && !info->device_refcount;
}

bool
nvkmd_nvgpu_native_backing_importable(const MemoryInfo *info)
{
   switch (info->type & MemState_Type) {
   /* GSV's raw 0x75 checks no CPU memory state; it pins the span through
    * SVC 0x81, whose rules on Switch 2 are not known. Only heap memory has
    * been tested.
    * The others are the states that Atmosphere's Switch 1 kernel marks
    * device-mappable (KMemoryState_FlagCanDeviceMap): heap and
    * svcMapPhysicalMemory (Normal), writable code (CodeData,
    * AliasCodeData) and svcMapMemory aliases (Stack). svcControlCodeMemory's
    * writable view (CodeOut, type 0x15) is not device-mappable there.
    * The kernel decides; this check only rejects the others early. */
   case MemType_Heap:
   case MemType_CodeMutable:
   case MemType_ModuleCodeMutable:
   case MemType_MappedMemory:
      break;
   default:
      return false;
   }
   return !info->attr && info->perm == BACKING_HEAP_PERMISSIONS &&
          !info->ipc_refcount && !info->device_refcount;
}

static bool
importable_span(const MemoryInfo *info, uint64_t base, uint64_t length)
{
   return info->addr <= base && info->size >= length &&
          base - info->addr <= info->size - length &&
          nvkmd_nvgpu_native_backing_importable(info);
}

static enum nvkmd_nvgpu_native_backing_status
fail(struct nvkmd_nvgpu_native_backing *backing,
     enum nvkmd_nvgpu_native_backing_status status, Result rc)
{
   backing->result = rc;
   return status;
}

/* Every CPU span whose SVC 0x80 prepare bit is set, from the moment it is set
 * until a release (or the rollback of a refused import) clears it again, and
 * whether its uncached attribute is set. These attributes belong to the
 * process, not to the GPU service. A program that exits without destroying
 * its Vulkan or EGL objects leaves spans registered, and the exit hook may
 * fail to release them; after it closes the GPU sessions, it returns them to
 * plain memory (nvkmd_nvgpu_native_backing_restore_abandoned). In an NRO, the
 * heap then goes to the next program in that state otherwise: the kernel
 * refuses, for example, transfer memory over those pages. */
#define BACKING_RECORDS 256

struct backing_record {
   uint64_t cpu_base, length; /* length 0: a free record */
   bool uncached;
};

static struct backing_record backing_records[BACKING_RECORDS];
static simple_mtx_t backing_records_mutex = SIMPLE_MTX_INITIALIZER;

static void
record_span(uint64_t cpu_base, uint64_t length)
{
   simple_mtx_lock(&backing_records_mutex);
   bool recorded = false;
   for (uint32_t i = 0; i < BACKING_RECORDS && !recorded; i++) {
      if (backing_records[i].length)
         continue;
      backing_records[i] = (struct backing_record){cpu_base, length, false};
      recorded = true;
   }
   simple_mtx_unlock(&backing_records_mutex);
   if (!recorded)
      mesa_logw("GPU span 0x%" PRIx64 "+0x%" PRIx64 " is not recorded: "
                "its attributes are not restored if it is abandoned",
                cpu_base, length);
}

static void
record_uncached(uint64_t cpu_base)
{
   simple_mtx_lock(&backing_records_mutex);
   for (uint32_t i = 0; i < BACKING_RECORDS; i++) {
      if (backing_records[i].length && backing_records[i].cpu_base == cpu_base)
         backing_records[i].uncached = true;
   }
   simple_mtx_unlock(&backing_records_mutex);
}

static void
forget_span(uint64_t cpu_base)
{
   simple_mtx_lock(&backing_records_mutex);
   for (uint32_t i = 0; i < BACKING_RECORDS; i++) {
      if (backing_records[i].length && backing_records[i].cpu_base == cpu_base)
         backing_records[i] = (struct backing_record){0};
   }
   simple_mtx_unlock(&backing_records_mutex);
}

uint32_t
nvkmd_nvgpu_native_backing_restore_abandoned(uint32_t *failed)
{
   uint32_t restored = 0, failures = 0;
   const uint64_t deadline =
      armGetSystemTick() + armNsToTicks(BACKING_CLEAR_RETRY_TIMEOUT_NS);
   simple_mtx_lock(&backing_records_mutex);
   for (uint32_t i = 0; i < BACKING_RECORDS; i++) {
      struct backing_record *record = &backing_records[i];
      if (!record->length)
         continue;
      void *const cpu = (void *)(uintptr_t)record->cpu_base;
      /* The steps and order of a release, without the GPU service, which
       * no longer has the span. The uncached attribute clears at once. */
      if (record->uncached) {
         if (R_FAILED(svcSetMemoryAttribute(cpu, record->length,
                                            BACKING_UNCACHED_ATTRIBUTE, 0))) {
            failures++;
            continue;
         }
         record->uncached = false;
      }
      /* The SVC 0x80 clear succeeds once the service has released the
       * span: retry it while it reports that, until the deadline. */
      Result rc = svcUnknown80(cpu, record->length, BACKING_PREPARE_BIT, 0);
      while (R_VALUE(rc) == BACKING_CLEAR_NOT_YET && armGetSystemTick() < deadline) {
         svcSleepThread(BACKING_CLEAR_RETRY_INTERVAL_NS);
         rc = svcUnknown80(cpu, record->length, BACKING_PREPARE_BIT, 0);
      }
      if (R_FAILED(rc)) {
         failures++;
         continue;
      }
      *record = (struct backing_record){0};
      restored++;
   }
   simple_mtx_unlock(&backing_records_mutex);
   if (failed)
      *failed = failures;
   return restored;
}

/* Registers a span. An imported span is not ours: it keeps its contents
 * and must be mode 1, because only the owner of the memory could change
 * its CPU attribute. */
static enum nvkmd_nvgpu_native_backing_status
backing_register(struct nvkmd_nvgpu_native_backing *backing,
                 struct nvkmd_nvgpu_native_runtime *runtime,
                 uint64_t cpu_base, uint64_t length, bool cpu_cached,
                 bool imported)
{
   if (!backing || !runtime || !cpu_base || !length ||
       (cpu_base & (BACKING_PAGE_B - 1)) || (length & (BACKING_PAGE_B - 1)) ||
       cpu_base >= BACKING_CPU_ADDRESS_LIMIT ||
       length > BACKING_CPU_ADDRESS_LIMIT - cpu_base)
      return NVKMD_NVGPU_BACKING_ARGUMENT;
   if (backing->self || backing->attempted ||
       !nvkmd_nvgpu_native_runtime_ready(runtime) ||
       !runtime->vm.client_registered ||
       runtime->vm.client >= BACKING_CLIENT_LIMIT)
      return NVKMD_NVGPU_BACKING_STATE;

   backing->self = backing;
   backing->attempted = true;
   backing->generation = runtime->generation;
   backing->cpu_base = cpu_base;
   backing->length = length;

   MemoryInfo info;
   if (!query(cpu_base, &info))
      return NVKMD_NVGPU_BACKING_QUERY;
   if (imported ? !importable_span(&info, cpu_base, length)
                : !heap_span(&info, cpu_base, length))
      return NVKMD_NVGPU_BACKING_PROFILE;

   if (!imported)
      memset((void *)(uintptr_t)cpu_base, 0, length);
   /* Mode 1 keeps the CPU caches on. Write the contents back to memory
    * before the GPU can see the span. */
   if (cpu_cached)
      nvkmd_nvgpu_native_dcache_clean_invalidate(cpu_base, length);
   Result rc = svcUnknown80((void *)(uintptr_t)cpu_base, length,
                            BACKING_PREPARE_BIT, BACKING_PREPARE_BIT);
   if (R_FAILED(rc))
      return fail(backing, NVKMD_NVGPU_BACKING_PREPARE, rc);
   backing->prepared = true;
   record_span(cpu_base, length);

   rc = gsvGpuRegisterMemory(&runtime->gpu, cpu_base, length,
                             runtime->vm.client, &backing->range_id);
   if (R_FAILED(rc) || !backing->range_id) {
      /* The service refused the span. Clear the SVC 0x80 bit again, so
       * the program gets its memory back as it came. */
      if (imported && R_FAILED(rc) &&
          R_SUCCEEDED(svcUnknown80((void *)(uintptr_t)cpu_base, length,
                                   BACKING_PREPARE_BIT, 0))) {
         forget_span(cpu_base);
         const Result refused = rc;
         memset(backing, 0, sizeof(*backing));
         backing->result = refused;
         return NVKMD_NVGPU_BACKING_REGISTER;
      }
      return fail(backing, NVKMD_NVGPU_BACKING_REGISTER, rc);
   }
   /* The range is registered from here, also when a check below fails: the
    * caller releases the backing. */
   backing->registered = true;

   if (!query(cpu_base, &info))
      return NVKMD_NVGPU_BACKING_QUERY;
   if (cpu_cached) {
      /* Mode 1 skips the attribute walk. The span must
       * still be CPU-cached. */
      if (info.attr & BACKING_UNCACHED_ATTRIBUTE)
         return NVKMD_NVGPU_BACKING_FINAL_ATTRIBUTES;
      backing->cpu_cached = true;
      backing->ready = true;
      return NVKMD_NVGPU_BACKING_OK;
   }
   if (info.attr != BACKING_UNCACHED_ATTRIBUTE) {
      rc = svcSetMemoryAttribute((void *)(uintptr_t)cpu_base, length,
                                 BACKING_UNCACHED_ATTRIBUTE,
                                 BACKING_UNCACHED_ATTRIBUTE);
      if (R_FAILED(rc))
         return fail(backing, NVKMD_NVGPU_BACKING_SET_ATTRIBUTES, rc);
   }
   backing->attribute_set = true;
   record_uncached(cpu_base);
   if (!query(cpu_base, &info))
      return NVKMD_NVGPU_BACKING_QUERY;
   if (!(info.attr & BACKING_UNCACHED_ATTRIBUTE))
      return NVKMD_NVGPU_BACKING_FINAL_ATTRIBUTES;
   /* In mode 2, the CPU attribute walk after registration sets bit 8 over
    * this whole span. Record it for the Vulkan memory policy. */
   backing->cpu_uncached = true;
   backing->ready = true;
   return NVKMD_NVGPU_BACKING_OK;
}

enum nvkmd_nvgpu_native_backing_status
nvkmd_nvgpu_native_backing_prepare(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t cpu_base, uint64_t length, bool cpu_cached)
{
   return backing_register(backing, runtime, cpu_base, length, cpu_cached,
                           false);
}

enum nvkmd_nvgpu_native_backing_status
nvkmd_nvgpu_native_backing_import(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t cpu_base, uint64_t length)
{
   return backing_register(backing, runtime, cpu_base, length, true, true);
}

enum nvkmd_nvgpu_native_backing_status
nvkmd_nvgpu_native_backing_release(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_slot *slot)
{
   if (!backing || !runtime || !slot)
      return NVKMD_NVGPU_BACKING_ARGUMENT;
   if (!nvkmd_nvgpu_native_backing_held(backing) || backing->maps ||
       backing->generation != runtime->generation || !slot->ready)
      return NVKMD_NVGPU_BACKING_STATE;

   /* No new map from here. Each step clears its flag when it succeeds, so
    * a call after a failure continues with the step that failed. */
   backing->ready = false;
   void *const cpu = (void *)(uintptr_t)backing->cpu_base;
   Result rc;

   if (backing->attribute_set) {
      rc = svcSetMemoryAttribute(cpu, backing->length,
                                 BACKING_UNCACHED_ATTRIBUTE, 0);
      if (R_FAILED(rc))
         return fail(backing, NVKMD_NVGPU_BACKING_SET_ATTRIBUTES, rc);
      backing->attribute_set = false;
   }

   /* An unmap or this range's own unregister may be pending on the slot;
    * its completion can end the registration. */
   if (backing->registered && !nvkmd_nvgpu_native_slot_settle(slot, &rc))
      return fail(backing, NVKMD_NVGPU_BACKING_SLOT, rc);
   if (backing->registered) {
      rc = gsvGpuUnregisterMemory(&runtime->gpu, backing->range_id,
                                  slot->index);
      const struct nvkmd_nvgpu_native_slot_pending request = {
         .request = NVKMD_NVGPU_NATIVE_SLOT_UNREGISTER,
         .backing = backing,
      };
      const enum nvkmd_nvgpu_native_slot_outcome outcome =
         nvkmd_nvgpu_native_slot_finish(slot, rc, &request, &rc);
      /* Refused: the range is still registered. */
      if (outcome == NVKMD_NVGPU_NATIVE_SLOT_REFUSED)
         return fail(backing, NVKMD_NVGPU_BACKING_UNREGISTER, rc);
      /* The slot completes the unregister before its next request. */
      if (outcome == NVKMD_NVGPU_NATIVE_SLOT_UNRESOLVED)
         return fail(backing, NVKMD_NVGPU_BACKING_WAIT, rc);
      backing->registered = false;
   }

   if (backing->prepared) {
      rc = svcUnknown80(cpu, backing->length, BACKING_PREPARE_BIT, 0);
      if (R_FAILED(rc))
         return fail(backing, NVKMD_NVGPU_BACKING_PREPARE, rc);
      backing->prepared = false;
      forget_span(backing->cpu_base);
   }

   memset(backing, 0, sizeof(*backing));
   return NVKMD_NVGPU_BACKING_OK;
}
