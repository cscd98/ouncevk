/* SPDX-License-Identifier: MIT */
/* Original OunceVK code. See nvkmd_nvgpu_native_release.h.
 */
#include "nvkmd_nvgpu_native_release.h"

#include "util/os_time.h"
#include "vulkan/runtime/vk_device.h"

#include <string.h>

static const uint64_t native_release_poll_ns = UINT64_C(1000000);

static struct nvkmd_nvgpu_native_release *
native_release(struct vk_sync *sync)
{
   return container_of(sync, struct nvkmd_nvgpu_native_release, base);
}

static VkResult
native_release_init(struct vk_device *device, struct vk_sync *sync,
                    uint64_t initial_value)
{
   (void)device;
   (void)initial_value;
   struct nvkmd_nvgpu_native_release *release = native_release(sync);
   memset((uint8_t *)release + sizeof(release->base), 0,
          sizeof(*release) - sizeof(release->base));
   return VK_SUCCESS;
}

static void
native_release_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   (void)sync;
}

/* Reads the counter one time with sync raw 0x77. The dependency's token is
 * the complemented producer ID. Returns VK_SUCCESS when the threshold is
 * reached and VK_NOT_READY when it is not. Mode 1 compares all 64 bits.
 * Mode 0 uses the sign of the wrapping 32-bit difference.
 */
static VkResult
native_release_poll(const struct nvkmd_nvgpu_native_release *release)
{
   uint64_t counter = 0;
   if (R_FAILED(gsvSyncReadCounter(release->sync, ~release->token, &counter, NULL)))
      return VK_ERROR_DEVICE_LOST;
   const bool reached = release->mode == 1 ?
      counter >= release->threshold :
      (int32_t)((uint32_t)counter - (uint32_t)release->threshold) >= 0;
   return reached ? VK_SUCCESS : VK_NOT_READY;
}

static VkResult
native_release_wait_many(struct vk_device *device, uint32_t wait_count,
                         const struct vk_sync_wait *waits,
                         enum vk_sync_wait_flags wait_flags,
                         uint64_t abs_timeout_ns)
{
   (void)device;
   /* VI2 accepted the release when the payload was created. */
   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;
   for (;;) {
      uint32_t reached = 0;
      for (uint32_t i = 0; i < wait_count; i++) {
         const VkResult result =
            native_release_poll(native_release(waits[i].sync));
         if (result == VK_SUCCESS)
            reached++;
         else if (result != VK_NOT_READY)
            return result;
      }
      if (reached == wait_count ||
          (reached && (wait_flags & VK_SYNC_WAIT_ANY)))
         return VK_SUCCESS;
      const uint64_t now = os_time_get_nano();
      if (now >= abs_timeout_ns)
         return VK_TIMEOUT;
      const uint64_t left = abs_timeout_ns - now;
      svcSleepThread((s64)(left < native_release_poll_ns ?
                           left : native_release_poll_ns));
   }
}

const struct vk_sync_type nvkmd_nvgpu_native_release_type = {
   .size = sizeof(struct nvkmd_nvgpu_native_release),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = native_release_init,
   .finish = native_release_finish,
   .wait_many = native_release_wait_many,
};

VkResult
nvkmd_nvgpu_native_release_create(
   struct vk_device *device, const uint8_t release[16],
   uint64_t shim_base, uint32_t shim_stride, uint32_t shim_count,
   GsvSync *sync, struct vk_sync **sync_out)
{
   uint32_t token;
   uint64_t threshold;
   memcpy(&token, release, sizeof(token));
   memcpy(&threshold, release + 8, sizeof(threshold));
   const uint32_t id = ~token;
   if (!shim_base || !shim_stride || id >= shim_count ||
       (uint64_t)id * shim_stride > UINT64_MAX - shim_base)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkResult result = vk_sync_create(device, &nvkmd_nvgpu_native_release_type,
                                    0, 0, sync_out);
   if (result != VK_SUCCESS)
      return result;
   struct nvkmd_nvgpu_native_release *payload = native_release(*sync_out);
   payload->shim_addr = shim_base + (uint64_t)id * shim_stride;
   payload->threshold_lo = (uint32_t)threshold;
   payload->token = token;
   payload->mode = release[4];
   payload->threshold = threshold;
   payload->sync = sync;
   return VK_SUCCESS;
}
