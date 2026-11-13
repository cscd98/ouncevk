/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_TERMINAL_H
#define NVKMD_NVGPU_NATIVE_TERMINAL_H

#include "nvkmd_nvgpu_native_arena.h"
#include "nvkmd_nvgpu_native_completion.h"

enum nvkmd_nvgpu_native_terminal_status {
   NVKMD_NVGPU_TERMINAL_OK,
   NVKMD_NVGPU_TERMINAL_ARGUMENT,
   NVKMD_NVGPU_TERMINAL_STATE,
   NVKMD_NVGPU_TERMINAL_ARENA,
   NVKMD_NVGPU_TERMINAL_PACKET,
   NVKMD_NVGPU_TERMINAL_BASELINE,
   NVKMD_NVGPU_TERMINAL_SUBMIT,
   NVKMD_NVGPU_TERMINAL_COMPLETION,
   NVKMD_NVGPU_TERMINAL_RELEASE,
};

struct nvkmd_nvgpu_native_terminal_io {
   void *context;
   bool (*wait_target)(void *context, uint32_t producer_id, uint32_t target);
};

/* A single fresh marker submitted after all other same-channel work. Its
 * command, GPU-written marker and guards stay mapped until producer, marker
 * and health all join. The caller blocks new submissions. It has already
 * closed software contexts and display-image users. A failure poisons the
 * arena and must block channel close. */
struct nvkmd_nvgpu_native_terminal {
   const struct nvkmd_nvgpu_native_terminal *self;
   struct nvkmd_nvgpu_native_slice slice;
   struct nvkmd_nvgpu_native_batch_entry entry;
   struct nvkmd_nvgpu_native_submit submit;
   struct nvkmd_nvgpu_native_completion completion;
   volatile uint32_t *guard_before;
   volatile uint32_t *marker;
   volatile uint32_t *guard_after;
   uint32_t marker_value;
   enum nvkmd_nvgpu_native_terminal_status status;
   bool attempted;
   bool complete;
};

enum nvkmd_nvgpu_native_terminal_status
nvkmd_nvgpu_native_terminal_drain(
   struct nvkmd_nvgpu_native_terminal *terminal,
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_terminal_io *io,
   bool quiescent);

/* As nvkmd_nvgpu_native_terminal_drain, at program exit with objects the
 * program never destroyed. Submissions may still be in flight and the arena
 * may hold live allocations. The marker follows them in channel order. So its
 * completion still covers all earlier work. The leaked objects are never used
 * again. */
enum nvkmd_nvgpu_native_terminal_status
nvkmd_nvgpu_native_terminal_drain_at_exit(
   struct nvkmd_nvgpu_native_terminal *terminal,
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_terminal_io *io);

#endif
