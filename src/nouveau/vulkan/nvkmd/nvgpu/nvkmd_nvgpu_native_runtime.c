/* SPDX-License-Identifier: MIT */
/* OunceVK's native game GPU services on libounce-nx. See
 * nvkmd_nvgpu_native_runtime.h.
 */
#include "nvkmd_nvgpu_native_runtime.h"

#include <string.h>

/* Device files of the delegated nvservices session.
 */
static const char native_ctrl_gpu_path[] = "/dev/nvhost-ctrl-gpu";
static const char native_as_gpu_path[] = "/dev/nvhost-as-gpu";
static const char native_gpu_path[] = "/dev/nvhost-gpu";
static const char native_tsg_gpu_path[] = "/dev/nvhost-tsg-gpu";

/* /dev/nvhost-ctrl-gpu ioctls.
 */
#define NATIVE_IOCTL_CTRL_GPU_TPC_MASKS      UINT32_C(0xc0184706) /* arg 24, out 4 */
#define NATIVE_IOCTL_CTRL_GPU_TOPOLOGY       UINT32_C(0xc0084713) /* arg 8, out 24 */
#define NATIVE_IOCTL_CTRL_GPU_ENGINE_CATALOG UINT32_C(0xc010471f) /* header 16, out 1024 */
#define NATIVE_IOCTL_CTRL_GPU_TIME           UINT32_C(0xc010471c) /* in/out 16 */
/* /dev/nvhost-as-gpu ioctls. The import inserts the VM ID at +4.
 */
#define NATIVE_IOCTL_AS_IMPORT  UINT32_C(0x40284109) /* in 40 */
#define NATIVE_IOCTL_AS_SHIM    UINT32_C(0x8010410b) /* out 16 */
#define NATIVE_IOCTL_AS_REGIONS UINT32_C(0xc0404108) /* in/out 64, records */

enum {
   NATIVE_TPC_MASK_BUFFER_B = 4,      /* masks argument +0 */
   NATIVE_TOPOLOGY_OUTPUT_B = 24,
   NATIVE_TOPOLOGY_PREFILL = 0xc0,    /* the output prefill byte */
   NATIVE_AS_IMPORT_ARGUMENT_B = 40,
   NATIVE_AS_IMPORT_VERSION = 5,      /* argument +0 */
   NATIVE_AS_SHIM_B = 16,
   NATIVE_AS_REGION_HEADER_B = 64,
   NATIVE_AS_REGION_RECORD_B = 24,
   NATIVE_ENGINE_HEADER_B = 16,
   NATIVE_GPU_TIME_B = 16,
   /* Frame mode registered at game startup.
    */
   NATIVE_FRAME_MODE = 2,
};

/* The AS imported for a game: user interval [0x08000000, 0xc000000000).
 */
#define NATIVE_AS_USER_FIRST UINT64_C(0x08000000)
#define NATIVE_AS_USER_LAST  UINT64_C(0xbfffffffff)
/* GPU client name, "nxvk" (the driver's former name) zero-padded to 8 bytes
 * (main raw 0x39). */
#define NATIVE_CLIENT_NAME UINT64_C(0x6b76786e)
/* Characteristics query fields that this policy reads. Q is the 0xd8-byte
 * output. */
#define NATIVE_Q_PROFILE_ID        0x00 /* u32 pair ORed: 0x17e native profile */
#define NATIVE_Q_PAGE_OVERRIDE     0x08
#define NATIVE_Q_BIG_PAGE_MASK_LO  0x28
#define NATIVE_Q_BIG_PAGE_MASK_HI  0x34
#define NATIVE_Q_FLAGS             0x50 /* u64, bit 26: AS syncpoint shim */
#define NATIVE_Q_REVISION          0x78 /* u16 */
#define NATIVE_PROFILE_ID          UINT32_C(0x17e)

static uint32_t
read32(const uint8_t *p)
{
   uint32_t value;
   memcpy(&value, p, sizeof(value));
   return value;
}

static uint64_t
read64(const uint8_t *p)
{
   uint64_t value;
   memcpy(&value, p, sizeof(value));
   return value;
}

static void
write32(uint8_t *p, uint32_t value)
{
   memcpy(p, &value, sizeof(value));
}

static void
write64(uint8_t *p, uint64_t value)
{
   memcpy(p, &value, sizeof(value));
}

static enum nvkmd_nvgpu_runtime_status
fail(struct nvkmd_nvgpu_native_runtime *r,
     enum nvkmd_nvgpu_runtime_status status, Result rc)
{
   r->error = status;
   r->result = rc;
   return status;
}

static uint32_t
characteristics_revision(const struct nvkmd_nvgpu_native_runtime *r)
{
   return (uint32_t)r->query_output[NATIVE_Q_REVISION] |
          ((uint32_t)r->query_output[NATIVE_Q_REVISION + 1] << 8);
}

/* nvhost-ctrl-gpu queries on a control fd that is closed afterwards. The
 * native profile screen and version floor are OunceVK policy: later code assumes
 * model 0x808.
 */
static enum nvkmd_nvgpu_runtime_status
query_control(struct nvkmd_nvgpu_native_runtime *r)
{
   u32 fd = 0;
   Result rc = nvOpen(&fd, native_ctrl_gpu_path);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_DRIVER_ERROR, rc);

   nvkmd_nvgpu_native_characteristics_init_arg(r->query_argument,
                                               sizeof(r->query_argument));
   rc = nvIoctl3(fd, NVKMD_NVGPU_NATIVE_CHARACTERISTICS_IOCTL, r->query_argument,
                 r->query_output, sizeof(r->query_output));

   uint8_t masks_argument[24] = { NATIVE_TPC_MASK_BUFFER_B };
   uint32_t tpc_mask = 0;
   if (R_SUCCEEDED(rc))
      rc = nvIoctl3(fd, NATIVE_IOCTL_CTRL_GPU_TPC_MASKS, masks_argument,
                    &tpc_mask, sizeof(tpc_mask));

   uint8_t topology_argument[8] = {0};
   memset(r->delegate.topology, NATIVE_TOPOLOGY_PREFILL, NATIVE_TOPOLOGY_OUTPUT_B);
   if (R_SUCCEEDED(rc))
      rc = nvIoctl3(fd, NATIVE_IOCTL_CTRL_GPU_TOPOLOGY, topology_argument,
                    r->delegate.topology, NATIVE_TOPOLOGY_OUTPUT_B);

   const Result close_rc = nvClose(fd);
   if (R_SUCCEEDED(rc))
      rc = close_rc;
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_DRIVER_ERROR, rc);
   r->delegate.tpc_mask = tpc_mask;

   struct nvkmd_nvgpu_native_characteristics decoded;
   if (nvkmd_nvgpu_native_characteristics_decode(
          (struct nvkmd_nvgpu_native_query_results){0},
          r->query_argument, sizeof(r->query_argument),
          r->query_output, sizeof(r->query_output), &decoded) !=
          NVKMD_NVGPU_NATIVE_DECODED ||
       nvkmd_nvgpu_native_profile_screen(&decoded) !=
          NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED ||
       decoded.versions[0] < 19)
      return fail(r, NVKMD_NVGPU_RUNTIME_UNSUPPORTED_POLICY, 0);
   r->delegate.characteristics = decoded;
   r->delegate.characteristics_decoded = true;
   nvkmd_nvgpu_native_resource_policy_decode(&decoded, &r->delegate.resource_policy);
   r->stage = NVKMD_NVGPU_RUNTIME_CONTROL_QUERIED_CLOSED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* Reads process information (GetInfo 0, 21, 22) for diagnostics only. */
static void
observe_process_resources(struct nvkmd_nvgpu_native_runtime *r)
{
   u64 value = 0;
   if (R_SUCCEEDED(svcGetInfo(&value, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
      r->allowed_core_mask = value;
      r->allowed_core_count = (unsigned)__builtin_popcountll(value);
   }
   if (R_SUCCEEDED(svcGetInfo(&value, InfoType_TotalNonSystemMemorySize,
                              CUR_PROCESS_HANDLE, 0)))
      r->process_total_bytes = value;
   if (R_SUCCEEDED(svcGetInfo(&value, InfoType_UsedNonSystemMemorySize,
                              CUR_PROCESS_HANDLE, 0)))
      r->process_used_bytes = value;
}

/* Main-session worker acquisition (main raw 0x1b/0x1c/0x25/0x26). The slot
 * event stays open: the service signals it for every later GPU map and range
 * retirement on this slot.
 */
static enum nvkmd_nvgpu_runtime_status
acquire_worker(struct nvkmd_nvgpu_native_runtime *r)
{
   r->worker.slot = NVKMD_NVGPU_RUNTIME_WORKER_SLOT;
   Result rc = gsvGpuGetSlotRange(&r->gpu, &r->worker.lower, &r->worker.upper);
   if (R_SUCCEEDED(rc) &&
       (r->worker.slot < r->worker.lower || r->worker.slot > r->worker.upper))
      return fail(r, NVKMD_NVGPU_RUNTIME_WORKER_ERROR, 0);

   if (R_SUCCEEDED(rc)) {
      rc = gsvGpuCreateSlotEvent(&r->gpu, r->worker.slot, &r->worker.event, false);
      r->worker.event_open = R_SUCCEEDED(rc);
   }
   if (R_SUCCEEDED(rc))
      rc = gsvGpuOpenWorker(&r->gpu, r->worker.slot, &r->worker.event);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_WORKER_ERROR, rc);
   r->worker.received = true;
   r->stage = NVKMD_NVGPU_RUNTIME_WORKER_OWNED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* GPU client, VM, its ranges, and the root's VM registration (main raw
 * 0x39/0x31/0xdb/0xda, root raw 0x1c2).
 */
static enum nvkmd_nvgpu_runtime_status
acquire_vm(struct nvkmd_nvgpu_native_runtime *r)
{
   Result rc = gsvGpuRegisterClient(&r->gpu, NATIVE_CLIENT_NAME, &r->vm.client);
   r->vm.client_registered = R_SUCCEEDED(rc);

   if (R_SUCCEEDED(rc))
      rc = gsvGpuCreateVm(&r->gpu, &r->vm.id);

   u32 user_count = 0;
   if (R_SUCCEEDED(rc))
      rc = gsvGpuGetVmUserRanges(&r->gpu, r->vm.id, r->vm.user, &user_count);
   r->vm.user_count = user_count;

   if (R_SUCCEEDED(rc))
      rc = gsvGpuGetVmGsvRange(&r->gpu, r->vm.id, &r->vm.gsv);

   if (R_SUCCEEDED(rc))
      rc = gsvRegisterVm(r->vm.id);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_VM_ERROR, rc);
   r->vm.registered = true;
   r->stage = NVKMD_NVGPU_RUNTIME_VM_IDENTITY_COMPLETE;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* The game AS import. The profile and user-range checks are OunceVK's narrow
 * policy for the one AS layout it supports.
 */
static enum nvkmd_nvgpu_runtime_status
import_as(struct nvkmd_nvgpu_native_runtime *r)
{
   const uint32_t revision = characteristics_revision(r);
   if ((read32(r->query_output + NATIVE_Q_PROFILE_ID) |
        read32(r->query_output + NATIVE_Q_PROFILE_ID + 4)) != NATIVE_PROFILE_ID ||
       revision < 9 || revision >= 0x8000 ||
       read32(r->query_output + NATIVE_Q_PAGE_OVERRIDE) >= 0x100 ||
       read32(r->query_output + NATIVE_Q_BIG_PAGE_MASK_LO) ||
       read32(r->query_output + NATIVE_Q_BIG_PAGE_MASK_HI) ||
       r->vm.user_count != 1 || r->vm.user[0].first != NATIVE_AS_USER_FIRST ||
       r->vm.user[0].last != NATIVE_AS_USER_LAST)
      return fail(r, NVKMD_NVGPU_RUNTIME_UNSUPPORTED_POLICY, 0);

   Result rc = nvOpen(&r->as.fd, native_as_gpu_path);
   r->as.open = R_SUCCEEDED(rc);

   uint8_t argument[NATIVE_AS_IMPORT_ARGUMENT_B] = {0};
   write32(argument, NATIVE_AS_IMPORT_VERSION);
   write32(argument + 4, r->vm.id);
   write64(argument + 16, NATIVE_AS_USER_FIRST);
   write64(argument + 24, NATIVE_AS_USER_LAST + 1); /* exclusive end */
   if (R_SUCCEEDED(rc))
      rc = nvIoctl(r->as.fd, NATIVE_IOCTL_AS_IMPORT, argument);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_AS_ERROR, rc);
   r->stage = NVKMD_NVGPU_RUNTIME_AS_IMPORT_RECEIVED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* After import: the syncpoint shim (revision >= 11 with flag bit 26), then
 * the VA regions (a size probe, then the records). One or two 24-byte region
 * records is the supported service profile.
 */
static enum nvkmd_nvgpu_runtime_status
query_as_post(struct nvkmd_nvgpu_native_runtime *r)
{
   r->as_post.shim_selected = characteristics_revision(r) >= 11 &&
      (read64(r->query_output + NATIVE_Q_FLAGS) & (UINT64_C(1) << 26));

   Result rc = 0;
   if (r->as_post.shim_selected) {
      uint8_t shim[NATIVE_AS_SHIM_B] = {0};
      rc = nvIoctl(r->as.fd, NATIVE_IOCTL_AS_SHIM, shim);
      if (R_FAILED(rc))
         return fail(r, NVKMD_NVGPU_RUNTIME_AS_ERROR, rc);
      r->as_post.shim_gpu_va = read64(shim);
      r->as_post.shim_stride = read32(shim + 8);
      r->as_post.shim_count = read32(shim + 12);
      /* A signed-negation representation check. */
      if (r->as_post.shim_stride > UINT32_C(0x80000000))
         return fail(r, NVKMD_NVGPU_RUNTIME_UNSUPPORTED_POLICY, 0);
   }

   uint8_t header[NATIVE_AS_REGION_HEADER_B] = {0};
   rc = nvIoctl(r->as.fd, NATIVE_IOCTL_AS_REGIONS, header);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_AS_ERROR, rc);
   const uint32_t required = read32(header + 8);
   if (required != NATIVE_AS_REGION_RECORD_B &&
       required != 2 * NATIVE_AS_REGION_RECORD_B)
      return fail(r, NVKMD_NVGPU_RUNTIME_UNSUPPORTED_POLICY, 0);

   uint8_t records[2 * NATIVE_AS_REGION_RECORD_B] = {0};
   memset(header, 0, sizeof(header));
   write32(header + 8, required);
   rc = nvIoctl3(r->as.fd, NATIVE_IOCTL_AS_REGIONS, header, records, required);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_AS_ERROR, rc);
   if (read32(header + 8) != required)
      return fail(r, NVKMD_NVGPU_RUNTIME_AS_ERROR, 0);

   r->as_post.region_count = required / NATIVE_AS_REGION_RECORD_B;
   for (uint32_t i = 0; i < r->as_post.region_count; i++) {
      const uint8_t *record = records + NATIVE_AS_REGION_RECORD_B * i;
      r->as_post.region[i].base = read64(record);
      r->as_post.region[i].page_size = read32(record + 8);
      r->as_post.region[i].page_count = read64(record + 16);
   }
   r->stage = NVKMD_NVGPU_RUNTIME_AS_POST_RECEIVED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* Opens a second control fd that stays open. It reads the engine catalog,
 * and later the GPU time. */
static enum nvkmd_nvgpu_runtime_status
query_engine_catalog(struct nvkmd_nvgpu_native_runtime *r)
{
   Result rc = nvOpen(&r->engine_catalog.fd, native_ctrl_gpu_path);
   r->engine_catalog.open = R_SUCCEEDED(rc);

   uint8_t header[NATIVE_ENGINE_HEADER_B] = {0};
   if (R_SUCCEEDED(rc))
      rc = nvIoctl3(r->engine_catalog.fd, NATIVE_IOCTL_CTRL_GPU_ENGINE_CATALOG,
                    header, r->engine_catalog.records,
                    sizeof(r->engine_catalog.records));
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_ERROR, rc);

   const uint32_t required = read32(header);
   if (required > sizeof(r->engine_catalog.records) ||
       (required % NVKMD_NVGPU_RUNTIME_ENGINE_RECORD_B))
      return fail(r, NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_ERROR, 0);
   r->engine_catalog.record_count = required / NVKMD_NVGPU_RUNTIME_ENGINE_RECORD_B;
   r->stage = NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_RECEIVED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

enum nvkmd_nvgpu_runtime_status
nvkmd_nvgpu_native_runtime_open(struct nvkmd_nvgpu_native_runtime *r,
                                uint64_t generation)
{
   if (!r || !generation)
      return NVKMD_NVGPU_RUNTIME_INVALID_ARGUMENT;
   if (r->self)
      return NVKMD_NVGPU_RUNTIME_BAD_STATE;
   memset(r, 0, sizeof(*r));
   r->self = r;
   r->generation = generation;
   r->tls = armGetTls();

   Result rc = smInitialize();
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_SERVICE_ERROR, rc);
   r->sm_initialized = true;

   /* This opens GSV (AM, then the root bootstrap), then the delegated
    * nvservices session with its clone and feature mask. */
   rc = nvInitialize();
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_DELEGATE_ERROR, rc);
   r->nv_initialized = true;
   r->delegate.pointer_size = (uint16_t)nvGetServiceSession()->pointer_buffer_size;
   r->stage = NVKMD_NVGPU_RUNTIME_DELEGATE_ATTACHED;

   enum nvkmd_nvgpu_runtime_status status = query_control(r);
   if (status != NVKMD_NVGPU_RUNTIME_OK)
      return status;
   observe_process_resources(r);

   rc = gsvOpenSync(&r->sync);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_SYNC_ERROR, rc);
   r->sync_open = true;
   r->stage = NVKMD_NVGPU_RUNTIME_SYNC_CONFIGURED;

   rc = gsvRegisterFrame(NATIVE_FRAME_MODE, &r->frame_permitted_mode);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_FRAME_ERROR, rc);
   r->stage = NVKMD_NVGPU_RUNTIME_FRAME_COMPLETE;

   rc = gsvOpenGpu(&r->gpu);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_MAIN_ERROR, rc);
   r->gpu_open = true;
   r->stage = NVKMD_NVGPU_RUNTIME_MAIN_PHASE_COMPLETE;

   status = acquire_worker(r);
   if (status == NVKMD_NVGPU_RUNTIME_OK)
      status = acquire_vm(r);
   if (status == NVKMD_NVGPU_RUNTIME_OK)
      status = import_as(r);
   if (status == NVKMD_NVGPU_RUNTIME_OK)
      status = query_as_post(r);
   if (status == NVKMD_NVGPU_RUNTIME_OK)
      status = query_engine_catalog(r);
   if (status != NVKMD_NVGPU_RUNTIME_OK)
      return status;

   rc = nvOpen(&r->gpu_file.fd, native_gpu_path);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_GPU_FILE_ERROR, rc);
   r->gpu_file.open = true;
   r->stage = NVKMD_NVGPU_RUNTIME_GPU_FILE_OPEN_RECEIVED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* TSG open.
 */
enum nvkmd_nvgpu_runtime_status
nvkmd_nvgpu_native_runtime_open_tsg(struct nvkmd_nvgpu_native_runtime *r)
{
   if (!nvkmd_nvgpu_native_runtime_ready(r) || r->tsg.open)
      return NVKMD_NVGPU_RUNTIME_BAD_STATE;
   if (armGetTls() != r->tls)
      return NVKMD_NVGPU_RUNTIME_WRONG_THREAD;
   const Result rc = nvOpen(&r->tsg.fd, native_tsg_gpu_path);
   if (R_FAILED(rc))
      return fail(r, NVKMD_NVGPU_RUNTIME_TSG_ERROR, rc);
   r->tsg.open = true;
   r->stage = NVKMD_NVGPU_RUNTIME_TSG_OPEN_RECEIVED;
   return NVKMD_NVGPU_RUNTIME_OK;
}

/* The reverse of the bootstrap, in the order OunceVK's earlier per-object
 * retirement used: GPU and TSG files, AS fd, client, engine fd, sync session,
 * worker session, its slot event, main session, then nvservices (clone,
 * primary), GSV (root, AM) and SM. The display child is closed by its own
 * owner before this.
 */
void
nvkmd_nvgpu_native_runtime_close(struct nvkmd_nvgpu_native_runtime *r)
{
   if (!r || r->self != r)
      return;
   /* The channel (GPU file) closes before its TSG.
    */
   if (r->gpu_file.open) {
      nvClose(r->gpu_file.fd);
      r->gpu_file.open = false;
   }
   if (r->tsg.open) {
      nvClose(r->tsg.fd);
      r->tsg.open = false;
   }
   if (r->as.open) {
      nvClose(r->as.fd);
      r->as.open = false;
   }
   if (r->vm.client_registered) {
      gsvGpuReleaseClient(&r->gpu, r->vm.client);
      r->vm.client_registered = false;
   }
   if (r->engine_catalog.open) {
      nvClose(r->engine_catalog.fd);
      r->engine_catalog.open = false;
   }
   if (r->sync_open) {
      gsvSyncClose(&r->sync);
      r->sync_open = false;
   }
   if (r->gpu_open && r->gpu.worker.session)
      tipcClose(&r->gpu.worker);
   if (r->worker.event_open) {
      eventClose(&r->worker.event);
      r->worker.event_open = false;
   }
   if (r->gpu_open) {
      gsvGpuClose(&r->gpu);
      r->gpu_open = false;
   }
   if (r->nv_initialized) {
      nvExit();
      r->nv_initialized = false;
   }
   if (r->sm_initialized) {
      smExit();
      r->sm_initialized = false;
   }
   r->self = NULL;
}

uint32_t
nvkmd_nvgpu_native_runtime_read_gpu_time(
   const struct nvkmd_nvgpu_native_runtime *r, uint64_t *time_ns)
{
   if (!time_ns || !nvkmd_nvgpu_native_runtime_ready(r) || !r->engine_catalog.open)
      return UINT32_MAX;
   uint8_t argument[NATIVE_GPU_TIME_B] = {0};
   const Result rc = nvIoctl(r->engine_catalog.fd, NATIVE_IOCTL_CTRL_GPU_TIME, argument);
   if (R_SUCCEEDED(rc))
      *time_ns = read64(argument);
   return rc;
}

/* Static projection of the type-0 group for the native profile.
 * C365/C369 are always present. Flags 40 and 43
 * add 902D and A140. The version and flag gates require C797/C7C0. */
struct nvkmd_nvgpu_native_type0_candidate
nvkmd_nvgpu_native_runtime_type0_candidate(const struct nvkmd_nvgpu_native_runtime *r)
{
   const struct nvkmd_nvgpu_native_type0_candidate absent = {0};
   if (!r || r->self != r || r->error != NVKMD_NVGPU_RUNTIME_OK ||
       r->stage < NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_RECEIVED ||
       !r->delegate.characteristics_decoded)
      return absent;

   const struct nvkmd_nvgpu_native_characteristics *profile = &r->delegate.characteristics;
   if (nvkmd_nvgpu_native_profile_screen(profile) != NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED ||
       profile->versions[0] < 31 || profile->versions[3] < 32 ||
       !(profile->flags & (UINT64_C(1) << 41)) ||
       !(profile->flags & (UINT64_C(1) << 42)))
      return absent;

   /* Find the selector of the record with type 0, instance 0. */
   uint32_t selector = UINT32_MAX;
   for (uint32_t i = 0; i < r->engine_catalog.record_count; i++) {
      const uint8_t *record = r->engine_catalog.records[i];
      if (read32(record) == 0 && read32(record + 4) == 0)
         selector = read32(record + 8);
   }
   if (selector > INT32_MAX)
      return absent;

   struct nvkmd_nvgpu_native_type0_candidate candidate = {
      .found = true, .generation = r->generation, .selector = selector,
   };
   if (profile->flags & (UINT64_C(1) << 40))
      candidate.classes[candidate.class_count++] = 0x902d;
   if (profile->flags & (UINT64_C(1) << 43))
      candidate.classes[candidate.class_count++] = 0xa140;
   candidate.classes[candidate.class_count++] = 0xc365;
   candidate.classes[candidate.class_count++] = 0xc369;
   candidate.classes[candidate.class_count++] = 0xc797;
   candidate.classes[candidate.class_count++] = 0xc7c0;
   return candidate;
}
