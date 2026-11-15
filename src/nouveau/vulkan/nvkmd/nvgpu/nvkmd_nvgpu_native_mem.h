/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_MEM_H
#define NVKMD_NVGPU_NATIVE_MEM_H

#include "nvkmd/nvkmd.h"
#include "nvkmd_nvgpu_native_arena.h"

/* A pending VA owns the optional reserved slice until creation succeeds.
 * After that, the memory object owns it. The backing, map and VA lease of
 * the arena outlive every local slice. */
VkResult nvkmd_nvgpu_native_mem_create(
   struct nvkmd_dev *dev,
   struct nvkmd_nvgpu_native_arena *arena,
   uint64_t size_B,
   uint64_t align_B,
   enum nvkmd_mem_flags flags,
   uint8_t pte_kind,
   uint16_t tile_mode,
   const struct nvkmd_nvgpu_native_slice *reserved,
   struct nvkmd_mem **mem_out);

/* An attempt to import a native image pins the local allocation. The WSI
 * owner can unpin it only after it retires every related remote object and
 * the saved display-release dependency. */
bool nvkmd_nvgpu_native_mem_pin_scanout(struct nvkmd_mem *base);
bool nvkmd_nvgpu_native_mem_unpin_scanout(struct nvkmd_mem *base);

/* Matches a pending VA reservation to the same memory allocation and
 * generation. A GPU address match alone is not sufficient, because the
 * address can be recycled. */
/* Maps bytes [offset_B, +size_B) of a native memory object again at
 * view_addr with the kind. See
 * nvkmd_nvgpu_native_owner_map_view. */
struct nvkmd_nvgpu_native_view_source;
VkResult nvkmd_nvgpu_native_mem_map_view(
   const struct nvkmd_mem *base, uint64_t offset_B, uint64_t size_B,
   uint64_t view_addr, uint8_t kind,
   struct nvkmd_nvgpu_native_view_source *source_out, bool *left_mapped_out);

bool nvkmd_nvgpu_native_mem_matches_slice(
   const struct nvkmd_mem *base,
   const struct nvkmd_nvgpu_native_slice *slice);

#endif
