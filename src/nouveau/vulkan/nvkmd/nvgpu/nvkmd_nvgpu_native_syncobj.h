/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_SYNCOBJ_H
#define NVKMD_NVGPU_NATIVE_SYNCOBJ_H

#include "vk_sync.h"

struct nvkmd_nvgpu_native_point;

/* A binary sync can refer to an accepted completion point before the
 * current signal-time drain. A completed wait still requires the channel
 * counter, the GPU marker, CPU visibility and a new health certificate. */
extern const struct vk_sync_type nvkmd_nvgpu_native_syncobj_type;

/* Publishes an accepted point before the caller's signal-time drain. A
 * Vulkan waiter owns a point reference that does not depend on the reusable
 * descriptor. */
VkResult nvkmd_nvgpu_native_syncobj_publish_point(
   struct vk_sync *sync, struct nvkmd_nvgpu_native_point *point);

/* Non-blocking view of what a wait on this sync depends on. VK_SUCCESS with
 * *point NULL: already signaled. VK_SUCCESS with *point: the published
 * point, with a reference the caller owns. VK_NOT_READY: nothing is
 * published yet (a wait before the signal). The caller must wait on the
 * CPU. */
VkResult nvkmd_nvgpu_native_syncobj_peek_point(
   struct vk_sync *sync, struct nvkmd_nvgpu_native_point **point);

/* Gives the native producer that a present can pass to VI2 instead of a CPU
 * wait. See nvkmd_nvgpu_native_point_dependency. A signaled sync is
 * complete.
 * VK_NOT_READY (nothing submitted yet, or a sync of another type) means the
 * caller must wait on the CPU.
 */
VkResult nvkmd_nvgpu_native_syncobj_dependency(
   struct vk_sync *sync, bool *pending, uint32_t *producer_id,
   uint32_t *target);

#endif
