/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_NVGPU_NATIVE_DEVICE_OWNER_H
#define NVKMD_NVGPU_NATIVE_DEVICE_OWNER_H

#include <vulkan/vulkan_core.h>
#include <ouncevk/profile.h>
#include "nvkmd_nvgpu_native_arena.h"
#include "nvkmd_nvgpu_native_budget.h"
#include "nvkmd_nvgpu_gpu_endpoint.h"
#include "nvkmd_nvgpu_native_backing.h"
#include "nvkmd_nvgpu_native_channel_setup.h"
#include "nvkmd_nvgpu_native_map.h"
#include "nvkmd_nvgpu_native_terminal.h"

#include "util/list.h"
#include "util/vma.h"

enum nvkmd_nvgpu_native_device_stage {
   NVKMD_NVGPU_DEVICE_EMPTY,
   NVKMD_NVGPU_DEVICE_CPU_RESERVED,
   NVKMD_NVGPU_DEVICE_BACKING,
   NVKMD_NVGPU_DEVICE_TSG,
   NVKMD_NVGPU_DEVICE_VA,
   NVKMD_NVGPU_DEVICE_MAP,
   NVKMD_NVGPU_DEVICE_ARENA,
   NVKMD_NVGPU_DEVICE_NOTIFIER,
   NVKMD_NVGPU_DEVICE_CHANNEL,
   NVKMD_NVGPU_DEVICE_CONFIGURED,
};

struct nvkmd_nvgpu_native_submission;
struct nvkmd_nvgpu_native_progress;

/* Submit-path counters for the diagnostic snapshot. */
struct nvkmd_nvgpu_native_submit_stats {
   uint32_t execs, exec_spans, signals, syncs;
   uint32_t flushes, flush_entries, baseline_reads;
   uint32_t joins_blocking, polls_pending, polls_reached;
   /* Flushes from copy-only (upload) contexts. Context waits that carry
    * at least one sync, and the syncs that they wait for. */
   uint32_t copy_flushes, ctx_waits, ctx_wait_syncs;
   /* Waits resolved to a same-channel point instead of a CPU wait, and
    * batches that then started with a head barrier. */
   uint32_t deferred_waits, head_barriers;
   /* Present dependencies, counted under the progress mutex. The three
    * cases are: a submitted producer pair given to VI2, a fence that is
    * already complete (empty dependency), and a fence that is not yet
    * submitted (CPU wait). */
   uint32_t present_producers, present_complete, present_unsubmitted;
   /* Joins that waited for the blocking join of another thread. */
   uint32_t join_waits;
   /* Segments released when empty, and failed releases. */
   uint32_t segments_released, segment_release_failures;
   /* Submissions carrying the ordered dirty-L2 flush for pending releases,
    * and segments released when such a submission retired. */
   uint32_t release_flushes, segments_released_on_retire;
   /* Segment creations that failed, and the latest failure:
    * step << 24 | low 24 bits of its Result (enum
    * nvkmd_nvgpu_native_segment_failure). */
   uint32_t segment_create_failures, last_segment_failure;
   /* Submitted acquires of a foreign shim (VI2 release). */
   uint32_t foreign_waits;
   /* Raw 0xe4 kind changes on the kind alias, and the failed ones. */
   uint32_t kind_changes, kind_failures;
   uint32_t view_maps, view_map_failures;
   /* Shim acquires for points of the other channel. */
   uint32_t channel_waits;
   /* Chunked memory: chunks registered and released, failures, chunked memory. */
   uint32_t chunks_registered, chunks_released, chunk_failures;
   uint32_t chunked_mems;
   /* Of chunks_registered: those that the reserve registered ahead. */
   uint32_t chunks_prepared;
   /* Imported host pointers (nvkmd_nvgpu_native_host_mem.c), and failed
    * imports or releases. */
   uint32_t host_imports, host_import_failures;
   /* Exec calls whose spans total at most 256 bytes, and the total of all
    * exec bytes. */
   uint32_t small_execs;
   uint64_t exec_bytes;
   /* armGetSystemTick (31.25 MHz) inside flush and inside joins. */
   uint64_t flush_ticks, join_ticks;
};

enum nvkmd_nvgpu_native_segment_kind {
   NVKMD_NVGPU_SEGMENT_UNCACHED,
   NVKMD_NVGPU_SEGMENT_CACHED,
   NVKMD_NVGPU_SEGMENT_READBACK,
};

/* Segments after the primary one: OunceVK's own table. The heap (each
 * dedicated segment is at least 10 MiB) runs out before it fills. */
#define NVKMD_NVGPU_NATIVE_SEGMENT_MAX 1024u

/* Why a segment was not created (the step of last_segment_failure). */
enum nvkmd_nvgpu_native_segment_failure {
   NVKMD_NVGPU_SEGMENT_FAILURE_NONE,
   NVKMD_NVGPU_SEGMENT_FAILURE_TABLE,    /* no free entry */
   NVKMD_NVGPU_SEGMENT_FAILURE_HEAP,     /* aligned_alloc failed */
   NVKMD_NVGPU_SEGMENT_FAILURE_VA,       /* VA heap exhausted */
   NVKMD_NVGPU_SEGMENT_FAILURE_BACKING,  /* attributes or raw 0x75 */
   NVKMD_NVGPU_SEGMENT_FAILURE_MAP,      /* raw 0xe3 */
   NVKMD_NVGPU_SEGMENT_FAILURE_ARENA,    /* local arena bind or shape */
};

/* One registered and mapped block of the process heap (cpu_allocation). It
 * is mapped at a VA from the VA heap of the owner. For the alignments, see
 * NVKMD_NVGPU_NATIVE_SEGMENT_ALIGN_B. A pooled segment is
 * NVKMD_NVGPU_NATIVE_POOL_SEGMENT_B long, and small allocations share it. A
 * dedicated segment has the size of one large allocation.
 *
 * A cacheable segment can also have a kind alias. OunceVK makes the alias for
 * the first depth/stencil-kind image in the segment. The alias is a second
 * map of the backing at its own VA, initially kind 6. For an image with a
 * depth/stencil PTE kind (1-5), raw 0xe4 changes its pages in the alias to
 * that kind. The image then uses the alias address. The kind-6 map of the
 * segment itself never changes. No kind is restored. Only images with a
 * different kind use the alias, and NIL gives every image whole 4 KiB pages.
 */
struct nvkmd_nvgpu_native_segment {
   enum nvkmd_nvgpu_native_segment_kind kind;
   bool ready, alias_attempted, alias_ready;
   /* A creation or release step failed. The remote state is unknown, so the
    * entry, its memory and its VA are never reused. */
   bool failed;
   bool dedicated;
   /* The segment is empty and does not fit in the free-segment cache. It
    * takes no new allocation. It is released when a completion with the
    * ordered dirty-L2 flush covers every earlier use. release_flush is the
    * serial of that flushing submission. It is 0 until the service accepts
    * such a submission. */
   bool release_pending;
   uint64_t release_flush;
   /* When the segment last became empty, in the owner's emptied_serial
    * order: the cache releases the smallest first. */
   uint64_t emptied_serial;
   /* The CPU memory, from malloc. */
   void *cpu_allocation;
   uint64_t length_B, va_align_B;
   struct nvkmd_nvgpu_native_backing backing;
   struct nvkmd_nvgpu_native_map_owner map, alias_map;
   struct nvkmd_nvgpu_native_va_lease lease, alias_lease;
   struct nvkmd_nvgpu_native_arena arena;
};

struct nvkmd_nvgpu_native_owner_lock;

/* A second channel on the copy engine, with its own
 * GPU file and TSG. step is the last step reached and result its Result. */
enum nvkmd_nvgpu_native_copy_channel_step {
   NVKMD_NVGPU_COPY_CHANNEL_NONE,
   NVKMD_NVGPU_COPY_CHANNEL_CATALOG,   /* no second copy selector */
   NVKMD_NVGPU_COPY_CHANNEL_GPU_FILE,  /* nvOpen /dev/nvhost-gpu */
   NVKMD_NVGPU_COPY_CHANNEL_TSG_FILE,  /* nvOpen /dev/nvhost-tsg-gpu */
   NVKMD_NVGPU_COPY_CHANNEL_OPEN,      /* allocate, AS bind, TSG bind */
   NVKMD_NVGPU_COPY_CHANNEL_CONFIGURE, /* ring, sync, class C7B5, ... */
   NVKMD_NVGPU_COPY_CHANNEL_READY,
};

struct nvkmd_nvgpu_native_copy_channel {
   uint32_t selector;
   uint32_t gpu_fd, tsg_fd;
   bool gpu_open, tsg_open, attempted, ready;
   enum nvkmd_nvgpu_native_copy_channel_step step;
   uint32_t channel_status;
   Result result;
   struct nvkmd_nvgpu_native_channel channel;
   struct nvkmd_nvgpu_native_channel_setup setup;
   struct nvkmd_nvgpu_gpu_endpoint endpoint;
};

/* The native GPU device. It has CPU backing segments mapped into one VA
 * lease, a notifier page and one configured channel. It uses the services
 * of the runtime. Its children must keep these addresses unchanged. The
 * channel setup keeps a pointer to its record. */
struct nvkmd_nvgpu_native_device_owner {
   const struct nvkmd_nvgpu_native_device_owner *self;
   struct nvkmd_nvgpu_native_runtime *runtime;
   /* The primary segment: commands, completions and the first host-visible
    * memory. */
   struct nvkmd_nvgpu_native_backing backing;
   struct nvkmd_nvgpu_native_map_owner map;
   struct nvkmd_nvgpu_native_arena arena;
   struct nvkmd_nvgpu_native_va_lease map_lease;
   /* Segments after the primary one, created on demand under the owner
    * lock. An entry is free when it is neither ready nor failed. */
   struct nvkmd_nvgpu_native_segment segments[NVKMD_NVGPU_NATIVE_SEGMENT_MAX];
   uint32_t segment_count;
   /* The list of ready arenas for batch preparation (the primary first).
    * Used under the owner lock. */
   const struct nvkmd_nvgpu_native_arena *batch_sources[1 + NVKMD_NVGPU_NATIVE_SEGMENT_MAX];
   /* GPU VA for segment maps and kind aliases: the lease after the primary
    * segment. */
   struct util_vma_heap va_heap;
   bool va_heap_ready;
   /* Segments waiting for a flushing submission (release_pending without
    * release_flush), and the serial of the last flushing submission. */
   uint32_t releases_waiting;
   /* Counts the times a segment became empty (see the segment's
    * emptied_serial). */
   uint64_t emptied_serial;
   /* The context whose batch last ran with its own 3D and compute state on
    * the primary channel. Every VkDevice of the process shares that one
    * channel, and NVK sets the channel state once per queue. A batch of
    * another context first replays its state (native_ctx state_replay).
    * 0: no context yet. */
   uint64_t state_context_id;
   uint64_t flush_serial;
   /* The process heap, for the reported heap size and budget. */
   uint64_t heap_size_B;
   /* Chunked memory: the pool of free registered chunks and the live chunked
    * memory objects (nvkmd_nvgpu_native_chunked.c). Used under the owner
    * lock. */
   struct nvkmd_nvgpu_native_chunk *free_chunks;
   uint32_t free_chunk_count;
   /* The number of free chunks that the reserve keeps ready
    * (nvkmd_nvgpu_native_chunk_reserve.h). 0 until an allocation had to
    * register a chunk. */
   uint32_t chunk_reserve_target;
   struct list_head chunked_mems;
   bool chunked_ready;
   /* Chunks that are out of the pool: a view still maps the chunk, or a
    * mapping of it may remain, or its registration or release failed. The
    * first kind returns to the pool with its last view's unmap; device
    * close tries the others again. */
   struct nvkmd_nvgpu_native_chunk *stranded_chunks;
   /* The view lists of VA objects that were destroyed while a view may
    * still be mapped (nvkmd_nvgpu_native_views.c). Device close unmaps
    * those views. */
   struct nvkmd_nvgpu_native_view_list *retired_view_lists;
   /* The registered imports of host pointers
    * (nvkmd_nvgpu_native_host_mem.c). Used under the owner lock. */
   struct list_head host_mems;
   bool host_mems_ready;
   /* Profiling: latency histograms, and the totals at the last profile
    * reset. Used under the owner lock. */
   struct ouncevk_profile_timer_stats profile_timers[OUNCEVK_PROFILE_TIMER_COUNT];
   /* The same timers since the profile log's last line
    * (nvkmd_nvgpu_native_profile_log.c). */
   struct ouncevk_profile_timer_stats log_timers[OUNCEVK_PROFILE_TIMER_COUNT];
   uint64_t profile_base_execs, profile_base_exec_bytes, profile_base_flushes;
   uint64_t profile_base_signals, profile_base_syncs;
   /* GPU-only and HOST_CACHED memory come from segments of their own kind. */
   bool segments_enabled;
   /* Depth/stencil-kind images can be placed in cacheable segments (each
    * gets its alias map). */
   bool kind_alias_ready;
   /* The completion slot of every worker request (map, kind change, unmap,
    * range unregister). The owner lock serializes the requests. */
   struct nvkmd_nvgpu_native_slot slot;
   /* The channel's notifier page: a mode-1 backing with no GPU map. */
   struct nvkmd_nvgpu_native_backing notifier;
   struct nvkmd_nvgpu_native_channel channel;
   struct nvkmd_nvgpu_native_channel_setup setup;
   /* The ZCULL context-switch buffer, a slice of the primary segment bound
    * to the graphics TSG. It stays for the device lifetime: the firmware keeps
    * the binding in the TSG graphics context, and nothing unbinds it.
    * zcull_bound is false when the queries or the bind failed; ZCULL is then
    * off. */
   struct nvkmd_nvgpu_native_slice zcull_context;
   bool zcull_bound;
   Result zcull_result;
   struct nvkmd_nvgpu_native_copy_channel copy;
   struct nvkmd_nvgpu_native_va_owner va;
   struct nvkmd_nvgpu_native_va_lease lease;
   struct nvkmd_nvgpu_native_terminal terminal;
   struct nvkmd_nvgpu_native_arena *arena_ref; /* &arena */
   /* Recursive lock over the channel, the in-flight chain, completion joins
    * and the arenas. A second native thread can use the services. Thus
    * callers take this lock instead of requiring the setup thread. */
   struct nvkmd_nvgpu_native_owner_lock *lock; /* opaque, until device_finish */
   bool lock_ready;
   struct nvkmd_nvgpu_gpu_endpoint endpoint;
   const void *tls;
   uint64_t mapped_gpu_base, mapped_length;
   bool mapped_ready;
   uint64_t cpu_base, cpu_extent_B;
   enum nvkmd_nvgpu_native_device_stage stage;
   uint32_t last_result;
   uint64_t last_mem_request_B;
   int32_t last_mem_result;
   uint64_t last_va_request_B;
   int32_t last_va_result;
   /* Accepted, not yet retired batches across the software contexts on
    * this physical channel. */
   uint32_t inflight_submissions, peak_inflight_submissions;
   uint32_t inflight_entries;
   /* A known lower bound of the mode-0 producer counter of the channel, for
    * one endpoint generation. It comes from an earlier baseline read or
    * from a target that was confirmed as reached. The counter only
    * increases. Thus the bound is a valid baseline for the next submit
    * while it stays within the 2^31 mode-0 window. */
   /* Local wrapper counts. They do not show that remote objects are
    * retired. */
   uint32_t active_pdevs, active_devices, active_vas;
   /* Contexts destroyed while their GPU state was unknown. They keep their
    * storage and the device until the process exits. */
   uint32_t kept_contexts;
   struct nvkmd_nvgpu_native_submission *inflight_first;
   struct nvkmd_nvgpu_native_submission *inflight_last;
   /* Local objects can be reused only after confirmed GPU completion and
    * arena release. Submissions with an unknown state stay on the in-flight
    * chain. */
   struct nvkmd_nvgpu_native_submission *recycled_submissions;
   struct nvkmd_nvgpu_native_submission *quarantined_submissions;
   /* The process-root reference. Points and contexts keep their own domain
    * references. */
   struct nvkmd_nvgpu_native_progress *progress;
   /* Both software contexts share one channel. An uncertain submit or
    * completion poisons that channel for every context on this owner. */
   bool attempted, ready, submission_failed, retiring;
   /* The completion event: an event slot of the process's GSV sync session
    * (gsvSyncAllocateSlot), armed for each target that a thread waits for.
    * The slot is freed when the owner closes. */
   bool gsv_event_tested;
   bool gsv_event_slot_allocated;
   uint32_t gsv_event_slot;
   bool gsv_event_open;
   Event gsv_event;
   bool gsv_event_disabled;
   /* A timeout keeps this registration live for a later wait on the same
    * accepted target. No other point may replace it while it is armed. */
   bool gsv_event_armed;
   /* A thread is blocked in the GSV event wait for inflight_first, with the
    * owner lock released. Joins are serialized. Other joiners wait for it
    * (nvkmd_nvgpu_native_owner_wait_join). Polls report not ready. */
   bool joining;
   uint32_t gsv_event_producer, gsv_event_target;
   uint32_t gsv_event_arms, gsv_event_wakes, gsv_event_timeouts;
   uint32_t gsv_event_failures;
   uint64_t gsv_event_wait_ticks;
   /* Diagnostic Results: create, arm, zero-timeout probe, timed wait,
    * timed-wait ticks, disarm. */
   uint32_t gsv_event_status[11];
   struct nvkmd_nvgpu_native_submit_stats stats;
};

struct nvkmd_nvgpu_native_device_io {
   void *context;
   /* Borrows the CPU span for GPU memory. The span is page aligned and at
    * least NVKMD_NVGPU_NATIVE_CPU_EXTENT_B long. It is ordinary heap memory,
    * disjoint from all other memory, and kept for the process lifetime. */
   uint32_t (*borrow_cpu)(void *context, uint64_t *base,
                          uint64_t *extent_B);
   void (*observe)(void *context,
                   enum nvkmd_nvgpu_native_device_stage stage,
                   uint32_t result);
};

/* Creates the device on an open runtime: CPU backing, TSG, VA, map, arena,
 * notifier page, channel and its configuration. After a failure, the
 * acquired resources stay for nvkmd_nvgpu_native_device_close. */
bool nvkmd_nvgpu_native_device_prepare(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_runtime *runtime,
   const struct nvkmd_nvgpu_native_device_io *io);

enum nvkmd_nvgpu_native_device_close_status {
   NVKMD_NVGPU_DEVICE_CLOSE_OK,
   NVKMD_NVGPU_DEVICE_CLOSE_ARGUMENT,
   NVKMD_NVGPU_DEVICE_CLOSE_TERMINAL,
   NVKMD_NVGPU_DEVICE_CLOSE_CHANNEL,
   NVKMD_NVGPU_DEVICE_CLOSE_SEGMENT,
   NVKMD_NVGPU_DEVICE_CLOSE_MAP,
   NVKMD_NVGPU_DEVICE_CLOSE_BACKING,
   NVKMD_NVGPU_DEVICE_CLOSE_NOTIFIER,
   /* A request on the completion slot is unresolved. */
   NVKMD_NVGPU_DEVICE_CLOSE_SLOT,
};

/* Retires the device after every Vulkan object that uses it is gone. The
 * steps are: a terminal marker on the channel, the notifier and error event
 * of the channel, the GSV completion event, every map and backing, the
 * notifier page and the request slot. The files and sessions of the runtime
 * stay open. Stops at the first step that fails and returns it. */
enum nvkmd_nvgpu_native_device_close_status nvkmd_nvgpu_native_device_close(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* As nvkmd_nvgpu_native_device_close, at program exit. The terminal marker
 * also covers submissions still in flight and memory that the program
 * never freed (nvkmd_nvgpu_native_terminal_drain_at_exit). Thus the
 * channel, maps and registrations are still retired. */
enum nvkmd_nvgpu_native_device_close_status
nvkmd_nvgpu_native_device_close_at_exit(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* After a successful close, frees the host memory of the owner (arenas,
 * submission records, progress domain, lock) and zeroes the owner.
 * nvkmd_nvgpu_native_device_prepare can then create a device again. It
 * refuses and changes nothing while a physical or logical device, VA,
 * context or timeline still refers to the owner. */
bool nvkmd_nvgpu_native_device_finish(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* For program exit, after a successful close that finish refused because
 * leaked Vulkan objects still refer to the owner. Marks the owner unusable
 * and detaches its progress domain. Frees nothing. The caller then closes
 * the runtime. The abandoned objects are never used again. */
void nvkmd_nvgpu_native_device_abandon(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* Logs each condition of nvkmd_nvgpu_native_device_common_valid and the
 * retiring flag, when an operation refuses the owner. Diagnostics only. */
void nvkmd_nvgpu_native_owner_log_refusal(
   const struct nvkmd_nvgpu_native_device_owner *owner, const char *where);

bool nvkmd_nvgpu_native_device_common_valid(
   const struct nvkmd_nvgpu_native_device_owner *owner);

/* Takes the owner lock. Returns false, and locks nothing, when the owner is
 * not ready. */
bool nvkmd_nvgpu_native_owner_lock(struct nvkmd_nvgpu_native_device_owner *owner);
void nvkmd_nvgpu_native_owner_unlock(struct nvkmd_nvgpu_native_device_owner *owner);

/* For a blocking kernel wait while the lock is held. suspend releases the
 * lock completely, at any recursion depth. resume takes it again at the
 * same depth. */
uint32_t nvkmd_nvgpu_native_owner_suspend(
   struct nvkmd_nvgpu_native_device_owner *owner);
void nvkmd_nvgpu_native_owner_resume(
   struct nvkmd_nvgpu_native_device_owner *owner, uint32_t depth);

/* wait_join: with the lock held, waits until the current blocking join
 * finishes (owner->joining clears). It releases the lock during the wait.
 * join_done: the joiner calls it to wake those waiters. */
void nvkmd_nvgpu_native_owner_wait_join(
   struct nvkmd_nvgpu_native_device_owner *owner);
void nvkmd_nvgpu_native_owner_join_done(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* Gives the image range at gpu_addr the uncompressed PTE kind (1-5) in the
 * kind alias of its segment. Returns the address of the range in the alias.
 * The range must be in the arena of a cacheable segment, 4 KiB aligned,
 * with a size that is a 4 KiB multiple. Takes the owner lock. Returns false
 * when there is no kind alias, the range is elsewhere, or the change
 * failed. */
bool nvkmd_nvgpu_native_owner_kind_alias(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_arena *arena,
   uint64_t gpu_addr, uint64_t size_B, uint8_t kind, uint64_t *alias_addr);

/* True when an allocation of size_B gets a dedicated segment. */
static inline bool
nvkmd_nvgpu_native_segment_dedicated(uint64_t size_B)
{
   return size_B > NVKMD_NVGPU_NATIVE_DEDICATED_MIN_B;
}

/* Creates a segment of this kind with room for size_B at align_B. Returns
 * its arena. The segment is pooled, or dedicated with size size_B (see
 * nvkmd_nvgpu_native_segment_dedicated). Returns NULL when the heap, the VA
 * heap or the segment table is exhausted, or when a service step failed.
 * Empty segments are released in two cases: when those of one kind exceed
 * NVKMD_NVGPU_NATIVE_SEGMENT_CACHE_B (see
 * nvkmd_nvgpu_native_owner_segment_emptied), or when creation fails for
 * lack of room. That release runs on the thread of the owner, with dev. The caller
 * holds the owner lock (game role only). */
struct nvkmd_dev;
struct vk_object_base;
struct nvkmd_nvgpu_native_arena *nvkmd_nvgpu_native_owner_grow(
   struct nvkmd_nvgpu_native_device_owner *owner,
   enum nvkmd_nvgpu_native_segment_kind kind,
   uint64_t size_B, uint64_t align_B,
   struct nvkmd_dev *dev, struct vk_object_base *log_obj);

/* Call after an allocation in arena is freed or unpinned. The segment may
 * now be empty, and the empty segments of its kind may then pass
 * NVKMD_NVGPU_NATIVE_SEGMENT_CACHE_B. In that case, this marks the ones
 * that have been empty longest for release, until the rest fit; they take
 * no more allocations. This segment, the most recently emptied, is marked
 * last: only when it is larger than the limit by itself. Any thread can
 * call it. It takes the owner lock. The release waits for the next
 * submission, which then carries the ordered dirty-L2 flush. The release
 * occurs when that submission retires.
 */
void nvkmd_nvgpu_native_owner_segment_emptied(
   struct nvkmd_nvgpu_native_device_owner *owner,
   struct nvkmd_nvgpu_native_arena *arena, struct nvkmd_dev *dev);

/* Call under the owner lock, after the service accepts a submission whose
 * batch has flush_dirty. Assigns that submission to the segments that wait
 * for a flush. Returns its flush serial, for
 * nvkmd_nvgpu_native_owner_release_flushed. */
uint64_t nvkmd_nvgpu_native_owner_claim_release_flush(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* Call under the owner lock, after the submission with this flush serial
 * retires (confirmed completion). Releases every segment that it covers. A
 * failed release marks the segment failed and is counted. */
void nvkmd_nvgpu_native_owner_release_flushed(
   struct nvkmd_nvgpu_native_device_owner *owner, uint64_t flush_serial);

/* Gets the size of the process heap and the bytes of it in no allocation
 * (libounce heap.h). The memory heap and its budget use these values. */
void nvkmd_nvgpu_native_heap_query(uint64_t *size_B, uint64_t *available_B);

/* Creates the owner's lock (once, during owner setup). */
bool nvkmd_nvgpu_native_owner_lock_init(
   struct nvkmd_nvgpu_native_device_owner *owner);
/* Destroys it (nvkmd_nvgpu_native_device_finish). No thread can hold it. */
void nvkmd_nvgpu_native_owner_lock_finish(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* GPU VA of the owner's VA heap for views (a VkImage plane with its own
 * VA). Returns 0 when no VA is free. Call with the owner lock held. */
uint64_t nvkmd_nvgpu_native_owner_reserve_va(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t size_B, uint64_t align_B);

void nvkmd_nvgpu_native_owner_release_va(
   struct nvkmd_nvgpu_native_device_owner *owner,
   uint64_t addr, uint64_t size_B);

/* Maps the bytes of the arena at [gpu_addr, +size_B) again at view_addr,
 * with the kind (1-6). The view uses the cacheability of the
 * arena. *backing_out is the backing to give to
 * nvkmd_nvgpu_native_owner_unmap_view. The segment of the arena is not
 * released while the view exists. */
struct nvkmd_nvgpu_native_view_source {
   struct nvkmd_nvgpu_native_backing *backing;
   uint64_t backing_offset_B;
   bool gpu_cacheable;
};

/* On a failure, *left_mapped_out says whether the mapping exists or may
 * exist all the same (the worker accepted the request, and its wait failed
 * or the mapping got another kind and could not be removed). The caller
 * then keeps the view, with *source_out, and unmaps it later. */
VkResult nvkmd_nvgpu_native_owner_map_view(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_arena *arena,
   uint64_t gpu_addr, uint64_t size_B,
   uint64_t view_addr, uint8_t kind,
   struct nvkmd_nvgpu_native_view_source *source_out,
   bool *left_mapped_out);

/* Maps backing bytes again at view_addr (a part of an earlier view, after
 * a partial unbind). *left_mapped_out is as for
 * nvkmd_nvgpu_native_owner_map_view. */
VkResult nvkmd_nvgpu_native_owner_map_backing(
   struct nvkmd_nvgpu_native_device_owner *owner,
   const struct nvkmd_nvgpu_native_view_source *source,
   uint64_t size_B, uint64_t view_addr, uint8_t kind, bool *left_mapped_out);

/* Opens the copy channel once. Returns true when it is ready.
 * The steps and results stay in owner->copy for diagnostics. A failure
 * does not change the primary channel. */
bool nvkmd_nvgpu_native_owner_open_copy_channel(
   struct nvkmd_nvgpu_native_device_owner *owner);

struct nvkmd_nvgpu_native_chunk;

/* Completes all earlier work on the primary channel and writes back dirty
 * GPU L2 lines (the QueueFinish prefix). Call with the owner lock held. */
VkResult nvkmd_nvgpu_native_owner_finish_flush(
   struct nvkmd_nvgpu_native_device_owner *owner, struct nvkmd_dev *dev);

#endif
