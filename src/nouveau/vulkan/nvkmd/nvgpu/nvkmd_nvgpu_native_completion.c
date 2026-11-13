/* SPDX-License-Identifier: MIT
 * OunceVK join of the GSV sync producer counter (mode 0), a caller-verified GPU
 * marker and fresh channel health. libounce-nx sends the counter read
 * (gsvSyncReadCounter) and the health ioctl (nvIoctl). This owner does not
 * free remote maps. It does not allow command storage reuse on an incomplete
 * join.
 */
#include "nvkmd_nvgpu_native_completion.h"

#include <stdint.h>
#include <string.h>

/* Channel error notification: u64 timestamp, u32 info32, u16 info16 and
 * u16 status, written by the service for GPU fd ioctl 0xc0104817. */
#define CHANNEL_HEALTH_IOCTL UINT32_C(0xc0104817)

struct channel_notification {
   uint64_t timestamp;
   uint32_t info32;
   uint16_t info16;
   uint16_t status;
};

enum mode0_progress {
   MODE0_INVALID,
   MODE0_PENDING,
   MODE0_REACHED,
};

/* Mode-0 counter progress for one positive outstanding interval, on the low
 * 32 bits of the observed counter. This arithmetic alone never confirms GPU
 * completion. */
static enum mode0_progress
mode0_progress(uint32_t baseline, uint32_t target, uint64_t observed)
{
   if (observed == UINT64_MAX)
      return MODE0_INVALID;
   const uint32_t distance = target - baseline;
   const uint32_t progress = (uint32_t)observed - baseline;
   if (!distance || distance >= UINT32_C(0x80000000) ||
       progress >= UINT32_C(0x80000000))
      return MODE0_INVALID;
   return progress >= distance ? MODE0_REACHED : MODE0_PENDING;
}

Result
nvkmd_nvgpu_native_completion_read_counter(
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint, uint64_t *counter)
{
   if (!nvkmd_nvgpu_gpu_endpoint_valid(endpoint) || !counter)
      return MAKERESULT(Module_Libnx, LibnxError_BadInput);
   return gsvSyncReadCounter(endpoint->sync, endpoint->producer_id, counter,
                             NULL);
}

Result
nvkmd_nvgpu_native_channel_health(uint32_t gpu_fd, uint16_t *status)
{
   struct channel_notification notification;
   memset(&notification, 0, sizeof(notification));
   const Result rc = nvIoctl(gpu_fd, CHANNEL_HEALTH_IOCTL, &notification);
   if (R_SUCCEEDED(rc) && status)
      *status = notification.status;
   return rc;
}

enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_begin_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint)
{
   if (!completion || !endpoint)
      return NVKMD_NVGPU_COMPLETION_ARGUMENT;
   if (completion->self || completion->begun ||
       !nvkmd_nvgpu_gpu_endpoint_valid(endpoint))
      return NVKMD_NVGPU_COMPLETION_STATE;

   completion->self = completion;
   completion->generation = endpoint->generation;
   completion->producer_id = endpoint->producer_id;
   uint64_t counter = 0;
   if (R_FAILED(nvkmd_nvgpu_native_completion_read_counter(endpoint, &counter)))
      return NVKMD_NVGPU_COMPLETION_BASELINE;
   completion->baseline_low = (uint32_t)counter;
   completion->begun = true;
   return NVKMD_NVGPU_COMPLETION_OK;
}

enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_begin_known_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   uint32_t baseline_low)
{
   if (!completion || !endpoint)
      return NVKMD_NVGPU_COMPLETION_ARGUMENT;
   if (completion->self || completion->begun ||
       !nvkmd_nvgpu_gpu_endpoint_valid(endpoint))
      return NVKMD_NVGPU_COMPLETION_STATE;

   completion->self = completion;
   completion->generation = endpoint->generation;
   completion->producer_id = endpoint->producer_id;
   completion->baseline_low = baseline_low;
   completion->begun = true;
   return NVKMD_NVGPU_COMPLETION_OK;
}

/* With wait, block on the GSV event hint first. Without wait, read the
 * producer counter once and accept only an already reached target. A pending
 * counter returns PROGRESS without marker or health requests. Either way the
 * counter, GPU marker and fresh channel health together confirm completion.
 * The event wake alone never does. */
static enum nvkmd_nvgpu_native_completion_status
completion_join(struct nvkmd_nvgpu_native_completion *completion,
                const struct nvkmd_nvgpu_native_submit *submit,
                const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
                const struct nvkmd_nvgpu_native_completion_io *io,
                bool wait)
{
   if (!completion || !submit || !endpoint || !io || !io->wait_target ||
       !io->verify)
      return NVKMD_NVGPU_COMPLETION_ARGUMENT;
   if (completion->self != completion || !completion->begun ||
       completion->complete ||
       !nvkmd_nvgpu_gpu_endpoint_valid(endpoint) ||
       completion->generation != endpoint->generation ||
       !submit->accepted || submit->generation != completion->generation ||
       submit->returned_id != completion->producer_id ||
       submit->returned_id != endpoint->producer_id)
      return NVKMD_NVGPU_COMPLETION_STATE;
   if (completion->attempted && completion->target != submit->returned_target)
      return NVKMD_NVGPU_COMPLETION_STATE;

   completion->attempted = true;
   completion->target = submit->returned_target;
   if (wait) {
      const enum nvkmd_nvgpu_native_event_result wait_result =
         io->wait_target(io->context, completion->producer_id,
                         completion->target);
      if (wait_result == NVKMD_NVGPU_EVENT_TIMEOUT)
         return NVKMD_NVGPU_COMPLETION_TIMEOUT;
      if (wait_result != NVKMD_NVGPU_EVENT_WAKE)
         return NVKMD_NVGPU_COMPLETION_WAIT;
   }

   uint64_t counter = 0;
   const Result counter_rc =
      nvkmd_nvgpu_native_completion_read_counter(endpoint, &counter);
   if (R_FAILED(counter_rc))
      return NVKMD_NVGPU_COMPLETION_WAIT;
   const enum mode0_progress progress =
      mode0_progress(completion->baseline_low, completion->target, counter);
   if (progress == MODE0_INVALID)
      return NVKMD_NVGPU_COMPLETION_STATE;
   if (progress != MODE0_REACHED)
      return NVKMD_NVGPU_COMPLETION_PROGRESS;

   __asm__ volatile("dsb sy" ::: "memory");
   const bool verified = io->verify(io->context);

   uint16_t health = UINT16_MAX;
   if (R_FAILED(nvkmd_nvgpu_native_channel_health(endpoint->gpu_fd, &health)))
      return NVKMD_NVGPU_COMPLETION_HEALTH;

   completion->complete = verified && !health;
   if (completion->complete)
      return NVKMD_NVGPU_COMPLETION_OK;
   if (!verified)
      return NVKMD_NVGPU_COMPLETION_VERIFY;
   return NVKMD_NVGPU_COMPLETION_HEALTH;
}

enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_finish_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_completion_io *io)
{
   /* An event wake is only a hint: it can come before the counter reaches the
    * target. Check the counter first, so that finished work needs no wake,
    * then wait for a wake and check again until the target is reached or
    * the wait times out or fails. A pending counter is not device loss; the
    * accepted batch and its storage stay alive meanwhile. */
   enum nvkmd_nvgpu_native_completion_status status =
      completion_join(completion, submit, endpoint, io, false);
   while (status == NVKMD_NVGPU_COMPLETION_PROGRESS)
      status = completion_join(completion, submit, endpoint, io, true);
   return status;
}

enum nvkmd_nvgpu_native_completion_status
nvkmd_nvgpu_native_completion_try_endpoint(
   struct nvkmd_nvgpu_native_completion *completion,
   const struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_completion_io *io)
{
   return completion_join(completion, submit, endpoint, io, false);
}
