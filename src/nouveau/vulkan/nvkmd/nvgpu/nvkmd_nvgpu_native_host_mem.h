/* SPDX-License-Identifier: MIT
 * Memory imported from a host pointer (VK_EXT_external_memory_host).
 *
 * The program owns the memory and keeps it. The import registers the span
 * with GSV as mode-1 backings, with its contents, and maps them at
 * consecutive offsets of one VA range from the owner's VA heap. The map is
 * GPU-cached, as for readback segments: batches end with the L2 dirty flush
 * and start with the L2 invalidate. The CPU view is the program's own
 * pointer, CPU-cached, so flush and invalidate run dc civac.
 *
 * The span is split where its kernel memory blocks end, and into parts of
 * at most NVKMD_NVGPU_NATIVE_HOST_PART_B, each registered on its own.
 */
#ifndef NVKMD_NVGPU_NATIVE_HOST_MEM_H
#define NVKMD_NVGPU_NATIVE_HOST_MEM_H

#include "nvkmd/nvkmd.h"
#include "nvkmd_nvgpu_native_device_owner.h"

/* The largest span registered as one backing: the size of the primary
 * segment, the largest registration that has run live. */
#define NVKMD_NVGPU_NATIVE_HOST_PART_B NVKMD_NVGPU_NATIVE_PRIMARY_B

/* The alignment of an imported pointer and size (one CPU page). It is
 * VkPhysicalDeviceExternalMemoryHostPropertiesEXT::
 * minImportedHostPointerAlignment. */
#define NVKMD_NVGPU_NATIVE_HOST_ALIGN_B 4096

/* True when the CPU page at address can be imported. */
bool nvkmd_nvgpu_native_host_ptr_importable(const void *address);

/* Imports [host_ptr, +size_B). Both are multiples of
 * NVKMD_NVGPU_NATIVE_HOST_ALIGN_B. Returns VK_ERROR_INVALID_EXTERNAL_HANDLE
 * when the span is not importable memory or the service refuses it. Call
 * without the owner lock. */
VkResult nvkmd_nvgpu_native_host_mem_create(
   struct nvkmd_dev *dev, struct nvkmd_nvgpu_native_device_owner *owner,
   void *host_ptr, uint64_t size_B, enum nvkmd_mem_flags flags,
   struct nvkmd_mem **mem_out);

/* At device close: unmaps and releases the imports that the program did
 * not free. Their memory objects stay until freed. Returns false when a
 * release failed. Call with the owner lock held. */
bool nvkmd_nvgpu_native_owner_release_host_mems(
   struct nvkmd_nvgpu_native_device_owner *owner);

#endif
