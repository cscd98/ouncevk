/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_COMPUTE_INFO_H
#define NVKMD_NVGPU_NATIVE_COMPUTE_INFO_H

#include "nvkmd_nvgpu_native_profile.h"
#include "nv_device_info.h"

/* This projection is only for the opt-in, zero-shared compute experiment.
 * It is not a Vulkan capability, a physical-device acceptance or a native SM
 * identity. The caller must own a native channel separately. The caller must
 * also verify the memory, submission, completion and advertised API behavior
 * of that channel. */
enum nvkmd_nvgpu_native_compute_info_status {
   NVKMD_NVGPU_COMPUTE_INFO_CANDIDATE,
   NVKMD_NVGPU_COMPUTE_INFO_NOT_ENABLED,
   NVKMD_NVGPU_COMPUTE_INFO_ARGUMENT,
   NVKMD_NVGPU_COMPUTE_INFO_PROFILE,
   NVKMD_NVGPU_COMPUTE_INFO_POLICY,
};

enum nvkmd_nvgpu_native_compute_info_status
nvkmd_nvgpu_native_compute_info(
   const struct nvkmd_nvgpu_native_characteristics *chars,
   const struct nvkmd_nvgpu_native_resource_policy *policy,
   bool allow_experimental_sm86,
   struct nv_device_info *out);

#endif
