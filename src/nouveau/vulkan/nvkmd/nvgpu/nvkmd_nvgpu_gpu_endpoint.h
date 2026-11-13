/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_GPU_ENDPOINT_H
#define NVKMD_NVGPU_GPU_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>

/* The channel's submit and completion route. It holds its GPU file, on
 * libounce-nx's delegated nvservices session, its producer ID and the GSV
 * sync session that reports the producer counter. The runtime owns the file
 * and the session. They stay open until every submit and completion on this
 * route has finished. */
struct nvkmd_nvgpu_gpu_endpoint {
   uint64_t generation;
   uint32_t gpu_fd;
   uint32_t producer_id;
   GsvSync *sync;
   bool gpu_fd_ready;
   /* A known reached counter value of this producer (mode-0 baseline),
    * valid for floor_generation. */
   bool floor_valid;
   uint64_t floor_generation;
   uint32_t floor;
};

static inline bool
nvkmd_nvgpu_gpu_endpoint_valid(const struct nvkmd_nvgpu_gpu_endpoint *endpoint)
{
   return endpoint && endpoint->generation && endpoint->gpu_fd_ready &&
          endpoint->sync && endpoint->producer_id;
}

#endif
