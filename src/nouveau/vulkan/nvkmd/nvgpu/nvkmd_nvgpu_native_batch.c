/* SPDX-License-Identifier: MIT */
/* Native GPFIFO composition. The CMIF-11 submit accepts an array of entries.
 * The ring allows R - 3 caller entries.
 */
#include "nvkmd_nvgpu_native_batch.h"

#include "nvkmd_nvgpu_semaphore_probe.h"

#include <stddef.h>

enum { NATIVE_BATCH_PAGE_B = 4096 };
static const uint32_t native_batch_guard_before = UINT32_C(0xb16b00b5);
static const uint32_t native_batch_guard_after = UINT32_C(0xa71ecafe);

const uint32_t
nvkmd_nvgpu_native_batch_head_barrier[NVKMD_NVGPU_NATIVE_BATCH_HEAD_WORDS] = {
   UINT32_C(0x20018040), /* NVC7B5_NOP (0x0100), one word, subchannel 4. */
   UINT32_C(0x00000000),
};

const uint32_t
nvkmd_nvgpu_native_batch_cache_invalidate[NVKMD_NVGPU_NATIVE_BATCH_CACHE_INVALIDATE_WORDS] = {
   UINT32_C(0x20010000), /* SET_OBJECT, subchannel 0 */
   UINT32_C(0x0000c797), /* Ampere 3D class */
   UINT32_C(0x80000044), /* WAIT_FOR_IDLE (0x0110), immediate 0 */
   UINT32_C(0x800004ce), /* INVALIDATE_TEXTURE_DATA_CACHE, lines all */
   UINT32_C(0x800004cd), /* INVALIDATE_TEXTURE_HEADER_CACHE, lines all */
   UINT32_C(0x90110087), /* INVALIDATE_SHADER_CACHES: instruction, data, constant */
};

const uint32_t
nvkmd_nvgpu_native_batch_l2_invalidate[NVKMD_NVGPU_NATIVE_BATCH_L2_INVALIDATE_WORDS] = {
   UINT32_C(0x2004c00a), /* MEM_OP_A-D, subchannel 6 */
   0, 0, 0,
   UINT32_C(0x70000000), /* MEM_OP_D: L2_INVALIDATE_CLEAN_LINES (14) */
};

const uint32_t
nvkmd_nvgpu_native_batch_dirty_flush[NVKMD_NVGPU_NATIVE_BATCH_DIRTY_FLUSH_WORDS] = {
   UINT32_C(0x80000014), /* SET_REFERENCE (0x50), immediate 0 */
   UINT32_C(0x2004c00a), /* MEM_OP_A-D, subchannel 6 */
   0, 0, 0,
   UINT32_C(0x80000000), /* MEM_OP_D: L2_FLUSH_DIRTY */
   UINT32_C(0x2004c00a), /* MEM_OP_A-D */
   0, 0, 0,              /* MEM_OP_C: SYS_MEMBAR */
   UINT32_C(0x28000000), /* MEM_OP_D: MEMBAR */
};

void
nvkmd_nvgpu_native_shim_acquire_encode(
   uint32_t words[NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS],
   uint64_t gpu_addr, uint32_t value)
{
   words[0] = UINT32_C(0x2004c004); /* SEMAPHOREA, 4 methods, subch 6 */
   words[1] = (uint32_t)(gpu_addr >> 32);
   words[2] = (uint32_t)gpu_addr;
   words[3] = value;
   words[4] = UINT32_C(0x00001004); /* ACQ_GEQ | ACQUIRE_SWITCH_ENABLED */
}

enum nvkmd_nvgpu_native_batch_status
nvkmd_nvgpu_native_batch_prepare(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   struct nvkmd_nvgpu_native_batch *batch)
{
   if (!count)
      return NVKMD_NVGPU_NATIVE_BATCH_ARGUMENT;
   return nvkmd_nvgpu_native_batch_prepare_waits(
      arena, spans, count, marker_value, head_barrier, NULL, 0, batch);
}

enum nvkmd_nvgpu_native_batch_status
nvkmd_nvgpu_native_batch_prepare_waits(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   const uint32_t *waits, uint32_t wait_words,
   struct nvkmd_nvgpu_native_batch *batch)
{
   const struct nvkmd_nvgpu_native_arena *const sources[] = { arena };
   return nvkmd_nvgpu_native_batch_prepare_sources(
      arena, sources, 1, spans, count, marker_value, head_barrier,
      waits, wait_words, false, false, false, false, NULL, 0, batch);
}

static bool
span_in_arena(const struct nvkmd_nvgpu_native_arena *arena,
              const struct nvkmd_nvgpu_native_span *span)
{
   return arena && arena->self == arena && arena->gpu_base &&
          arena->cursor <= arena->length &&
          span->gpu_addr >= arena->gpu_base &&
          span->gpu_addr - arena->gpu_base <= arena->cursor &&
          span->size_B <= arena->cursor - (span->gpu_addr - arena->gpu_base);
}

/* The source arena holding a span, or NULL. */
static const struct nvkmd_nvgpu_native_arena *
span_source(const struct nvkmd_nvgpu_native_arena *const *sources,
            uint32_t source_count, const struct nvkmd_nvgpu_native_span *span)
{
   for (uint32_t i = 0; i < source_count; i++) {
      if (span_in_arena(sources[i], span))
         return sources[i];
   }
   return NULL;
}

enum nvkmd_nvgpu_native_batch_status
nvkmd_nvgpu_native_batch_prepare_sources(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_arena *const *sources, uint32_t source_count,
   const struct nvkmd_nvgpu_native_span *spans,
   uint32_t count, uint32_t marker_value, bool head_barrier,
   const uint32_t *waits, uint32_t wait_words, bool flush_dirty,
   bool invalidate_l2, bool copy_channel, bool invalidate_caches,
   const uint32_t *replay, uint32_t replay_words,
   struct nvkmd_nvgpu_native_batch *batch)
{
   const bool head = head_barrier || wait_words || invalidate_l2 ||
                     copy_channel || invalidate_caches || replay_words;
   if (!arena || !sources || !source_count ||
       (count && !spans) || !batch || !marker_value ||
       (!count && !wait_words) ||
       (wait_words && !waits) || (replay_words && !replay) ||
       wait_words > NVKMD_NVGPU_NATIVE_BATCH_MAX_WAIT_WORDS ||
       wait_words % NVKMD_NVGPU_NATIVE_SHIM_ACQUIRE_WORDS ||
       count > (uint32_t)NVKMD_NVGPU_NATIVE_BATCH_MAX_SPANS - head ||
       arena->self != arena || !arena->cpu_base || !arena->gpu_base ||
       arena->cursor > arena->length || batch->prepared || batch->slice.gpu)
      return NVKMD_NVGPU_NATIVE_BATCH_ARGUMENT;

   const uint32_t flush_words =
      flush_dirty ? NVKMD_NVGPU_NATIVE_BATCH_DIRTY_FLUSH_WORDS : 0;
   const uint32_t packet_bytes =
      (flush_words + NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS) * sizeof(uint32_t);
   const uint32_t result_bytes = 3 * sizeof(uint32_t);
   const uint32_t invalidate_words =
      invalidate_l2 ? NVKMD_NVGPU_NATIVE_BATCH_L2_INVALIDATE_WORDS : 0;
   const uint32_t object_words =
      copy_channel ? NVKMD_NVGPU_NATIVE_BATCH_COPY_OBJECT_WORDS : 0;
   const uint32_t cache_words =
      invalidate_caches ? NVKMD_NVGPU_NATIVE_BATCH_CACHE_INVALIDATE_WORDS : 0;
   const uint32_t head_words = object_words + wait_words + invalidate_words +
      cache_words + replay_words +
      (head_barrier ? NVKMD_NVGPU_NATIVE_BATCH_HEAD_WORDS : 0);
   const uint32_t head_bytes = head_words * sizeof(uint32_t);
   uint64_t source_bytes = 0;
   for (uint32_t i = 0; i < count; i++) {
      const struct nvkmd_nvgpu_native_span *span = &spans[i];
      if (!span->size_B || (span->size_B & 3u) ||
          (span->gpu_addr & 3u) ||
          span->size_B / 4 > UINT32_C(0x1fffff) ||
          (i + 1 == count && span->incomplete))
         return NVKMD_NVGPU_NATIVE_BATCH_ARGUMENT;
      if (!span_source(sources, source_count, span))
         return NVKMD_NVGPU_NATIVE_BATCH_SOURCE;
      if (source_bytes > UINT64_MAX - span->size_B)
         return NVKMD_NVGPU_NATIVE_BATCH_LIMIT;
      source_bytes += span->size_B;
   }
   if (source_bytes > arena->length ||
       source_bytes > UINT64_MAX - packet_bytes - result_bytes - head_bytes ||
       source_bytes + packet_bytes + result_bytes + head_bytes > arena->length)
      return NVKMD_NVGPU_NATIVE_BATCH_LIMIT;

   /* Only the completion marker packet, its guards and an optional head
    * barrier (after the guards) are batch-owned. */
   struct nvkmd_nvgpu_native_slice slice = {0};
   if (nvkmd_nvgpu_native_arena_alloc_reclaimable(
          arena, packet_bytes + result_bytes + head_bytes,
          NATIVE_BATCH_PAGE_B, &slice) != NVKMD_NVGPU_NATIVE_ARENA_OK)
      return NVKMD_NVGPU_NATIVE_BATCH_ARENA;

   /* A source that overlaps a freshly allocated slice was not a current
    * allocation. Never submit a span that points at released storage. */
   for (uint32_t i = 0; i < count; i++) {
      if (span_source(sources, source_count, &spans[i]) != arena)
         continue; /* the slice is in the primary arena */
      const uint64_t source_offset = spans[i].gpu_addr - arena->gpu_base;
      if (source_offset < slice.offset + slice.length &&
          slice.offset < source_offset + spans[i].size_B) {
         nvkmd_nvgpu_native_arena_release(arena, &slice);
         return NVKMD_NVGPU_NATIVE_BATCH_SOURCE;
      }
   }

   /* Each NVK push span is submitted in place, as on Linux NVK. Vulkan
    * forbids a reset or free of a pending command buffer. NVK reuses push
    * memory only after its sync point completes. So the sources outlive the
    * join. A reference, not a copy, also keeps GPU-patched no-prefetch pushes
    * visible to the GPU. */
   uint8_t *const command = (uint8_t *)(uintptr_t)slice.cpu;
   const uint32_t first = head;
   if (head) {
      /* Foreign waits come first. Then the same-channel barrier. Both come
       * before every span of this batch. */
      uint32_t *const words =
         (uint32_t *)(void *)(command + packet_bytes + result_bytes);
      uint32_t w = 0;
      if (copy_channel) {
         /* SET_OBJECT on subchannel 4: the copy class. NVK's upload
          * context does not bind it; on the primary channel the main
          * queue does. */
         words[w++] = UINT32_C(0x20018000);
         words[w++] = UINT32_C(0x0000c7b5);
      }
      for (uint32_t i = 0; i < wait_words; i++)
         words[w++] = waits[i];
      for (uint32_t i = 0; i < invalidate_words; i++)
         words[w++] = nvkmd_nvgpu_native_batch_l2_invalidate[i];
      for (uint32_t i = 0; i < cache_words; i++)
         words[w++] = nvkmd_nvgpu_native_batch_cache_invalidate[i];
      for (uint32_t i = 0; i < replay_words; i++)
         words[w++] = replay[i];
      for (uint32_t i = 0; head_barrier &&
                          i < NVKMD_NVGPU_NATIVE_BATCH_HEAD_WORDS; i++)
         words[w++] = nvkmd_nvgpu_native_batch_head_barrier[i];
      batch->entries[0] = (struct nvkmd_nvgpu_native_batch_entry) {
         .gpu_va = slice.gpu + packet_bytes + result_bytes,
         .word_count = head_words,
      };
   }
   for (uint32_t i = 0; i < count; i++) {
      batch->entries[first + i] = (struct nvkmd_nvgpu_native_batch_entry) {
         .gpu_va = spans[i].gpu_addr,
         .word_count = spans[i].size_B / 4,
      };
   }

   /* The completion entry: the optional dirty flush, then the marker. */
   uint32_t *const packet = (uint32_t *)(void *)command;
   for (uint32_t i = 0; i < flush_words; i++)
      packet[i] = nvkmd_nvgpu_native_batch_dirty_flush[i];
   const uint64_t marker_gpu = slice.gpu + packet_bytes + sizeof(uint32_t);
   _Static_assert(NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS == 9,
                  "the copy-channel marker has 9 words");
   if (copy_channel) {
      /* Host methods only: SEMAPHOREA-D release of 4 bytes after the
       * channel is idle (RELEASE_WFI enabled), padded with Host NOPs to
       * the length of the 3D marker. */
      uint32_t *const marker = packet + flush_words;
      marker[0] = UINT32_C(0x2004c004); /* SEMAPHOREA-D, subch 6 */
      marker[1] = (uint32_t)(marker_gpu >> 32);
      marker[2] = (uint32_t)marker_gpu;
      marker[3] = marker_value;
      marker[4] = UINT32_C(0x01000002); /* RELEASE, 4 bytes, WFI */
      marker[5] = UINT32_C(0x2001c002); /* NOP, subch 6 */
      marker[6] = 0;
      marker[7] = UINT32_C(0x2001c002); /* NOP, subch 6 */
      marker[8] = 0;
   } else if (!nvkmd_nvgpu_semaphore_probe_encode(packet + flush_words,
                                                  marker_gpu, marker_value)) {
      nvkmd_nvgpu_native_arena_release(arena, &slice);
      return NVKMD_NVGPU_NATIVE_BATCH_MARKER;
   }
   batch->entries[first + count] = (struct nvkmd_nvgpu_native_batch_entry) {
      .gpu_va = slice.gpu,
      .word_count = flush_words + NVKMD_NVGPU_SEMAPHORE_PROBE_WORDS,
   };
   volatile uint32_t *const results =
      (volatile uint32_t *)(void *)(command + packet_bytes);
   results[0] = native_batch_guard_before;
   results[1] = 0;
   results[2] = native_batch_guard_after;

   batch->slice = slice;
   batch->command_gpu = slice.gpu;
   batch->command_words = (uint32_t)(packet_bytes / sizeof(uint32_t));
   batch->entry_count = first + count + 1;
   batch->marker_value = marker_value;
   batch->flush_dirty = flush_dirty;
   batch->guard_before = &results[0];
   batch->marker = &results[1];
   batch->guard_after = &results[2];
   batch->prepared = true;
   return NVKMD_NVGPU_NATIVE_BATCH_OK;
}

bool
nvkmd_nvgpu_native_batch_verify(
   const struct nvkmd_nvgpu_native_batch *batch)
{
   return batch && batch->prepared && batch->slice.gpu &&
          batch->command_gpu == batch->slice.gpu &&
          batch->entry_count >= 2 &&
          batch->entry_count <= NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES &&
          batch->guard_before && batch->marker && batch->guard_after &&
          *batch->guard_before == native_batch_guard_before &&
          *batch->marker == batch->marker_value &&
          *batch->guard_after == native_batch_guard_after;
}
