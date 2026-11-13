/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_SEMAPHORE_PROBE_H
#define NVKMD_NVGPU_SEMAPHORE_PROBE_H

#include <stdint.h>

enum { NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS = 9 };

/* Encode a C797 class bind and a flush-enabled release. This builds command
 * words only. It does not submit the words or confirm completion. */
int nvkmd_nvgpu_semaphore_probe_encode(
   uint32_t words[NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS],
   uint64_t result_gpu_address, uint32_t value);

#endif
