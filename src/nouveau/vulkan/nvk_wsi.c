/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_wsi.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_image.h"
#include "nvk_instance.h"
#include "nvkmd/nvkmd.h"
#include "vk_fence.h"
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(nvk_physical_device, pdev, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

#if defined(NVK_NATIVE_HEADLESS) && defined(NVK_NATIVE_BACKING_ENABLED)
#include "wsi_switch_native.h"
#include "wsi_switch_native_owner.h"
#include "nvkmd/nvgpu/nvkmd_nvgpu_discovery.h"
#include "nvkmd/nvgpu/nvkmd_nvgpu_native_device_owner.h"
#include "nvkmd/nvgpu/nvkmd_nvgpu_native_mem.h"
#include "nvkmd/nvgpu/nvkmd_nvgpu_native_syncobj.h"
#include "nvkmd/nvgpu/nvkmd_nvgpu_native_release.h"
#include <switch.h>

/* Returns the VI2 layer of the NWindow of the window (libounce-nx
 * nwindowGetVi2Layer). The layer token of the window must still name that
 * layer. Otherwise this returns NULL. A swapchain on a window without a
 * token takes the token of the current layer.
 */
static Vi2Layer *
native_vi_layer(const struct wsi_native_vi_window *window)
{
   Vi2Layer *layer = NULL;
   if (!window || R_FAILED(nwindowGetVi2Layer(window->nwindow, &layer)) ||
       !window->layer_token || window->layer_token != layer->token)
      return NULL;
   return layer;
}

static VkResult
native_vi_open(struct wsi_native_vi_window *window)
{
   Vi2Layer *layer = NULL;
   if (R_FAILED(nwindowGetVi2Layer(window->nwindow, &layer)))
      return VK_ERROR_SURFACE_LOST_KHR;
   if (window->layer_token)
      return window->layer_token == layer->token ?
         VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
   window->layer_token = layer->token;
   return VK_SUCCESS;
}

static bool
native_vi_arena_holds(const struct nvkmd_nvgpu_native_arena *arena,
                      uint64_t address, uint64_t size)
{
   return address >= arena->cpu_base &&
          address - arena->cpu_base <= arena->cursor &&
          size <= arena->cursor - (address - arena->cpu_base);
}

/* Finds the arena that holds a mapped allocation. Also finds the backing
 * whose GSV range the display uses to register the allocation. This is the
 * backing of the primary segment or of a later segment. An image that does
 * not fit the primary segment, such as a 4K swapchain image, goes in a
 * later segment. The arena of a later segment starts at the base of its
 * own backing.
 */
static bool
native_vi_find_backing(const struct nvkmd_nvgpu_native_device_owner *device,
                       uint64_t address, uint64_t size,
                       const struct nvkmd_nvgpu_native_arena **out_arena,
                       const struct nvkmd_nvgpu_native_backing **out_backing)
{
   if (native_vi_arena_holds(&device->arena, address, size)) {
      *out_arena = &device->arena;
      *out_backing = &device->backing;
      return true;
   }
   for (uint32_t i = 0; i < device->segment_count; i++) {
      const struct nvkmd_nvgpu_native_segment *segment = &device->segments[i];
      if (segment->ready && native_vi_arena_holds(&segment->arena, address, size)) {
         *out_arena = &segment->arena;
         *out_backing = &segment->backing;
         return true;
      }
   }
   return false;
}

static VkResult
native_vi_import(VkDevice _device, VkImage _image, VkDeviceMemory _memory,
                  struct wsi_native_vi_window *window,
                  struct wsi_native_vi_import *import)
{
   VK_FROM_HANDLE(nvk_image, image, _image);
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);
   struct nvkmd_nvgpu_native_scanout_view view;
   if (!nvkmd_nvgpu_native_scanout_view(&view) || !native_vi_layer(window) ||
       image->plane_count != 1 || mem->dedicated_image != image)
      return VK_ERROR_INITIALIZATION_FAILED;
   const struct nil_image *nil = &image->planes[0].nil;
   /* Native GSV image profile. It does not use the legacy nvmap kind 0xfe.
    * The NIL block height counts GOBs (8 rows). The image layout counts
    * rows. Layout block format 1 uses the Turing sector order. The NIL
    * TuringColor2D writer uses that same sector order.
    */
   enum { NATIVE_KIND = 6, NATIVE_TILE_HEIGHT = 4, GOB_ROW_BYTES = 64,
          GOB_HEIGHT_LOG2 = 3, NATIVE_RGBA8 = 1,
          NATIVE_BLOCK_FORMAT_TURING_2D = 1,
          /* Image memory type for display images. */
          NATIVE_IMAGE_MEMORY_TYPE = 3 };
   if (nil->pte_kind != NATIVE_KIND ||
       nil->levels[0].tiling.gob_type != NIL_GOB_TYPE_TURING_COLOR2_D ||
       nil->levels[0].tiling.y_log2 != NATIVE_TILE_HEIGHT ||
       !nil->levels[0].row_stride_B ||
       nil->levels[0].row_stride_B % GOB_ROW_BYTES ||
       image->planes[0].plane_offset_B ||
       !mem->mem->va || image->planes[0].addr != mem->mem->va->addr)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   void *cpu = NULL;
   VkResult result = nvkmd_mem_map(mem->mem, &mem->vk.base,
                                    NVKMD_MEM_MAP_RD, NULL, &cpu);
   if (result != VK_SUCCESS)
      return result;
   uint64_t address = (uintptr_t)cpu;
   const struct nvkmd_nvgpu_native_arena *arena = NULL;
   const struct nvkmd_nvgpu_native_backing *backing = NULL;
   /* Another thread can add segments or move arena cursors. Read them
    * under the owner lock, and keep only the two values that the GSV
    * registration needs. */
   if (!nvkmd_nvgpu_native_owner_lock(view.device))
      return VK_ERROR_DEVICE_LOST;
   const bool found = native_vi_find_backing(view.device, address,
                                             mem->mem->size_B,
                                             &arena, &backing);
   const u32 range_id = found ? backing->range_id : 0;
   const uint64_t range_offset_B = found ? address - arena->cpu_base : 0;
   nvkmd_nvgpu_native_owner_unlock(view.device);
   if (!found || (address & (nil->align_B - 1)))
      return VK_ERROR_INITIALIZATION_FAILED;
   GsvImageLayout layout;
   gsvMakeImageLayout(&layout, window->width, window->height,
                      NATIVE_TILE_HEIGHT + GOB_HEIGHT_LOG2,
                      nil->levels[0].row_stride_B / GOB_ROW_BYTES,
                      NATIVE_RGBA8, NATIVE_BLOCK_FORMAT_TURING_2D, 0);
   u64 size = 0;
   if (R_FAILED(gsvGetImageSize(&layout, &size)) || size != nil->size_B ||
       size > mem->mem->size_B)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   /* The memory stays pinned from the first mutating import step until
    * native_vi_retire_image.
    */
   if (!nvkmd_nvgpu_native_mem_pin_scanout(mem->mem))
      return VK_ERROR_INITIALIZATION_FAILED;
   import->remote_use = true;
   u32 memory_id = 0;
   if (R_FAILED(gsvReserveImageMemory(NATIVE_IMAGE_MEMORY_TYPE, size,
                                      &memory_id)) || !memory_id)
      return VK_ERROR_SURFACE_LOST_KHR;
   import->memory_id = memory_id;
   if (R_FAILED(gsvRegisterImageMemory(memory_id, address, size,
                                       range_id, range_offset_B)))
      return VK_ERROR_SURFACE_LOST_KHR;
   u32 image_id = 0;
   if (R_FAILED(gsvCreateImage(&layout, memory_id, &image_id)) || !image_id)
      return VK_ERROR_SURFACE_LOST_KHR;
   import->image_id = image_id;
   return R_SUCCEEDED(gsvCreateDisplayAlias(image_id, 1)) ?
      VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult
native_vi_availability(struct wsi_native_vi_window *window, uint64_t *mask)
{
   Vi2Layer *layer = native_vi_layer(window);
   if (!layer || R_FAILED(vi2GetLayerAvailability(layer, mask)))
      return VK_ERROR_SURFACE_LOST_KHR;
   return VK_SUCCESS;
}

static VkResult
native_vi_counter(struct wsi_native_vi_window *window,
                   uint32_t token, uint64_t *counter, uint8_t *mode)
{
   struct nvkmd_nvgpu_native_scanout_view view;
   if (!native_vi_layer(window) || !nvkmd_nvgpu_native_scanout_view(&view))
      return VK_ERROR_SURFACE_LOST_KHR;
   /* The token of the dependency is the complement of the producer ID. */
   return R_SUCCEEDED(gsvSyncReadCounter(&view.runtime->sync, ~token,
                                         counter, mode)) ?
      VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

/* Consumes a pending availability notification if one exists. It does not
 * wait.
 */
static VkResult
native_vi_availability_consume(struct wsi_native_vi_window *window)
{
   Vi2Layer *layer = native_vi_layer(window);
   if (!layer)
      return VK_ERROR_SURFACE_LOST_KHR;
   const Result rc = vi2WaitLayerAvailability(layer, 0);
   return R_SUCCEEDED(rc) || R_VALUE(rc) == KERNELRESULT(TimedOut) ?
      VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult
native_vi_availability_wait(struct wsi_native_vi_window *window,
                            uint64_t timeout_ns)
{
   Vi2Layer *layer = native_vi_layer(window);
   if (!layer)
      return VK_ERROR_SURFACE_LOST_KHR;
   const Result rc = vi2WaitLayerAvailability(layer, timeout_ns);
   if (R_SUCCEEDED(rc))
      return VK_SUCCESS;
   return R_VALUE(rc) == KERNELRESULT(TimedOut) ?
      VK_TIMEOUT : VK_ERROR_SURFACE_LOST_KHR;
}

/* Makes a GPU-waitable payload from a pending VI2 release. The queue waits
 * on the syncpoint shim slot of the counter. CPU waits poll sync raw 0x77.
 */
static VkResult
native_vi_release_sync(struct vk_device *device,
                       struct wsi_native_vi_window *window,
                       const uint8_t release[16], struct vk_sync **sync_out)
{
   (void)window;
   struct nvkmd_nvgpu_native_scanout_view view;
   uint64_t base = 0;
   uint32_t stride = 0, count = 0;
   if (!nvkmd_nvgpu_native_scanout_view(&view) ||
       !nvkmd_nvgpu_native_syncpoint_shim(&base, &stride, &count))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   return nvkmd_nvgpu_native_release_create(device, release, base, stride,
                                            count, &view.runtime->sync,
                                            sync_out);
}

/* Makes the VI2 f5 producer16 for the present fence. Its layout:
 * - bytes 0-3: the u32 complement of returned_id of the accepted submit.
 * - byte 4: mode 0.
 * - bytes 5-7: zero.
 * - bytes 8-15: returned_target, zero-extended to u64.
 * VI2 consumes the dependency later (deferred). VI2 skips the dependency
 * for an all-zero producer16.
 */
static VkResult
native_vi_fence_dependency(VkDevice _device, VkFence _fence,
                           uint8_t producer[16])
{
   (void)_device;
   VK_FROM_HANDLE(vk_fence, fence, _fence);
   memset(producer, 0, 16);
   if (!fence)
      return VK_NOT_READY;
   bool pending = false;
   uint32_t id = 0, target = 0;
   const VkResult result = nvkmd_nvgpu_native_syncobj_dependency(
      vk_fence_get_active_sync(fence), &pending, &id, &target);
   if (result != VK_SUCCESS || !pending)
      return result;
   if (id == UINT32_MAX) /* invalid ID: not a usable dependency */
      return VK_NOT_READY;
   const uint32_t token = ~id;
   for (unsigned i = 0; i < 4; i++) {
      producer[i] = (uint8_t)(token >> (8 * i));
      producer[8 + i] = (uint8_t)(target >> (8 * i));
   }
   return VK_SUCCESS;
}

static VkResult
native_vi_present(struct wsi_native_vi_window *window, uint8_t slot,
                   uint32_t image, const uint8_t producer[16],
                   uint64_t *mask, uint8_t release[16])
{
   Vi2Layer *layer = native_vi_layer(window);
   if (!layer)
      return VK_ERROR_SURFACE_LOST_KHR;
   Vi2PresentOptions options;
   vi2MakeBasicPresentOptions(&options, window->width, window->height,
                              armGetSystemTick());
   /* Use the frame rate or swap interval of the window at the current
    * refresh rate of the display (libounce-nx nwindowSetFrameRates).
    */
   nwindowGetPresentInterval(window->nwindow, &options.interval_numerator,
                             &options.interval_denominator);
   Vi2Dependency producer_dependency;
   Vi2Dependency release_dependency;
   memcpy(producer_dependency.data, producer, sizeof(producer_dependency.data));
   if (R_FAILED(vi2PresentLayer(layer, slot, image, &options,
                                &producer_dependency, mask,
                                &release_dependency)))
      return VK_ERROR_SURFACE_LOST_KHR;
   memcpy(release, release_dependency.data, sizeof(release_dependency.data));
   return VK_SUCCESS;
}

/* The no-image f4 clears the present-image flag. */
#define NATIVE_VI_PRESENT_IMAGE_FLAG UINT64_C(1)

static VkResult
native_vi_disable(struct wsi_native_vi_window *window, uint8_t slot,
                  uint64_t *mask)
{
   Vi2Layer *layer = native_vi_layer(window);
   if (!layer)
      return VK_ERROR_SURFACE_LOST_KHR;
   Vi2PresentOptions options;
   vi2MakeBasicPresentOptions(&options, window->width, window->height,
                              armGetSystemTick());
   options.flags &= ~NATIVE_VI_PRESENT_IMAGE_FLAG;
   Vi2Dependency release_dependency;
   struct wsn_dependency release;
   if (R_FAILED(vi2UnregisterLayerImage(layer, slot, &options, mask,
                                        &release_dependency)))
      return VK_ERROR_SURFACE_LOST_KHR;
   memcpy(release.bytes, release_dependency.data, sizeof(release.bytes));
   return wsn_dependency_semantic_empty(&release) ?
      VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
}

static VkResult
native_vi_retire_image(VkDevice _device, VkDeviceMemory _memory,
                       struct wsi_native_vi_window *window,
                       struct wsi_native_vi_import *import)
{
   (void)_device;
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);
   if (!mem || !mem->mem || !import->remote_use ||
       !import->memory_id || !import->image_id)
      return VK_ERROR_SURFACE_LOST_KHR;
   if (window->observe)
      window->observe(window->observe_context, 860, import->image_id);
   gsvDestroyDisplayAlias(import->image_id);
   if (window->observe)
      window->observe(window->observe_context, 861, import->image_id);
   gsvDestroyImage(import->image_id);
   if (window->observe)
      window->observe(window->observe_context, 862, import->memory_id);
   gsvReleaseImageMemory(import->memory_id);
   import->remote_use = false;
   if (!nvkmd_nvgpu_native_mem_unpin_scanout(mem->mem))
      return VK_ERROR_SURFACE_LOST_KHR;
   import->image_id = 0;
   import->memory_id = 0;
   return VK_SUCCESS;
}

static void native_vi_pause(uint64_t ns) { svcSleepThread(ns); }
#endif

VkResult
nvk_init_wsi(struct nvk_physical_device *pdev)
{
   VkResult result;

   struct wsi_device_options wsi_options = {
      .sw_device = false
   };
   result = wsi_device_init(&pdev->wsi_device,
                            nvk_physical_device_to_handle(pdev),
                            nvk_wsi_proc_addr, &pdev->vk.instance->alloc,
                            nvkmd_pdev_get_drm_primary_fd(pdev->nvkmd),
                            &nvk_physical_device_instance(pdev)->drirc.options,
                            &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   pdev->wsi_device.supports_scanout = false;
   pdev->wsi_device.supports_modifiers =
      pdev->vk.supported_extensions.table.EXT_image_drm_format_modifier;

#if defined(VK_USE_PLATFORM_VI_NN) && defined(NVK_NATIVE_HEADLESS) && defined(NVK_NATIVE_BACKING_ENABLED)
   if (!pdev->nvkmd->has_borrowed_display_targets) {
      pdev->wsi_device.vi.native_open = native_vi_open;
      pdev->wsi_device.vi.native_import = native_vi_import;
      pdev->wsi_device.vi.native_availability = native_vi_availability;
      pdev->wsi_device.vi.native_counter = native_vi_counter;
      pdev->wsi_device.vi.native_availability_consume =
         native_vi_availability_consume;
      pdev->wsi_device.vi.native_availability_wait =
         native_vi_availability_wait;
      pdev->wsi_device.vi.native_release_sync = native_vi_release_sync;
      pdev->wsi_device.vi.native_fence_dependency = native_vi_fence_dependency;
      pdev->wsi_device.vi.native_present = native_vi_present;
      pdev->wsi_device.vi.native_disable = native_vi_disable;
      pdev->wsi_device.vi.native_retire_image = native_vi_retire_image;
      pdev->wsi_device.vi.native_pause = native_vi_pause;
   }
#endif

   pdev->vk.wsi_device = &pdev->wsi_device;

   return result;
}

void
nvk_finish_wsi(struct nvk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}
