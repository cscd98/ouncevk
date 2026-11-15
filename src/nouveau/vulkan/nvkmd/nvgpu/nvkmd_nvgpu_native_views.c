/* SPDX-License-Identifier: MIT
 * Views of backing bytes at reserved GPU VAs, and what keeps a view's
 * record and VA when it may still be mapped. Original OunceVK code.
 */
#include "nvkmd_nvgpu_native_views.h"
#include "nvkmd_nvgpu_native_chunked.h"
#include "nvkmd_nvgpu_native_map.h"

#include "util/log.h"
#include "util/macros.h"

#include <stdlib.h>

struct nvkmd_nvgpu_native_va_hold *
nvkmd_nvgpu_native_va_hold_create(uint64_t addr, uint64_t size_B)
{
   struct nvkmd_nvgpu_native_va_hold *hold = calloc(1, sizeof(*hold));
   if (!hold)
      return NULL;
   hold->addr = addr;
   hold->size_B = size_B;
   hold->references = 1;
   return hold;
}

void
nvkmd_nvgpu_native_va_hold_drop(struct nvkmd_nvgpu_native_device_owner *owner,
                                struct nvkmd_nvgpu_native_va_hold *hold)
{
   if (--hold->references)
      return;
   nvkmd_nvgpu_native_owner_release_va(owner, hold->addr, hold->size_B);
   free(hold);
}

/* A view that outlived its VA object: its mapping may remain. */
struct nvkmd_nvgpu_native_retired_view {
   struct nvkmd_nvgpu_native_retired_view *next;
   struct nvkmd_nvgpu_native_backing *backing;
   uint64_t addr, size_B;
   struct nvkmd_nvgpu_native_va_hold *hold;
};

bool
nvkmd_nvgpu_native_owner_map_view_range(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t backing_offset_B, uint64_t size_B,
   uint64_t view_addr, uint8_t kind,
   bool gpu_cacheable, bool *left_mapped_out)
{
   Result rc = 0;
   const enum nvkmd_nvgpu_native_map_status status =
      nvkmd_nvgpu_native_map_range(owner->runtime, backing, view_addr,
                                   backing_offset_B, size_B, kind,
                                   gpu_cacheable, &owner->slot, &rc);
   *left_mapped_out = false;
   if (status == NVKMD_NVGPU_NATIVE_MAP_OK) {
      owner->stats.view_maps++;
      return true;
   }
   owner->stats.view_map_failures++;
   mesa_loge("native view: map of 0x%llx bytes, kind %u, failed: status %u, "
             "result 0x%x", (unsigned long long)size_B, kind,
             (unsigned)status, rc);
   if (status == NVKMD_NVGPU_NATIVE_MAP_KIND)
      *left_mapped_out =
         nvkmd_nvgpu_native_unmap_range(owner->runtime, backing, view_addr,
                                        size_B, &owner->slot, &rc) !=
         NVKMD_NVGPU_NATIVE_MAP_OK;
   else if (status == NVKMD_NVGPU_NATIVE_MAP_WAIT)
      *left_mapped_out = true;
   return false;
}

VkResult
nvkmd_nvgpu_native_owner_map_backing(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_view_source *source,
   uint64_t size_B, uint64_t view_addr, uint8_t kind, bool *left_mapped_out)
{
   if (!owner || !source || !source->backing || !left_mapped_out || !size_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   *left_mapped_out = false;
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return VK_ERROR_DEVICE_LOST;
   const bool mapped = nvkmd_nvgpu_native_owner_map_view_range(
      owner, source->backing, source->backing_offset_B, size_B, view_addr,
      kind, source->gpu_cacheable, left_mapped_out);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return mapped ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

bool
nvkmd_nvgpu_native_owner_unmap_view(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t view_addr, uint64_t size_B)
{
   if (!owner || !backing || !nvkmd_nvgpu_native_owner_lock(owner))
      return false;
   Result rc = 0;
   const bool unmapped =
      nvkmd_nvgpu_native_unmap_range(owner->runtime, backing, view_addr,
                                     size_B, &owner->slot, &rc) ==
      NVKMD_NVGPU_NATIVE_MAP_OK;
   if (!unmapped) {
      owner->stats.view_map_failures++;
      mesa_loge("native view: unmap of 0x%llx bytes failed: result 0x%x",
                (unsigned long long)size_B, rc);
   } else if (owner->stranded_chunks) {
      /* This may have been the last view of a chunk whose memory the
       * program freed first. */
      nvkmd_nvgpu_native_owner_reclaim_chunks(owner);
   }
   nvkmd_nvgpu_native_owner_unlock(owner);
   return unmapped;
}

bool
nvkmd_nvgpu_native_view_list_reserve(struct nvkmd_nvgpu_native_view_list *list)
{
   if (list->count < list->capacity)
      return true;
   const uint32_t capacity = MAX2(4, list->capacity * 2);
   struct nvkmd_nvgpu_native_view *views =
      realloc(list->views, capacity * sizeof(*views));
   if (!views)
      return false;
   list->views = views;
   list->capacity = capacity;
   return true;
}

VkResult
nvkmd_nvgpu_native_view_list_map(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list,
   const struct nvkmd_nvgpu_native_view *view)
{
   if (!nvkmd_nvgpu_native_view_list_reserve(list))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bool left_mapped = false;
   const VkResult result = nvkmd_nvgpu_native_owner_map_backing(
      owner, &view->source, view->size_B, list->va_addr + view->offset_B,
      list->pte_kind, &left_mapped);
   if (result == VK_SUCCESS || left_mapped)
      list->views[list->count++] = *view;
   return result;
}

VkResult
nvkmd_nvgpu_native_view_list_unbind(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list,
   uint64_t offset_B, uint64_t size_B)
{
   if (size_B > UINT64_MAX - offset_B)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   const uint64_t end_B = offset_B + size_B;
   /* The parts to keep, at most two for each view, and the view that each
    * comes from. */
   struct nvkmd_nvgpu_native_view *keep =
      calloc(2 * (size_t)list->count + 1, sizeof(*keep));
   uint32_t *keep_from = calloc(2 * (size_t)list->count + 1,
                                sizeof(*keep_from));
   bool *removed = calloc((size_t)list->count + 1, sizeof(*removed));
   if (!keep || !keep_from || !removed) {
      free(keep);
      free(keep_from);
      free(removed);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   uint32_t keep_count = 0;
   for (uint32_t i = 0; i < list->count; i++) {
      const struct nvkmd_nvgpu_native_view *view = &list->views[i];
      const uint64_t view_end = view->offset_B + view->size_B;
      if (view_end <= offset_B || view->offset_B >= end_B)
         continue;
      if (view->offset_B < offset_B) {
         keep_from[keep_count] = i;
         keep[keep_count++] = (struct nvkmd_nvgpu_native_view) {
            .offset_B = view->offset_B,
            .size_B = offset_B - view->offset_B,
            .source = view->source,
         };
      }
      if (view_end > end_B) {
         struct nvkmd_nvgpu_native_view_source tail = view->source;
         tail.backing_offset_B += end_B - view->offset_B;
         keep_from[keep_count] = i;
         keep[keep_count++] = (struct nvkmd_nvgpu_native_view) {
            .offset_B = end_B,
            .size_B = view_end - end_B,
            .source = tail,
         };
      }
   }
   /* Remove every view that the range touches, as whole records. A view
    * whose unmap failed stays whole: its mapping may remain, so nothing is
    * mapped in its place. */
   VkResult result = VK_SUCCESS;
   uint32_t kept = 0;
   for (uint32_t i = 0; i < list->count; i++) {
      const struct nvkmd_nvgpu_native_view view = list->views[i];
      const bool touched = view.offset_B + view.size_B > offset_B &&
                           view.offset_B < end_B;
      if (touched &&
          nvkmd_nvgpu_native_owner_unmap_view(owner,
                                              view.source.backing,
                                              list->va_addr + view.offset_B,
                                              view.size_B)) {
         removed[i] = true;
         continue;
      }
      if (touched)
         result = VK_ERROR_DEVICE_LOST;
      list->views[kept++] = view;
   }
   list->count = kept;
   /* Map the kept parts of the removed views again. */
   for (uint32_t i = 0; i < keep_count; i++) {
      if (!removed[keep_from[i]])
         continue;
      const VkResult mapped = nvkmd_nvgpu_native_view_list_map(owner, list, &keep[i]);
      if (mapped != VK_SUCCESS) {
         result = mapped;
         break;
      }
   }
   free(keep);
   free(keep_from);
   free(removed);
   return result;
}

/* Gives a view that may still be mapped to the owner, with a reference to
 * the hold of its VA. */
static void
native_view_retire(struct nvkmd_nvgpu_native_device_owner *owner,
                   const struct nvkmd_nvgpu_native_view_list *list,
                   const struct nvkmd_nvgpu_native_view *view,
                   struct nvkmd_nvgpu_native_va_hold *hold)
{
   /* The reference stays also without a record: the VA is then never
    * returned, because nothing can unmap the view any more. */
   hold->references++;
   struct nvkmd_nvgpu_native_retired_view *retired =
      calloc(1, sizeof(*retired));
   if (!retired) {
      mesa_loge("native view: no memory for the record of a view of 0x%llx "
                "bytes that may still be mapped",
                (unsigned long long)view->size_B);
      return;
   }
   retired->backing = view->source.backing;
   retired->addr = list->va_addr + view->offset_B;
   retired->size_B = view->size_B;
   retired->hold = hold;
   retired->next = owner->retired_views;
   owner->retired_views = retired;
}

void
nvkmd_nvgpu_native_view_list_finish(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list)
{
   uint32_t kept = 0;
   for (uint32_t i = 0; i < list->count; i++) {
      const struct nvkmd_nvgpu_native_view view = list->views[i];
      if (!nvkmd_nvgpu_native_owner_unmap_view(owner, view.source.backing,
                                               list->va_addr + view.offset_B,
                                               view.size_B))
         list->views[kept++] = view;
   }
   if (!kept) {
      nvkmd_nvgpu_native_owner_release_va(owner, list->va_addr,
                                          list->va_size_B);
   } else {
      mesa_loge("native view: %u views of a VA of 0x%llx bytes may still be "
                "mapped; device close unmaps them", kept,
                (unsigned long long)list->va_size_B);
      struct nvkmd_nvgpu_native_va_hold *hold =
         nvkmd_nvgpu_native_va_hold_create(list->va_addr, list->va_size_B);
      /* Without a hold, the VA stays reserved and the views are lost. */
      if (hold) {
         for (uint32_t i = 0; i < kept; i++)
            native_view_retire(owner, list, &list->views[i], hold);
         nvkmd_nvgpu_native_va_hold_drop(owner, hold);
      }
   }
   free(list->views);
   list->views = NULL;
   list->count = 0;
   list->capacity = 0;
}

bool
nvkmd_nvgpu_native_owner_release_retired_views(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_retired_view *view = owner->retired_views;
   owner->retired_views = NULL;
   while (view) {
      struct nvkmd_nvgpu_native_retired_view *next = view->next;
      Result rc = 0;
      if (nvkmd_nvgpu_native_unmap_range(owner->runtime, view->backing,
                                         view->addr, view->size_B,
                                         &owner->slot, &rc) ==
          NVKMD_NVGPU_NATIVE_MAP_OK) {
         nvkmd_nvgpu_native_va_hold_drop(owner, view->hold);
         free(view);
      } else {
         mesa_loge("native view: unmap of 0x%llx bytes failed again: "
                   "result 0x%x", (unsigned long long)view->size_B, rc);
         view->next = owner->retired_views;
         owner->retired_views = view;
      }
      view = next;
   }
   /* A chunk whose memory was freed may have waited for one of them. */
   if (owner->stranded_chunks)
      nvkmd_nvgpu_native_owner_reclaim_chunks(owner);
   return !owner->retired_views;
}
