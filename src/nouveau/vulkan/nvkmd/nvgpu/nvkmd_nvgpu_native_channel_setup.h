/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_CHANNEL_SETUP_H
#define NVKMD_NVGPU_NATIVE_CHANNEL_SETUP_H

#include "nvkmd_nvgpu_native_channel.h"

enum nvkmd_nvgpu_native_channel_setup_status {
   NVKMD_NVGPU_CHANNEL_SETUP_OK,
   NVKMD_NVGPU_CHANNEL_SETUP_ARGUMENT,
   NVKMD_NVGPU_CHANNEL_SETUP_STATE,
   NVKMD_NVGPU_CHANNEL_SETUP_RING,
   NVKMD_NVGPU_CHANNEL_SETUP_SYNC,
   NVKMD_NVGPU_CHANNEL_SETUP_CLASS,
   NVKMD_NVGPU_CHANNEL_SETUP_EVENT,
   NVKMD_NVGPU_CHANNEL_SETUP_NOTIFIER,
   NVKMD_NVGPU_CHANNEL_SETUP_POINTER,
   NVKMD_NVGPU_CHANNEL_SETUP_TIMING,
   NVKMD_NVGPU_CHANNEL_SETUP_PREEMPTION,
   NVKMD_NVGPU_CHANNEL_SETUP_POINTER_READBACK,
   NVKMD_NVGPU_CHANNEL_SETUP_HEALTH,
};

/* The configured channel: ring, sync point, class, error event, notifier,
 * user pointer, timeslice and preemption. The payloads and the pointer
 * record stay allocated for the channel lifetime. Keep this object at a
 * stable address. */
struct nvkmd_nvgpu_native_channel_setup {
   const struct nvkmd_nvgpu_native_channel_setup *self;
   uint64_t generation;
   uint32_t ring_payload[8];
   _Alignas(8) uint32_t sync_payload[4];
   _Alignas(8) uint32_t class_payload[4];
   _Alignas(8) uint32_t notifier_payload[6];
   _Alignas(8) uint64_t pointer_payload;
   _Alignas(8) uint32_t timing_payload[2];
   _Alignas(8) uint32_t preemption_payload[2];
   _Alignas(8) uint64_t pointer_readback;
   struct {
      uint32_t fd;
      uint32_t tsg;
      uint64_t generation;
   } record;
   uint64_t signal_gpu_va;
   Event error_event;
   Result result;
   bool attempted, error_event_open, notifier_enabled, configured;
};

/* Continues an opened channel through the call order. notifier_handle is the
 * notifier page's local handle, published after registration.
 */
enum nvkmd_nvgpu_native_channel_setup_status
nvkmd_nvgpu_native_channel_configure(
   struct nvkmd_nvgpu_native_channel_setup *setup,
   const struct nvkmd_nvgpu_native_channel *channel,
   uint32_t notifier_handle);

/* As nvkmd_nvgpu_native_channel_configure, with another object class for
 * the channel, for example the copy class C7B5 on a copy channel. */
enum nvkmd_nvgpu_native_channel_setup_status
nvkmd_nvgpu_native_channel_configure_class(
   struct nvkmd_nvgpu_native_channel_setup *setup,
   const struct nvkmd_nvgpu_native_channel *channel,
   uint32_t notifier_handle, uint32_t object_class);

/* Returns the producer ID the ring allocation reported. */
static inline uint32_t
nvkmd_nvgpu_native_channel_producer_id(
   const struct nvkmd_nvgpu_native_channel_setup *setup)
{
   return setup->ring_payload[3];
}

/* The first part of the channel close order. Disable the notifier (zero
 * notifier ioctl) and close the error event. The channel and TSG descriptors
 * are the runtime's GPU and TSG files. nvkmd_nvgpu_native_runtime_close
 * closes them next. The caller has drained the channel.
 */
Result nvkmd_nvgpu_native_channel_close(
   struct nvkmd_nvgpu_native_channel_setup *setup,
   struct nvkmd_nvgpu_native_channel *channel);

#endif
