/* SPDX-License-Identifier: MIT
 * OunceVK owner for the channel continuation. libounce-nx sends each ioctl
 * (nvIoctl) and the error event query (nvQueryEvent).
 */
#include "nvkmd_nvgpu_native_channel_setup.h"
#include "nvkmd_nvgpu_native_completion.h"

#include <string.h>

/* GPU fd ioctls of the configuration, in call order. */
#define SETUP_RING_IOCTL UINT32_C(0xc020481a)
#define SETUP_SYNC_IOCTL UINT32_C(0xc010481e)
#define SETUP_CLASS_IOCTL UINT32_C(0xc0104809)
#define SETUP_NOTIFIER_IOCTL UINT32_C(0xc018480c)
#define SETUP_POINTER_IOCTL UINT32_C(0x40084714)
#define SETUP_TIMING_IOCTL UINT32_C(0x40044803)
#define SETUP_PREEMPTION_IOCTL UINT32_C(0xc008481f)
#define SETUP_POINTER_READBACK_IOCTL UINT32_C(0x80084715)

/* Ring: 0x800-byte entries, 8 of them, mode 2. */
#define SETUP_RING_ENTRY_B 0x800
#define SETUP_RING_ENTRY_COUNT 8
#define SETUP_RING_MODE 2
#define SETUP_CLASS_C797 0xc797
#define SETUP_CLASS_TOKEN UINT32_C(0xdeadbeef)
#define SETUP_ERROR_EVENT_ID 7
#define SETUP_NOTIFIER_B 0x1000
#define SETUP_TIMING_VALUE 1000
#define SETUP_PREEMPTION_CLASS 2

static enum nvkmd_nvgpu_native_channel_setup_status
setup_fail(struct nvkmd_nvgpu_native_channel_setup *setup,
           enum nvkmd_nvgpu_native_channel_setup_status status, Result rc)
{
   setup->result = rc;
   return status;
}

enum nvkmd_nvgpu_native_channel_setup_status
nvkmd_nvgpu_native_channel_configure(
   struct nvkmd_nvgpu_native_channel_setup *setup,
   const struct nvkmd_nvgpu_native_channel *channel,
   uint32_t notifier_handle)
{
   return nvkmd_nvgpu_native_channel_configure_class(setup, channel,
                                                     notifier_handle,
                                                     SETUP_CLASS_C797);
}

enum nvkmd_nvgpu_native_channel_setup_status
nvkmd_nvgpu_native_channel_configure_class(
   struct nvkmd_nvgpu_native_channel_setup *setup,
   const struct nvkmd_nvgpu_native_channel *channel,
   uint32_t notifier_handle, uint32_t object_class)
{
   if (!setup || !channel || !notifier_handle)
      return NVKMD_NVGPU_CHANNEL_SETUP_ARGUMENT;
   if (setup->self || setup->attempted || channel->self != channel ||
       !channel->tsg_bound)
      return NVKMD_NVGPU_CHANNEL_SETUP_STATE;

   setup->self = setup;
   setup->attempted = true;
   setup->generation = channel->generation;
   const uint32_t fd = channel->channel_fd;

   setup->ring_payload[0] = SETUP_RING_ENTRY_B;
   setup->ring_payload[1] = SETUP_RING_ENTRY_COUNT;
   setup->ring_payload[2] = SETUP_RING_MODE;
   Result rc = nvIoctl(fd, SETUP_RING_IOCTL, setup->ring_payload);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_RING, rc);
   /* The service returns the producer ID at [3] and its current value at
    * [4]. The request words and the reserved tail are unchanged. */
   if (setup->ring_payload[0] != SETUP_RING_ENTRY_B ||
       setup->ring_payload[1] != SETUP_RING_ENTRY_COUNT ||
       setup->ring_payload[2] != SETUP_RING_MODE ||
       !setup->ring_payload[3] || setup->ring_payload[5] ||
       setup->ring_payload[6] || setup->ring_payload[7])
      return NVKMD_NVGPU_CHANNEL_SETUP_RING;

   rc = nvIoctl(fd, SETUP_SYNC_IOCTL, setup->sync_payload);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_SYNC, rc);
   setup->signal_gpu_va = (uint64_t)setup->sync_payload[0] |
                          ((uint64_t)setup->sync_payload[1] << 32);
   if (!setup->signal_gpu_va)
      return NVKMD_NVGPU_CHANNEL_SETUP_SYNC;

   setup->class_payload[0] = object_class;
   setup->class_payload[2] = SETUP_CLASS_TOKEN;
   rc = nvIoctl(fd, SETUP_CLASS_IOCTL, setup->class_payload);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_CLASS, rc);

   /* CMIF 4 QueryEvent(channel, 7): the channel's error event. */
   rc = nvQueryEvent(fd, SETUP_ERROR_EVENT_ID, &setup->error_event);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_EVENT, rc);
   setup->error_event_open = true;

   setup->notifier_payload[2] = SETUP_NOTIFIER_B;
   setup->notifier_payload[4] = notifier_handle;
   rc = nvIoctl(fd, SETUP_NOTIFIER_IOCTL, setup->notifier_payload);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_NOTIFIER, rc);
   setup->notifier_enabled = true;

   setup->record.fd = channel->channel_fd;
   setup->record.tsg = channel->tsg_fd;
   setup->record.generation = channel->generation;
   setup->pointer_payload = (uint64_t)(uintptr_t)&setup->record;
   rc = nvIoctl(fd, SETUP_POINTER_IOCTL, &setup->pointer_payload);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_POINTER, rc);

   /* The channel timeout and the graphics/compute preemption mode apply
    * to the 3D channel. On the copy channel the timeout ioctl failed
    * (0x275c, 9 November 2026); the standalone timeslice call does not
    * change scheduling state in any case. */
   if (object_class == SETUP_CLASS_C797) {
      setup->timing_payload[0] = SETUP_TIMING_VALUE;
      rc = nvIoctl(fd, SETUP_TIMING_IOCTL, setup->timing_payload);
      if (R_FAILED(rc))
         return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_TIMING, rc);

      setup->preemption_payload[1] = SETUP_PREEMPTION_CLASS;
      rc = nvIoctl(fd, SETUP_PREEMPTION_IOCTL, setup->preemption_payload);
      if (R_FAILED(rc))
         return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_PREEMPTION, rc);
   }

   rc = nvIoctl(fd, SETUP_POINTER_READBACK_IOCTL, &setup->pointer_readback);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_POINTER_READBACK, rc);
   if (setup->pointer_readback != setup->pointer_payload)
      return NVKMD_NVGPU_CHANNEL_SETUP_POINTER_READBACK;

   uint16_t health = UINT16_MAX;
   rc = nvkmd_nvgpu_native_channel_health(fd, &health);
   if (R_FAILED(rc))
      return setup_fail(setup, NVKMD_NVGPU_CHANNEL_SETUP_HEALTH, rc);
   if (health)
      return NVKMD_NVGPU_CHANNEL_SETUP_HEALTH;

   setup->configured = true;
   return NVKMD_NVGPU_CHANNEL_SETUP_OK;
}

Result
nvkmd_nvgpu_native_channel_close(struct nvkmd_nvgpu_native_channel_setup *setup,
                                 struct nvkmd_nvgpu_native_channel *channel)
{
   Result rc = 0;
   if (setup->notifier_enabled) {
      memset(setup->notifier_payload, 0, sizeof(setup->notifier_payload));
      rc = nvIoctl(channel->channel_fd, SETUP_NOTIFIER_IOCTL,
                   setup->notifier_payload);
      if (R_FAILED(rc))
         return rc;
      setup->notifier_enabled = false;
   }
   if (setup->error_event_open) {
      eventClose(&setup->error_event);
      setup->error_event_open = false;
   }
   setup->configured = false;
   channel->tsg_bound = false;
   return rc;
}
