/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_PDEV_H
#define NVKMD_NVGPU_NATIVE_PDEV_H

#include "nvkmd/nvkmd.h"
#include "nvkmd_nvgpu_native_device_owner.h"

/* Creates the local backend object for one accepted native owner. Creation
 * alone does not expose the pdev through vkEnumeratePhysicalDevices. The
 * common Vulkan properties, queue and device paths must first be narrowed. */
VkResult nvkmd_nvgpu_native_create_pdev(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nv_device_info *info,
   enum nvk_debug debug_flags,
   struct nvkmd_pdev **pdev_out);

#endif
