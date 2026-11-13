/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_PROGRESS_H
#define NVKMD_NVGPU_NATIVE_PROGRESS_H

#include "nvkmd_nvgpu_native_completion.h"

#include "c11/threads.h"
#include "util/cnd_monotonic.h"

#include <stdatomic.h>

#include <vulkan/vulkan_core.h>

struct nvkmd_nvgpu_native_device_owner;
struct nvkmd_nvgpu_native_submission;
struct vk_device;
struct vk_sync_timeline_state;
struct nvkmd_nvgpu_native_timeline;

enum nvkmd_nvgpu_native_point_state {
   NVKMD_NVGPU_POINT_PREPARED,
   NVKMD_NVGPU_POINT_SUBMITTED,
   NVKMD_NVGPU_POINT_COMPLETE,
};

/* One progress domain per physical channel. The current owner stays for the
 * process lifetime. Its root reference keeps this domain alive. No service
 * call or GPU wait may run under mutex. */
struct nvkmd_nvgpu_native_progress {
   atomic_uint refs;
   mtx_t mutex;
   struct u_cnd_monotonic changed;
   struct nvkmd_nvgpu_native_device_owner *owner;
   uint64_t next_serial, next_context_id;
   VkResult lost;
   struct nvkmd_nvgpu_native_timeline *timelines;
};

/* Independent of the recyclable submission record. The physical channel
 * borrows the record. A sync or waiter may hold this point after the record is
 * reused. */
struct nvkmd_nvgpu_native_point {
   atomic_uint refs;
   struct nvkmd_nvgpu_native_progress *progress;
   struct nvkmd_nvgpu_native_submission *storage;
   struct nvkmd_nvgpu_native_completion completion;
   uint64_t generation, serial, context_id;
   uint32_t producer_id, target;
   enum nvkmd_nvgpu_native_point_state state;
};

VkResult nvkmd_nvgpu_native_progress_create(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_progress **out);
void nvkmd_nvgpu_native_progress_ref(struct nvkmd_nvgpu_native_progress *progress);
void nvkmd_nvgpu_native_progress_unref(struct nvkmd_nvgpu_native_progress *progress);
VkResult nvkmd_nvgpu_native_progress_context_id(
   struct nvkmd_nvgpu_native_progress *progress, uint64_t *out);
void nvkmd_nvgpu_native_progress_fail(
   struct nvkmd_nvgpu_native_progress *progress, VkResult error);
VkResult nvkmd_nvgpu_native_progress_register_timeline(
   struct nvkmd_nvgpu_native_progress *progress,
   struct vk_device *device, struct vk_sync_timeline_state *state);
void nvkmd_nvgpu_native_progress_unregister_timeline(
   struct nvkmd_nvgpu_native_progress *progress,
   struct vk_device *device, struct vk_sync_timeline_state *state);

VkResult nvkmd_nvgpu_native_point_create(
   struct nvkmd_nvgpu_native_progress *progress, uint64_t context_id,
   struct nvkmd_nvgpu_native_point **out);
void nvkmd_nvgpu_native_point_ref(struct nvkmd_nvgpu_native_point *point);
void nvkmd_nvgpu_native_point_unref(struct nvkmd_nvgpu_native_point *point);
VkResult nvkmd_nvgpu_native_point_bind_storage(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage);
VkResult nvkmd_nvgpu_native_point_discard_unsent(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage);
VkResult nvkmd_nvgpu_native_point_accept(
   struct nvkmd_nvgpu_native_point *point,
   const struct nvkmd_nvgpu_native_submit *submit);
VkResult nvkmd_nvgpu_native_point_release_storage(
   struct nvkmd_nvgpu_native_point *point,
   struct nvkmd_nvgpu_native_submission *storage);
VkResult nvkmd_nvgpu_native_point_complete(
   struct nvkmd_nvgpu_native_point *point);
bool nvkmd_nvgpu_native_point_is_complete(
   struct nvkmd_nvgpu_native_point *point);
/* The native producer a consumer outside this driver (VI2 present) can
 * wait on, without a CPU wait. VK_SUCCESS with *pending true: the accepted
 * {returned_id, returned_target} of the point's submission. VK_SUCCESS with
 * *pending false: already complete. VK_NOT_READY: prepared, not submitted. */
VkResult nvkmd_nvgpu_native_point_dependency(
   struct nvkmd_nvgpu_native_point *point, bool *pending,
   uint32_t *producer_id, uint32_t *target);
/* Waits for a point. On the channel owner's TLS thread, the waiter itself
 * finishes accepted work in channel order. Signals no longer drain. A timeout
 * already in the past polls without blocking. Other threads wait for the owner
 * thread to complete the point. */
VkResult nvkmd_nvgpu_native_point_wait(
   struct nvkmd_nvgpu_native_point *point, bool pending_only,
   uint64_t abs_timeout_ns);

/* Implemented by the native context (nvkmd_nvgpu_native_ctx.c). */
bool nvkmd_nvgpu_native_owner_can_drive(
   const struct nvkmd_nvgpu_native_device_owner *owner);
VkResult nvkmd_nvgpu_native_owner_drive_point(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_point *point, bool block);

#endif
