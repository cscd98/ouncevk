/* SPDX-License-Identifier: MIT
 * Original OunceVK code. It manages the local geometry of a retained native
 * CPU/GPU mapping. It does not own the service-side registration.
 */
#include "nvkmd_nvgpu_native_arena.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { NATIVE_ARENA_PAGE_BYTES = 4096 };

struct nvkmd_nvgpu_native_arena_record {
   uint64_t offset, length, serial;
   bool pinned, released;
   struct nvkmd_nvgpu_native_arena_record *next;
};

static int
power_of_two(uint64_t value)
{
   return value && !(value & (value - 1));
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_init(struct nvkmd_nvgpu_native_arena *arena,
                              uint64_t generation, uint64_t cpu_base,
                              uint64_t gpu_base, uint64_t length)
{
   if (!arena || !generation || !cpu_base || !gpu_base || !length ||
       (cpu_base & (NATIVE_ARENA_PAGE_BYTES - 1)) ||
       (gpu_base & (NATIVE_ARENA_PAGE_BYTES - 1)) ||
       (length & (NATIVE_ARENA_PAGE_BYTES - 1)) ||
       length > UINT64_MAX - cpu_base || length > UINT64_MAX - gpu_base)
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   if (arena->self || arena->generation || arena->cpu_base || arena->gpu_base ||
       arena->length || arena->cursor || arena->coherent_eligible ||
       arena->recycle_poisoned || arena->next_serial ||
       arena->reused_bytes || arena->live_records || arena->free_ranges)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;

   arena->self = arena;
   arena->generation = generation;
   arena->cpu_base = cpu_base;
   arena->gpu_base = gpu_base;
   arena->length = length;
   arena->cursor = 0;
   arena->next_serial = 1;
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

static bool
reclaimable_slice_matches(const struct nvkmd_nvgpu_native_arena *arena,
                          const struct nvkmd_nvgpu_native_slice *slice,
                          const struct nvkmd_nvgpu_native_arena_record *record)
{
   return slice->generation == arena->generation &&
          slice->serial == record->serial &&
          slice->offset == record->offset &&
          slice->length == record->length &&
          slice->cpu == arena->cpu_base + record->offset &&
          slice->gpu == arena->gpu_base + record->offset;
}

static struct nvkmd_nvgpu_native_arena_record **
find_live_record(struct nvkmd_nvgpu_native_arena *arena,
                 const struct nvkmd_nvgpu_native_slice *slice)
{
   struct nvkmd_nvgpu_native_arena_record **link = &arena->live_records;
   while (*link) {
      if ((*link)->serial == slice->serial &&
          reclaimable_slice_matches(arena, slice, *link))
         return link;
      link = &(*link)->next;
   }
   return NULL;
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_alloc_reclaimable(
   struct nvkmd_nvgpu_native_arena *arena, uint64_t length,
   uint64_t alignment, struct nvkmd_nvgpu_native_slice *slice)
{
   if (!arena || !slice || !length || !power_of_two(alignment) ||
       alignment < NATIVE_ARENA_PAGE_BYTES ||
       length > UINT64_MAX - (NATIVE_ARENA_PAGE_BYTES - 1))
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   if (arena->self != arena || !arena->generation || !arena->length ||
       arena->cursor > arena->length || arena->recycle_poisoned ||
       arena->retiring ||
       !arena->next_serial || arena->next_serial == UINT64_MAX)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;
   if ((arena->cpu_base & (alignment - 1)) !=
       (arena->gpu_base & (alignment - 1)))
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;

   const uint64_t padded =
      (length + NATIVE_ARENA_PAGE_BYTES - 1) &
      ~(uint64_t)(NATIVE_ARENA_PAGE_BYTES - 1);
   struct nvkmd_nvgpu_native_arena_record **link = &arena->free_ranges;
   while (*link) {
      struct nvkmd_nvgpu_native_arena_record *range = *link;
      const uint64_t pad =
         (alignment - ((arena->gpu_base + range->offset) &
                       (alignment - 1))) & (alignment - 1);
      if (pad <= range->length && padded <= range->length - pad) {
         struct nvkmd_nvgpu_native_arena_record *live = malloc(sizeof(*live));
         if (!live)
            return NVKMD_NVGPU_NATIVE_ARENA_HOST_MEMORY;
         struct nvkmd_nvgpu_native_arena_record *tail = NULL;
         const uint64_t suffix = range->length - pad - padded;
         if (pad && suffix) {
            tail = malloc(sizeof(*tail));
            if (!tail) {
               free(live);
               return NVKMD_NVGPU_NATIVE_ARENA_HOST_MEMORY;
            }
         }
         const uint64_t offset = range->offset + pad;
         if (pad && suffix) {
            *tail = (struct nvkmd_nvgpu_native_arena_record) {
               .offset = offset + padded, .length = suffix,
               .next = range->next,
            };
            range->length = pad;
            range->next = tail;
         } else if (pad) {
            range->length = pad;
         } else if (suffix) {
            range->offset = offset + padded;
            range->length = suffix;
         } else {
            *link = range->next;
            free(range);
         }
         *live = (struct nvkmd_nvgpu_native_arena_record) {
            .offset = offset, .length = padded,
            .serial = arena->next_serial++, .next = arena->live_records,
         };
         arena->live_records = live;
         *slice = (struct nvkmd_nvgpu_native_slice) {
            .generation = arena->generation,
            .cpu = arena->cpu_base + offset,
            .gpu = arena->gpu_base + offset,
            .offset = offset, .length = padded, .serial = live->serial,
         };
         arena->reused_bytes += padded;
         return NVKMD_NVGPU_NATIVE_ARENA_OK;
      }
      link = &range->next;
   }

   struct nvkmd_nvgpu_native_arena_record *live = malloc(sizeof(*live));
   if (!live)
      return NVKMD_NVGPU_NATIVE_ARENA_HOST_MEMORY;
   struct nvkmd_nvgpu_native_slice fresh = {0};
   const enum nvkmd_nvgpu_native_arena_status status =
      nvkmd_nvgpu_native_arena_alloc(arena, length, alignment, &fresh);
   if (status != NVKMD_NVGPU_NATIVE_ARENA_OK) {
      free(live);
      return status;
   }
   *live = (struct nvkmd_nvgpu_native_arena_record) {
      .offset = fresh.offset, .length = fresh.length,
      .serial = arena->next_serial++, .next = arena->live_records,
   };
   arena->live_records = live;
   fresh.serial = live->serial;
   *slice = fresh;
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_pin(struct nvkmd_nvgpu_native_arena *arena,
                             const struct nvkmd_nvgpu_native_slice *slice)
{
   if (!arena || !slice || !slice->serial || arena->self != arena)
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   if (arena->retiring)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;
   struct nvkmd_nvgpu_native_arena_record **link =
      find_live_record(arena, slice);
   if (!link || (*link)->released)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;
   (*link)->pinned = true;
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

static void
recycle_live_record(struct nvkmd_nvgpu_native_arena *arena,
                    struct nvkmd_nvgpu_native_arena_record **link)
{
   struct nvkmd_nvgpu_native_arena_record *record = *link;
   *link = record->next;
   record->serial = 0;
   record->next = NULL;
   struct nvkmd_nvgpu_native_arena_record **free_link =
      &arena->free_ranges;
   struct nvkmd_nvgpu_native_arena_record *previous = NULL;
   while (*free_link && (*free_link)->offset < record->offset) {
      previous = *free_link;
      free_link = &(*free_link)->next;
   }
   record->next = *free_link;
   *free_link = record;
   if (previous && previous->offset + previous->length == record->offset) {
      previous->length += record->length;
      previous->next = record->next;
      free(record);
      record = previous;
   }
   if (record->next && record->offset + record->length ==
                       record->next->offset) {
      struct nvkmd_nvgpu_native_arena_record *next = record->next;
      record->length += next->length;
      record->next = next->next;
      free(next);
   }
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_unpin(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_slice *slice)
{
   if (!arena || !slice || !slice->serial || arena->self != arena)
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   struct nvkmd_nvgpu_native_arena_record **link =
      find_live_record(arena, slice);
   if (!link || !(*link)->pinned)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;
   (*link)->pinned = false;
   if ((*link)->released && !arena->recycle_poisoned)
      recycle_live_record(arena, link);
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_release(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_slice *slice)
{
   if (!arena || !slice || !slice->serial || arena->self != arena)
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   struct nvkmd_nvgpu_native_arena_record **link =
      find_live_record(arena, slice);
   if (!link || (*link)->released)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;
   if ((*link)->pinned || arena->recycle_poisoned) {
      (*link)->released = true;
      return NVKMD_NVGPU_NATIVE_ARENA_OK;
   }
   recycle_live_record(arena, link);
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

void
nvkmd_nvgpu_native_arena_poison(struct nvkmd_nvgpu_native_arena *arena)
{
   if (arena && arena->self == arena)
      arena->recycle_poisoned = true;
}

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_alloc(struct nvkmd_nvgpu_native_arena *arena,
                               uint64_t length, uint64_t alignment,
                               struct nvkmd_nvgpu_native_slice *slice)
{
   if (!arena || !slice || !length || !power_of_two(alignment) ||
       alignment < NATIVE_ARENA_PAGE_BYTES ||
       length > UINT64_MAX - (NATIVE_ARENA_PAGE_BYTES - 1))
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   if (arena->self != arena || !arena->generation || !arena->length ||
       arena->cursor > arena->length || arena->retiring)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;

   /* One offset must satisfy both CPU and GPU alignment. */
   if ((arena->cpu_base & (alignment - 1)) !=
       (arena->gpu_base & (alignment - 1)))
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   const uint64_t padded =
      (length + NATIVE_ARENA_PAGE_BYTES - 1) &
      ~(uint64_t)(NATIVE_ARENA_PAGE_BYTES - 1);
   const uint64_t alignment_offset =
      (alignment - ((arena->gpu_base + arena->cursor) & (alignment - 1))) &
      (alignment - 1);
   if (alignment_offset > arena->length - arena->cursor ||
       padded > arena->length - arena->cursor - alignment_offset)
      return NVKMD_NVGPU_NATIVE_ARENA_EXHAUSTED;
   const uint64_t offset = arena->cursor + alignment_offset;
   *slice = (struct nvkmd_nvgpu_native_slice) {
      .generation = arena->generation,
      .cpu = arena->cpu_base + offset,
      .gpu = arena->gpu_base + offset,
      .offset = offset,
      .length = padded,
   };
   arena->cursor = offset + padded;
   return NVKMD_NVGPU_NATIVE_ARENA_OK;
}

void
nvkmd_nvgpu_native_arena_finish(struct nvkmd_nvgpu_native_arena *arena)
{
   if (!arena)
      return;
   struct nvkmd_nvgpu_native_arena_record *lists[2] = {
      arena->live_records, arena->free_ranges,
   };
   for (unsigned i = 0; i < 2; i++) {
      while (lists[i]) {
         struct nvkmd_nvgpu_native_arena_record *next = lists[i]->next;
         free(lists[i]);
         lists[i] = next;
      }
   }
   memset(arena, 0, sizeof(*arena));
}

uint64_t
nvkmd_nvgpu_native_arena_live_bytes(const struct nvkmd_nvgpu_native_arena *arena)
{
   if (!arena || arena->self != arena)
      return 0;
   uint64_t bytes = 0;
   for (const struct nvkmd_nvgpu_native_arena_record *r = arena->live_records;
        r; r = r->next)
      if (!r->released)
         bytes += r->length;
   return bytes;
}
