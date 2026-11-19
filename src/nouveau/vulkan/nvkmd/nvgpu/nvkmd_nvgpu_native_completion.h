/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_COMPLETION_H
#define NVKMD_NVGPU_NATIVE_COMPLETION_H

#include "nvkmd_nvgpu_native_submit.h"

enum nvkmd_nvgpu_native_completion_status {
   NVKMD_NVGPU_COMPLETION_OK,
   NVKMD_NVGPU_COMPLETION_ARGUMENT,
   NVKMD_NVGPU_COMPLETION_STATE,
   NVKMD_NVGPU_COMPLETION_BASELINE,
   NVKMD_NVGPU_COMPLETION_TIMEOUT,
   NVKMD_NVGPU_COMPLETION_WAIT,
   NVKMD_NVGPU_COMPLETION_PROGRESS,
   NVKMD_NVGPU_COMPLETION_VERIFY,
   NVKMD_NVGPU_COMPLETION_HEALTH,
};

/* The event is only a scheduling hint. An expired caller wait does not
 * invalidate an accepted producer target or its GPU storage. */
enum nvkmd_nvgpu_native_event_result {
   NVKMD_NVGPU_EVENT_WAKE,
   NVKMD_NVGPU_EVENT_TIMEOUT,
   NVKMD_NVGPU_EVENT_ERROR,
};

struct nvkmd_nvgpu_native_completion_io {
   void *context;
   /* Wait for this accepted producer target. A wake alone does not confirm
    * completion. The join still checks counter, marker and channel health. */
   enum nvkmd_nvgpu_native_event_result
   (*wait_target)(void *context, uint32_t producer_id, uint32_t target);
   /* Check the caller's GPU-written marker after the barrier. False means
    * completion is not confirmed. */
   bool (*verify)(void *context);
};

/* The submit payload, command span and marker storage stay borrowed until the
 * join succeeds. A timed-out wait may resume with the same accepted submit. A
 * timeout never allows storage release. */
struct nvkmd_nvgpu_native_completion {
   const struct nvkmd_nvgpu_native_completion *self;
   uint64_t generation;
   uint32_t producer_id;
   uint32_t baseline_low;
   uint32_t target;
   bool begun, attempted, complete;
   /* The channel's error notification at the last join that reached the
    * target: the error code, its detail and the status (0 when healthy). A
    * failed join logs them. */
   uint32_t health_info32;
   uint16_t health_info16;
   uint16_t health_status;
};

/* Begin with a fresh baseline read of the producer counter. */
enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_begin_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint);

/* Begin without a baseline read. baseline_low must be a known lower bound of
 * this endpoint generation's producer counter. Use a value it has already
 * shown (a prior baseline read) or a target already reached. The mode-0
 * counter only advances. So such a value is a valid baseline while the new
 * target is within the 2^31 window. The join still checks that window. */
enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_begin_known_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   uint32_t baseline_low);

/* Blocking join: wait for the target, then require the counter, the
 * caller's marker and fresh channel health. */
enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_finish_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_completion_io *io);

/* Non-blocking variant. No event arm or wait. It reads the counter once. It
 * returns PROGRESS if the target is not reached yet, with no marker or health
 * request. Otherwise it runs the same marker and health checks as finish.
 * Either variant may later join the same accepted target. */
enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_try_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_completion_io *io);

/* One producer-counter read for the endpoint's producer. */
Result nvkmd_nvgpu_native_completion_read_counter(
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint, uint64_t *counter);

/* Fresh channel health (GPU fd ioctl 0xc0104817): Result of the request,
 * with *status the notification's status field (0 when healthy). */
Result nvkmd_nvgpu_native_channel_health(uint32_t gpu_fd, uint16_t *status);

#endif
