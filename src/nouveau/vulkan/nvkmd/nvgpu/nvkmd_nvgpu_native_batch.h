/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_BATCH_H
#define NVKMD_NVGPU_NATIVE_BATCH_H

#include "nvkmd_nvgpu_native_arena.h"

#include <stdbool.h>
#include <stdint.h>

/* The 0x800-entry ring allows at most 0x800 - 3 caller entries. Reserve the
 * final caller entry for the GPU-written completion marker. */
enum {
   NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES = 0x800 - 3,
   NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS =
      NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES - 1,
};

struct nvkmd_nvgpu_native_span {
   uint64_t gpu_addr;
   uint32_t size_B;
   bool incomplete;
};

enum nvkmd_nvgpu_native_batch_status {
   NVKMD_NVGPU_NATIVE_BATCH_OK,
   NVKMD_NVGPU_NATIVE_BATCH_ARGUMENT,
   NVKMD_NVGPU_NATIVE_BATCH_SOURCE,
   NVKMD_NVGPU_NATIVE_BATCH_LIMIT,
   NVKMD_NVGPU_NATIVE_BATCH_ARENA,
   NVKMD_NVGPU_NATIVE_BATCH_MARKER,
};

struct nvkmd_nvgpu_native_batch_entry {
   uint64_t gpu_va;
   uint32_t word_count;
};

/* References arena-backed NVK spans in place. It adds one final C797 marker
 * entry from a small reclaimable slice. Each source span stays a separate
 * GPFIFO entry in the same ioctl. This keeps incomplete-method chains alive.
 * Sources must stay allocated until the join. Vulkan forbids reuse of a
 * pending command buffer, and NVK syncs its upload-stream push memory. The
 * caller must join producer, marker and health before it releases the marker
 * slice. */
struct nvkmd_nvgpu_native_batch {
   struct nvkmd_nvgpu_native_slice slice;
   uint64_t command_gpu;
   uint32_t command_words;
   uint32_t entry_count;
   struct nvkmd_nvgpu_native_batch_entry
      entries[NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES];
   uint32_t marker_value;
   /* The completion entry starts with the ordered dirty-L2 flush. */
   bool flush_dirty;
   volatile uint32_t *guard_before;
   volatile uint32_t *marker;
   volatile uint32_t *guard_after;
   bool prepared;
};

/* Head barrier. A first entry holds one copy-engine NOP (subchannel 4,
 * NVC7B5_NOP 0x0100). Every batch ends with the 3D (subchannel 0) marker.
 * So this is a graphics to copy-engine subchannel switch. On that switch,
 * Host waits until the graphics engine finishes its methods and goes idle.
 * Host then sends a sysmem barrier flush before any later method runs. The
 * switch back to graphics also forces an FE cache invalidate (open-gpu-doc
 * manuals/ampere/ga100/dev_ram.ref.txt, "If a method stream
 * subchannel-switches ..."). It orders a batch after all earlier work on
 * this channel without a CPU join. */
enum { NVKMD_NVGPU_NATIVE_BATCH_HEAD_WORDS = 2 };
extern const uint32_t
   nvkmd_nvgpu_native_batch_head_barrier[NVKMD_NVGPU_NATIVE_BATCH_HEAD_WORDS];

/* The GPU L2 clean-line invalidate before the spans of a batch, so that the
 * GPU reads the CPU's writes to GPU-cached memory (MEM_OP operation 14,
 * with MEM_OP_A-C written as zero). */
enum { NVKMD_NVGPU_NATIVE_BATCH_L2_INVALIDATE_WORDS = 5 };

/* On the copy channel, the head binds the copy class to
 * subchannel 4, and the completion marker uses Host methods only. */
enum { NVKMD_NVGPU_NATIVE_BATCH_COPY_OBJECT_WORDS = 2 };

/* After a wait for the other channel, on the primary channel: the GPU
 * caches that the other channel's writes do not update (texture data,
 * texture headers, shader instruction/data/constant caches) are
 * invalidated. A semaphore acquire does not invalidate them, and NVK does
 * so only for pipeline barriers. */
enum { NVKMD_NVGPU_NATIVE_BATCH_CACHE_INVALIDATE_WORDS = 6 };
extern const uint32_t
   nvkmd_nvgpu_native_batch_cache_invalidate[NVKMD_NVGPU_NATIVE_BATCH_CACHE_INVALIDATE_WORDS];
extern const uint32_t
   nvkmd_nvgpu_native_batch_l2_invalidate[NVKMD_NVGPU_NATIVE_BATCH_L2_INVALIDATE_WORDS];

/* The ordered flush before a CPU-flush fence. It is SET_REFERENCE(0), which
 * waits for every earlier method on the channel and issues a system membar.
 * Then come L2_FLUSH_DIRTY and SYS_MEMBAR. The dirty flush is sent with
 * explicit zero MEM_OP_A-C, as the public header requires. Once a completion
 * behind these words is seen, no dirty L2 line of earlier work remains. Its
 * memory can then return to the CPU. Pool finalize relies on such a fence and
 * flushes nothing itself.
 */
enum { NVKMD_NVGPU_NATIVE_BATCH_DIRTY_FLUSH_WORDS = 11 };
extern const uint32_t
   nvkmd_nvgpu_native_batch_dirty_flush[NVKMD_NVGPU_NATIVE_BATCH_DIRTY_FLUSH_WORDS];

/* With head_barrier, count may be at most NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS
 * - 1 and the batch has count + 2 entries. */
enum nvkmd_nvgpu_native_batch_status nvkmd_nvgpu_native_batch_prepare(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   struct nvkmd_nvgpu_native_batch *batch);

/* A Host semaphore acquire on a syncpoint shim slot, for a VI2 release. It is
 * an increasing-method packet of four Host methods SEMAPHOREA-D (NVC76F
 * 0x10-0x1c; Host methods ignore the subchannel field 6), address high and
 * low, the 32-bit payload, and operation ACQ_GEQ with ACQUIRE_SWITCH_ENABLED
 * (0x1004; open-gpu-doc classes/host/clc76f.h). The comparison is NVIDIA's
 * circular 32-bit one for this legacy acquire.
 */
enum { NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS = 5 };
void nvkmd_nvgpu_native_shim_acquire_encode(
   uint32_t words[NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS],
   uint64_t gpu_addr, uint32_t value);

/* As nvkmd_nvgpu_native_batch_prepare, with wait_words (whole shim
 * acquires) at the start of the head entry, before the optional barrier.
 * With waits, count may be zero: the batch then only waits and completes. */
enum { NVKMD_NVGPU_NATIVE_BATCH_MAX_WAIT_WORDS =
          8 * NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS };
enum nvkmd_nvgpu_native_batch_status nvkmd_nvgpu_native_batch_prepare_waits(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   const uint32_t *waits, uint32_t wait_words,
   struct nvkmd_nvgpu_native_batch *batch);

/* As nvkmd_nvgpu_native_batch_prepare_waits, with the spans allowed in any
 * of the source arenas: the device's later memory segments as well as the
 * primary arena, which holds the batch's own packet. NVK places command
 * memory in a later segment once the primary one is full (for example
 * with 4K swapchain images). With flush_dirty, the completion entry starts
 * with nvkmd_nvgpu_native_batch_dirty_flush. With invalidate_l2, the head
 * entry has nvkmd_nvgpu_native_batch_l2_invalidate after the waits. With
 * replay_words, the head entry has those words (a context's channel state)
 * after the invalidations and before the barrier. */
enum nvkmd_nvgpu_native_batch_status nvkmd_nvgpu_native_batch_prepare_sources(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_arena *const *sources, uint32_t source_count,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   const uint32_t *waits, uint32_t wait_words, bool flush_dirty,
   bool invalidate_l2, bool copy_channel, bool invalidate_caches,
   const uint32_t *replay, uint32_t replay_words,
   struct nvkmd_nvgpu_native_batch *batch);

/* Requires the caller's successful CPU visibility barrier. Marker equality
 * alone never confirms GPU completion. */
bool nvkmd_nvgpu_native_batch_verify(
   const struct nvkmd_nvgpu_native_batch *batch);

#endif
