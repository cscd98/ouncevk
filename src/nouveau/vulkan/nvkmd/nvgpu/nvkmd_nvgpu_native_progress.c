/* SPDX-License-Identifier: MIT */
/* OunceVK lifetime owner for accepted native GPU progress. The completion tuple
 * comes from the validated submit reply. The point survives independent of
 * the recycled command descriptor storage. */
#include "nvkmd_nvgpu_native_progress.h"

#include "nvkmd_nvgpu_native_device_owner.h"

#include "util/os_time.h"
#include "util/timespec.h"
#include "vk_sync_timeline.h"

#include <assert.h>
#include <stdlib.h>

struct nvkmd_nvgpu_native_timeline {
   struct nvkmd_nvgpu_native_timeline *next;
   struct vk_device *device;
   struct vk_sync_timeline_state *state;
   bool notified;
};

VkResult
nvkmd_nvgpu_native_progress_create(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_progress **out)
{
   if (!out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   if (!nvkmd_nvgpu_native_device_common_valid(owner))
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = calloc(1, sizeof(*progress));
   if (!progress)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mtx_init(&progress->mutex, mtx_plain) != thrd_success) {
      free(progress);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (u_cnd_monotonic_init(&progress->changed) != thrd_success) {
      mtx_destroy(&progress->mutex);
      free(progress);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   atomic_init(&progress->refs, 1);
   progress->owner = owner;
   progress->lost = VK_SUCCESS;
   *out = progress;
   return VK_SUCCESS;
}

void
nvkmd_nvgpu_native_progress_ref(struct nvkmd_nvgpu_native_progress *progress)
{
   assert(progress && atomic_load(&progress->refs));
   atomic_fetch_add(&progress->refs, 1);
}

void
nvkmd_nvgpu_native_progress_unref(struct nvkmd_nvgpu_native_progress *progress)
{
   if (!progress)
      return;
   assert(atomic_load(&progress->refs));
   if (atomic_fetch_sub(&progress->refs, 1) != 1)
      return;
   assert(!progress->timelines);
   u_cnd_monotonic_destroy(&progress->changed);
   mtx_destroy(&progress->mutex);
   free(progress);
}

VkResult
nvkmd_nvgpu_native_progress_context_id(
   struct nvkmd_nvgpu_native_progress *progress, uint64_t *out)
{
   if (!progress || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   mtx_lock(&progress->mutex);
   const VkResult result = progress->lost != VK_SUCCESS ||
                           progress->next_context_id == UINT64_MAX ?
                           VK_ERROR_DEVICE_LOST : VK_SUCCESS;
   if (result == VK_SUCCESS)
      *out = ++progress->next_context_id;
   mtx_unlock(&progress->mutex);
   return result;
}

void
nvkmd_nvgpu_native_progress_fail(
   struct nvkmd_nvgpu_native_progress *progress, VkResult error)
{
   if (!progress || error == VK_SUCCESS)
      return;
   mtx_lock(&progress->mutex);
   if (progress->lost == VK_SUCCESS)
      progress->lost = error;
   u_cnd_monotonic_broadcast(&progress->changed);
   mtx_unlock(&progress->mutex);

   /* Never take a timeline mutex while holding the progress mutex. Mark one
    * registered state at a time, retain it, then notify outside our lock.
    * This also avoids a fixed cap or allocation in the loss path. */
   for (;;) {
      struct vk_sync_timeline_state *state = NULL;
      struct vk_device *device = NULL;
      mtx_lock(&progress->mutex);
      for (struct nvkmd_nvgpu_native_timeline *entry = progress->timelines;
           entry; entry = entry->next) {
         if (entry->notified)
            continue;
         entry->notified = true;
         state = entry->state;
         device = entry->device;
         vk_sync_timeline_state_ref(state);
         break;
      }
      error = progress->lost;
      mtx_unlock(&progress->mutex);
      if (!state)
         break;
      vk_sync_timeline_state_fail(state, error);
      vk_sync_timeline_state_unref(device, state);
   }
}

VkResult
nvkmd_nvgpu_native_progress_register_timeline(
   struct nvkmd_nvgpu_native_progress *progress,
   struct vk_device *device, struct vk_sync_timeline_state *state)
{
   if (!progress || !device || !state)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_timeline *entry = calloc(1, sizeof(*entry));
   if (!entry)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   entry->device = device;
   entry->state = state;
   mtx_lock(&progress->mutex);
   const VkResult result = progress->lost;
   if (result == VK_SUCCESS) {
      vk_sync_timeline_state_ref(state);
      entry->next = progress->timelines;
      progress->timelines = entry;
   }
   mtx_unlock(&progress->mutex);
   if (result != VK_SUCCESS)
      free(entry);
   return result;
}

void
nvkmd_nvgpu_native_progress_unregister_timeline(
   struct nvkmd_nvgpu_native_progress *progress,
   struct vk_device *device, struct vk_sync_timeline_state *state)
{
   if (!progress || !device || !state)
      return;
   mtx_lock(&progress->mutex);
   struct nvkmd_nvgpu_native_timeline **link = &progress->timelines;
   while (*link && ((*link)->state != state || (*link)->device != device))
      link = &(*link)->next;
   struct nvkmd_nvgpu_native_timeline *entry = *link;
   if (entry)
      *link = entry->next;
   const VkResult error = progress->lost;
   mtx_unlock(&progress->mutex);
   if (!entry)
      return;
   if (error != VK_SUCCESS)
      vk_sync_timeline_state_fail(state, error);
   vk_sync_timeline_state_unref(device, state);
   free(entry);
}

VkResult
nvkmd_nvgpu_native_point_create(
   struct nvkmd_nvgpu_native_progress *progress, uint64_t context_id,
   struct nvkmd_nvgpu_native_point **out)
{
   if (!out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   if (!progress || !context_id)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_point *point = calloc(1, sizeof(*point));
   if (!point)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   mtx_lock(&progress->mutex);
   if (progress->lost != VK_SUCCESS || progress->next_serial == UINT64_MAX) {
      mtx_unlock(&progress->mutex);
      free(point);
      return VK_ERROR_DEVICE_LOST;
   }
   point->serial = ++progress->next_serial;
   point->generation = progress->owner->endpoint.generation;
   point->context_id = context_id;
   point->state = NVKMD_NVGPU_POINT_PREPARED;
   point->progress = progress;
   atomic_init(&point->refs, 1);
   nvkmd_nvgpu_native_progress_ref(progress);
   mtx_unlock(&progress->mutex);
   *out = point;
   return VK_SUCCESS;
}

void
nvkmd_nvgpu_native_point_ref(struct nvkmd_nvgpu_native_point *point)
{
   assert(point && atomic_load(&point->refs));
   atomic_fetch_add(&point->refs, 1);
}

void
nvkmd_nvgpu_native_point_unref(struct nvkmd_nvgpu_native_point *point)
{
   if (!point)
      return;
   assert(atomic_load(&point->refs));
   if (atomic_fetch_sub(&point->refs, 1) != 1)
      return;
   assert(!point->storage && point->state != NVKMD_NVGPU_POINT_SUBMITTED);
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   free(point);
   nvkmd_nvgpu_native_progress_unref(progress);
}

VkResult
nvkmd_nvgpu_native_point_bind_storage(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage)
{
   if (!point || !storage)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool valid = progress->lost == VK_SUCCESS &&
                      point->state == NVKMD_NVGPU_POINT_PREPARED &&
                      !point->storage;
   if (valid)
      point->storage = storage;
   mtx_unlock(&progress->mutex);
   return valid ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VkResult
nvkmd_nvgpu_native_point_discard_unsent(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage)
{
   if (!point || !storage)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool valid = point->state == NVKMD_NVGPU_POINT_PREPARED &&
                      point->storage == storage;
   if (valid)
      point->storage = NULL;
   mtx_unlock(&progress->mutex);
   return valid ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VkResult
nvkmd_nvgpu_native_point_accept(
   struct nvkmd_nvgpu_native_point *point,
   const struct nvkmd_nvgpu_native_submit *submit)
{
   if (!point || !submit)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool valid = progress->lost == VK_SUCCESS &&
                      point->state == NVKMD_NVGPU_POINT_PREPARED &&
                      point->storage && submit->accepted &&
                      submit->generation == point->generation &&
                      submit->returned_id ==
                         point->completion.producer_id;
   if (valid) {
      point->producer_id = submit->returned_id;
      point->target = submit->returned_target;
      point->state = NVKMD_NVGPU_POINT_SUBMITTED;
      u_cnd_monotonic_broadcast(&progress->changed);
   }
   mtx_unlock(&progress->mutex);
   return valid ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VkResult
nvkmd_nvgpu_native_point_dependency(
   struct nvkmd_nvgpu_native_point *point, bool *pending,
   uint32_t *producer_id, uint32_t *target)
{
   if (!point || !pending || !producer_id || !target)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   struct nvkmd_nvgpu_native_submit_stats *stats =
      &progress->owner->stats;
   VkResult result = VK_SUCCESS;
   *pending = false;
   mtx_lock(&progress->mutex);
   if (progress->lost != VK_SUCCESS) {
      result = progress->lost;
   } else if (point->state == NVKMD_NVGPU_POINT_PREPARED) {
      stats->present_unsubmitted++;
      result = VK_NOT_READY;
   } else if (point->state == NVKMD_NVGPU_POINT_SUBMITTED) {
      *pending = true;
      *producer_id = point->producer_id;
      *target = point->target;
      stats->present_producers++;
   } else {
      stats->present_complete++;
   }
   mtx_unlock(&progress->mutex);
   return result;
}

VkResult
nvkmd_nvgpu_native_point_release_storage(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage)
{
   if (!point || !storage)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool valid = progress->lost == VK_SUCCESS &&
                      point->state == NVKMD_NVGPU_POINT_SUBMITTED &&
                      point->completion.complete && point->storage == storage;
   if (valid)
      point->storage = NULL;
   mtx_unlock(&progress->mutex);
   return valid ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VkResult
nvkmd_nvgpu_native_point_complete(struct nvkmd_nvgpu_native_point *point)
{
   if (!point)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool valid = progress->lost == VK_SUCCESS &&
                      point->state == NVKMD_NVGPU_POINT_SUBMITTED &&
                      !point->storage && point->completion.complete;
   if (valid) {
      point->state = NVKMD_NVGPU_POINT_COMPLETE;
      u_cnd_monotonic_broadcast(&progress->changed);
   }
   mtx_unlock(&progress->mutex);
   return valid ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

bool
nvkmd_nvgpu_native_point_is_complete(struct nvkmd_nvgpu_native_point *point)
{
   if (!point)
      return true;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   mtx_lock(&progress->mutex);
   const bool complete = point->state == NVKMD_NVGPU_POINT_COMPLETE;
   mtx_unlock(&progress->mutex);
   return complete;
}

VkResult
nvkmd_nvgpu_native_point_wait(
   struct nvkmd_nvgpu_native_point *point, bool pending_only,
   uint64_t abs_timeout_ns)
{
   if (!point)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_progress *progress = point->progress;
   /* Nothing else finishes the channel's accepted work. So a waiter drives
    * completion itself, under the owner lock and outside the progress
    * mutex. */
   if (nvkmd_nvgpu_native_owner_can_drive(progress->owner)) {
      for (;;) {
         mtx_lock(&progress->mutex);
         const VkResult lost = progress->lost;
         const bool ready = point->state == NVKMD_NVGPU_POINT_COMPLETE ||
                            (pending_only &&
                             point->state == NVKMD_NVGPU_POINT_SUBMITTED);
         mtx_unlock(&progress->mutex);
         if (lost != VK_SUCCESS)
            return lost;
         if (ready)
            return VK_SUCCESS;
         if (pending_only)
            return VK_TIMEOUT; /* A pending wait never finishes work. */
         const bool block = abs_timeout_ns > os_time_get_nano();
         const VkResult result =
            nvkmd_nvgpu_native_owner_drive_point(progress->owner, point, block);
         if (result == VK_SUCCESS)
            continue;
         if (result == VK_NOT_READY)
            return VK_TIMEOUT;
         if (result == VK_TIMEOUT) {
            /* The event hint expired. The accepted target stays valid and
             * may be joined again until the caller's deadline. */
            if (abs_timeout_ns <= os_time_get_nano())
               return VK_TIMEOUT;
            continue;
         }
         return result;
      }
   }
   struct timespec deadline;
   if (abs_timeout_ns != UINT64_MAX)
      timespec_from_nsec(&deadline, abs_timeout_ns);
   mtx_lock(&progress->mutex);
   while (progress->lost == VK_SUCCESS &&
          point->state != NVKMD_NVGPU_POINT_COMPLETE &&
          !(pending_only && point->state == NVKMD_NVGPU_POINT_SUBMITTED)) {
      const int result = abs_timeout_ns == UINT64_MAX ?
         u_cnd_monotonic_wait(&progress->changed, &progress->mutex) :
         u_cnd_monotonic_timedwait(&progress->changed, &progress->mutex,
                                   &deadline);
      if (result == thrd_timedout) {
         const bool ready = point->state == NVKMD_NVGPU_POINT_COMPLETE ||
                            (pending_only &&
                             point->state == NVKMD_NVGPU_POINT_SUBMITTED);
         const VkResult outcome = progress->lost != VK_SUCCESS ?
            progress->lost : ready ? VK_SUCCESS : VK_TIMEOUT;
         mtx_unlock(&progress->mutex);
         return outcome;
      }
      if (result != thrd_success) {
         mtx_unlock(&progress->mutex);
         return VK_ERROR_DEVICE_LOST;
      }
   }
   const VkResult outcome = progress->lost != VK_SUCCESS ?
      progress->lost : VK_SUCCESS;
   mtx_unlock(&progress->mutex);
   return outcome;
}
