/* SPDX-License-Identifier: MIT
 * CPU data-cache maintenance for the native CPU-cached readback pool.
 * Original OunceVK code. The sequence reads the line size from CTR_EL0.DminLine
 * (bits 19:16, 4 << n bytes). It runs dc civac on every line that covers
 * the range, then runs dsb sy.
 */
#ifndef NVKMD_NVGPU_NATIVE_CACHE_H
#define NVKMD_NVGPU_NATIVE_CACHE_H

#include <stdint.h>

/* Cleans and invalidates [cpu, cpu + size) to the point of coherency. The
 * range must be mapped. */
static inline void
nvkmd_nvgpu_native_dcache_clean_invalidate(uint64_t cpu, uint64_t size)
{
   if (!size || cpu > UINT64_MAX - size)
      return;
#if defined(__aarch64__)
   uint64_t ctr;
   __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
   const uint64_t line = UINT64_C(4) << ((ctr >> 16) & 0xf);
   for (uint64_t at = cpu & ~(line - 1); at < cpu + size; at += line)
      __asm__ volatile("dc civac, %0" : : "r"(at) : "memory");
   __asm__ volatile("dsb sy" : : : "memory");
#else
   (void)cpu;
#endif
}

#endif
