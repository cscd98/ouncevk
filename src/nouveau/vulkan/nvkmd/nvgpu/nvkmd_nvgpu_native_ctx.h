/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_CTX_H
#define NVKMD_NVGPU_NATIVE_CTX_H

#include "nvkmd/nvkmd.h"
#include "nvkmd_nvgpu_native_device_owner.h"

/* At most two software contexts (upload and compute) share one hardware
 * channel on the original TLS thread. Each submit completes before another
 * context may use the channel. Each context uses reclaimable copied command
 * storage after a full completion join. A failed submission poisons the
 * shared owner. Remote resource release is not available. Only the discovery
 * TLS owner may submit. */
/* endpoint is the channel of the context: &owner->endpoint (the primary
 * channel) or &owner->copy.endpoint (copy-only contexts). */
VkResult nvkmd_nvgpu_native_ctx_create(
   struct nvkmd_dev *dev,
   struct nvkmd_nvgpu_native_device_owner *owner,
   enum nvkmd_engines engines,
   struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   uint32_t *active_context_count,
   struct nvkmd_ctx **ctx_out);

/* Reuse the channel's GSV event for a terminal marker after software contexts
 * have closed. A wake is only a scheduling hint. The terminal completion owner
 * still checks producer, marker and health. */
bool
nvkmd_nvgpu_native_wait_terminal_target(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint32_t producer_id, uint32_t target);

/* Frees the owner's recycled and quarantined submission records once its
 * channel is closed (nvkmd_nvgpu_native_device_finish). */
void
nvkmd_nvgpu_native_ctx_free_submissions(
   struct nvkmd_nvgpu_native_device_owner *owner);

#endif
