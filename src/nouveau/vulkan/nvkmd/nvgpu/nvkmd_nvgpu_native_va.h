/* SPDX-License-Identifier: MIT */

#ifndef NVKMD_NVGPU_NATIVE_VA_H
#define NVKMD_NVGPU_NATIVE_VA_H

#include "nvkmd_nvgpu_native_runtime.h"

enum nvkmd_nvgpu_native_va_status {
   NVKMD_NVGPU_NATIVE_VA_OK,
   NVKMD_NVGPU_NATIVE_VA_ARGUMENT,
   NVKMD_NVGPU_NATIVE_VA_STATE,
   NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED,
};

struct nvkmd_nvgpu_native_va_lease {
   const struct nvkmd_nvgpu_native_va_owner *owner;
   uint64_t runtime_generation;
   uint32_t vm_id, as_fd;
   uint64_t start, length; /* GPU VA only; one whole backing, end exclusive. */
   uint64_t serial;
};

/* Local GPU VA planning over the AS that the runtime imports. It uses the
 * first AS region below the syncpoint shim. It gives one lease, which is
 * never released. It allocates nothing remotely. Use it only on the thread
 * of the runtime. */
struct nvkmd_nvgpu_native_va_owner {
   const struct nvkmd_nvgpu_native_va_owner *self;
   struct nvkmd_nvgpu_native_runtime *runtime;
   uint64_t generation;
   uint32_t vm_id, as_fd;
   uint64_t start, end, shim_base; /* [start,end) excludes [shim_base,VM end). */
   bool leased;
};

/* Checks and accepts the AS geometry of the runtime (AS regions, VM user
 * window, shim). Then claims the runtime. The runtime must be ready. */
enum nvkmd_nvgpu_native_va_status nvkmd_nvgpu_native_va_admit(
   struct nvkmd_nvgpu_native_va_owner *owner,
   struct nvkmd_nvgpu_native_runtime *runtime);

struct nvkmd_nvgpu_native_va_result {
   enum nvkmd_nvgpu_native_va_status status;
   struct nvkmd_nvgpu_native_va_lease lease;
};

/* Gives one aligned lease for a whole backing. An invalid request changes
 * nothing. */
struct nvkmd_nvgpu_native_va_result nvkmd_nvgpu_native_va_lease_whole(
   struct nvkmd_nvgpu_native_va_owner *owner, uint64_t length,
   uint64_t alignment);

#endif
