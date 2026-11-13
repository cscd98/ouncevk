/* SPDX-License-Identifier: MIT */
/* The native game GPU services, on libounce-nx: the GSV1 root and its sync
 * and GPU (main + worker) sessions (switch2/services/gsv.h), and the
 * GSV-delegated nvservices session (services/nv.h). The bootstrap order is
 * root bootstrap, delegated nvservices, sync setup, frame mode, GPU remote
 * boot. One process-lifetime owner, opened and closed on one thread.
 */
#ifndef NVKMD_NVGPU_NATIVE_RUNTIME_H
#define NVKMD_NVGPU_NATIVE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <switch.h>

#include "nvkmd_nvgpu_native_profile.h"

/* Engine-catalog records that the 1024-byte output buffer holds. */
#define NVKMD_NVGPU_RUNTIME_ENGINE_RECORDS 64
#define NVKMD_NVGPU_RUNTIME_ENGINE_RECORD_B 16
/* Sizes of the VM user-range and AS-region tables that the services return. */
#define NVKMD_NVGPU_RUNTIME_VM_USER_RANGES 4
#define NVKMD_NVGPU_RUNTIME_AS_REGIONS 2
/* Main-session slot used for worker acquisition and GPU map completion. */
#define NVKMD_NVGPU_RUNTIME_WORKER_SLOT 0u

struct nvkmd_nvgpu_native_va_owner;

/* Progress through the bootstrap. Each value names the last completed phase. */
enum nvkmd_nvgpu_runtime_stage {
   NVKMD_NVGPU_RUNTIME_EMPTY,
   NVKMD_NVGPU_RUNTIME_ROOT_BOOTSTRAP_COMPLETE,
   NVKMD_NVGPU_RUNTIME_DELEGATE_ATTACHED,
   NVKMD_NVGPU_RUNTIME_CONTROL_QUERIED_CLOSED,
   NVKMD_NVGPU_RUNTIME_SYNC_CONFIGURED,
   NVKMD_NVGPU_RUNTIME_FRAME_COMPLETE,
   NVKMD_NVGPU_RUNTIME_MAIN_PHASE_COMPLETE,
   NVKMD_NVGPU_RUNTIME_WORKER_OWNED,
   NVKMD_NVGPU_RUNTIME_VM_IDENTITY_COMPLETE,
   NVKMD_NVGPU_RUNTIME_AS_IMPORT_RECEIVED,
   NVKMD_NVGPU_RUNTIME_AS_POST_RECEIVED,
   NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_RECEIVED,
   NVKMD_NVGPU_RUNTIME_GPU_FILE_OPEN_RECEIVED,
   NVKMD_NVGPU_RUNTIME_TSG_OPEN_RECEIVED,
};

enum nvkmd_nvgpu_runtime_status {
   NVKMD_NVGPU_RUNTIME_OK,
   NVKMD_NVGPU_RUNTIME_INVALID_ARGUMENT,
   NVKMD_NVGPU_RUNTIME_BAD_STATE,
   NVKMD_NVGPU_RUNTIME_WRONG_THREAD,
   NVKMD_NVGPU_RUNTIME_SERVICE_ERROR,
   NVKMD_NVGPU_RUNTIME_ROOT_ERROR,
   NVKMD_NVGPU_RUNTIME_DELEGATE_ERROR,
   NVKMD_NVGPU_RUNTIME_DRIVER_ERROR,
   NVKMD_NVGPU_RUNTIME_SYNC_ERROR,
   NVKMD_NVGPU_RUNTIME_UNSUPPORTED_POLICY,
   NVKMD_NVGPU_RUNTIME_FRAME_ERROR,
   NVKMD_NVGPU_RUNTIME_MAIN_ERROR,
   NVKMD_NVGPU_RUNTIME_WORKER_ERROR,
   NVKMD_NVGPU_RUNTIME_VM_ERROR,
   NVKMD_NVGPU_RUNTIME_AS_ERROR,
   NVKMD_NVGPU_RUNTIME_ENGINE_CATALOG_ERROR,
   NVKMD_NVGPU_RUNTIME_GPU_FILE_ERROR,
   NVKMD_NVGPU_RUNTIME_TSG_ERROR,
};

/* The type-0 engine selector and the classes the native profile reports. */
struct nvkmd_nvgpu_native_type0_candidate {
   bool found;
   uint64_t generation;
   uint32_t selector;
   uint32_t class_count;
   uint32_t classes[6]; /* Sorted static type0/instance0 membership. */
};

struct nvkmd_nvgpu_runtime_as_region {
   uint64_t base;
   uint32_t page_size;
   uint64_t page_count;
};

struct nvkmd_nvgpu_native_runtime {
   const struct nvkmd_nvgpu_native_runtime *self;
   uint64_t generation;
   void *tls;                       /* The opening thread's IPC buffer. */
   enum nvkmd_nvgpu_runtime_stage stage;
   enum nvkmd_nvgpu_runtime_status error;
   Result result;                   /* libounce-nx Result of a failed step. */

   bool sm_initialized;
   bool nv_initialized;             /* Also keeps GSV (and AM) open. */

   struct {
      uint16_t pointer_size;        /* Delegated session's pointer buffer. */
      bool characteristics_decoded;
      struct nvkmd_nvgpu_native_characteristics characteristics;
      struct nvkmd_nvgpu_native_resource_policy resource_policy;
      uint32_t tpc_mask;
      uint8_t topology[24];
   } delegate;
   uint8_t query_argument[NVKMD_NVGPU_NATIVE_CHARACTERISTICS_ARG_SIZE];
   uint8_t query_output[NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE];

   uint64_t allowed_core_mask, process_total_bytes, process_used_bytes;
   unsigned allowed_core_count;

   bool sync_open;
   GsvSync sync;
   uint64_t frame_permitted_mode;

   bool gpu_open;
   GsvGpu gpu;
   struct {
      uint32_t slot, lower, upper;
      bool event_open;
      Event event;                  /* The slot's completion event. */
      bool received;
   } worker;

   struct {
      bool client_registered;
      uint8_t client;
      uint32_t id;
      uint64_t user_count;
      GsvGpuVaRange user[NVKMD_NVGPU_RUNTIME_VM_USER_RANGES];
      GsvGpuVaRange gsv;
      bool registered;
   } vm;

   struct {
      bool open;
      uint32_t fd;
   } as;
   struct {
      bool shim_selected;
      uint64_t shim_gpu_va;
      uint32_t shim_stride, shim_count;
      uint32_t region_count;
      struct nvkmd_nvgpu_runtime_as_region region[NVKMD_NVGPU_RUNTIME_AS_REGIONS];
   } as_post;
   struct {
      bool open;
      uint32_t fd;                  /* /dev/nvhost-ctrl-gpu, kept for GPU time. */
      uint32_t record_count;
      uint8_t records[NVKMD_NVGPU_RUNTIME_ENGINE_RECORDS][NVKMD_NVGPU_RUNTIME_ENGINE_RECORD_B];
   } engine_catalog;
   struct {
      bool open;
      uint32_t fd;                  /* /dev/nvhost-gpu */
   } gpu_file;
   struct {
      bool open;
      uint32_t fd;                  /* /dev/nvhost-tsg-gpu */
   } tsg;

   /* One local VA planner claim after the AS is imported. */
   const struct nvkmd_nvgpu_native_va_owner *va_claim;
};

/* Runs the bootstrap on the calling thread, up to and including the GPU
 * channel file. On failure, error, stage and result describe the failed
 * step. The services that are already open stay open until
 * nvkmd_nvgpu_native_runtime_close. */
enum nvkmd_nvgpu_runtime_status
nvkmd_nvgpu_native_runtime_open(struct nvkmd_nvgpu_native_runtime *runtime,
                                uint64_t generation);

/* Opens the TSG file. Device creation calls it after the CPU backing is
 * registered. */
enum nvkmd_nvgpu_runtime_status
nvkmd_nvgpu_native_runtime_open_tsg(struct nvkmd_nvgpu_native_runtime *runtime);

/* Closes every service the runtime opened, in reverse order. The device
 * owner's maps, ranges and channel must be retired first. */
void
nvkmd_nvgpu_native_runtime_close(struct nvkmd_nvgpu_native_runtime *runtime);

/* GPU time in nanoseconds: ioctl 0xc010471c on the engine fd (routed to the
 * duplicate session by libounce-nx).
 */
uint32_t
nvkmd_nvgpu_native_runtime_read_gpu_time(
   const struct nvkmd_nvgpu_native_runtime *runtime, uint64_t *time_ns);

/* Returns the type-0 engine selector and class list. This is a pure
 * function. It sends no service request. */
struct nvkmd_nvgpu_native_type0_candidate
nvkmd_nvgpu_native_runtime_type0_candidate(
   const struct nvkmd_nvgpu_native_runtime *runtime);

/* Returns true when open succeeded up to the GPU file and the caller's
 * thread is the opening thread. */
static inline bool
nvkmd_nvgpu_native_runtime_ready(const struct nvkmd_nvgpu_native_runtime *runtime)
{
   return runtime && runtime->self == runtime &&
          runtime->error == NVKMD_NVGPU_RUNTIME_OK &&
          runtime->stage >= NVKMD_NVGPU_RUNTIME_GPU_FILE_OPEN_RECEIVED;
}

#endif
