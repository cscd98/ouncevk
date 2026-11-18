/* SPDX-License-Identifier: MIT */
/* Original OunceVK discovery of the native Switch 2 GPU on libounce-nx: the
 * process's runtime, device owner and display layer, and their retirement.
 */
#include "nvkmd_nvgpu_discovery.h"
#include <ouncevk/switch2_menu_role.h>

#include "vk_log.h"

#include <stdlib.h>
#include <string.h>

#include "nvkmd_nvgpu_native_compute_info.h"
#include "nvkmd_nvgpu_native_completion.h"
#include "nvkmd_nvgpu_native_backing.h"
#include "nvkmd_nvgpu_native_device_owner.h"
#include "nvkmd_nvgpu_native_pdev.h"
#include "nvkmd_nvgpu_native_profile_log.h"
#include "nvkmd_nvgpu_native_chunk_reserve.h"

#include "util/log.h"
#include "util/simple_mtx.h"

#include <switch.h>

/* Return values of nvkmd_nvgpu_native_shutdown. See nvkmd_nvgpu_discovery.h. */
#define NATIVE_SHUTDOWN_DEVICE 0x100u
#define NATIVE_SHUTDOWN_DEVICE_IN_USE 0x200u
#define NATIVE_SHUTDOWN_LAYER 0x400u

static struct nvkmd_nvgpu_native_runtime native_runtime;
/* The generation of the last runtime that opened. Each open uses a new
 * generation. Thus a handle of a shut-down runtime does not validate against
 * the next runtime. */
static uint64_t native_runtime_generation;
static struct nvkmd_nvgpu_native_device_owner native_device;
static bool native_runtime_attempted;
/* native_device is closed (its GPU state is retired) but not finished. The
 * finish step fails while Vulkan objects still refer to the device. */
static bool native_device_closed;

static struct {
   struct nvkmd_nvgpu_native_device_io io;
   const void *tls;
   bool registered;
} native_platform;


bool
ouncevk_switch2_menu_resource_user_id(uint64_t *out)
{
   if (!out)
      return false;
   *out = 0;
   return true;
}

bool
nvkmd_nvgpu_native_register_platform(const struct nvkmd_nvgpu_native_device_io *io)
{
   if (!io || !io->borrow_cpu || native_platform.registered ||
       native_runtime_attempted)
      return false;
   native_platform.io = *io;
   native_platform.tls = armGetTls();
   native_platform.registered = true;
   return true;
}

/* The CPU span that nvkmd_nvgpu_native_acquire_default_platform allocates. */
static void *default_platform_span;

static uint32_t
default_platform_borrow_cpu(void *context, uint64_t *base, uint64_t *extent_B)
{
   (void)context;
   if (!default_platform_span)
      return 1;
   *base = (uintptr_t)default_platform_span;
   *extent_B = NVKMD_NVGPU_NATIVE_CPU_EXTENT_B;
   return 0;
}

bool
nvkmd_nvgpu_native_acquire_default_platform(void)
{
   if (native_platform.registered)
      return true;
   if (native_runtime_attempted)
      return false;
   default_platform_span = aligned_alloc(NVKMD_NVGPU_NATIVE_PLACEMENT_ALIGN_B,
                                         NVKMD_NVGPU_NATIVE_CPU_EXTENT_B);
   if (!default_platform_span)
      return false;
   const struct nvkmd_nvgpu_native_device_io io = {
      .borrow_cpu = default_platform_borrow_cpu,
   };
   if (!nvkmd_nvgpu_native_register_platform(&io)) {
      free(default_platform_span);
      default_platform_span = NULL;
      return false;
   }
   return true;
}

uint32_t
nvkmd_nvgpu_native_release_default_platform(void)
{
   if (!default_platform_span)
      return 0;
   const uint32_t status = nvkmd_nvgpu_native_shutdown();
   /* If the GPU retirement failed, the GPU can still use the span. Do not
    * free it. */
   if (!status || status == NATIVE_SHUTDOWN_LAYER) {
      free(default_platform_span);
      default_platform_span = NULL;
   }
   return status;
}

static bool
native_device_usable(void)
{
   return nvkmd_nvgpu_native_device_common_valid(&native_device) &&
          !native_device.retiring;
}

bool
nvkmd_nvgpu_native_diag_counter_read(Result *result, uint64_t *counter)
{
   if (!result || !counter || !native_device_usable())
      return false;
   *result = nvkmd_nvgpu_native_completion_read_counter(
      &native_device.endpoint, counter);
   return true;
}

int
nvkmd_nvgpu_native_engine_catalog(uint32_t (*records)[4],
                                  uint32_t max_records)
{
   if (!records || !native_device_usable() || !native_device.runtime)
      return -1;
   const struct nvkmd_nvgpu_native_runtime *r = native_device.runtime;
   const uint32_t count = r->engine_catalog.record_count;
   for (uint32_t i = 0; i < count && i < max_records; i++)
      memcpy(records[i], r->engine_catalog.records[i], sizeof(records[i]));
   return (int)count;
}

bool
nvkmd_nvgpu_native_budget_snapshot(
   struct nvkmd_nvgpu_native_budget_snapshot *out)
{
   struct nvkmd_nvgpu_native_device_owner *d = &native_device;
   if (!out || !native_device_usable() ||
       !nvkmd_nvgpu_native_owner_lock(d))
      return false;
   *out = (struct nvkmd_nvgpu_native_budget_snapshot) {
      .length_B = d->arena.length,
      .cursor_B = d->arena.cursor,
      .reused_B = d->arena.reused_bytes,
      .live_B = nvkmd_nvgpu_native_arena_live_bytes(&d->arena),
      .segments = d->segment_count,
      .active_devices = d->active_devices,
      .active_vas = d->active_vas,
      .last_mem_request_B = d->last_mem_request_B,
      .last_va_request_B = d->last_va_request_B,
      .last_mem_result = d->last_mem_result,
      .last_va_result = d->last_va_result,
      .inflight_submissions = d->inflight_submissions,
      .peak_inflight_submissions = d->peak_inflight_submissions,
      .gsv_event_tested = d->gsv_event_tested,
      .gsv_event_arms = d->gsv_event_arms,
      .gsv_event_wakes = d->gsv_event_wakes,
      .gsv_event_timeouts = d->gsv_event_timeouts,
      .gsv_event_failures = d->gsv_event_failures,
      .gsv_event_wait_ticks = d->gsv_event_wait_ticks,
      .execs = d->stats.execs,
      .exec_spans = d->stats.exec_spans,
      .signals = d->stats.signals,
      .syncs = d->stats.syncs,
      .flushes = d->stats.flushes,
      .flush_entries = d->stats.flush_entries,
      .baseline_reads = d->stats.baseline_reads,
      .joins_blocking = d->stats.joins_blocking,
      .polls_pending = d->stats.polls_pending,
      .polls_reached = d->stats.polls_reached,
      .copy_flushes = d->stats.copy_flushes,
      .ctx_waits = d->stats.ctx_waits,
      .ctx_wait_syncs = d->stats.ctx_wait_syncs,
      .deferred_waits = d->stats.deferred_waits,
      .head_barriers = d->stats.head_barriers,
      .present_producers = d->stats.present_producers,
      .present_complete = d->stats.present_complete,
      .present_unsubmitted = d->stats.present_unsubmitted,
      .join_waits = d->stats.join_waits,
      .kind_changes = d->stats.kind_changes,
      .kind_failures = d->stats.kind_failures,
      .compressed_mems = d->stats.compressed_mems,
      .compression_fallbacks = d->stats.compression_fallbacks,
      .kind_alias_ready = d->kind_alias_ready,
      .foreign_waits = d->stats.foreign_waits,
      .segments_released = d->stats.segments_released,
      .segment_release_failures = d->stats.segment_release_failures,
      .segment_create_failures = d->stats.segment_create_failures,
      .last_segment_failure = d->stats.last_segment_failure,
      .segments_released_on_retire = d->stats.segments_released_on_retire,
      .release_flushes = d->stats.release_flushes,
      .small_execs = d->stats.small_execs,
      .exec_bytes = d->stats.exec_bytes,
      .flush_ticks = d->stats.flush_ticks,
      .join_ticks = d->stats.join_ticks,
   };
   for (unsigned i = 0; i < 11; i++)
      out->gsv_event_status[i] = d->gsv_event_status[i];
   /* Uncached segments add to the primary totals. cached_* is the sum of
    * the GPU-cacheable segments. readback_* is the sum of the readback
    * segments. */
   for (uint32_t i = 0; i < d->segment_count; i++) {
      const struct nvkmd_nvgpu_native_segment *segment = &d->segments[i];
      out->failed_segments += segment->failed;
      if (!segment->ready)
         continue;
      out->ready_segments++;
      const uint64_t live = nvkmd_nvgpu_native_arena_live_bytes(&segment->arena);
      switch (segment->kind) {
      case NVKMD_NVGPU_SEGMENT_UNCACHED:
         out->length_B += segment->arena.length;
         out->cursor_B += segment->arena.cursor;
         out->live_B += live;
         break;
      case NVKMD_NVGPU_SEGMENT_CACHED:
         out->cached_length_B += segment->arena.length;
         out->cached_cursor_B += segment->arena.cursor;
         out->cached_live_B += live;
         break;
      case NVKMD_NVGPU_SEGMENT_READBACK:
         out->readback_length_B += segment->arena.length;
         break;
      }
   }
   nvkmd_nvgpu_native_owner_unlock(d);
   return true;
}

bool
nvkmd_nvgpu_native_scanout_view(struct nvkmd_nvgpu_native_scanout_view *out)
{
   if (!out || !native_device_usable())
      return false;
   out->runtime = &native_runtime;
   out->device = &native_device;
   return true;
}

bool
nvkmd_nvgpu_native_syncpoint_shim(uint64_t *base, uint32_t *stride,
                                  uint32_t *count)
{
   const struct nvkmd_nvgpu_native_runtime *r = &native_runtime;
   if (!base || !stride || !count || !nvkmd_nvgpu_native_runtime_ready(r) ||
       !r->as_post.shim_selected || !r->as_post.shim_gpu_va ||
       !r->as_post.shim_stride || !r->as_post.shim_count)
      return false;
   *base = r->as_post.shim_gpu_va;
   *stride = r->as_post.shim_stride;
   *count = r->as_post.shim_count;
   return true;
}

/* Closes the device one time: it drains GPU work, closes the channel, and
 * releases the segments and the notifier. Then it frees the host state of
 * the device. At exit, the drain also includes work and memory that the
 * program leaked. */
static uint32_t
native_close_device(bool at_exit)
{
   if (!native_device.attempted)
      return 0;
   /* The log and the chunk reserve use the device: they stop first. */
   nvkmd_nvgpu_native_profile_log_stop();
   nvkmd_nvgpu_native_chunk_reserve_stop();
   if (!native_device_closed) {
      const enum nvkmd_nvgpu_native_device_close_status status = at_exit ?
         nvkmd_nvgpu_native_device_close_at_exit(&native_device) :
         nvkmd_nvgpu_native_device_close(&native_device);
      if (status != NVKMD_NVGPU_DEVICE_CLOSE_OK)
         return NATIVE_SHUTDOWN_DEVICE | status;
      native_device_closed = true;
   }
   if (!nvkmd_nvgpu_native_device_finish(&native_device))
      return NATIVE_SHUTDOWN_DEVICE_IN_USE;
   native_device_closed = false;
   return 0;
}

/* Closes the runtime's sessions and the default window's VI2 layer. After
 * this, a new registration can open the GPU again. */
static uint32_t
native_close_runtime(void)
{
   nvkmd_nvgpu_native_runtime_close(&native_runtime);
   /* Everything is retired. A new registration can open the GPU again. */
   native_runtime_attempted = false;
   memset(&native_platform, 0, sizeof(native_platform));
   /* The VI2 layer of the default NWindow holds the GSV root. The GSV root
    * delegates nvservices only one time, so the next open needs a new root.
    * The next swapchain opens the layer again. */
   if (R_FAILED(nwindowReleaseVi2Layer(nwindowGetDefault())))
      return NATIVE_SHUTDOWN_LAYER;
   return 0;
}

uint32_t
nvkmd_nvgpu_native_shutdown(void)
{
   const uint32_t status = native_close_device(false);
   if (status)
      return status;
   return native_close_runtime();
}

/* libounce-nx's exit hook (__appExit). It runs after the program's own
 * cleanup. A program can leave Vulkan or EGL objects alive (no
 * vkDestroyDevice or eglTerminate). Without this hook, the runtime then
 * keeps its sm, GSV and nvservices sessions. In an NRO, these sessions
 * outlive the NRO. The loader and the next NRO then cannot open those
 * services, because the sm port has no spare session while an NRO runs.
 * No program code runs after this hook, so the hook abandons the leaked
 * objects. It still closes the device in the traced order (drain, channel,
 * maps, registrations). It closes the sessions even when the device close
 * failed, because a leaked session always breaks the loader. Last, it returns
 * the CPU spans that the close did not release to plain memory: their
 * attributes belong to the process, and in an NRO the next program gets the
 * heap. */
static uint32_t native_exit_status;

void __nx_gpu_exit(void);
void
__nx_gpu_exit(void)
{
   if (!native_runtime_attempted && !native_device.attempted)
      return;
   const uint32_t device = native_close_device(true);
   if (device)
      mesa_logw("Native GPU at exit: device retirement status 0x%x; "
                "closing its sessions", device);
   if (device == NATIVE_SHUTDOWN_DEVICE_IN_USE)
      nvkmd_nvgpu_native_device_abandon(&native_device);
   native_exit_status = device | native_close_runtime();
   uint32_t refused = 0;
   const uint32_t restored = nvkmd_nvgpu_native_backing_restore_abandoned(&refused);
   if (restored || refused)
      mesa_logw("Native GPU at exit: restored %u abandoned spans, %u refused",
                restored, refused);
   /* The GPU can use the default span until the device is closed. */
   if (!native_exit_status && default_platform_span) {
      free(default_platform_span);
      default_platform_span = NULL;
   }
}

uint32_t
nvkmd_nvgpu_native_exit_status(void)
{
   return native_exit_status;
}

/* Serializes native_open. Instances can be created on any thread. */
static simple_mtx_t native_open_mutex = SIMPLE_MTX_INITIALIZER;

/* Opens the runtime and the device owner on the registration thread, one
 * time for each registration. After a failure, both stay open until
 * nvkmd_nvgpu_native_shutdown closes them. If the program registered no
 * platform, this registers the default platform on the calling thread.
 * After the first open, any thread can use the device: each IPC call uses
 * the message buffer of the calling thread, and the owner lock serializes
 * the shared state. */
static VkResult
native_open_locked(struct vk_object_base *log_obj)
{
   if (!native_platform.registered &&
       !nvkmd_nvgpu_native_acquire_default_platform())
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "Native GPU platform is not registered");
   if (!native_runtime_attempted && native_platform.tls != armGetTls())
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "Native GPU must open on its registration thread");
   if (!native_runtime_attempted) {
      native_runtime_attempted = true;
      if (nvkmd_nvgpu_native_runtime_open(&native_runtime,
                                          ++native_runtime_generation) !=
          NVKMD_NVGPU_RUNTIME_OK)
         return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                          "Native GPU runtime failed: stage=%u error=%u "
                          "result=0x%x",
                          (unsigned)native_runtime.stage,
                          (unsigned)native_runtime.error,
                          native_runtime.result);
      if (!nvkmd_nvgpu_native_device_prepare(&native_device, &native_runtime,
                                             &native_platform.io))
         return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                          "Native GPU device failed: stage=%u result=0x%x",
                          (unsigned)native_device.stage,
                          native_device.last_result);
   }
   if (!native_device_usable())
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "Native GPU device unavailable: stage=%u result=0x%x",
                       (unsigned)native_device.stage,
                       native_device.last_result);
   nvkmd_nvgpu_native_profile_log_start(&native_device);
   nvkmd_nvgpu_native_chunk_reserve_start(&native_device);
   return VK_SUCCESS;
}

static VkResult
native_open(struct vk_object_base *log_obj)
{
   simple_mtx_lock(&native_open_mutex);
   const VkResult result = native_open_locked(log_obj);
   simple_mtx_unlock(&native_open_mutex);
   return result;
}

/* The most ZCULL subregions NIL handles (nil/zcull.rs MAX_SUBREGIONS). */
#define NATIVE_ZCULL_MAX_SUBREGIONS 16

/* Publishes ZCULL in the device information when the context buffer is
 * bound. The geometry words come in wire order: the last three are the
 * subregion width and height alignments and the subregion count, while
 * Mesa's structure has the count first. Values NIL cannot use leave ZCULL
 * off.
 */
static void
native_publish_zcull(struct nv_device_info *info)
{
   const struct nvkmd_nvgpu_native_zcull_query *zcull =
      &native_runtime.delegate.zcull;
   if (!native_device.zcull_bound) {
      mesa_logd("native ZCULL off: queried %d bound %d query 0x%x bind 0x%x",
                zcull->queried, native_device.zcull_bound,
                zcull->result, native_device.zcull_result);
      return;
   }

   const uint32_t *w = zcull->info;
   const struct nv_zcull_device_info zcull_info = {
      .width_align_pixels = w[0],
      .height_align_pixels = w[1],
      .pixel_squares_by_aliquots = w[2],
      .aliquot_total = w[3],
      .zcull_region_byte_multiplier = w[4],
      .zcull_region_header_size = w[5],
      .zcull_subregion_header_size = w[6],
      .subregion_count = w[9],
      .subregion_width_align_pixels = w[7],
      .subregion_height_align_pixels = w[8],
      .ctxsw_size = zcull->context_size_B,
      .ctxsw_align = NVKMD_NVGPU_NATIVE_ZCULL_CONTEXT_ALIGN_B,
   };
   mesa_logi("native ZCULL: align %ux%u squares/aliquot %u aliquots %u "
             "multiplier %u headers %u/%u subregions %u (%ux%u) context %u",
             zcull_info.width_align_pixels, zcull_info.height_align_pixels,
             zcull_info.pixel_squares_by_aliquots, zcull_info.aliquot_total,
             zcull_info.zcull_region_byte_multiplier,
             zcull_info.zcull_region_header_size,
             zcull_info.zcull_subregion_header_size,
             zcull_info.subregion_count,
             zcull_info.subregion_width_align_pixels,
             zcull_info.subregion_height_align_pixels,
             zcull_info.ctxsw_size);
   if (!zcull_info.width_align_pixels || !zcull_info.height_align_pixels ||
       !zcull_info.pixel_squares_by_aliquots || !zcull_info.aliquot_total ||
       !zcull_info.zcull_region_byte_multiplier ||
       !zcull_info.subregion_count ||
       zcull_info.subregion_count > NATIVE_ZCULL_MAX_SUBREGIONS ||
       !zcull_info.subregion_width_align_pixels ||
       !zcull_info.subregion_height_align_pixels)
      return;
   info->zcull_info = zcull_info;
   info->has_zcull_info = true;
}

VkResult
nvkmd_nvgpu_try_create_pdev(struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   if (pdev_out == NULL)
      return vk_error(log_obj, VK_ERROR_INITIALIZATION_FAILED);
   *pdev_out = NULL;

   VkResult result = native_open(log_obj);
   if (result != VK_SUCCESS)
      return result;

   struct nv_device_info info;
   const enum nvkmd_nvgpu_native_compute_info_status info_status =
      nvkmd_nvgpu_native_compute_info(&native_runtime.delegate.characteristics,
                                      &native_runtime.delegate.resource_policy,
                                      true, &info);
   if (info_status != NVKMD_NVGPU_COMPUTE_INFO_CANDIDATE)
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Native compute profile rejected: %u",
                       (unsigned)info_status);
   /* A transfer queue family on the copy channel. */
   info.has_transfer_queue =
      nvkmd_nvgpu_native_owner_open_copy_channel(&native_device);
   native_publish_zcull(&info);
   return nvkmd_nvgpu_native_create_pdev(&native_device, &info, debug_flags,
                                         pdev_out);
}

/* Profiling counters. */

static unsigned
native_profile_bucket(uint64_t us)
{
   unsigned bucket = 0;
   while (bucket + 1 < OUNCEVK_PROFILE_BUCKETS && us >= (UINT64_C(2) << bucket))
      bucket++;
   return bucket;
}

void
ouncevk_profile_record(enum ouncevk_profile_timer timer, uint64_t ticks)
{
   struct nvkmd_nvgpu_native_device_owner *d = &native_device;
   if ((unsigned)timer >= OUNCEVK_PROFILE_TIMER_COUNT ||
       !native_device_usable() || !nvkmd_nvgpu_native_owner_lock(d))
      return;
   const uint64_t us = armTicksToNs(ticks) / 1000;
   struct ouncevk_profile_timer_stats *const sets[] = {
      &d->profile_timers[timer],
      &d->log_timers[timer],
   };
   for (unsigned i = 0; i < ARRAY_SIZE(sets); i++) {
      struct ouncevk_profile_timer_stats *stats = sets[i];
      stats->count++;
      stats->total_us += us;
      stats->max_us = MAX2(stats->max_us, us);
      stats->buckets[native_profile_bucket(us)]++;
   }
   nvkmd_nvgpu_native_owner_unlock(d);
}

void
ouncevk_profile_record_ns(enum ouncevk_profile_timer timer, uint64_t ns)
{
   ouncevk_profile_record(timer, armNsToTicks(ns));
}

bool
ouncevk_profile_read(struct ouncevk_profile *out, bool reset)
{
   struct nvkmd_nvgpu_native_device_owner *d = &native_device;
   if (!out || !native_device_usable() || !nvkmd_nvgpu_native_owner_lock(d))
      return false;
   *out = (struct ouncevk_profile) {
      .execs = d->stats.execs - d->profile_base_execs,
      .exec_bytes = d->stats.exec_bytes - d->profile_base_exec_bytes,
      .flushes = d->stats.flushes - d->profile_base_flushes,
      .signals = d->stats.signals - d->profile_base_signals,
      .syncs = d->stats.syncs - d->profile_base_syncs,
      .peak_inflight_submissions = d->peak_inflight_submissions,
   };
   memcpy(out->timers, d->profile_timers, sizeof(out->timers));
   if (reset) {
      d->profile_base_execs = d->stats.execs;
      d->profile_base_exec_bytes = d->stats.exec_bytes;
      d->profile_base_flushes = d->stats.flushes;
      d->profile_base_signals = d->stats.signals;
      d->profile_base_syncs = d->stats.syncs;
      memset(d->profile_timers, 0, sizeof(d->profile_timers));
   }
   nvkmd_nvgpu_native_owner_unlock(d);
   return true;
}

const char *
ouncevk_profile_timer_name(enum ouncevk_profile_timer timer)
{
   switch (timer) {
   case OUNCEVK_PROFILE_FLUSH: return "flush";
   case OUNCEVK_PROFILE_JOIN: return "join";
   case OUNCEVK_PROFILE_ACQUIRE: return "acquire";
   case OUNCEVK_PROFILE_PRESENT: return "present";
   case OUNCEVK_PROFILE_SUBMIT: return "submit";
   case OUNCEVK_PROFILE_ALLOCATE: return "allocate";
   case OUNCEVK_PROFILE_SERVICE_MAP: return "service-map";
   case OUNCEVK_PROFILE_SERVICE_UNMAP: return "service-unmap";
   case OUNCEVK_PROFILE_SERVICE_KIND: return "service-kind";
   case OUNCEVK_PROFILE_SERVICE_REGISTER: return "service-register";
   case OUNCEVK_PROFILE_SERVICE_UNREGISTER: return "service-unregister";
   case OUNCEVK_PROFILE_BACKING_HEAP: return "backing-heap";
   case OUNCEVK_PROFILE_BACKING_CLEAR: return "backing-clear";
   case OUNCEVK_PROFILE_BACKING_MARK: return "backing-mark";
   case OUNCEVK_PROFILE_BACKING_UNCACHE: return "backing-uncache";
   default: return "unknown";
   }
}

bool
nvkmd_nvgpu_native_copy_channel_probe(uint32_t out[4])
{
   struct nvkmd_nvgpu_native_device_owner *d = &native_device;
   if (!out || !native_device_usable())
      return false;
   (void)nvkmd_nvgpu_native_owner_open_copy_channel(d);
   out[0] = d->copy.step;
   out[1] = d->copy.channel_status;
   out[2] = d->copy.result;
   out[3] = d->copy.selector;
   return true;
}
