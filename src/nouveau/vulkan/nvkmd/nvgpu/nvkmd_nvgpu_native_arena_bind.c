/* SPDX-License-Identifier: MIT
 * Original OunceVK code. It joins the owners of the native backing, map and
 * VA lease into one arena.
 */
#include "nvkmd_nvgpu_native_arena.h"

#include "nvkmd_nvgpu_native_backing.h"
#include "nvkmd_nvgpu_native_map.h"

enum nvkmd_nvgpu_native_arena_status
nvkmd_nvgpu_native_arena_bind(
   struct nvkmd_nvgpu_native_arena *arena,
   const struct nvkmd_nvgpu_native_runtime *runtime,
   const struct nvkmd_nvgpu_native_backing *backing,
   const struct nvkmd_nvgpu_native_map_owner *map,
   const struct nvkmd_nvgpu_native_va_lease *lease)
{
   if (!arena || !runtime || !backing || !map || !lease)
      return NVKMD_NVGPU_NATIVE_ARENA_ARGUMENT;
   if (runtime->self != runtime || !runtime->generation ||
       runtime->stage != NVKMD_NVGPU_RUNTIME_TSG_OPEN_RECEIVED ||
       backing->self != backing || !backing->ready ||
       (!backing->cpu_uncached && !backing->cpu_cached) ||
       backing->generation != runtime->generation ||
       !backing->range_id || !backing->maps ||
       map->self != map || !map->ready ||
       map->generation != runtime->generation ||
       map->vm_id != runtime->vm.id ||
       map->range_id != backing->range_id ||
       map->length != backing->length ||
       lease->runtime_generation != runtime->generation ||
       lease->vm_id != runtime->vm.id || lease->as_fd != runtime->as.fd ||
       lease->start != map->gpu_va || lease->length != map->length)
      return NVKMD_NVGPU_NATIVE_ARENA_STATE;

   enum nvkmd_nvgpu_native_arena_status result = nvkmd_nvgpu_native_arena_init(
      arena, runtime->generation, backing->cpu_base, map->gpu_va,
      backing->length);
   /* The arena is host-coherent only when the CPU side (mode 2) and the GPU
    * map are both uncached. A CPU-cached backing (mode 1) is host-mappable
    * but not coherent, also behind a GPU-cached map: CPU maintenance and
    * the batch's GPU cache operations publish the data. */
   if (result == NVKMD_NVGPU_NATIVE_ARENA_OK) {
      arena->coherent_eligible = map->gpu_uncached && backing->cpu_uncached;
      arena->host_mappable = map->gpu_uncached || backing->cpu_cached;
      arena->cpu_cached = backing->cpu_cached;
   }
   return result;
}
