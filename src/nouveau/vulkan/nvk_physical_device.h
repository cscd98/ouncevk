/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_PHYSICAL_DEVICE_H
#define NVK_PHYSICAL_DEVICE_H 1

#include "nvk_private.h"

#include "nvk_debug.h"
#include "nv_device_info.h"

#include "vk_physical_device.h"
#include "vk_sync.h"

#include "wsi_common.h"

#include <sys/types.h>

struct nak_compiler;
struct nvk_instance;
struct nvk_physical_device;
struct nvkmd_pdev;

struct nvk_queue_family {
   VkQueueFlags queue_flags;
   uint32_t queue_count;
   VkQueueGlobalPriority max_priority;
};

struct nvk_memory_heap {
   uint64_t size;
   uint64_t used;
   /* When not NULL, the heap's usage is counted here instead of in used: a
    * counter that the physical devices of every instance in the process
    * share, because they share the heap itself. */
   uint64_t *shared_used;
   VkMemoryHeapFlags flags;
   uint64_t (*available)(struct nvk_physical_device *pdev);
};

/* The counter of the memory that the process has allocated from heap. */
static inline uint64_t *
nvk_memory_heap_used(struct nvk_memory_heap *heap)
{
   return heap->shared_used ? heap->shared_used : &heap->used;
}

/* The memory that the process has allocated from heap, in bytes. */
static inline uint64_t
nvk_memory_heap_used_B(const struct nvk_memory_heap *heap)
{
   return p_atomic_read(heap->shared_used ? heap->shared_used : &heap->used);
}

struct nvk_physical_device {
   struct vk_physical_device vk;
   struct nv_device_info info;
   enum nvk_debug debug_flags;
   bool ssbo_align_4b;

   struct nvkmd_pdev *nvkmd;

   struct nak_compiler *nak;
   struct wsi_device wsi_device;

   uint8_t device_uuid[VK_UUID_SIZE];

   // TODO: add mapable VRAM heap if possible
   struct nvk_memory_heap mem_heaps[3];
   VkMemoryType mem_types[3];
   uint8_t mem_heap_count;
   uint8_t mem_type_count;

   struct nvk_queue_family queue_families[3];
   uint8_t queue_family_count;
};

static inline uint32_t
nvk_min_cbuf_alignment(const struct nv_device_info *info)
{
   return info->cls_eng3d >= 0xC597 /* TURING_A */ ? 64 : 256;
}

VK_DEFINE_HANDLE_CASTS(nvk_physical_device,
   vk.base,
   VkPhysicalDevice,
   VK_OBJECT_TYPE_PHYSICAL_DEVICE)

static inline uint32_t
nvk_use_edb_buffer_views(const struct nvk_physical_device *pdev)
{
   return pdev->debug_flags & NVK_DEBUG_FORCE_EDB_BVIEW;
}

static inline const struct nvk_instance *
nvk_physical_device_instance(const struct nvk_physical_device *pdev)
{
   return (struct nvk_instance *)pdev->vk.instance;
}

VkResult nvk_create_drm_physical_device(struct vk_instance *vk_instance,
                                        struct _drmDevice *drm_device,
                                        struct vk_physical_device **pdev_out);

#ifdef __SWITCH__
VkResult nvk_enumerate_physical_device(struct vk_instance *vk_instance);
#endif

void nvk_physical_device_destroy(struct vk_physical_device *vk_device);

VkExtent2D nvk_max_shading_rate(const struct nvk_physical_device *pdev,
                                VkSampleCountFlagBits samples);

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
    defined(VK_USE_PLATFORM_XCB_KHR) || \
    defined(VK_USE_PLATFORM_XLIB_KHR) || \
    defined(VK_USE_PLATFORM_DISPLAY_KHR) || \
    defined(VK_USE_PLATFORM_VI_NN)
#define NVK_USE_WSI_PLATFORM
#endif

#if defined(NVK_NATIVE_HEADLESS) && defined(NVK_USE_WSI_PLATFORM) && !defined(VK_USE_PLATFORM_VI_NN)
#error "Native profile supports only the dedicated VI2 surface platform"
#endif

#endif
