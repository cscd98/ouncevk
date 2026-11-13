/* SPDX-License-Identifier: MIT
 * OunceVK channel owner. The call order is allocate, AS bind, then modern TSG
 * bind. libounce-nx's nvIoctl sends each ioctl.
 */
#include "nvkmd_nvgpu_native_channel.h"

#include <limits.h>

#define CHANNEL_ALLOCATE_IOCTL UINT32_C(0x40044820)
#define CHANNEL_AS_BIND_IOCTL UINT32_C(0x40044101)
#define CHANNEL_TSG_BIND_IOCTL UINT32_C(0xc018540b)

/* Characteristics versions and the TSG feature the call order needs. */
#define CHANNEL_ALLOCATE_VERSION 0x1f
#define CHANNEL_AS_BIND_VERSION 0x20
#define CHANNEL_TSG_VERSION 11
#define CHANNEL_TSG_FEATURE 0x100

enum nvkmd_nvgpu_native_channel_status
nvkmd_nvgpu_native_channel_open(
   struct nvkmd_nvgpu_native_channel *channel,
   const struct nvkmd_nvgpu_native_runtime *runtime)
{
   if (!channel || !runtime)
      return NVKMD_NVGPU_CHANNEL_ARGUMENT;
   if (channel->self || channel->attempted ||
       !nvkmd_nvgpu_native_runtime_ready(runtime) ||
       runtime->stage != NVKMD_NVGPU_RUNTIME_TSG_OPEN_RECEIVED ||
       !runtime->tsg.open)
      return NVKMD_NVGPU_CHANNEL_STATE;

   const struct nvkmd_nvgpu_native_characteristics *c =
      &runtime->delegate.characteristics;
   if (c->versions[0] < CHANNEL_ALLOCATE_VERSION ||
       c->versions[3] < CHANNEL_AS_BIND_VERSION ||
       c->versions[1] < CHANNEL_TSG_VERSION ||
       !(c->flags & CHANNEL_TSG_FEATURE))
      return NVKMD_NVGPU_CHANNEL_PROFILE;

   const struct nvkmd_nvgpu_native_type0_candidate type0 =
      nvkmd_nvgpu_native_runtime_type0_candidate(runtime);
   if (!type0.found || type0.selector > INT32_MAX)
      return NVKMD_NVGPU_CHANNEL_CATALOG;
   return nvkmd_nvgpu_native_channel_open_on(channel, runtime, type0.selector,
                                             runtime->gpu_file.fd,
                                             runtime->tsg.fd);
}

enum nvkmd_nvgpu_native_channel_status
nvkmd_nvgpu_native_channel_open_on(
   struct nvkmd_nvgpu_native_channel *channel,
   const struct nvkmd_nvgpu_native_runtime *runtime,
   uint32_t selector, uint32_t gpu_fd, uint32_t tsg_fd)
{
   if (!channel || !runtime || selector > INT32_MAX)
      return NVKMD_NVGPU_CHANNEL_ARGUMENT;
   if (channel->self || channel->attempted ||
       !nvkmd_nvgpu_native_runtime_ready(runtime))
      return NVKMD_NVGPU_CHANNEL_STATE;

   channel->self = channel;
   channel->attempted = true;
   channel->generation = runtime->generation;
   channel->selector = selector;
   channel->channel_fd = gpu_fd;
   channel->as_fd = runtime->as.fd;
   channel->tsg_fd = tsg_fd;

   channel->result = nvIoctl(channel->channel_fd, CHANNEL_ALLOCATE_IOCTL,
                             &channel->selector);
   if (R_FAILED(channel->result))
      return NVKMD_NVGPU_CHANNEL_ALLOCATE;
   channel->allocated = true;

   channel->as_payload = channel->channel_fd;
   channel->result = nvIoctl(channel->as_fd, CHANNEL_AS_BIND_IOCTL,
                             &channel->as_payload);
   if (R_FAILED(channel->result))
      return NVKMD_NVGPU_CHANNEL_AS_BIND;
   channel->as_bound = true;

   channel->tsg_payload[0] = channel->channel_fd;
   channel->result = nvIoctl(channel->tsg_fd, CHANNEL_TSG_BIND_IOCTL,
                             channel->tsg_payload);
   if (R_FAILED(channel->result))
      return NVKMD_NVGPU_CHANNEL_TSG_BIND;
   channel->tsg_bound = true;
   return NVKMD_NVGPU_CHANNEL_OK;
}
