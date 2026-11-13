/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_RELEASE_H
#define NVKMD_NVGPU_NATIVE_RELEASE_H

#include "vulkan/runtime/vk_sync.h"

#include <stdint.h>

#include <switch.h>

/* A VI2 image release as a Vulkan sync payload: the temporary payload of an
 * acquire semaphore or fence. Queue waits on it become a Host acquire on the
 * counter's syncpoint shim slot. CPU waits poll raw 0x77 with the full mode-1
 * comparison.
 */
struct nvkmd_nvgpu_native_release {
   struct vk_sync base;
   /* GPU wait: shim slot address and low 32 bits of the threshold. */
   uint64_t shim_addr;
   uint32_t threshold_lo;
   /* CPU wait: the full dependency and the GSV sync session. */
   uint32_t token;
   uint8_t mode;
   uint64_t threshold;
   GsvSync *sync;
};

extern const struct vk_sync_type nvkmd_nvgpu_native_release_type;

/* Creates a release payload for the 16-byte VI2 dependency (u32 token,
 * u8 mode, 3 zero bytes, u64 threshold). The shim slot is base + id *
 * stride with id = ~token < count. VK_ERROR_FEATURE_NOT_PRESENT when the AS
 * has no shim or the id is out of range.
 */
VkResult nvkmd_nvgpu_native_release_create(
   struct vk_device *device, const uint8_t release[16],
   uint64_t shim_base, uint32_t shim_stride, uint32_t shim_count,
   GsvSync *sync, struct vk_sync **sync_out);

#endif
