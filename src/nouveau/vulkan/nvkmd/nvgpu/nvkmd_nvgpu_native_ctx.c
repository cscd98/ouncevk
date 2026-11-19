/* SPDX-License-Identifier: MIT */
/* Native Vulkan context. It keeps accepted command batches until a waiter on
 * the owner thread, a sync, a capacity drain or destruction joins completion.
 * Signals publish pending points instead of draining. The delegated sessions
 * and physical channel stay on their original runtime thread. An uncertain
 * operation poisons the shared channel. */
#include "nvkmd_nvgpu_native_ctx.h"
#include "nvkmd_nvgpu_native_release.h"

#include "nvkmd_nvgpu_native_batch.h"
#include "nvkmd_nvgpu_native_completion.h"
#include "nvkmd_nvgpu_native_progress.h"
#include "nvkmd_nvgpu_native_submit.h"
#include "nvkmd_nvgpu_native_syncobj.h"
#include "nvkmd_nvgpu_semaphore_probe.h"

#include "util/macros.h"
#include "util/os_time.h"
#include "vk_object.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <switch.h>

#include "util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
   NATIVE_CTX_SUBMIT_FLAGS = 6, /* No-prefetch, not-main entry. */
   NATIVE_CTX_SPAN_FLAGS = 1,
   NATIVE_CTX_FIRST_MARKER = 0xb6e9,
   /* Internal chunk target, not a Vulkan or service submission limit. */
   NATIVE_CTX_COPY_CHUNK_B = 1024 * 1024,
   /* Local pressure bound, not a hardware ring or Vulkan queue limit. */
   NATIVE_CTX_MAX_INFLIGHT = 8,
};
static const uint64_t native_ctx_wait_ns = UINT64_C(5000000000);
/* The wait for GSV to acknowledge a cancelled event registration. It runs
 * with the owner lock held, so it is bounded.
 */
static const uint64_t native_gsv_cancel_wait_ns = UINT64_C(1000000000);

/* Debug switches for cross-queue synchronization faults. 0 is the normal
 * behavior. A test program can set them between submissions. Each batch
 * reads them when it is prepared or when it adds a wait.
 * Bit 0: every batch ends with the dirty L2 flush before its marker.
 * Bit 1: every batch starts with the L2 invalidate.
 * Bit 2: the primary channel waits on the CPU for copy-channel points,
 *        not with a GPU shim acquire.
 * Bit 3 (8): every primary-channel batch starts with the 3D wait-for-idle
 *        and the texture and shader cache invalidations.
 * Bit 4 (16): GPU-only memory comes from GPU-uncached segments
 *        (nvkmd_nvgpu_native_pdev.c).
 * Bit 8 (256): copy-only contexts (transfer queues, NVK's upload queue) use
 *        the primary channel, not the copy channel.
 * Bit 5 (32): every barrier waits for idle and emits an L2 sequence:
 *        dirty flush, system memory barrier, L2 invalidate
 *        (nvk_cmd_buffer.c). */
uint32_t ouncevk_debug_sync_flags;

/* Programs that cannot set the switches (the CTS) can put them in a file on
 * the SD card. The first context creation reads it. */
void ouncevk_debug_sync_flags_load(void);

void
ouncevk_debug_sync_flags_load(void)
{
   static bool loaded;
   if (loaded)
      return;
   loaded = true;
   FILE *file = fopen("sdmc:/config/ouncevk/debug-sync.txt", "r");
   if (!file)
      return;
   unsigned value = 0;
   if (fscanf(file, "%i", (int *)&value) == 1) {
      ouncevk_debug_sync_flags = value;
      mesa_logi("native debug sync flags: %u", value);
   }
   fclose(file);
}

struct nvkmd_nvgpu_native_ctx;

struct nvkmd_nvgpu_native_submission {
   struct nvkmd_nvgpu_native_submission *next;
   struct nvkmd_nvgpu_native_batch batch;
   struct nvkmd_nvgpu_native_submit submit;
   struct nvkmd_nvgpu_native_point *point;
   /* The channel that runs it: the primary or the copy channel. */
   struct nvkmd_nvgpu_gpu_endpoint *endpoint;
   /* Its batch carries the ordered dirty-L2 flush for these pending
    * segment releases (nvkmd_nvgpu_native_owner_claim_release_flush), or
    * 0. */
   uint64_t release_flush;
};

struct nvkmd_nvgpu_native_ctx {
   struct nvkmd_ctx base;
   struct nvkmd_nvgpu_native_device_owner *owner;
   struct nvkmd_nvgpu_native_progress *progress;
   /* The channel of this context: the primary channel, or the
    * copy channel for a copy-only context of the device. */
   struct nvkmd_nvgpu_gpu_endpoint *endpoint;
   struct nvkmd_nvgpu_native_point *last_point;
   /* The newest same-channel point this submission waits on, resolved without
    * a CPU join. Signals publish a point no older than it. The next flushed
    * batch starts with a head barrier. Cleared after the signal that ends the
    * submission. */
   struct nvkmd_nvgpu_native_point *dependency;
   /* Foreign waits (VI2 releases) for the next batch: shim acquires at its
    * head. Earlier pending work is flushed before one is recorded. So they
    * cover only later work.
    */
   uint32_t waits[NVKMD_NVGPU_NATIVE_BATCH_MAX_WAIT_WORDS];
   uint32_t wait_words;
   /* The waits include one for the other channel: the next batch
    * invalidates the GPU caches after them. */
   bool channel_wait;
   /* The words that set this context's channel state (its queue's context
    * init and current pools). See native_ctx_needs_replay. */
   uint32_t *state_replay;
   uint32_t state_replay_words;
   uint64_t context_id;
   enum nvkmd_engines engines;
   uint32_t *active_context_count;
   struct nvkmd_nvgpu_native_span pending[NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS];
   uint32_t pending_count;
   uint64_t pending_bytes;
   uint32_t next_marker;
   bool failed;
   bool destroyed;
};

static const struct nvkmd_ctx_ops native_ctx_ops;

static struct nvkmd_nvgpu_native_ctx *
native_ctx(struct nvkmd_ctx *base)
{
   return container_of(base, struct nvkmd_nvgpu_native_ctx, base);
}

/* Every context operation runs under the owner lock. So any thread may submit,
 * wait and finish work on the shared channel.
 */
static bool
native_ctx_lock(struct nvkmd_nvgpu_native_ctx *ctx)
{
   return nvkmd_nvgpu_native_owner_lock(ctx->owner);
}

static void
native_ctx_unlock(struct nvkmd_nvgpu_native_ctx *ctx)
{
   nvkmd_nvgpu_native_owner_unlock(ctx->owner);
}

static bool
native_ctx_verify(void *context)
{
   const struct nvkmd_nvgpu_native_point *point = context;
   const struct nvkmd_nvgpu_native_submission *submission = point->storage;
   if (!submission)
      return false;
   return nvkmd_nvgpu_native_batch_verify(&submission->batch);
}

/* The completion event: an event slot of the process's GSV sync session,
 * allocated from libounce (gsvSyncAllocateSlot), because other users of the
 * session in this process (for example a video decoder) have their own. The
 * process has one device owner, so all VkDevices share this event. It is
 * created once (sync raw 0x26) and armed for each waited target (sync raw
 * 0xd9). A wake is only a scheduling hint. The completion join still checks
 * the counter, the GPU marker and fresh channel health before it releases
 * storage.
 */
#define NATIVE_GSV_EVENT_MODE 0u

/* Ends the registration of a timed-out wait: disarm it, then wait for the
 * event. GSV signals the event when it cancels a registration and when one
 * completes, and an already reached target left it signaled, so this wait
 * returns at once. The event stays signaled until the next pending arm, and
 * that next wait arms its own target. Returns false if the event cannot be
 * used any more.
 */
static bool
native_gsv_event_cancel(struct nvkmd_nvgpu_native_device_owner *owner)
{
   Result rc = gsvSyncDisarmEvent(owner->endpoint.sync, owner->gsv_event_slot);
   owner->gsv_event_status[8] = rc;
   if (R_SUCCEEDED(rc)) {
      rc = eventWait(&owner->gsv_event, native_gsv_cancel_wait_ns);
      owner->gsv_event_status[10] = rc;
   }
   owner->gsv_event_armed = false;
   if (R_FAILED(rc)) {
      owner->gsv_event_disabled = true;
      owner->gsv_event_failures++;
      return false;
   }
   return true;
}

static enum nvkmd_nvgpu_native_event_result
native_gsv_event_wait_owner(struct nvkmd_nvgpu_native_device_owner *owner,
                            uint32_t producer_id, uint32_t target)
{
   if (owner->gsv_event_disabled)
      return NVKMD_NVGPU_EVENT_ERROR;
   GsvSync *const sync = owner->endpoint.sync;
   const bool first = !owner->gsv_event_tested;
   if (first) {
      owner->gsv_event_tested = true;
      for (unsigned i = 0; i < 11; i++)
         owner->gsv_event_status[i] = UINT32_MAX;
   }
   Result rc;
   if (!owner->gsv_event_slot_allocated) {
      rc = gsvSyncAllocateSlot(sync, &owner->gsv_event_slot);
      owner->gsv_event_status[9] = rc;
      if (R_FAILED(rc)) {
         owner->gsv_event_disabled = true;
         owner->gsv_event_failures++;
         return NVKMD_NVGPU_EVENT_ERROR;
      }
      owner->gsv_event_slot_allocated = true;
   }
   if (!owner->gsv_event_open) {
      /* Manual-clear: the client never clears it. The event starts
       * signaled. Arming a pending target makes GSV clear it and signal it
       * at completion; arming an already reached target keeps the existing
       * signal, which is what ends that wait. An auto-clear event loses that
       * signal: vkmark's first GPU wait hung on the console (14 November
       * 2026). A wake can also come from a cancellation, so the join checks
       * the counter itself.
       */
      rc = gsvSyncCreateEvent(sync, owner->gsv_event_slot, &owner->gsv_event,
                              false);
      owner->gsv_event_status[0] = rc;
      if (R_FAILED(rc)) {
         owner->gsv_event_disabled = true;
         owner->gsv_event_failures++;
         return NVKMD_NVGPU_EVENT_ERROR;
      }
      owner->gsv_event_open = true;
   }
   if (owner->gsv_event_armed) {
      if (owner->gsv_event_producer != producer_id ||
          owner->gsv_event_target != target)
         return NVKMD_NVGPU_EVENT_ERROR;
   } else {
      rc = gsvSyncArmEvent(sync, owner->gsv_event_slot, producer_id, target,
                           NATIVE_GSV_EVENT_MODE);
      owner->gsv_event_status[2] = rc;
      if (R_FAILED(rc)) {
         owner->gsv_event_disabled = true;
         owner->gsv_event_failures++;
         return NVKMD_NVGPU_EVENT_ERROR;
      }
      owner->gsv_event_armed = true;
      owner->gsv_event_producer = producer_id;
      owner->gsv_event_target = target;
      owner->gsv_event_arms++;
   }
   const uint64_t before = armGetSystemTick();
   /* No global lock across the blocking wait. The arm, disarm and every
    * service request stay under the owner lock. Only this kernel wait on our
    * own event runs without it. So other threads can submit, allocate and free
    * meanwhile. owner->joining keeps other joiners (and polls) off
    * inflight_first until this join has finished it.
    */
   const bool release = owner->lock_ready;
   uint32_t depth = 0;
   if (release) {
      owner->joining = true;
      depth = nvkmd_nvgpu_native_owner_suspend(owner);
   }
   const Result wait_rc = eventWait(&owner->gsv_event, native_ctx_wait_ns);
   if (release) {
      nvkmd_nvgpu_native_owner_resume(owner, depth);
      owner->joining = false;
      nvkmd_nvgpu_native_owner_join_done(owner);
   }
   const uint64_t elapsed = armGetSystemTick() - before;
   owner->gsv_event_wait_ticks += elapsed;
   owner->gsv_event_status[5] = wait_rc;
   owner->gsv_event_status[7] = (uint32_t)elapsed;
   if (R_SUCCEEDED(wait_rc)) {
      owner->gsv_event_armed = false;
      owner->gsv_event_wakes++;
      return NVKMD_NVGPU_EVENT_WAKE;
   }
   if (R_VALUE(wait_rc) == KERNELRESULT(TimedOut)) {
      owner->gsv_event_timeouts++;
      if (!native_gsv_event_cancel(owner))
         return NVKMD_NVGPU_EVENT_ERROR;
      return NVKMD_NVGPU_EVENT_TIMEOUT;
   }
   owner->gsv_event_failures++;
   owner->gsv_event_status[8] = gsvSyncDisarmEvent(sync, owner->gsv_event_slot);
   owner->gsv_event_armed = false;
   owner->gsv_event_disabled = true;
   return NVKMD_NVGPU_EVENT_ERROR;
}

static enum nvkmd_nvgpu_native_event_result
native_gsv_event_wait(void *context, uint32_t producer_id, uint32_t target)
{
   struct nvkmd_nvgpu_native_point *point = context;
   return native_gsv_event_wait_owner(point->progress->owner,
                                       producer_id, target);
}

bool
nvkmd_nvgpu_native_wait_terminal_target(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint32_t producer_id, uint32_t target)
{
   if (!nvkmd_nvgpu_native_device_common_valid(owner) ||
       owner->submission_failed)
      return false;
   return native_gsv_event_wait_owner(owner, producer_id, target) ==
      NVKMD_NVGPU_EVENT_WAKE;
}

static VkResult
native_owner_fail(struct nvkmd_nvgpu_native_device_owner *owner)
{
   owner->submission_failed = true;
   nvkmd_nvgpu_native_progress_fail(owner->progress, VK_ERROR_DEVICE_LOST);
   mesa_loge("native: submission failed; the primary arena is poisoned");
   nvkmd_nvgpu_native_arena_poison(owner->arena_ref);
   return VK_ERROR_DEVICE_LOST;
}

/* Marks the context and its owner failed. The log names the source line,
 * because the failure is permanent for the process. */
static VkResult
native_ctx_fail_at(struct nvkmd_nvgpu_native_ctx *ctx, unsigned line)
{
   mesa_loge("native: context failed at nvkmd_nvgpu_native_ctx.c:%u", line);
   ctx->failed = true;
   return native_owner_fail(ctx->owner);
}
#define native_ctx_fail(ctx) native_ctx_fail_at((ctx), __LINE__)

static struct nvkmd_nvgpu_native_submission *
native_submission_take(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_submission *submission =
      owner->recycled_submissions;
   if (!submission) {
      submission = calloc(1, sizeof(*submission));
      return submission;
   }

   owner->recycled_submissions = submission->next;
   submission->next = NULL;
   submission->batch.slice = (struct nvkmd_nvgpu_native_slice) {0};
   submission->batch.command_gpu = 0;
   submission->batch.command_words = 0;
   submission->batch.entry_count = 0;
   submission->batch.marker_value = 0;
   submission->batch.flush_dirty = false;
   submission->batch.guard_before = NULL;
   submission->batch.marker = NULL;
   submission->batch.guard_after = NULL;
   submission->batch.prepared = false;
   submission->submit.self = NULL;
   submission->submit.generation = 0;
   submission->submit.returned_id = 0;
   submission->submit.returned_target = 0;
   submission->submit.attempted = false;
   submission->submit.send_started = false;
   submission->submit.accepted = false;
   submission->point = NULL;
   submission->release_flush = 0;
   /* The large entry arrays and control bytes are fully overwritten for
    * the next declared entry count before they are passed to the service. */
   return submission;
}

static VkResult
native_submission_release_unsent(struct nvkmd_nvgpu_native_device_owner *owner,
                                 struct nvkmd_nvgpu_native_submission *submission,
                                 VkResult result)
{
   if (nvkmd_nvgpu_native_arena_release(owner->arena_ref,
                                        &submission->batch.slice) !=
       NVKMD_NVGPU_NATIVE_ARENA_OK) {
      submission->next = owner->quarantined_submissions;
      owner->quarantined_submissions = submission;
      return native_owner_fail(owner);
   }
   submission->next = owner->recycled_submissions;
   owner->recycled_submissions = submission;
   return result;
}

static VkResult
native_submission_discard_unsent(struct nvkmd_nvgpu_native_device_owner *owner,
                                 struct nvkmd_nvgpu_native_submission *submission,
                                 VkResult result)
{
   struct nvkmd_nvgpu_native_submission **link = &owner->inflight_first;
   while (*link && *link != submission)
      link = &(*link)->next;
   if (!*link || submission->submit.send_started)
      return native_owner_fail(owner);
   *link = submission->next;
   if (owner->inflight_last == submission) {
      owner->inflight_last = NULL;
      for (struct nvkmd_nvgpu_native_submission *it = owner->inflight_first;
           it; it = it->next)
         owner->inflight_last = it;
   }
   struct nvkmd_nvgpu_native_point *point = submission->point;
   if (nvkmd_nvgpu_native_point_discard_unsent(point, submission) != VK_SUCCESS)
      return native_owner_fail(owner);
   submission->point = NULL;
   nvkmd_nvgpu_native_point_unref(point);
   return native_submission_release_unsent(owner, submission, result);
}

/* Join the oldest accepted submission on the channel's TLS thread. With block,
 * wait on the GSV event hint. Without block, poll the producer counter once and
 * return VK_NOT_READY while its target is pending. A successful join releases
 * that submission's command storage and completes its point. */
static VkResult
native_owner_join_first_mode(struct nvkmd_nvgpu_native_device_owner *owner,
                             bool block)
{
   /* Another thread is blocked joining inflight_first with the lock released
    * (see native_gsv_event_wait_owner). A poll does not wait. A blocking join
    * waits for that join and reports progress. The caller re-checks what it was
    * waiting for. */
   if (owner->joining) {
      if (!block)
         return VK_NOT_READY;
      nvkmd_nvgpu_native_owner_wait_join(owner);
      return VK_SUCCESS;
   }
   struct nvkmd_nvgpu_native_submission *submission = owner->inflight_first;
   if (!submission)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_point *point = submission->point;
   if (!point || !submission->submit.accepted)
      return native_owner_fail(owner);
   const struct nvkmd_nvgpu_native_completion_io completion_io = {
      .context = point,
      .wait_target = native_gsv_event_wait,
      .verify = native_ctx_verify,
   };
   const uint64_t join_start = armGetSystemTick();
   const enum nvkmd_nvgpu_native_completion_status status = block ?
      nvkmd_nvgpu_native_completion_finish_endpoint(
         &point->completion, &submission->submit,
         submission->endpoint, &completion_io) :
      nvkmd_nvgpu_native_completion_try_endpoint(
         &point->completion, &submission->submit,
         submission->endpoint, &completion_io);
   const uint64_t join_ticks = armGetSystemTick() - join_start;
   owner->stats.join_ticks += join_ticks;
   if (block) {
      owner->stats.joins_blocking++;
      ouncevk_profile_record(OUNCEVK_PROFILE_JOIN, join_ticks);
   } else if (status == NVKMD_NVGPU_COMPLETION_PROGRESS)
      owner->stats.polls_pending++;
   else
      owner->stats.polls_reached++;
   if (status == NVKMD_NVGPU_COMPLETION_TIMEOUT)
      return VK_TIMEOUT;
   if (!block && status == NVKMD_NVGPU_COMPLETION_PROGRESS)
      return VK_NOT_READY;
   if (status != NVKMD_NVGPU_COMPLETION_OK) {
      mesa_loge("native: completion join failed: status %u, blocking %u, "
                "producer %u, baseline %u, target %u, channel error 0x%x "
                "detail 0x%x status 0x%x",
                (unsigned)status, (unsigned)block,
                point->completion.producer_id, point->completion.baseline_low,
                point->completion.target, point->completion.health_info32,
                (unsigned)point->completion.health_info16,
                (unsigned)point->completion.health_status);
      return native_owner_fail(owner);
   }
   if (nvkmd_nvgpu_native_arena_release(
          owner->arena_ref, &submission->batch.slice) !=
       NVKMD_NVGPU_NATIVE_ARENA_OK)
      return native_owner_fail(owner);
   if (nvkmd_nvgpu_native_point_release_storage(point, submission) != VK_SUCCESS ||
       nvkmd_nvgpu_native_point_complete(point) != VK_SUCCESS)
      return native_owner_fail(owner);
   /* The reached target is now a known counter floor. Raise the floor only
    * forward (mode-0 modular order) and only within one endpoint generation. */
   struct nvkmd_nvgpu_gpu_endpoint *endpoint = submission->endpoint;
   if (endpoint->floor_valid &&
       endpoint->floor_generation == point->completion.generation &&
       point->completion.target - endpoint->floor < UINT32_C(0x80000000))
      endpoint->floor = point->completion.target;

   owner->inflight_first = submission->next;
   if (!owner->inflight_first)
      owner->inflight_last = NULL;
   owner->inflight_entries -= submission->batch.entry_count;
   owner->inflight_submissions--;
   const uint64_t release_flush = submission->release_flush;
   submission->point = NULL;
   submission->next = owner->recycled_submissions;
   owner->recycled_submissions = submission;
   nvkmd_nvgpu_native_point_unref(point);
   /* Its completion followed the ordered dirty-L2 flush: the segments
    * freed before it was submitted can go back to the heap. */
   if (release_flush)
      nvkmd_nvgpu_native_owner_release_flushed(owner, release_flush);
   return VK_SUCCESS;
}

static VkResult
native_owner_join_first(struct nvkmd_nvgpu_native_device_owner *owner)
{
   return native_owner_join_first_mode(owner, true);
}

bool
nvkmd_nvgpu_native_owner_can_drive(
   const struct nvkmd_nvgpu_native_device_owner *owner)
{
   return owner && owner->lock_ready;
}

/* Waiters, not signals, finish accepted work. Join in channel order until this
 * point completes, under the owner lock (any thread). The caller must not hold
 * the progress mutex. Without block, join only submissions whose counter has
 * already reached its target. Return VK_NOT_READY at the first pending one. So
 * zero-timeout polls never wait. */
VkResult
nvkmd_nvgpu_native_owner_drive_point(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_point *point, bool block)
{
   if (!point || !nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_INITIALIZATION_FAILED;
   VkResult result = VK_SUCCESS;
   while (!nvkmd_nvgpu_native_point_is_complete(point)) {
      if (owner->submission_failed || owner->retiring ||
          !owner->inflight_first) {
         result = native_owner_fail(owner);
         break;
      }
      result = native_owner_join_first_mode(owner, block);
      if (result != VK_SUCCESS)
         break;
   }
   nvkmd_nvgpu_native_owner_unlock(owner);
   return result;
}

static struct nvkmd_nvgpu_native_point *
native_ctx_signal_point(struct nvkmd_nvgpu_native_ctx *ctx);

/* Joins this context's work, and a same-channel dependency it carries. */
static VkResult
native_ctx_drain(struct nvkmd_nvgpu_native_ctx *ctx)
{
   struct nvkmd_nvgpu_native_point *target = native_ctx_signal_point(ctx);
   while (target && !nvkmd_nvgpu_native_point_is_complete(target)) {
      if (!ctx->owner->inflight_first)
         return native_ctx_fail(ctx);
      VkResult result = native_owner_join_first(ctx->owner);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

/* A signal could not be published after the work was sent. A host memory
 * failure does not damage the GPU state: wait for the work, so nothing in
 * flight depends on the unpublished signal, and return the error. Any other
 * failure leaves an unknown state and fails the context. */
static VkResult
native_ctx_signal_error(struct nvkmd_nvgpu_native_ctx *ctx, VkResult error)
{
   if (error != VK_ERROR_OUT_OF_HOST_MEMORY)
      return native_ctx_fail(ctx);
   const VkResult drained = native_ctx_drain(ctx);
   return drained == VK_SUCCESS ? error : drained;
}

static VkResult
native_ctx_flush_pending(struct nvkmd_nvgpu_native_ctx *ctx);

static VkResult
native_ctx_flush_locked(struct nvkmd_ctx *base, struct vk_object_base *log_obj)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   const bool counted = ctx->pending_count != 0;
   const uint64_t start = counted ? armGetSystemTick() : 0;
   const uint32_t entries = ctx->pending_count + 1;
   VkResult result = native_ctx_flush_pending(ctx);
   if (counted) {
      const uint64_t flush_ticks = armGetSystemTick() - start;
      ctx->owner->stats.flush_ticks += flush_ticks;
      ouncevk_profile_record(OUNCEVK_PROFILE_FLUSH, flush_ticks);
      if (result == VK_SUCCESS) {
         ctx->owner->stats.flushes++;
         ctx->owner->stats.flush_entries += entries;
         if (ctx->engines == NVKMD_ENGINE_COPY)
            ctx->owner->stats.copy_flushes++;
      }
   }
   return result;
}

/* Prepares the batch from the pending spans. NVK's push memory can lie in any
 * ready segment, not only the primary one (a 4K swapchain fills that). So every
 * ready arena is a source. The list is the owner's. The caller holds the owner
 * lock. The list is rebuilt on each call, because a join between calls releases
 * the lock. While segment releases wait for a flush, the completion carries the
 * ordered dirty-L2 flush. */
/* True when this context's 3D and compute work runs on the primary
 * channel. Every VkDevice of the process shares that channel. NVK sets the
 * channel state (texture header and sampler pools, shader local memory,
 * constant buffer 0 and the other context init state) once per queue: when
 * another device's queue set it, this context's work would use the other
 * device's (maybe freed) pools. */
static bool
native_ctx_sets_channel_state(const struct nvkmd_nvgpu_native_ctx *ctx)
{
   return ctx->endpoint == &ctx->owner->endpoint &&
          (ctx->engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE));
}

/* The next batch of this context must first replay its channel state. */
static bool
native_ctx_needs_replay(const struct nvkmd_nvgpu_native_ctx *ctx)
{
   return native_ctx_sets_channel_state(ctx) && ctx->state_replay_words &&
          ctx->owner->state_context_id != ctx->context_id;
}

static enum nvkmd_nvgpu_native_batch_status
native_ctx_prepare_batch(struct nvkmd_nvgpu_native_ctx *ctx, bool head_barrier,
                         struct nvkmd_nvgpu_native_batch *batch)
{
   const bool replay = native_ctx_needs_replay(ctx);
   struct nvkmd_nvgpu_native_device_owner *owner = ctx->owner;
   const struct nvkmd_nvgpu_native_arena **sources = owner->batch_sources;
   uint32_t source_count = 0;
   /* A GPU-cached readback segment needs two GPU cache operations: the L2
    * invalidate before the spans (CPU writes reach the GPU) and the dirty
    * flush before the completion (GPU writes reach the CPU). Imported host
    * memory is mapped the same way. */
   bool readback = owner->host_mems_ready && !list_is_empty(&owner->host_mems);
   sources[source_count++] = owner->arena_ref;
   for (uint32_t i = 0; i < owner->segment_count; i++) {
      if (!owner->segments[i].ready)
         continue;
      sources[source_count++] = &owner->segments[i].arena;
      if (owner->segments[i].kind == NVKMD_NVGPU_SEGMENT_READBACK)
         readback = true;
   }
   return nvkmd_nvgpu_native_batch_prepare_sources(
      owner->arena_ref, sources, source_count,
      ctx->pending, ctx->pending_count,
      ctx->next_marker, head_barrier, ctx->waits, ctx->wait_words,
      owner->releases_waiting > 0 || readback ||
         ctx->endpoint != &owner->endpoint ||
         (ouncevk_debug_sync_flags & 1),
      readback || (ouncevk_debug_sync_flags & 2),
      ctx->endpoint != &owner->endpoint,
      (ctx->channel_wait || (ouncevk_debug_sync_flags & 8)) &&
         ctx->endpoint == &owner->endpoint,
      replay ? ctx->state_replay : NULL,
      replay ? ctx->state_replay_words : 0, batch);
}

static VkResult
native_ctx_flush_pending(struct nvkmd_nvgpu_native_ctx *ctx)
{
   if (ctx->failed || ctx->owner->submission_failed ||
       ctx->owner->retiring ||
       ctx->destroyed)
      return VK_ERROR_DEVICE_LOST;
   /* A batch with only foreign waits still runs: a later signal must not
    * complete before them. */
   if (!ctx->pending_count && !ctx->wait_words)
      return VK_SUCCESS;

   struct nvkmd_nvgpu_native_device_owner *owner = ctx->owner;
   /* GPU work after a same-channel wait starts with the batch head barrier:
    * the channel then completes, idles and flushes all earlier work before
    * running this batch (see nvkmd_nvgpu_native_batch.h). */
   const bool head_barrier = ctx->dependency &&
      !nvkmd_nvgpu_native_point_is_complete(ctx->dependency);
   const bool head = head_barrier || ctx->wait_words ||
                     native_ctx_needs_replay(ctx);
   if (head &&
       ctx->pending_count > (uint32_t)NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS - 1)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   const uint32_t needed_entries = ctx->pending_count + 1 + head;
   while (owner->inflight_first &&
          (owner->inflight_submissions == NATIVE_CTX_MAX_INFLIGHT ||
           owner->inflight_entries >
              NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES - needed_entries)) {
      VkResult result = native_owner_join_first(owner);
      if (result != VK_SUCCESS)
         return result;
   }
   if (owner->inflight_entries >
       NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES - needed_entries)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct nvkmd_nvgpu_native_submission *submission =
      native_submission_take(owner);
   if (!submission)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!ctx->next_marker)
      ctx->next_marker = NATIVE_CTX_FIRST_MARKER;
   else if (++ctx->next_marker == 0)
      ++ctx->next_marker;
   enum nvkmd_nvgpu_native_batch_status batch_status =
      native_ctx_prepare_batch(ctx, head_barrier, &submission->batch);
   /* Submitted slices may be the only reason the arena is full. Reclaim the
    * oldest completed batch before reporting an allocation failure. */
   while (batch_status == NVKMD_NVGPU_NATIVE_BATCH_ARENA &&
          owner->inflight_first) {
      VkResult result = native_owner_join_first(owner);
      if (result != VK_SUCCESS) {
         free(submission);
         return result;
      }
      batch_status =
         native_ctx_prepare_batch(ctx, head_barrier, &submission->batch);
   }
   if (batch_status != NVKMD_NVGPU_NATIVE_BATCH_OK) {
      free(submission);
      if (batch_status == NVKMD_NVGPU_NATIVE_BATCH_ARENA)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return native_ctx_fail(ctx);
   }

   struct nvkmd_nvgpu_native_point *point = NULL;
   VkResult point_result = nvkmd_nvgpu_native_point_create(
      ctx->progress, ctx->context_id, &point);
   if (point_result != VK_SUCCESS)
      return native_submission_release_unsent(owner, submission, point_result);
   point_result = nvkmd_nvgpu_native_point_bind_storage(point, submission);
   if (point_result != VK_SUCCESS) {
      nvkmd_nvgpu_native_point_unref(point);
      return native_submission_release_unsent(owner, submission, point_result);
   }
   submission->point = point;

   if (owner->inflight_last)
      owner->inflight_last->next = submission;
   else
      owner->inflight_first = submission;
   owner->inflight_last = submission;

   /* A known counter floor for this endpoint generation is a valid mode-0
    * baseline. It saves one service transaction per submit. Otherwise read the
    * counter and remember it. */
   struct nvkmd_nvgpu_gpu_endpoint *endpoint = ctx->endpoint;
   submission->endpoint = endpoint;
   const bool floor_known =
      endpoint->floor_valid &&
      endpoint->floor_generation == endpoint->generation;
   const enum nvkmd_nvgpu_native_completion_status begin_status = floor_known ?
      nvkmd_nvgpu_native_completion_begin_known_endpoint(
         &point->completion, endpoint, endpoint->floor) :
      nvkmd_nvgpu_native_completion_begin_endpoint(
         &point->completion, endpoint);
   if (begin_status != NVKMD_NVGPU_COMPLETION_OK) {
      native_submission_discard_unsent(owner, submission,
                                       VK_ERROR_DEVICE_LOST);
      return native_ctx_fail(ctx);
   }
   if (!floor_known) {
      owner->stats.baseline_reads++;
      endpoint->floor = point->completion.baseline_low;
      endpoint->floor_generation = endpoint->generation;
      endpoint->floor_valid = true;
   }

   if (nvkmd_nvgpu_native_submit_once_endpoint(
          &submission->submit, endpoint,
          submission->batch.entries,
          submission->batch.entry_count,
          NATIVE_CTX_SPAN_FLAGS,
          NATIVE_CTX_SUBMIT_FLAGS) != NVKMD_NVGPU_SUBMIT_OK) {
      if (!submission->submit.send_started)
         native_submission_discard_unsent(owner, submission,
                                          VK_ERROR_DEVICE_LOST);
      return native_ctx_fail(ctx);
   }
   if (nvkmd_nvgpu_native_point_accept(point, &submission->submit) != VK_SUCCESS)
      goto device_lost;
   if (submission->batch.flush_dirty)
      submission->release_flush =
         nvkmd_nvgpu_native_owner_claim_release_flush(owner);
   nvkmd_nvgpu_native_point_ref(point);
   struct nvkmd_nvgpu_native_point *old_point = ctx->last_point;
   ctx->last_point = point;
   nvkmd_nvgpu_native_point_unref(old_point);
   ctx->owner->inflight_submissions++;
   ctx->owner->inflight_entries += submission->batch.entry_count;
   if (ctx->owner->inflight_submissions >
       ctx->owner->peak_inflight_submissions)
      ctx->owner->peak_inflight_submissions =
         ctx->owner->inflight_submissions;
   ctx->pending_count = 0;
   ctx->pending_bytes = 0;
   if (native_ctx_sets_channel_state(ctx))
      owner->state_context_id = ctx->context_id;
   owner->stats.foreign_waits +=
      ctx->wait_words / NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS;
   ctx->wait_words = 0;
   ctx->channel_wait = false;
   /* This batch runs after the dependency in channel order. */
   if (head_barrier)
      owner->stats.head_barriers++;
   nvkmd_nvgpu_native_point_unref(ctx->dependency);
   ctx->dependency = NULL;
   return VK_SUCCESS;

device_lost:
   return native_ctx_fail(ctx);
}

static void
native_ctx_destroy_locked(struct nvkmd_ctx *base)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   ctx->destroyed = true;
   /* Work that was recorded but never submitted can be dropped: the GPU
    * never saw it, and the spans point into the caller's memory. A failed
    * device creation destroys its queue context in this state. Keeping the
    * context for it poisoned the primary arena for the whole process. */
   ctx->pending_count = 0;
   ctx->pending_bytes = 0;
   ctx->wait_words = 0;
   ctx->channel_wait = false;
   if (!ctx->failed &&
       native_ctx_drain(ctx) != VK_SUCCESS) {
      /* Vulkan destruction cannot report this failure. Keep every arena
       * slice at its current address until the owner can be closed. */
      ctx->owner->kept_contexts++;
      mesa_loge("native: context drain failed at destroy; the primary "
                "arena is poisoned");
      nvkmd_nvgpu_native_arena_poison(ctx->owner->arena_ref);
      return;
   }
   /* Keep the context and all its in-flight storage when a request remains
    * uncertain, or when another thread cannot drain this session owner. */
   if (ctx->failed || ctx->pending_count ||
       (ctx->last_point &&
        !nvkmd_nvgpu_native_point_is_complete(ctx->last_point))) {
      ctx->owner->kept_contexts++;
      mesa_loge("native: context kept at destroy (failed %d, pending %u, "
                "last point complete %d); the primary arena is poisoned",
                ctx->failed, (unsigned)ctx->pending_count,
                !ctx->last_point ||
                   nvkmd_nvgpu_native_point_is_complete(ctx->last_point));
      nvkmd_nvgpu_native_arena_poison(ctx->owner->arena_ref);
      return;
   }
   if (ctx->active_context_count && *ctx->active_context_count)
      --*ctx->active_context_count;
   nvkmd_nvgpu_native_point_unref(ctx->dependency);
   nvkmd_nvgpu_native_point_unref(ctx->last_point);
   nvkmd_nvgpu_native_progress_unref(ctx->progress);
   free(ctx->state_replay);
   free(ctx);
}

/* Points of one progress domain complete in serial (channel) order. */
static struct nvkmd_nvgpu_native_point *
native_point_newer(struct nvkmd_nvgpu_native_point *a,
                   struct nvkmd_nvgpu_native_point *b)
{
   if (!a)
      return b;
   if (!b)
      return a;
   return b->serial > a->serial ? b : a;
}

/* A wait on work already accepted by this physical channel needs no CPU join.
 * Signals of the submission publish a point at or after it. Its completion
 * implies the waited point's completion, because joins run in channel order.
 * New work in the submission starts with a head barrier. So the channel runs it
 * after the waited work. VK_SUCCESS with *point NULL: nothing to wait for.
 * VK_SUCCESS with *point: a referenced same-channel dependency. VK_NOT_READY:
 * the caller waits on the CPU. */
static VkResult
native_ctx_resolve_wait(struct nvkmd_nvgpu_native_ctx *ctx,
                        struct vk_device *device,
                        const struct vk_sync_wait *wait,
                        struct nvkmd_nvgpu_native_point **point_out)
{
   *point_out = NULL;
   struct vk_sync *sync = wait->sync;
   struct vk_sync_timeline_point *timeline_point = NULL;
   struct vk_sync_timeline *timeline = vk_sync_as_timeline(sync);
   if (timeline) {
      VkResult result = vk_sync_timeline_get_point(
         device, timeline, wait->wait_value, &timeline_point);
      if (result != VK_SUCCESS)
         return result == VK_NOT_READY ? VK_NOT_READY : result;
      if (!timeline_point)
         return VK_SUCCESS;
      sync = &timeline_point->sync;
   }
   VkResult result = VK_NOT_READY;
   if (sync->type == &nvkmd_nvgpu_native_syncobj_type) {
      struct nvkmd_nvgpu_native_point *point = NULL;
      result = nvkmd_nvgpu_native_syncobj_peek_point(sync, &point);
      if (result == VK_SUCCESS && point) {
         if (point->progress != ctx->progress ||
             (point->state != NVKMD_NVGPU_POINT_SUBMITTED &&
              point->state != NVKMD_NVGPU_POINT_COMPLETE)) {
            nvkmd_nvgpu_native_point_unref(point);
            result = VK_NOT_READY;
         } else if (nvkmd_nvgpu_native_point_is_complete(point)) {
            nvkmd_nvgpu_native_point_unref(point);
         } else {
            *point_out = point;
         }
      }
   }
   if (timeline_point)
      vk_sync_timeline_point_unref(device, timeline_point);
   return result;
}

/* The point a signal of this submission publishes: its own last batch, or
 * a newer same-channel dependency it waited on without new work. */
static struct nvkmd_nvgpu_native_point *
native_ctx_signal_point(struct nvkmd_nvgpu_native_ctx *ctx)
{
   return native_point_newer(ctx->last_point, ctx->dependency);
}

/* A shim acquire for a point of another channel of this process: the
 * syncpoint shim of the point's producer, at its target value. */
static VkResult
native_ctx_add_channel_wait(struct nvkmd_nvgpu_native_ctx *ctx,
                            struct vk_object_base *log_obj,
                            const struct nvkmd_nvgpu_native_point *point)
{
   const struct nvkmd_nvgpu_native_runtime *runtime = ctx->owner->runtime;
   if (!runtime->as_post.shim_selected || !runtime->as_post.shim_stride ||
       point->producer_id >= runtime->as_post.shim_count)
      return VK_ERROR_DEVICE_LOST;
   if (ctx->pending_count ||
       ctx->wait_words == NVKMD_NVGPU_NATIVE_BATCH_MAX_WAIT_WORDS) {
      VkResult result = native_ctx_flush_locked(&ctx->base, log_obj);
      if (result != VK_SUCCESS)
         return result;
   }
   const uint64_t shim = runtime->as_post.shim_gpu_va +
      (uint64_t)point->producer_id * runtime->as_post.shim_stride;
   nvkmd_nvgpu_native_shim_acquire_encode(&ctx->waits[ctx->wait_words],
                                          shim, point->target);
   ctx->wait_words += NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS;
   ctx->channel_wait = true;
   ctx->owner->stats.channel_waits++;
   return VK_SUCCESS;
}

static VkResult
native_ctx_wait_locked(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                       uint32_t wait_count, const struct vk_sync_wait *waits)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (ctx->failed || ctx->owner->submission_failed ||
       ctx->destroyed)
      return VK_ERROR_DEVICE_LOST;
   if (wait_count && !waits)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (wait_count) {
      ctx->owner->stats.ctx_waits++;
      ctx->owner->stats.ctx_wait_syncs += wait_count;
   }
   for (uint32_t i = 0; i < wait_count; i++) {
      /* A VI2 release: a shim acquire at the head of the next batch. Work
       * already pending is not covered by it.
       */
      if (waits[i].sync->type == &nvkmd_nvgpu_native_release_type) {
         const struct nvkmd_nvgpu_native_release *release =
            container_of(waits[i].sync, struct nvkmd_nvgpu_native_release,
                         base);
         if (ctx->pending_count ||
             ctx->wait_words == NVKMD_NVGPU_NATIVE_BATCH_MAX_WAIT_WORDS) {
            VkResult result = native_ctx_flush_locked(base, log_obj);
            if (result != VK_SUCCESS)
               return result;
         }
         nvkmd_nvgpu_native_shim_acquire_encode(
            &ctx->waits[ctx->wait_words], release->shim_addr,
            release->threshold_lo);
         ctx->wait_words += NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS;
         continue;
      }
      struct nvkmd_nvgpu_native_point *point = NULL;
      VkResult result = native_ctx_resolve_wait(ctx, log_obj->device,
                                                &waits[i], &point);
      if (result == VK_SUCCESS && point &&
          point->producer_id != ctx->endpoint->producer_id) {
         if (ctx->endpoint == &ctx->owner->endpoint &&
             !(ouncevk_debug_sync_flags & 4)) {
            /* A point of the copy channel: the GPU waits for its counter
             * with a shim acquire at the head of the next batch.
             * Copy batches end with a wait-for-idle release, so the counter
             * passes only after their writes. Work already pending is not
             * covered by it. */
            result = native_ctx_add_channel_wait(ctx, log_obj, point);
         } else {
            /* A point of the primary channel: its batches end with a
             * pipelined 3D release, so its counter can pass before the 3D
             * pipeline has written the work (CTS
             * synchronization.op.multi_queue image cases). Wait on the CPU
             * for the point's verified completion. */
            while (result == VK_SUCCESS &&
                   !nvkmd_nvgpu_native_point_is_complete(point)) {
               result = ctx->owner->inflight_first ?
                  native_owner_join_first(ctx->owner) :
                  native_ctx_fail(ctx);
            }
         }
         nvkmd_nvgpu_native_point_unref(point);
         if (result != VK_SUCCESS)
            return result;
         continue;
      }
      if (result == VK_SUCCESS) {
         if (point) {
            ctx->owner->stats.deferred_waits++;
            /* Work already pending is not covered by this wait. */
            if (!ctx->dependency && ctx->pending_count) {
               result = native_ctx_flush_locked(base, log_obj);
               if (result != VK_SUCCESS) {
                  nvkmd_nvgpu_native_point_unref(point);
                  return result;
               }
            }
            /* Keep the newest. It implies the others. */
            struct nvkmd_nvgpu_native_point *keep =
               native_point_newer(ctx->dependency, point);
            struct nvkmd_nvgpu_native_point *drop =
               keep == point ? ctx->dependency : point;
            ctx->dependency = keep;
            nvkmd_nvgpu_native_point_unref(drop);
         }
         continue;
      }
      if (result != VK_NOT_READY)
         return result;
      /* Not on this channel yet: wait on the CPU without the owner lock,
       * so the thread that will signal it can still submit. */
      native_ctx_unlock(ctx);
      result = vk_sync_wait(
         log_obj->device, waits[i].sync, waits[i].wait_value,
         VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(native_ctx_wait_ns));
      if (!native_ctx_lock(ctx))
         return VK_ERROR_DEVICE_LOST;
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static VkResult
native_ctx_exec_locked(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                       uint32_t count, const struct nvkmd_ctx_exec *execs)
{
   (void)log_obj;
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (ctx->failed || ctx->owner->submission_failed ||
       ctx->destroyed)
      return VK_ERROR_DEVICE_LOST;
   if (count && !execs)
      return VK_ERROR_INITIALIZATION_FAILED;
   ctx->owner->stats.execs++;
   ctx->owner->stats.exec_spans += count;
   uint64_t exec_bytes = 0;
   for (uint32_t i = 0; i < count; i++)
      exec_bytes += execs[i].size_B;
   ctx->owner->stats.exec_bytes += exec_bytes;
   if (exec_bytes <= 256)
      ctx->owner->stats.small_execs++;
   for (uint32_t i = 0; i < count;) {
      /* A pending dependency takes one entry for its head barrier. */
      const uint32_t max_spans =
         NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS - (ctx->dependency != NULL);
      uint32_t run = 1;
      while (i + run < count && execs[i + run - 1].incomplete)
         run++;
      if (run > max_spans)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      uint64_t run_bytes = 0;
      for (uint32_t j = 0; j < run; j++) {
         if (run_bytes > UINT64_MAX - execs[i + j].size_B)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         run_bytes += execs[i + j].size_B;
      }
      const uint64_t tail_bytes =
         NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS * 4 + 12;
      if (ctx->owner->arena_ref->length < tail_bytes ||
          run_bytes > ctx->owner->arena_ref->length - tail_bytes)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      if (run > max_spans - ctx->pending_count ||
          (ctx->pending_count &&
           (ctx->pending_bytes >= NATIVE_CTX_COPY_CHUNK_B ||
            run_bytes > NATIVE_CTX_COPY_CHUNK_B - ctx->pending_bytes))) {
         if (ctx->pending[ctx->pending_count - 1].incomplete)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         VkResult result = native_ctx_flush_locked(base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }
      ctx->pending_bytes += run_bytes;
      for (uint32_t j = 0; j < run; j++, i++) {
         ctx->pending[ctx->pending_count++] =
            (struct nvkmd_nvgpu_native_span) {
               .gpu_addr = execs[i].addr,
               .size_B = execs[i].size_B,
               .incomplete = execs[i].incomplete,
            };
      }
   }
   return VK_SUCCESS;
}

static VkResult
native_ctx_bind(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                uint32_t count, const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (count && !binds)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   /* A bind is an immediate worker request on the CPU, not a queue
    * command. So all earlier GPU work on both channels, which
    * includes every point that this submission waits for, completes
    * first. */
   VkResult result = VK_SUCCESS;
   while (result == VK_SUCCESS && ctx->owner->inflight_first)
      result = native_owner_join_first(ctx->owner);
   ctx->wait_words = 0;
   ctx->channel_wait = false;
   for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
      const struct nvkmd_ctx_bind *bind = &binds[i];
      if (bind->op == NVKMD_BIND_OP_BIND)
         result = nvkmd_va_bind_mem(bind->va, log_obj, bind->va_offset_B,
                                    bind->mem, bind->mem_offset_B,
                                    bind->range_B);
      else
         result = nvkmd_va_unbind(bind->va, log_obj, bind->va_offset_B,
                                  bind->range_B);
   }
   native_ctx_unlock(ctx);
   return result;
}

static VkResult
native_ctx_signal_locked(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                         uint32_t count, const struct vk_sync_signal *signals)
{
   if (native_ctx(base)->owner->retiring)
      return VK_ERROR_DEVICE_LOST;
   if (count && !signals)
      return VK_ERROR_INITIALIZATION_FAILED;
   native_ctx(base)->owner->stats.signals++;
   struct vk_sync_timeline_point **timeline_points = NULL;
   if (count) {
      timeline_points = calloc(count, sizeof(*timeline_points));
      if (!timeline_points)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < count; i++) {
      struct vk_sync_timeline *timeline = vk_sync_as_timeline(signals[i].sync);
      if (!timeline)
         continue;
      result = vk_sync_timeline_alloc_point(log_obj->device, timeline,
                                            signals[i].signal_value,
                                            &timeline_points[i]);
      if (result != VK_SUCCESS)
         goto cleanup;
   }
   result = native_ctx_flush_locked(base, log_obj);
   if (result != VK_SUCCESS)
      goto cleanup;
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   for (uint32_t i = 0; i < count; i++) {
      struct vk_sync *target = timeline_points[i] ?
         &timeline_points[i]->sync : signals[i].sync;
      if (target->type != &nvkmd_nvgpu_native_syncobj_type)
         continue;
      struct nvkmd_nvgpu_native_point *signal_point =
         native_ctx_signal_point(ctx);
      if (signal_point)
         result = nvkmd_nvgpu_native_syncobj_publish_point(
            target, signal_point);
      else
         result = vk_sync_signal(log_obj->device, target, 0);
      if (result != VK_SUCCESS) {
         result = native_ctx_signal_error(ctx, result);
         goto cleanup;
      }
      if (timeline_points[i]) {
         struct vk_sync_timeline_point *point = timeline_points[i];
         timeline_points[i] = NULL; /* install consumes even on broadcast error */
         result = vk_sync_timeline_point_install(log_obj->device, point);
         if (result != VK_SUCCESS) {
            result = native_ctx_signal_error(ctx, result);
            goto cleanup;
         }
      }
   }
   /* Native sync objects and timeline points now carry the pending point.
    * Their waiters finish the work. Only a foreign sync type, which can be
    * signaled only on the CPU, still needs the batch complete before signal. */
   bool cpu_signal = false;
   for (uint32_t i = 0; i < count; i++) {
      if (signals[i].sync->type != &nvkmd_nvgpu_native_syncobj_type &&
          !vk_sync_as_timeline(signals[i].sync))
         cpu_signal = true;
   }
   if (cpu_signal) {
      result = native_ctx_drain(ctx);
      if (result != VK_SUCCESS)
         goto cleanup;
   }
   for (uint32_t i = 0; i < count; i++) {
      if (signals[i].sync->type == &nvkmd_nvgpu_native_syncobj_type ||
          vk_sync_as_timeline(signals[i].sync))
         continue;
      result = vk_sync_signal(log_obj->device,
                              signals[i].sync, signals[i].signal_value);
      if (result != VK_SUCCESS)
         goto cleanup;
   }
cleanup:
   for (uint32_t i = 0; i < count; i++) {
      if (timeline_points[i])
         vk_sync_timeline_point_unref(log_obj->device, timeline_points[i]);
   }
   free(timeline_points);
   /* The signal ends the submission. Its waits do not order later ones. */
   if (result == VK_SUCCESS) {
      nvkmd_nvgpu_native_point_unref(native_ctx(base)->dependency);
      native_ctx(base)->dependency = NULL;
   }
   return result;
}

static VkResult
native_ctx_sync_locked(struct nvkmd_ctx *base, struct vk_object_base *log_obj)
{
   native_ctx(base)->owner->stats.syncs++;
   VkResult result = native_ctx_flush_locked(base, log_obj);
   if (result != VK_SUCCESS)
      return result;
   return native_ctx_drain(native_ctx(base));
}

static VkResult
native_ctx_flush(struct nvkmd_ctx *base, struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_ctx_flush_locked(base, log_obj);
   native_ctx_unlock(ctx);
   return result;
}

static void
native_ctx_destroy(struct nvkmd_ctx *base)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   struct nvkmd_nvgpu_native_device_owner *owner = ctx->owner;
   if (!nvkmd_nvgpu_native_owner_lock(owner)) {
      /* Cannot drain: keep the context and its storage (as before). */
      ctx->destroyed = true;
      mesa_loge("native: owner lock failed at context destroy; the primary "
                "arena is poisoned");
      nvkmd_nvgpu_native_arena_poison(owner->arena_ref);
      return;
   }
   native_ctx_destroy_locked(base); /* may free ctx */
   nvkmd_nvgpu_native_owner_unlock(owner);
}

static VkResult
native_ctx_wait(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                uint32_t wait_count, const struct vk_sync_wait *waits)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result =
      native_ctx_wait_locked(base, log_obj, wait_count, waits);
   native_ctx_unlock(ctx);
   return result;
}

static VkResult
native_ctx_exec(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                uint32_t count, const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_ctx_exec_locked(base, log_obj, count, execs);
   native_ctx_unlock(ctx);
   return result;
}

static VkResult
native_ctx_signal(struct nvkmd_ctx *base, struct vk_object_base *log_obj,
                  uint32_t count, const struct vk_sync_signal *signals)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result =
      native_ctx_signal_locked(base, log_obj, count, signals);
   native_ctx_unlock(ctx);
   return result;
}

static VkResult
native_ctx_sync(struct nvkmd_ctx *base, struct vk_object_base *log_obj)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   if (!native_ctx_lock(ctx))
      return VK_ERROR_DEVICE_LOST;
   const VkResult result = native_ctx_sync_locked(base, log_obj);
   native_ctx_unlock(ctx);
   return result;
}

static VkResult
native_ctx_set_state_replay(struct nvkmd_ctx *base, const uint32_t *words,
                            uint32_t word_count)
{
   struct nvkmd_nvgpu_native_ctx *ctx = native_ctx(base);
   uint32_t *copy = NULL;
   if (word_count) {
      copy = malloc(word_count * sizeof(*copy));
      if (!copy)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      memcpy(copy, words, word_count * sizeof(*copy));
   }
   if (!native_ctx_lock(ctx)) {
      free(copy);
      return VK_ERROR_DEVICE_LOST;
   }
   uint32_t *old = ctx->state_replay;
   ctx->state_replay = copy;
   ctx->state_replay_words = word_count;
   native_ctx_unlock(ctx);
   free(old);
   return VK_SUCCESS;
}

static const struct nvkmd_ctx_ops native_ctx_ops = {
   .destroy = native_ctx_destroy,
   .wait = native_ctx_wait,
   .exec = native_ctx_exec,
   .bind = native_ctx_bind,
   .signal = native_ctx_signal,
   .flush = native_ctx_flush,
   .sync = native_ctx_sync,
   .set_state_replay = native_ctx_set_state_replay,
};

VkResult
nvkmd_nvgpu_native_ctx_create(
   struct nvkmd_dev *dev,
   struct nvkmd_nvgpu_native_device_owner *owner,
   enum nvkmd_engines engines,
   struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   uint32_t *active_context_count,
   struct nvkmd_ctx **ctx_out)
{
   if (!ctx_out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *ctx_out = NULL;
   const bool copy_endpoint = owner && endpoint == &owner->copy.endpoint;
   if (!dev || !active_context_count || !endpoint ||
       (endpoint != &owner->endpoint && !copy_endpoint) ||
       (copy_endpoint && (!owner->copy.ready || engines != NVKMD_ENGINE_COPY)) ||
       !nvkmd_nvgpu_native_device_common_valid(owner) ||
       owner->retiring ||
       !owner->progress ||
       (engines != NVKMD_ENGINE_BIND &&
        (!(engines & (NVKMD_ENGINE_COMPUTE | NVKMD_ENGINE_COPY)) ||
         (engines & (NVKMD_ENGINE_BIND | NVKMD_ENGINE_2D |
                     NVKMD_ENGINE_M2MF))))) {
      nvkmd_nvgpu_native_owner_log_refusal(owner, "create context");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   ouncevk_debug_sync_flags_load();
   struct nvkmd_nvgpu_native_ctx *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   ctx->base.ops = &native_ctx_ops;
   ctx->base.dev = dev;
   ctx->owner = owner;
   ctx->endpoint = endpoint;
   ctx->engines = engines;
   ctx->progress = owner->progress;
   VkResult result = nvkmd_nvgpu_native_progress_context_id(
      ctx->progress, &ctx->context_id);
   if (result != VK_SUCCESS) {
      free(ctx);
      return result;
   }
   nvkmd_nvgpu_native_progress_ref(ctx->progress);
   ctx->active_context_count = active_context_count;
   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

static void
native_free_submission_list(struct nvkmd_nvgpu_native_submission *submission)
{
   while (submission) {
      struct nvkmd_nvgpu_native_submission *next = submission->next;
      free(submission);
      submission = next;
   }
}

void
nvkmd_nvgpu_native_ctx_free_submissions(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   native_free_submission_list(owner->recycled_submissions);
   owner->recycled_submissions = NULL;
   native_free_submission_list(owner->quarantined_submissions);
   owner->quarantined_submissions = NULL;
}
