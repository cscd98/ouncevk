/* SPDX-License-Identifier: MIT */
/* OunceVK terminal owner. It uses the C797 marker, submit and checked completion
 * join.
 */
#include "nvkmd_nvgpu_native_terminal.h"

#include "nvkmd_nvgpu_semaphore_probe.h"

#include <stddef.h>
#include <stdint.h>

enum {
   TERMINAL_ALIGN_B = 4096,
   TERMINAL_SPAN_FLAGS = 1,
   TERMINAL_SUBMIT_FLAGS = 6,
   TERMINAL_GUARD_BEFORE = 0xb16b00b5,
   TERMINAL_GUARD_AFTER = 0xa71ecafe,
   TERMINAL_MARKER_VALUE = 0xb6e90001,
};

struct terminal_call {
   struct nvkmd_nvgpu_native_terminal *terminal;
   const struct nvkmd_nvgpu_native_terminal_io *io;
};

static enum nvkmd_nvgpu_native_event_result
terminal_wait_target(void *context, uint32_t producer_id, uint32_t target)
{
   const struct terminal_call *call = context;
   return call->io->wait_target(call->io->context, producer_id, target) ?
      NVKMD_NVGPU_EVENT_WAKE : NVKMD_NVGPU_EVENT_ERROR;
}

static bool
terminal_verify(void *context)
{
   const struct terminal_call *call = context;
   const struct nvkmd_nvgpu_native_terminal *terminal = call->terminal;
   return terminal->guard_before && terminal->marker && terminal->guard_after &&
          *terminal->guard_before == TERMINAL_GUARD_BEFORE &&
          *terminal->marker == terminal->marker_value &&
          *terminal->guard_after == TERMINAL_GUARD_AFTER;
}

enum nvkmd_nvgpu_native_terminal_status
nvkmd_nvgpu_native_terminal_drain(
   struct nvkmd_nvgpu_native_terminal *terminal,
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_terminal_io *io,
   bool quiescent)
{
   if (!quiescent || (arena && arena->live_records))
      return NVKMD_NVGPU_TERMINAL_STATE;
   return nvkmd_nvgpu_native_terminal_drain_at_exit(terminal, arena,
                                                    endpoint, io);
}

enum nvkmd_nvgpu_native_terminal_status
nvkmd_nvgpu_native_terminal_drain_at_exit(
   struct nvkmd_nvgpu_native_terminal *terminal,
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_terminal_io *io)
{
   if (!terminal || !arena || !endpoint || !io || !io->wait_target)
      return NVKMD_NVGPU_TERMINAL_ARGUMENT;
   if (terminal->self || terminal->attempted ||
       arena->self != arena || arena->generation != endpoint->generation ||
       arena->recycle_poisoned ||
       !nvkmd_nvgpu_gpu_endpoint_valid(endpoint))
      return NVKMD_NVGPU_TERMINAL_STATE;

   terminal->self = terminal;
   terminal->attempted = true;
   const uint32_t packet_bytes = NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS * 4;
   const uint32_t result_bytes = 3 * 4;
   if (nvkmd_nvgpu_native_arena_alloc_reclaimable(
          arena, packet_bytes + result_bytes, TERMINAL_ALIGN_B,
          &terminal->slice) != NVKMD_NVGPU_NATIVE_ARENA_OK)
      goto fail_arena;

   uint32_t *const packet = (uint32_t *)(uintptr_t)terminal->slice.cpu;
   const uint64_t marker_gpu = terminal->slice.gpu + packet_bytes + 4;
   terminal->marker_value = TERMINAL_MARKER_VALUE;
   if (!nvkmd_nvgpu_semaphore_probe_encode(packet, marker_gpu,
                                           terminal->marker_value))
      goto fail_packet;
   volatile uint32_t *const results =
      (volatile uint32_t *)(void *)(packet + NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS);
   terminal->guard_before = &results[0];
   terminal->marker = &results[1];
   terminal->guard_after = &results[2];
   *terminal->guard_before = TERMINAL_GUARD_BEFORE;
   *terminal->marker = 0;
   *terminal->guard_after = TERMINAL_GUARD_AFTER;
   terminal->entry.gpu_va = terminal->slice.gpu;
   terminal->entry.word_count = NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS;

   struct terminal_call call = {.terminal = terminal, .io = io};
   const struct nvkmd_nvgpu_native_completion_io completion_io = {
      .context = &call,
      .wait_target = terminal_wait_target,
      .verify = terminal_verify,
   };
   if (nvkmd_nvgpu_native_completion_begin_endpoint(
          &terminal->completion, endpoint) != NVKMD_NVGPU_COMPLETION_OK)
      goto fail_baseline;
   if (nvkmd_nvgpu_native_submit_once_endpoint(
          &terminal->submit, endpoint, &terminal->entry, 1,
          TERMINAL_SPAN_FLAGS, TERMINAL_SUBMIT_FLAGS) != NVKMD_NVGPU_SUBMIT_OK)
      goto fail_submit;
   if (nvkmd_nvgpu_native_completion_finish_endpoint(
          &terminal->completion, &terminal->submit, endpoint,
          &completion_io) != NVKMD_NVGPU_COMPLETION_OK)
      goto fail_completion;

   if (nvkmd_nvgpu_native_arena_release(arena, &terminal->slice) !=
       NVKMD_NVGPU_NATIVE_ARENA_OK)
      goto fail_release;
   terminal->slice = (struct nvkmd_nvgpu_native_slice) {0};
   terminal->guard_before = NULL;
   terminal->marker = NULL;
   terminal->guard_after = NULL;
   terminal->complete = true;
   return terminal->status = NVKMD_NVGPU_TERMINAL_OK;

fail_release:
   terminal->status = NVKMD_NVGPU_TERMINAL_RELEASE;
   goto fail;
fail_completion:
   terminal->status = NVKMD_NVGPU_TERMINAL_COMPLETION;
   goto fail;
fail_submit:
   terminal->status = NVKMD_NVGPU_TERMINAL_SUBMIT;
   goto fail;
fail_baseline:
   terminal->status = NVKMD_NVGPU_TERMINAL_BASELINE;
   goto fail;
fail_packet:
   terminal->status = NVKMD_NVGPU_TERMINAL_PACKET;
   goto fail;
fail_arena:
   terminal->status = NVKMD_NVGPU_TERMINAL_ARENA;
fail:
   nvkmd_nvgpu_native_arena_poison(arena);
   return terminal->status;
}
