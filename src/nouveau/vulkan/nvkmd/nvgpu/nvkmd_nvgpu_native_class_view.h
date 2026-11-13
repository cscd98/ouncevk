/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_CLASS_VIEW_H
#define NVKMD_NVGPU_NATIVE_CLASS_VIEW_H

#include "nvkmd_nvgpu_native_runtime.h"

/* A private, temporary association of the accepted normalized
 * type0/instance0 selector with the separately reduced 3D and compute
 * classes. It is not a channel selector, nv_device_info, shader target or
 * device readiness result. The two FIFO fields describe different
 * namespaces on purpose. Their relationship is unknown, even when
 * associated is true.
 */
struct nvkmd_nvgpu_native_class_view {
   bool associated;
   uint64_t generation;
   uint32_t normalized_type, instance, selector;
   uint32_t class_3d, class_compute;
   uint32_t descriptor_fifo_class, reported_fifo_class;
};

/* A pure observation under the serialized owner contract of the runtime.
 * Validate it again at each use and after each owner change. The generation
 * alone does not show that the owner is still alive. All refusal paths
 * return an all-zero value. */
struct nvkmd_nvgpu_native_class_view
nvkmd_nvgpu_native_class_view(const struct nvkmd_nvgpu_native_runtime *r);

#endif
