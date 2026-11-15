/* SPDX-License-Identifier: MIT
 * Views: mappings of backing bytes at a GPU VA that has no memory of its
 * own (an image plane, a sparse buffer).
 *
 * A mapping whose unmap failed may still exist. Its record and its VA are
 * kept: in the VA's view list while the VA object lives, and with the owner
 * afterwards, until device close unmaps it.
 */
#ifndef NVKMD_NVGPU_NATIVE_VIEWS_H
#define NVKMD_NVGPU_NATIVE_VIEWS_H

#include "nvkmd_nvgpu_native_device_owner.h"

/* A GPU VA range that stays out of the VA heap after its object was
 * destroyed, because mappings in it may remain. Each such mapping has one
 * reference; the range returns to the heap with the last. */
struct nvkmd_nvgpu_native_va_hold {
   uint64_t addr, size_B;
   uint32_t references;
};

/* A hold with one reference, the caller's. NULL when there is no memory;
 * the caller then leaves the VA reserved. */
struct nvkmd_nvgpu_native_va_hold *
nvkmd_nvgpu_native_va_hold_create(uint64_t addr, uint64_t size_B);

/* Drops one reference. Call with the owner lock held. */
void nvkmd_nvgpu_native_va_hold_drop(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_va_hold *hold);

/* One bound range of a reserved VA: memory mapped again at the VA. */
struct nvkmd_nvgpu_native_view {
   uint64_t offset_B, size_B;
   struct nvkmd_nvgpu_native_view_source source;
};

/* The views of one reserved VA. They do not overlap. Every function takes
 * the owner lock itself or is called with it held. */
struct nvkmd_nvgpu_native_view_list {
   uint64_t va_addr, va_size_B;
   uint8_t pte_kind;
   struct nvkmd_nvgpu_native_view *views;
   uint32_t count, capacity;
};

/* Makes room for one more view. A mapping must not exist without its view,
 * so this comes before the map request. */
bool nvkmd_nvgpu_native_view_list_reserve(
   struct nvkmd_nvgpu_native_view_list *list);

/* Maps the view's backing bytes at its place in the VA and adds it to the
 * list. A view that failed but may be mapped all the same is added too, so
 * that it is unmapped with the others and its VA is not reused. */
VkResult nvkmd_nvgpu_native_view_list_map(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list,
   const struct nvkmd_nvgpu_native_view *view);

/* Unbinds [offset_B, +size_B) of the VA. e5 removes whole mappings only,
 * so a view that the range covers in part is removed and its remaining
 * head and tail are mapped again from the same backing (remove, then a
 * distinct e3: no atomic replacement). */
VkResult nvkmd_nvgpu_native_view_list_unbind(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list,
   uint64_t offset_B, uint64_t size_B);

/* For the destruction of the VA object: unmaps every view and returns the
 * VA to the heap. Views that may still be mapped go to the owner, which
 * keeps the VA until device close has unmapped them. */
void nvkmd_nvgpu_native_view_list_finish(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_view_list *list);

/* Maps backing bytes at view_addr for a view. Returns false when the view
 * cannot be used. *left_mapped_out then says whether its mapping exists or
 * may exist all the same: the worker accepted the request, and either its
 * completion was not seen, or the mapping has another kind and could not
 * be removed. The caller keeps a view record for such a mapping. Call with
 * the owner lock held. */
bool nvkmd_nvgpu_native_owner_map_view_range(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_backing *backing,
   uint64_t backing_offset_B, uint64_t size_B,
   uint64_t view_addr, uint8_t kind,
   bool gpu_cacheable, bool *left_mapped_out);

/* At device close: unmaps the views that outlived their VA object and
 * returns their VAs. Returns false when a view may still be mapped. */
bool nvkmd_nvgpu_native_owner_release_retired_views(
   struct nvkmd_nvgpu_native_device_owner *owner);

#endif
