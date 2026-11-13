/* SPDX-License-Identifier: MIT
 * OunceVK owner for a native channel submit. The submit is CMIF 11 (Ioctl2)
 * 0xc018481b, sent after command cache ordering. libounce-nx's nvIoctl2 sends
 * it.
 */
#include "nvkmd_nvgpu_native_submit.h"

#include <stdint.h>
#include <string.h>

#define SUBMIT_IOCTL UINT32_C(0xc018481b)
#define SUBMIT_GPU_VA_LIMIT (UINT64_C(1) << 40)
#define SUBMIT_WORD_COUNT_MAX UINT32_C(0x1fffff)

static void
write_le32(uint8_t *bytes, uint32_t value)
{
   for (unsigned i = 0; i < 4; i++)
      bytes[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t
read_le32(const uint8_t *bytes)
{
   return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
          ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* GPFIFO entry: low word the 4-byte-aligned GPU VA, high word the VA's
 * upper bits, the word count at bit 10, bit 9 set, and bit 31 when the
 * span flags request no prefetch (flag 4). */
static bool
encode_entry(uint8_t out[8], const struct nvkmd_nvgpu_native_batch_entry *entry,
             uint32_t span_flags)
{
   if ((entry->gpu_va & 3) || entry->gpu_va >= SUBMIT_GPU_VA_LIMIT ||
       !entry->word_count || entry->word_count > SUBMIT_WORD_COUNT_MAX)
      return false;
   const uint32_t low = (uint32_t)entry->gpu_va & UINT32_C(0xfffffffc);
   const uint32_t high = (uint32_t)(entry->gpu_va >> 32) |
                         (entry->word_count << 10) | UINT32_C(0x200) |
                         ((span_flags & 4) ? UINT32_C(0x80000000) : 0);
   write_le32(out, low);
   write_le32(out + 4, high);
   return true;
}

enum nvkmd_nvgpu_native_submit_status
nvkmd_nvgpu_native_submit_once_endpoint(
   struct nvkmd_nvgpu_native_submit *submit,
   const struct nvkmd_nvgpu_gpu_endpoint *endpoint,
   const struct nvkmd_nvgpu_native_batch_entry *entries,
   uint32_t entry_count,
   uint32_t span_flags,
   uint32_t submit_flags)
{
   if (!submit || !endpoint || !entries || !entry_count ||
       entry_count > NVKMD_NVGPU_NATIVE_BATCH_MAX_ENTRIES)
      return NVKMD_NVGPU_SUBMIT_ARGUMENT;
   if (submit->self || submit->attempted ||
       !nvkmd_nvgpu_gpu_endpoint_valid(endpoint))
      return NVKMD_NVGPU_SUBMIT_STATE;

   submit->self = submit;
   submit->attempted = true;
   submit->generation = endpoint->generation;
   for (uint32_t i = 0; i < entry_count; i++) {
      if (!encode_entry(submit->entry + 8 * i, &entries[i], span_flags))
         return NVKMD_NVGPU_SUBMIT_ENTRY;
   }

   /* Control: +8 entry count, +12 flags. The service returns the producer
    * ID at +16 and its target at +20. */
   memset(submit->control, 0, sizeof(submit->control));
   write_le32(submit->control + 8, entry_count);
   write_le32(submit->control + 12, submit_flags);

   __asm__ volatile("dsb sy" ::: "memory");
   submit->send_started = true;
   submit->result = nvIoctl2(endpoint->gpu_fd, SUBMIT_IOCTL, submit->control,
                             submit->entry, (size_t)entry_count * 8);
   if (R_FAILED(submit->result))
      return NVKMD_NVGPU_SUBMIT_REPLY;

   const uint32_t returned_count = read_le32(submit->control + 8);
   const uint32_t returned_flags = read_le32(submit->control + 12);
   const uint32_t returned_id = read_le32(submit->control + 16);
   /* The producer target is a wrapping low-32 counter. Zero is a valid
    * target right after UINT32_MAX. Completion checks progress against its
    * pre-submit baseline. */
   if (returned_count != entry_count || returned_flags != submit_flags ||
       returned_id != endpoint->producer_id)
      return NVKMD_NVGPU_SUBMIT_RESULT;
   submit->returned_id = returned_id;
   submit->returned_target = read_le32(submit->control + 20);
   submit->accepted = true;
   return NVKMD_NVGPU_SUBMIT_OK;
}
