/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_DISCOVERY_H
#define NVKMD_NVGPU_DISCOVERY_H

#include "nvk_debug.h"
#include "nvkmd_nvgpu_native_runtime.h"
#include <vulkan/vulkan_core.h>

struct vk_object_base;
struct nvkmd_pdev;
struct nvkmd_nvgpu_native_device_io;
struct nvkmd_nvgpu_native_device_owner;

/* The process has one native GPU: one runtime (libounce-nx services) and one
 * device owner. The first Vulkan discovery creates them. They stay alive
 * until nvkmd_nvgpu_native_shutdown. After a complete shutdown, the platform
 * is unregistered, and a new registration opens the GPU again. Swapchains
 * present to the VI2 layer of libounce-nx's default NWindow
 * (nwindowGetVi2Layer). */

/* Registers the CPU span provider and observer for the device owner. Call it
 * before Vulkan discovery. The context must stay alive until shutdown. Only
 * one registration can exist at a time. Register on the thread that later
 * enumerates devices. A complete nvkmd_nvgpu_native_shutdown ends the
 * registration. */
bool nvkmd_nvgpu_native_register_platform(
   const struct nvkmd_nvgpu_native_device_io *io);

/* Registers one heap span from aligned_alloc as the CPU span. Use it in a
 * program that registers no platform itself, such as a program that uses
 * only EGL. Returns true when a platform is registered, by this call or
 * earlier by the program. Call it on the thread that later enumerates
 * devices. */
bool nvkmd_nvgpu_native_acquire_default_platform(void);

/* If nvkmd_nvgpu_native_acquire_default_platform registered the platform,
 * this calls nvkmd_nvgpu_native_shutdown and frees the span. Otherwise the
 * program owns the platform, and this does nothing. Returns the shutdown
 * value. A value of 0 means that everything is retired. After that, a later
 * acquire opens the GPU again. */
uint32_t nvkmd_nvgpu_native_release_default_platform(void);

/* Diagnostic snapshot of the device owner's arenas and submit counters. It
 * does not allocate memory or send a service request. */
struct nvkmd_nvgpu_native_budget_snapshot {
   /* Totals for the uncached segments (primary first), the GPU-cacheable
    * segments and the readback segments. reused_B is for the primary
    * segment only. */
   uint64_t length_B, cursor_B, reused_B;
   uint64_t cached_length_B, cached_cursor_B;
   uint64_t live_B, cached_live_B;            /* unreleased records */
   uint64_t readback_length_B;
   uint32_t segments;                         /* after the primary one */
   uint32_t foreign_waits;                    /* VI2 release shim acquires */
   uint32_t segments_released, segment_release_failures;
   /* Ready segments, failed entries, failed creations and the latest
    * failure, from the stats of nvkmd_nvgpu_native_device_owner. */
   uint32_t ready_segments, failed_segments;
   uint32_t segment_create_failures, last_segment_failure;
   /* Segments released when a submission with the ordered dirty-L2 flush
    * retired, and the number of those submissions. */
   uint32_t segments_released_on_retire, release_flushes;
   uint32_t active_devices, active_vas;
   uint64_t last_mem_request_B, last_va_request_B;
   int32_t last_mem_result, last_va_result;
   uint32_t inflight_submissions, peak_inflight_submissions;
   bool gsv_event_tested;
   uint32_t gsv_event_status[11];
   uint32_t gsv_event_arms, gsv_event_wakes, gsv_event_timeouts;
   uint32_t gsv_event_failures;
   uint64_t gsv_event_wait_ticks;
   uint32_t execs, exec_spans, signals, syncs;
   uint32_t flushes, flush_entries, baseline_reads;
   uint32_t joins_blocking, polls_pending, polls_reached;
   uint32_t copy_flushes, ctx_waits, ctx_wait_syncs, deferred_waits;
   uint32_t head_barriers;
   uint32_t present_producers, present_complete, present_unsubmitted;
   uint32_t join_waits;
   uint32_t kind_changes, kind_failures;
   /* Compressed memory objects made, and compressed requests that the
    * service mapped with the ordinary fallback kind. */
   uint32_t compressed_mems, compression_fallbacks;
   bool kind_alias_ready;
   uint32_t small_execs;
   uint64_t exec_bytes;
   uint64_t flush_ticks, join_ticks;
};
bool nvkmd_nvgpu_native_budget_snapshot(
   struct nvkmd_nvgpu_native_budget_snapshot *out);

/* Copies the GPU engine catalog (ioctl 0xc010471f) that the runtime read:
 * up to max_records records of {type, instance, selector, 0}. Returns the
 * record count, or -1 when no device is ready. For diagnostics (B6). */
/* Opens the copy channel once, then gives its last step, the
 * step's status, its Result and the engine selector. Returns false when
 * the native device is not open. */
bool nvkmd_nvgpu_native_copy_channel_probe(uint32_t out[4]);

int nvkmd_nvgpu_native_engine_catalog(uint32_t (*records)[4],
                                      uint32_t max_records);

/* Reads the producer counter of the device's channel one time. Any thread
 * can call it. Returns false when no device is ready. */
bool nvkmd_nvgpu_native_diag_counter_read(Result *result, uint64_t *counter);

/* Gives the WSI the ready runtime and device. Returns false when no device
 * is ready or the device is retiring. */
struct nvkmd_nvgpu_native_scanout_view {
   struct nvkmd_nvgpu_native_runtime *runtime;
   struct nvkmd_nvgpu_native_device_owner *device;
};
bool nvkmd_nvgpu_native_scanout_view(
   struct nvkmd_nvgpu_native_scanout_view *out);

/* Gives the syncpoint shim of the native AS. Returns false when the shim
 * was not selected. */
bool nvkmd_nvgpu_native_syncpoint_shim(uint64_t *base, uint32_t *stride,
                                       uint32_t *count);

/* Retires everything in reverse order when no Vulkan object remains: first
 * the device owner, then the runtime's services. It stops at the first step
 * that fails and leaves the remaining resources open. Return values:
 * - 0: everything is retired.
 * - 0x100 | the device owner's nvkmd_nvgpu_native_device_close_status: the
 *   close of the device owner failed.
 * - 0x200: a Vulkan object still uses the device owner.
 * - 0x400: the release of the VI2 layer failed. After this, the GPU cannot
 *   open again.
 * When the GPU is retired, the platform registration ends. The default
 * NWindow's VI2 layer is then released (nwindowReleaseVi2Layer), so the next
 * registration opens the GPU on a new GSV root. The VI2 layer belongs to
 * libounce-nx's NWindow, which closes when the application exits. */
uint32_t nvkmd_nvgpu_native_shutdown(void);

/* Returns the result of libounce-nx's exit hook (__nx_gpu_exit). The hook
 * retires the GPU after the program's own cleanup, also when the program
 * leaked Vulkan or EGL objects. The values are the same as for
 * nvkmd_nvgpu_native_shutdown. They include 0x200 (device in use) when the
 * hook abandoned leaked objects. */
uint32_t nvkmd_nvgpu_native_exit_status(void);

/* Horizon discovery entry point. Opens the native GPU and creates its
 * physical device. */
VkResult nvkmd_nvgpu_try_create_pdev(struct vk_object_base *log_obj,
                                    enum nvk_debug debug_flags,
                                    struct nvkmd_pdev **pdev_out);

#endif
