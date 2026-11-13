/* SPDX-License-Identifier: MIT */

/* Original OunceVK local-only VA lease policy. It does not reserve VA
 * remotely, map memory or provide GPU backing.
 */
#include "nvkmd_nvgpu_native_va.h"

#include <string.h>

#define VA_PAGE UINT64_C(0x1000)
/* The game AS the runtime imports: [0x08000000, 0xc000000000). */
#define SELECTED_AS_FIRST UINT64_C(0x08000000)
#define SELECTED_AS_END UINT64_C(0xc000000000)

static bool
power_of_two(uint64_t n)
{
   return n && !(n & (n - 1));
}

/* The region must lie in exactly one VM user range. That range must also
 * contain the shim base, or end one byte before it. The inclusive VM user
 * interval ends one byte before the exclusive AS end. The shim query can
 * return that exclusive end as the shim base. Thus the base need not lie
 * inside the user interval. */
static bool
user_window(const struct nvkmd_nvgpu_native_runtime *r, uint64_t first,
            uint64_t end, uint64_t shim_base)
{
   unsigned matches = 0;
   if (first >= end || end > shim_base || shim_base > SELECTED_AS_END)
      return false;
   for (unsigned i = 0; i < r->vm.user_count; i++) {
      const uint64_t lo = r->vm.user[i].first, last = r->vm.user[i].last;
      if (lo < SELECTED_AS_FIRST || lo > last || last >= SELECTED_AS_END)
         return false;
      for (unsigned j = 0; j < i; j++) {
         if (lo <= r->vm.user[j].last && r->vm.user[j].first <= last)
            return false;
      }
      if (first >= lo && end - 1 <= last && shim_base >= lo &&
          shim_base <= last + 1)
         matches++;
   }
   return matches == 1;
}

static bool
region(const struct nvkmd_nvgpu_native_runtime *r, unsigned i,
       uint64_t *start, uint64_t *end)
{
   const uint64_t base = r->as_post.region[i].base;
   const uint64_t count = r->as_post.region[i].page_count;
   const uint32_t page = r->as_post.region[i].page_size;
   if (!power_of_two(page) || page < VA_PAGE || !count ||
       (base & (page - 1)) || count > UINT64_MAX / page ||
       base > UINT64_MAX - count * page)
      return false;
   *start = base;
   *end = base + count * page;
   return *end <= SELECTED_AS_END;
}

enum nvkmd_nvgpu_native_va_status
nvkmd_nvgpu_native_va_admit(struct nvkmd_nvgpu_native_va_owner *o,
                            struct nvkmd_nvgpu_native_runtime *r)
{
   if (!o || !r)
      return NVKMD_NVGPU_NATIVE_VA_ARGUMENT;
   if (o->self || r->va_claim || !nvkmd_nvgpu_native_runtime_ready(r) ||
       armGetTls() != r->tls)
      return NVKMD_NVGPU_NATIVE_VA_STATE;

   /* Accept one region, or two disjoint regions. The first region must use
    * 4 KiB pages. The second region is not used. The regions must be below
    * a selected, page-aligned shim. */
   if (r->as_post.region_count < 1 || r->as_post.region_count > 2 ||
       !r->as_post.shim_selected ||
       !r->as_post.shim_count || r->as_post.shim_count == UINT32_MAX ||
       !power_of_two(r->as_post.shim_stride) || r->as_post.shim_stride < VA_PAGE ||
       (r->as_post.shim_gpu_va & (VA_PAGE - 1)))
      return NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED;
   uint64_t first[2] = {0}, end[2] = {0};
   for (unsigned i = 0; i < r->as_post.region_count; i++) {
      if (!region(r, i, &first[i], &end[i]) ||
          !user_window(r, first[i], end[i], r->as_post.shim_gpu_va))
         return NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED;
   }
   if (r->as_post.region[0].page_size != VA_PAGE ||
       (r->as_post.region_count == 2 && first[0] < end[1] && first[1] < end[0]))
      return NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED;

   *o = (struct nvkmd_nvgpu_native_va_owner) {
      .self = o,
      .runtime = r,
      .generation = r->generation,
      .vm_id = r->vm.id,
      .as_fd = r->as.fd,
      .start = first[0],
      .end = end[0],
      .shim_base = r->as_post.shim_gpu_va,
   };
   r->va_claim = o;
   return NVKMD_NVGPU_NATIVE_VA_OK;
}

struct nvkmd_nvgpu_native_va_result
nvkmd_nvgpu_native_va_lease_whole(struct nvkmd_nvgpu_native_va_owner *o,
                                  uint64_t length, uint64_t alignment)
{
   struct nvkmd_nvgpu_native_va_result result = {
      .status = NVKMD_NVGPU_NATIVE_VA_STATE,
   };
   if (!o || o->self != o || !o->runtime || o->runtime->va_claim != o ||
       o->runtime->generation != o->generation || o->leased)
      return result;
   if (!length || (length & (VA_PAGE - 1)) ||
       !power_of_two(alignment) || alignment < VA_PAGE) {
      result.status = NVKMD_NVGPU_NATIVE_VA_ARGUMENT;
      return result;
   }
   if (o->start > UINT64_MAX - (alignment - 1)) {
      result.status = NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED;
      return result;
   }
   const uint64_t start = (o->start + alignment - 1) & ~(alignment - 1);
   if (start >= o->end || length > o->end - start) {
      result.status = NVKMD_NVGPU_NATIVE_VA_UNSUPPORTED;
      return result;
   }
   o->leased = true;
   result.status = NVKMD_NVGPU_NATIVE_VA_OK;
   result.lease = (struct nvkmd_nvgpu_native_va_lease) {
      .owner = o,
      .runtime_generation = o->generation,
      .vm_id = o->vm_id,
      .as_fd = o->as_fd,
      .start = start,
      .length = length,
      .serial = 1,
   };
   return result;
}
