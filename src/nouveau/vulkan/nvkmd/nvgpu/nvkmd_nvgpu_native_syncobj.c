/* SPDX-License-Identifier: MIT */
/* Binary software fence that keeps an accepted GPU completion point. A
 * signal publishes the pending point. A wait on the channel owner thread
 * retires the work. Completion still requires the channel counter, the GPU
 * marker and the health certificate.
 * Original implementation that uses Mesa's vk_sync interface. */
#include "nvkmd_nvgpu_native_syncobj.h"

#include "nvkmd_nvgpu_native_progress.h"

#include "c11/threads.h"
#include "util/cnd_monotonic.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/timespec.h"

#include <stdbool.h>
#include <stdint.h>

struct nvkmd_nvgpu_native_syncobj {
   struct vk_sync base;
   mtx_t mutex;
   struct u_cnd_monotonic changed;
   bool signaled;
   struct nvkmd_nvgpu_native_point *point;
};

static struct nvkmd_nvgpu_native_syncobj *
native_syncobj(struct vk_sync *sync)
{
   return container_of(sync, struct nvkmd_nvgpu_native_syncobj, base);
}

static VkResult
native_syncobj_init(struct vk_device *device,
                    struct vk_sync *sync,
                    uint64_t initial_value)
{
   (void)device;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   if (mtx_init(&obj->mutex, mtx_plain) != thrd_success)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (u_cnd_monotonic_init(&obj->changed) != thrd_success) {
      mtx_destroy(&obj->mutex);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   obj->point = NULL;
   obj->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
native_syncobj_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   nvkmd_nvgpu_native_point_unref(obj->point);
   u_cnd_monotonic_destroy(&obj->changed);
   mtx_destroy(&obj->mutex);
}

VkResult
nvkmd_nvgpu_native_syncobj_publish_point(
   struct vk_sync *sync, struct nvkmd_nvgpu_native_point *point)
{
   if (!sync || sync->type != &nvkmd_nvgpu_native_syncobj_type ||
       !point || (point->state != NVKMD_NVGPU_POINT_SUBMITTED &&
                  point->state != NVKMD_NVGPU_POINT_COMPLETE))
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   nvkmd_nvgpu_native_point_ref(point);
   mtx_lock(&obj->mutex);
   struct nvkmd_nvgpu_native_point *old = obj->point;
   obj->point = point;
   obj->signaled = false;
   u_cnd_monotonic_broadcast(&obj->changed);
   mtx_unlock(&obj->mutex);
   nvkmd_nvgpu_native_point_unref(old);
   return VK_SUCCESS;
}

VkResult
nvkmd_nvgpu_native_syncobj_peek_point(
   struct vk_sync *sync, struct nvkmd_nvgpu_native_point **point)
{
   if (!sync || sync->type != &nvkmd_nvgpu_native_syncobj_type || !point)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   VkResult result = VK_SUCCESS;
   *point = NULL;
   mtx_lock(&obj->mutex);
   if (!obj->signaled) {
      if (obj->point) {
         nvkmd_nvgpu_native_point_ref(obj->point);
         *point = obj->point;
      } else {
         result = VK_NOT_READY;
      }
   }
   mtx_unlock(&obj->mutex);
   return result;
}

VkResult
nvkmd_nvgpu_native_syncobj_dependency(
   struct vk_sync *sync, bool *pending, uint32_t *producer_id,
   uint32_t *target)
{
   if (!pending || !producer_id || !target)
      return VK_ERROR_INITIALIZATION_FAILED;
   *pending = false;
   if (!sync || sync->type != &nvkmd_nvgpu_native_syncobj_type)
      return VK_NOT_READY;
   struct nvkmd_nvgpu_native_point *point = NULL;
   VkResult result = nvkmd_nvgpu_native_syncobj_peek_point(sync, &point);
   if (result != VK_SUCCESS || !point)
      return result;
   result = nvkmd_nvgpu_native_point_dependency(point, pending, producer_id,
                                                target);
   nvkmd_nvgpu_native_point_unref(point);
   return result;
}

static VkResult
native_syncobj_signal(struct vk_device *device,
                      struct vk_sync *sync,
                      uint64_t value)
{
   (void)device;
   (void)value;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   mtx_lock(&obj->mutex);
   struct nvkmd_nvgpu_native_point *old = obj->point;
   obj->point = NULL;
   obj->signaled = true;
   u_cnd_monotonic_broadcast(&obj->changed);
   mtx_unlock(&obj->mutex);
   nvkmd_nvgpu_native_point_unref(old);
   return VK_SUCCESS;
}

static VkResult
native_syncobj_reset(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   mtx_lock(&obj->mutex);
   struct nvkmd_nvgpu_native_point *old = obj->point;
   obj->point = NULL;
   obj->signaled = false;
   u_cnd_monotonic_broadcast(&obj->changed);
   mtx_unlock(&obj->mutex);
   nvkmd_nvgpu_native_point_unref(old);
   return VK_SUCCESS;
}

static VkResult
native_syncobj_move(struct vk_device *device,
                    struct vk_sync *dst,
                    struct vk_sync *src)
{
   (void)device;
   if (dst == src)
      return VK_SUCCESS;
   struct nvkmd_nvgpu_native_syncobj *dst_obj = native_syncobj(dst);
   struct nvkmd_nvgpu_native_syncobj *src_obj = native_syncobj(src);
   struct nvkmd_nvgpu_native_syncobj *first =
      (uintptr_t)dst_obj < (uintptr_t)src_obj ? dst_obj : src_obj;
   struct nvkmd_nvgpu_native_syncobj *second =
      first == dst_obj ? src_obj : dst_obj;
   mtx_lock(&first->mutex);
   mtx_lock(&second->mutex);
   struct nvkmd_nvgpu_native_point *old = dst_obj->point;
   dst_obj->point = src_obj->point;
   src_obj->point = NULL;
   dst_obj->signaled = src_obj->signaled;
   src_obj->signaled = false;
   u_cnd_monotonic_broadcast(&dst_obj->changed);
   u_cnd_monotonic_broadcast(&src_obj->changed);
   mtx_unlock(&second->mutex);
   mtx_unlock(&first->mutex);
   nvkmd_nvgpu_native_point_unref(old);
   return VK_SUCCESS;
}

static VkResult
native_syncobj_wait(struct vk_device *device,
                    struct vk_sync *sync,
                    uint64_t wait_value,
                    enum vk_sync_wait_flags wait_flags,
                    uint64_t abs_timeout_ns)
{
   (void)device;
   (void)wait_value;
   struct nvkmd_nvgpu_native_syncobj *obj = native_syncobj(sync);
   mtx_lock(&obj->mutex);
   if (obj->signaled) {
      mtx_unlock(&obj->mutex);
      return VK_SUCCESS;
   }
   if (obj->point) {
      struct nvkmd_nvgpu_native_point *point = obj->point;
      nvkmd_nvgpu_native_point_ref(point);
      mtx_unlock(&obj->mutex);
      const VkResult result = nvkmd_nvgpu_native_point_wait(
         point, (wait_flags & VK_SYNC_WAIT_PENDING) != 0, abs_timeout_ns);
      nvkmd_nvgpu_native_point_unref(point);
      return result;
   }
   if (wait_flags & VK_SYNC_WAIT_PENDING) {
      mtx_unlock(&obj->mutex);
      return VK_TIMEOUT;
   }
   struct timespec deadline;
   if (abs_timeout_ns != UINT64_MAX)
      timespec_from_nsec(&deadline, abs_timeout_ns);
   while (!obj->signaled) {
      if (obj->point) {
         struct nvkmd_nvgpu_native_point *point = obj->point;
         nvkmd_nvgpu_native_point_ref(point);
         mtx_unlock(&obj->mutex);
         const VkResult result = nvkmd_nvgpu_native_point_wait(
            point, false, abs_timeout_ns);
         nvkmd_nvgpu_native_point_unref(point);
         return result;
      }
      const int result = abs_timeout_ns == UINT64_MAX ?
         u_cnd_monotonic_wait(&obj->changed, &obj->mutex) :
         u_cnd_monotonic_timedwait(&obj->changed, &obj->mutex, &deadline);
      if (result == thrd_timedout) {
         const bool signaled = obj->signaled;
         mtx_unlock(&obj->mutex);
         return signaled ? VK_SUCCESS : VK_TIMEOUT;
      }
      if (result != thrd_success) {
         mtx_unlock(&obj->mutex);
         return VK_ERROR_DEVICE_LOST;
      }
   }
   mtx_unlock(&obj->mutex);
   return VK_SUCCESS;
}

const struct vk_sync_type nvkmd_nvgpu_native_syncobj_type = {
   .size = sizeof(struct nvkmd_nvgpu_native_syncobj),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = native_syncobj_init,
   .finish = native_syncobj_finish,
   .signal = native_syncobj_signal,
   .reset = native_syncobj_reset,
   .move = native_syncobj_move,
   .wait = native_syncobj_wait,
};
