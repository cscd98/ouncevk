/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_SUBMIT_H
#define NVKMD_NVGPU_NATIVE_SUBMIT_H

#include "nvkmd_nvgpu_native_batch.h"
#include "nvkmd_nvgpu_gpu_endpoint.h"

enum nvkmd_nvgpu_native_submit_status {
   NVKMD_NVGPU_SUBMIT_OK,
   NVKMD_NVGPU_SUBMIT_ARGUMENT,
   NVKMD_NVGPU_SUBMIT_STATE,
   NVKMD_NVGPU_SUBMIT_ENTRY,
   NVKMD_NVGPU_SUBMIT_REPLY,
   NVKMD_NVGPU_SUBMIT_RESULT,
};

/* The 24-byte control and the entry array stay allocated until the GPU
 * command completes. A successful reply alone does not confirm completion or
 * allow storage reuse. */
struct nvkmd_nvgpu_native_submit {
   const struct nvkmd_nvgpu_native_submit *self;
   uint64_t generation;
   _Alignas(8) uint8_t control[24];
   _Alignas(8) uint8_t entry[NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES * 8];
   uint32_t returned_id;
   uint32_t returned_target;
   Result result;
   /* attempted is set before local encoding. send_started marks the first
    * possible remote effect. Only a failure before send_started can reuse
    * this submission's GPU storage without confirmed completion. */
   bool attempted, send_started, accepted;
};

/* Submits entries on the endpoint's channel. It uses GPU fd ioctl 0xc018481b
 * with the entry array as the extra input (nvIoctl2). It runs after a full
 * barrier so the GPU sees the command words. */
enum nvkmd_nvgpu_native_submit_status
nvkmd_nvgpu_native_submit_once_endpoint(
   struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_batch_entry *entries,
   uint32_t entry_count,
   uint32_t span_flags,
   uint32_t submit_flags);

#endif
