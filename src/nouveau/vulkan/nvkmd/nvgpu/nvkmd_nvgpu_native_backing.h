/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_BACKING_H
#define NVKMD_NVGPU_NATIVE_BACKING_H

#include "nvkmd_nvgpu_native_runtime.h"

struct nvkmd_nvgpu_native_slot;

enum nvkmd_nvgpu_native_backing_status {
   NVKMD_NVGPU_BACKING_OK,
   NVKMD_NVGPU_BACKING_ARGUMENT,
   NVKMD_NVGPU_BACKING_STATE,
   NVKMD_NVGPU_BACKING_QUERY,
   NVKMD_NVGPU_BACKING_PROFILE,
   NVKMD_NVGPU_BACKING_PREPARE,
   NVKMD_NVGPU_BACKING_REGISTER,
   NVKMD_NVGPU_BACKING_SET_ATTRIBUTES,
   NVKMD_NVGPU_BACKING_FINAL_ATTRIBUTES,
   NVKMD_NVGPU_BACKING_UNREGISTER,
   NVKMD_NVGPU_BACKING_WAIT,
   /* An earlier request on the slot is unresolved: nothing was sent. */
   NVKMD_NVGPU_BACKING_SLOT,
};

/* A CPU span registered with the GPU worker as one range. Its bytes and
 * the worker and client of the runtime stay alive through every map of the
 * range and every GPU use. Keep the object at a stable address. */
struct nvkmd_nvgpu_native_backing {
   const struct nvkmd_nvgpu_native_backing *self;
   uint64_t generation;
   uint64_t cpu_base, length;
   uint32_t range_id;
   uint32_t maps;             /* live maps of this range */
   Result result;             /* the failed step's Result */
   bool attempted, ready, cpu_uncached;
   /* Mode 1: the CPU span keeps its cached attribute (no attribute-8 walk).
    * CPU maps need explicit cache maintenance. */
   bool cpu_cached;
   /* What a release still has to undo. Each is set when its step of the
    * registration succeeds, also when a later step fails, and cleared when
    * its step of the release succeeds:
    * the SVC 0x80 prepare bit is set over the span; */
   bool prepared;
   /* the worker registered the range (range_id); */
   bool registered;
   /* the uncached CPU attribute is set over the span (mode 2). */
   bool attribute_set;
};

/* True while a release has something to undo: from the first successful
 * step of a registration until a release completes. A backing can be held
 * without being ready, after a registration or a release that failed. */
static inline bool
nvkmd_nvgpu_native_backing_held(const struct nvkmd_nvgpu_native_backing *backing)
{
   return backing->self == backing &&
          (backing->prepared || backing->registered || backing->attribute_set);
}

/* Registers a page-aligned heap span. When cpu_cached is false, this is
 * mode 2. The steps are a QueryMemory check, zeroing, SVC 0x80 (mask and
 * value 0x20) and worker raw 0x75. Then CPU attribute 8 (uncached) is set
 * over the span. cpu_cached selects mode 1. Registration and map do not
 * change, but the span keeps its cached attribute. A failure after a
 * request was sent makes the backing unusable.
 */
enum nvkmd_nvgpu_native_backing_status nvkmd_nvgpu_native_backing_prepare(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t cpu_base, uint64_t length, bool cpu_cached);

/* The same for mode 2, without the zeroing: the caller has cleared the
 * whole span, and nothing wrote to it since. A caller can thus clear the
 * memory before it takes the owner lock for the registration. */
enum nvkmd_nvgpu_native_backing_status
nvkmd_nvgpu_native_backing_prepare_cleared(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t cpu_base, uint64_t length);

/* Registers a span of memory that the program owns, for an imported host
 * pointer (VK_EXT_external_memory_host). As mode 1, but the span keeps its
 * contents, and besides heap it can be memory of another state that may
 * be device-mappable (see nvkmd_nvgpu_native_backing_importable).
 * The span stays CPU-cached. When the service refuses the span, the SVC
 * 0x80 bit is cleared again and the backing can be used for another try.
 * Release it with nvkmd_nvgpu_native_backing_release, which leaves the
 * contents.
 */
enum nvkmd_nvgpu_native_backing_status nvkmd_nvgpu_native_backing_import(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   uint64_t cpu_base, uint64_t length);

/* True when svcQueryMemory's info describes memory that
 * nvkmd_nvgpu_native_backing_import accepts: a device-mappable state,
 * read/write, no attributes and no IPC or device references. */
bool nvkmd_nvgpu_native_backing_importable(const MemoryInfo *info);

/* Releases a held backing that has no live maps. For mode 2, it first
 * restores the CPU attribute (SVC 3, mask 8, value 0). It then sends worker
 * raw 0x76 on the request slot and waits for its completion. Last, it
 * clears SVC 0x80 (mask 0x20, value 0).
 *
 * The backing is not ready afterwards. A step that fails leaves the steps
 * before it done, and a later call continues with the failed step: a
 * refused unregister request is sent again, and after a completed one only
 * the SVC 0x80 clear is repeated.
 */
enum nvkmd_nvgpu_native_backing_status nvkmd_nvgpu_native_backing_release(
   struct nvkmd_nvgpu_native_backing *backing,
   struct nvkmd_nvgpu_native_runtime *runtime,
   struct nvkmd_nvgpu_native_slot *slot);

/* Returns every span that is still registered to plain memory: clears its
 * uncached attribute (mode 2) and its SVC 0x80 bit, as a release does. For
 * the exit hook only, after it closed the GPU sessions, so the service no
 * longer has the spans: a program that exits without destroying its Vulkan
 * or EGL objects leaves spans that the hook may fail to release. The service
 * releases the spans asynchronously after the sessions close, and the SVC
 * 0x80 clear reports 2001-0106 until it has; this retries the clear while it
 * does, at most 2 seconds in all.
 * Returns the number of spans restored; *failed (if not NULL) receives the
 * number the kernel refused. */
uint32_t nvkmd_nvgpu_native_backing_restore_abandoned(uint32_t *failed);

#endif
