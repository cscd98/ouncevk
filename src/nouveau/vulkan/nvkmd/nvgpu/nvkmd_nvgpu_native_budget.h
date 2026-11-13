/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_BUDGET_H
#define NVKMD_NVGPU_NATIVE_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/* OunceVK experiment budgets. They are not hardware limits. Three uncompressed
 * 1920x1080 RGBA8 NIL images (kind 6, tile mode 0x40) occupy 0x1950000
 * bytes. NIL y4 means 128 rows per block. GSV receives height exponent 7.
 * GSV 1c3 must still confirm the required length of each image before
 * import.
 *
 * Local VA placement is aligned to 64 KiB. The caller's pool length is
 * variable. The process heap limits GPU memory, not a fixed span. */
#define NVKMD_NVGPU_NATIVE_1080P_IMAGE_B UINT64_C(0x870000)
#define NVKMD_NVGPU_NATIVE_1080P_IMAGE_ALIGN_B UINT64_C(8192)
#define NVKMD_NVGPU_NATIVE_1080P_IMAGE_COUNT 3u
/* The borrowed CPU span holds only the primary segment and the notifier
 * page. Every other segment is its own block of the process heap. OunceVK
 * allocates such a block when an allocation does not fit. It frees the
 * block when the segment is released. Each segment is registered (raw 0x75)
 * once and mapped (raw 0xe3) once. Segment kinds:
 * - uncached: mode 2 (CPU-uncached) and a GPU-uncached map (cacheability
 *   0). For commands, completions and host-visible coherent memory. The
 *   primary segment is created at setup and holds the submission chunks.
 * - cacheable: mode 2 and a GPU-cacheable map (cacheability 1). For
 *   GPU-only memory.
 * - readback: mode 1 (the CPU span keeps its cached attribute) and a
 *   GPU-uncached map. For HOST_CACHED memory that is not coherent. It is
 *   flushed and invalidated with dc civac by VA.
 * Every map and kind change uses the completion slot of the primary map. A
 * slot serves one request at a time.
 *
 * Small allocations share pooled segments of NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B.
 * An allocation larger than NVKMD_NVGPU_NATIVE_DEDICATED_MIN_B gets a
 * segment of its own, with its own size. Freeing that allocation can then
 * return its memory to the heap. On the console, creating a segment takes
 * about 1.9 ms (registration, map and clearing 16 MiB). Pooling keeps this
 * cost off the small-allocation path. */
#define NVKMD_NVGPU_NATIVE_PRIMARY_B (UINT64_C(64) * 1024 * 1024)
#define NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B (UINT64_C(16) * 1024 * 1024)
#define NVKMD_NVGPU_NATIVE_DEDICATED_MIN_B (NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B / 2)
/* The GPU start of a segment is aligned to NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B.
 * Its CPU start is aligned to NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B. A larger
 * requested alignment applies to both. Allocations in the segment thus keep
 * matching residues (8 KiB images, 64 KiB command chunks). The CPU side
 * stays at 64 KiB alignment. With 2 MiB-aligned heap blocks, about 2 MiB
 * per segment was unusable. In one test, 740 dedicated segments filled
 * 7.4 GiB of an 8.96 GiB heap. Dedicated segment lengths are rounded to the
 * placement alignment. */
#define NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B (UINT64_C(2) * 1024 * 1024)
/* Empty segments are kept for reuse until they hold this many bytes. Above
 * this limit, the next growth releases them first. */
#define NVKMD_NVGPU_NATIVE_SEGMENT_CACHE_B (UINT64_C(64) * 1024 * 1024)
/* GPU VA lease: the primary segment is at its start. After it is a VA heap
 * for the map and kind-alias map of every other segment. The service
 * accepts user VA in [0x08000000, 0xc000000000).
 * A smaller lease is the fallback. Nothing is reserved remotely until a
 * segment is mapped. */
#define NVKMD_NVGPU_NATIVE_GPU_VA_LEASE_B (UINT64_C(64) * 1024 * 1024 * 1024)
#define NVKMD_NVGPU_NATIVE_GPU_VA_LEASE_MIN_B (UINT64_C(1) * 1024 * 1024 * 1024)
#define NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B UINT64_C(65536)
#define NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B UINT64_C(4096)
#define NVKMD_NVGPU_NATIVE_CPU_EXTENT_B \
   (NVKMD_NVGPU_NATIVE_PRIMARY_B + NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B + \
    NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B)

struct nvkmd_nvgpu_native_budget_layout {
   uint64_t backing_base; /* chunk 0; the primary segment starts here */
   uint64_t notifier_base;
};

/* Selects disjoint backing and notifier spans inside a CPU suffix that the
 * caller already owns. CPU and GPU starts get the same alignment. Arena
 * allocations need matching residues for images (8 KiB) and for the common
 * NVK command and shader chunks (64 KiB). This checks local geometry only.
 * Registration and map still need their ordinary service replies. On
 * invalid input, the output does not change. */
static inline bool
nvkmd_nvgpu_native_budget_layout(uint64_t cpu_base, uint64_t extent_B,
                                struct nvkmd_nvgpu_native_budget_layout *out)
{
   const uint64_t page_mask = NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B - 1;
   const uint64_t align_mask = NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B - 1;
   if (!out || !cpu_base || (cpu_base & page_mask) ||
       (extent_B & page_mask) || extent_B < NVKMD_NVGPU_NATIVE_CPU_EXTENT_B ||
       extent_B > UINT64_MAX - cpu_base)
      return false;
   const uint64_t padding = (0 - cpu_base) & align_mask;
   if (padding > extent_B ||
       NVKMD_NVGPU_NATIVE_PRIMARY_B > extent_B - padding ||
       NVKMD_NVGPU_NATIVE_NOTIFIER_PAGE_B >
          extent_B - padding - NVKMD_NVGPU_NATIVE_PRIMARY_B)
      return false;
   *out = (struct nvkmd_nvgpu_native_budget_layout) {
      .backing_base = cpu_base + padding,
      .notifier_base = cpu_base + padding + NVKMD_NVGPU_NATIVE_PRIMARY_B,
   };
   return true;
}

#endif
