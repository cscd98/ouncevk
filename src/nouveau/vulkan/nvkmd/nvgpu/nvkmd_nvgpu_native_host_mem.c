/* SPDX-License-Identifier: MIT
 * Memory imported from a host pointer. See nvkmd_nvgpu_native_host_mem.h.
 */
#include "nvkmd_nvgpu_native_host_mem.h"
#include "nvkmd_nvgpu_native_cache.h"
#include "nvkmd_nvgpu_native_map.h"

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"

#include <stdlib.h>
#include <string.h>

/* The uncompressed PTE kind of ordinary memory (see
 * nvkmd_nvgpu_native_mem.c). */
#define HOST_MEM_PTE_KIND 6

/* One part of the span. */
struct native_host_part {
   uint64_t offset_B, size_B; /* in the span */
   struct nvkmd_nvgpu_native_backing backing;
   /* The backing is registered with the GPU service, until its release
    * succeeds. */
   bool registered;
   /* The part is, or may be, mapped in the VA, until its unmap succeeds. */
   bool mapped;
};

struct nvkmd_nvgpu_native_host_mem {
   struct nvkmd_mem base;
   struct nvkmd_va va;
   struct nvkmd_nvgpu_native_device_owner *owner;
   struct list_head link; /* in owner->host_mems, while registered */
   uint64_t cpu_base;
   bool registered;
   /* The program freed the memory, or its import failed, while a part
    * could not be retired. The owner keeps the record and frees it. */
   bool abandoned;
   uint32_t part_count;
   struct native_host_part parts[];
};

static bool
query(uint64_t address, MemoryInfo *info)
{
   u32 page_info = 0;
   memset(info, 0, sizeof(*info));
   return R_SUCCEEDED(svcQueryMemory(info, &page_info, address));
}

bool
nvkmd_nvgpu_native_host_ptr_importable(const void *address)
{
   MemoryInfo info;
   return address &&
          !((uintptr_t)address % NVKMD_NVGPU_NATIVE_HOST_ALIGN_B) &&
          query((uintptr_t)address, &info) &&
          nvkmd_nvgpu_native_backing_importable(&info);
}

/* Splits [base, +size_B) into parts: at the end of each kernel memory
 * block and at NVKMD_NVGPU_NATIVE_HOST_PART_B. Writes them to parts when
 * it is not NULL. Returns the number of parts, or 0 when a block of the
 * span is not importable. */
static uint32_t
native_host_split(uint64_t base, uint64_t size_B,
                  struct native_host_part *parts)
{
   uint32_t count = 0;
   uint64_t done_B = 0;
   while (done_B < size_B) {
      MemoryInfo info;
      const uint64_t at = base + done_B;
      if (!query(at, &info) || !nvkmd_nvgpu_native_backing_importable(&info))
         return 0;
      const uint64_t block_end = info.addr + info.size;
      if (block_end <= at)
         return 0;
      const uint64_t in_block_B = MIN2(block_end - at, size_B - done_B);
      for (uint64_t taken_B = 0; taken_B < in_block_B;) {
         const uint64_t part_B =
            MIN2(in_block_B - taken_B, NVKMD_NVGPU_NATIVE_HOST_PART_B);
         if (count == UINT32_MAX)
            return 0;
         if (parts)
            parts[count] = (struct native_host_part) {
               .offset_B = done_B + taken_B,
               .size_B = part_B,
            };
         count++;
         taken_B += part_B;
      }
      done_B += in_block_B;
   }
   return count;
}

/* Unmaps and releases each part, then returns the VA to the heap and takes
 * the memory off the owner's list. Call with the owner lock held.
 *
 * Returns false when a part may still be mapped or registered. The memory
 * then stays on the owner's list with its VA, so that neither is reused,
 * and a later call tries again.
 *
 * No GPU work is submitted here. Every batch that ran while the memory was
 * registered ends with the dirty L2 writeback (native_ctx_prepare_batch),
 * and the program completes its use of the memory before it frees it.
 */
static bool
native_host_mem_retire(struct nvkmd_nvgpu_native_host_mem *mem)
{
   struct nvkmd_nvgpu_native_device_owner *owner = mem->owner;
   if (!mem->registered)
      return true;

   bool retired = true;
   for (uint32_t i = 0; i < mem->part_count; i++) {
      struct native_host_part *part = &mem->parts[i];
      if (part->mapped) {
         Result rc = 0;
         if (nvkmd_nvgpu_native_unmap_range(owner->runtime, &part->backing,
                                            mem->va.addr + part->offset_B,
                                            part->size_B, &owner->slot,
                                            &rc) !=
             NVKMD_NVGPU_NATIVE_MAP_OK) {
            mesa_loge("host memory: unmap failed: 0x%x", rc);
            owner->stats.host_import_failures++;
            retired = false;
            continue;
         }
         part->mapped = false;
      }
      if (!part->registered)
         continue;
      /* A release that failed earlier left the backing not ready: its
       * remote state is unknown and it cannot be released again. */
      if (!part->backing.ready ||
          nvkmd_nvgpu_native_backing_release(&part->backing, owner->runtime,
                                             &owner->slot) !=
             NVKMD_NVGPU_BACKING_OK) {
         mesa_loge("host memory: release failed: 0x%x; the program's "
                   "memory may stay registered", part->backing.result);
         owner->stats.host_import_failures++;
         retired = false;
         continue;
      }
      part->registered = false;
   }
   if (!retired)
      return false;

   nvkmd_nvgpu_native_owner_release_va(owner, mem->va.addr, mem->va.size_B);
   mem->registered = false;
   list_del(&mem->link);
   return true;
}

static void
native_host_mem_free(struct nvkmd_mem *base)
{
   struct nvkmd_nvgpu_native_host_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_host_mem, base);
   struct nvkmd_nvgpu_native_device_owner *owner = mem->owner;
   if (nvkmd_nvgpu_native_owner_lock(owner)) {
      if (!native_host_mem_retire(mem))
         mem->abandoned = true;
      nvkmd_nvgpu_native_owner_unlock(owner);
   }
   simple_mtx_destroy(&base->map_mutex);
   /* An abandoned record stays on the owner's list. */
   if (!mem->abandoned)
      free(mem);
}

static VkResult
native_host_mem_map(struct nvkmd_mem *base, struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags flags, void *fixed_addr,
                    void **map_out)
{
   (void)log_obj;
   const struct nvkmd_nvgpu_native_host_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_host_mem, base);
   /* The CPU view is the imported pointer; it cannot be placed. */
   if (!map_out || fixed_addr || (flags & NVKMD_MEM_MAP_FIXED) ||
       !(flags & NVKMD_MEM_MAP_RDWR))
      return VK_ERROR_MEMORY_MAP_FAILED;
   *map_out = (void *)(uintptr_t)mem->cpu_base;
   return VK_SUCCESS;
}

static void
native_host_mem_unmap(struct nvkmd_mem *base,
                      enum nvkmd_mem_map_flags flags, void *map)
{
   /* The program's memory stays where it is. */
   (void)base;
   (void)flags;
   (void)map;
}

/* Flush and invalidate both run dc civac, as for readback memory. */
static void
native_host_mem_sync(struct nvkmd_mem *base, uint64_t offset_B,
                     uint64_t range_B)
{
   const struct nvkmd_nvgpu_native_host_mem *mem =
      container_of(base, struct nvkmd_nvgpu_native_host_mem, base);
   if (offset_B >= base->size_B)
      return;
   nvkmd_nvgpu_native_dcache_clean_invalidate(
      mem->cpu_base + offset_B, MIN2(range_B, base->size_B - offset_B));
}

static uint32_t
native_host_mem_log_handle(struct nvkmd_mem *base)
{
   (void)base;
   return 0;
}

static const struct nvkmd_mem_ops native_host_mem_ops = {
   .free = native_host_mem_free,
   .map = native_host_mem_map,
   .unmap = native_host_mem_unmap,
   .sync_to_gpu = native_host_mem_sync,
   .sync_from_gpu = native_host_mem_sync,
   .log_handle = native_host_mem_log_handle,
};

/* Registers and maps each part. Call with the owner lock held. On failure,
 * the parts done so far are retired by the caller. */
static VkResult
native_host_mem_register(struct nvkmd_nvgpu_native_host_mem *mem)
{
   struct nvkmd_nvgpu_native_device_owner *owner = mem->owner;
   for (uint32_t i = 0; i < mem->part_count; i++) {
      struct native_host_part *part = &mem->parts[i];
      const enum nvkmd_nvgpu_native_backing_status status =
         nvkmd_nvgpu_native_backing_import(&part->backing, owner->runtime,
                                           mem->cpu_base + part->offset_B,
                                           part->size_B);
      if (status != NVKMD_NVGPU_BACKING_OK) {
         mesa_loge("host memory: registering 0x%llx bytes at 0x%llx failed: "
                   "status %u, result 0x%x",
                   (unsigned long long)part->size_B,
                   (unsigned long long)(mem->cpu_base + part->offset_B),
                   (unsigned)status, part->backing.result);
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }
      part->registered = true;
      Result rc = 0;
      const enum nvkmd_nvgpu_native_map_status mapped =
         nvkmd_nvgpu_native_map_range(owner->runtime, &part->backing,
                                      mem->va.addr + part->offset_B, 0,
                                      part->size_B, HOST_MEM_PTE_KIND, true,
                                      &owner->slot, &rc);
      /* With MAP_KIND the mapping exists, with another kind. With MAP_WAIT
       * it may exist. Both are unmapped when the memory is retired. */
      part->mapped = mapped == NVKMD_NVGPU_NATIVE_MAP_OK ||
                     mapped == NVKMD_NVGPU_NATIVE_MAP_KIND ||
                     mapped == NVKMD_NVGPU_NATIVE_MAP_WAIT;
      if (mapped != NVKMD_NVGPU_NATIVE_MAP_OK) {
         mesa_loge("host memory: map failed: status %u, result 0x%x",
                   (unsigned)mapped, rc);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }
   return VK_SUCCESS;
}

VkResult
nvkmd_nvgpu_native_host_mem_create(
   struct nvkmd_dev *dev, struct nvkmd_nvgpu_native_device_owner *owner,
   void *host_ptr, uint64_t size_B, enum nvkmd_mem_flags flags,
   struct nvkmd_mem **mem_out)
{
   const uint64_t base = (uintptr_t)host_ptr;
   if (!dev || !owner || !mem_out || !host_ptr || !size_B ||
       base % NVKMD_NVGPU_NATIVE_HOST_ALIGN_B ||
       size_B % NVKMD_NVGPU_NATIVE_HOST_ALIGN_B ||
       size_B > UINT64_MAX - base)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   const uint32_t count = native_host_split(base, size_B, NULL);
   if (!count)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct nvkmd_nvgpu_native_host_mem *mem =
      calloc(1, sizeof(*mem) + (size_t)count * sizeof(mem->parts[0]));
   if (!mem)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   mem->owner = owner;
   mem->cpu_base = base;
   mem->part_count = count;
   /* The span can change between the two walks; then refuse it. */
   if (native_host_split(base, size_B, mem->parts) != count) {
      free(mem);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   if (!nvkmd_nvgpu_native_owner_lock(owner)) {
      free(mem);
      return VK_ERROR_DEVICE_LOST;
   }
   if (!owner->host_mems_ready) {
      list_inithead(&owner->host_mems);
      owner->host_mems_ready = true;
   }
   const uint64_t va = nvkmd_nvgpu_native_owner_reserve_va(
      owner, size_B, NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B);
   if (!va) {
      nvkmd_nvgpu_native_owner_unlock(owner);
      free(mem);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   mem->va = (struct nvkmd_va) {
      .dev = dev,
      .addr = va,
      .size_B = size_B,
   };
   mem->registered = true;
   list_addtail(&mem->link, &owner->host_mems);
   const VkResult result = native_host_mem_register(mem);
   if (result != VK_SUCCESS) {
      owner->stats.host_import_failures++;
      if (!native_host_mem_retire(mem))
         mem->abandoned = true;
      nvkmd_nvgpu_native_owner_unlock(owner);
      if (!mem->abandoned)
         free(mem);
      return result;
   }
   owner->stats.host_imports++;
   nvkmd_nvgpu_native_owner_unlock(owner);

   nvkmd_mem_init(dev, &mem->base, &native_host_mem_ops,
                  flags | NVKMD_MEM_CAN_MAP | NVKMD_MEM_CPU_CACHED, size_B,
                  NVKMD_NVGPU_NATIVE_HOST_ALIGN_B);
   mem->base.va = &mem->va;
   *mem_out = &mem->base;
   return VK_SUCCESS;
}

bool
nvkmd_nvgpu_native_owner_release_host_mems(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner->host_mems_ready)
      return true;
   bool released = true;
   list_for_each_entry_safe(struct nvkmd_nvgpu_native_host_mem, mem,
                            &owner->host_mems, link) {
      if (!native_host_mem_retire(mem)) {
         released = false;
         continue;
      }
      /* The program no longer has an abandoned record. */
      if (mem->abandoned)
         free(mem);
   }
   return released;
}
