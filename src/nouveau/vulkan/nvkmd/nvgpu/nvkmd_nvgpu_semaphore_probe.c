/* SPDX-License-Identifier: MIT
 * The final seven words use selector 2, reduction 0 and flush 1. The class
 * binding is the C797 prefix. This nine-word composition was validated on
 * the hardware.
 */
#include "nvkmd_nvgpu_semaphore_probe.h"

int
nvkmd_nvgpu_semaphore_probe_encode(
   uint32_t words[NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS],
   uint64_t result_gpu_address, uint32_t value)
{
   if (!words || !result_gpu_address || (result_gpu_address & 3u) || !value)
      return 0;

   const uint32_t packet[NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS] = {
      UINT32_C(0x20010000), /* SET_OBJECT, one word, subchannel 0. */
      UINT32_C(0x0000c797), /* Ampere 3D class. */
      UINT32_C(0x80000451), /* FLUSH_PENDING_WRITES. */
      UINT32_C(0x200406c0), /* Four semaphore method words. */
      (uint32_t)(result_gpu_address >> 32),
      (uint32_t)result_gpu_address,
      value,
      UINT32_C(0x1000f010), /* Release, one-word payload, flush enabled. */
      UINT32_C(0x800003e0), /* Submission tail. */
   };
   for (unsigned i = 0; i < NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS; ++i)
      words[i] = packet[i];
   return 1;
}
