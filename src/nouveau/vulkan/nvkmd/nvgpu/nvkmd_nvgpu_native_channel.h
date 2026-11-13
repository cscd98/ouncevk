/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_CHANNEL_H
#define NVKMD_NVGPU_NATIVE_CHANNEL_H

#include "nvkmd_nvgpu_native_runtime.h"

#include <stdint.h>

enum nvkmd_nvgpu_native_channel_status {
   NVKMD_NVGPU_CHANNEL_OK,
   NVKMD_NVGPU_CHANNEL_ARGUMENT,
   NVKMD_NVGPU_CHANNEL_STATE,
   NVKMD_NVGPU_CHANNEL_PROFILE,
   NVKMD_NVGPU_CHANNEL_CATALOG,
   NVKMD_NVGPU_CHANNEL_ALLOCATE,
   NVKMD_NVGPU_CHANNEL_AS_BIND,
   NVKMD_NVGPU_CHANNEL_TSG_BIND,
};

/* The runtime's GPU file as a channel bound to its AS and TSG. Keep it at
 * a stable address while the channel is open. */
struct nvkmd_nvgpu_native_channel {
   const struct nvkmd_nvgpu_native_channel *self;
   uint64_t generation;
   uint32_t selector;
   uint32_t channel_fd, as_fd, tsg_fd;
   uint32_t as_payload;
   uint32_t tsg_payload[6];
   Result result;
   bool attempted, allocated, as_bound, tsg_bound;
};

/* Allocates the channel on the type-0 engine (GPU fd 0x40044820), binds it
 * to the AS (AS fd 0x40044101) and to the TSG (TSG fd 0xc018540b). Requires
 * the runtime at TSG_OPEN_RECEIVED. */
enum nvkmd_nvgpu_native_channel_status nvkmd_nvgpu_native_channel_open(
   struct nvkmd_nvgpu_native_channel *channel,
   const struct nvkmd_nvgpu_native_runtime *runtime);

/* As nvkmd_nvgpu_native_channel_open, on an engine selector of the engine
 * catalog, with a GPU file and a TSG file that the caller opened (one
 * channel and one TSG for each queue). */
enum nvkmd_nvgpu_native_channel_status nvkmd_nvgpu_native_channel_open_on(
   struct nvkmd_nvgpu_native_channel *channel,
   const struct nvkmd_nvgpu_native_runtime *runtime,
   uint32_t selector, uint32_t gpu_fd, uint32_t tsg_fd);

#endif
