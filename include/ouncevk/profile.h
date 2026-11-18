/* SPDX-License-Identifier: MIT
 * OunceVK submit-path and presentation profile.
 *
 * The driver keeps totals and latency histograms in memory. Reading them
 * costs nothing on the measured path. A program reads them between
 * measured phases, for example after each benchmark scene, and can reset
 * them at the same time.
 *
 * Any program can also have the driver log them: when the file
 * sdmc:/config/ouncevk/profile-log.txt exists, the driver writes one line
 * per interval to sdmc:/config/ouncevk/profile.log, with per-frame
 * submissions, GPU kicks, waits, allocations and GPU-service requests. The
 * file holds the interval in milliseconds (default 1000). The log keeps its
 * own totals: ouncevk_profile_read with reset does not disturb it.
 */
#ifndef OUNCEVK_PROFILE_H
#define OUNCEVK_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ouncevk_profile_timer {
   /* Submission of one batch to the GPU channel (the service call). */
   OUNCEVK_PROFILE_FLUSH,
   /* A blocking wait for the completion of a submission. */
   OUNCEVK_PROFILE_JOIN,
   /* vkAcquireNextImageKHR on a native swapchain. */
   OUNCEVK_PROFILE_ACQUIRE,
   /* vkQueuePresentKHR on a native swapchain. */
   OUNCEVK_PROFILE_PRESENT,
   /* The driver's work for one vkQueueSubmit (including its flushes). */
   OUNCEVK_PROFILE_SUBMIT,
   /* vkAllocateMemory. */
   OUNCEVK_PROFILE_ALLOCATE,
   /* GPU-service memory requests, from the send to the completion: a map,
    * an unmap, a kind change, a range registration and its release. */
   OUNCEVK_PROFILE_SERVICE_MAP,
   OUNCEVK_PROFILE_SERVICE_UNMAP,
   OUNCEVK_PROFILE_SERVICE_KIND,
   OUNCEVK_PROFILE_SERVICE_REGISTER,
   OUNCEVK_PROFILE_SERVICE_UNREGISTER,
   /* The CPU steps of registering memory the driver owns: obtaining the
    * memory from the heap, clearing it, marking it for the GPU (SVC 0x80),
    * and making it CPU-uncached (SVC 3 and its queries). */
   OUNCEVK_PROFILE_BACKING_HEAP,
   OUNCEVK_PROFILE_BACKING_CLEAR,
   OUNCEVK_PROFILE_BACKING_MARK,
   OUNCEVK_PROFILE_BACKING_UNCACHE,
   OUNCEVK_PROFILE_TIMER_COUNT,
};

#define OUNCEVK_PROFILE_BUCKETS 20

/* Bucket 0 counts durations under 2 microseconds. Bucket i counts
 * durations from 2^i to 2^(i+1) microseconds. The last bucket also counts
 * all longer durations. */
struct ouncevk_profile_timer_stats {
   uint64_t count;
   uint64_t total_us;
   uint64_t max_us;
   uint32_t buckets[OUNCEVK_PROFILE_BUCKETS];
};

struct ouncevk_profile {
   /* Totals since the last reset. */
   uint64_t execs;
   uint64_t exec_bytes;
   uint64_t flushes;
   uint64_t signals;
   uint64_t syncs;
   /* Most submissions in flight at one time (since the process started). */
   uint32_t peak_inflight_submissions;
   struct ouncevk_profile_timer_stats timers[OUNCEVK_PROFILE_TIMER_COUNT];
};

/* Copies the profile to *out. With reset, the totals and histograms start
 * again from zero. Returns false when the native GPU device is not open. */
bool ouncevk_profile_read(struct ouncevk_profile *out, bool reset);

/* A short name for a timer, such as "flush". */
const char *ouncevk_profile_timer_name(enum ouncevk_profile_timer timer);

/* Adds one duration in system ticks. The driver and its WSI call this. */
void ouncevk_profile_record(enum ouncevk_profile_timer timer, uint64_t ticks);

/* The same, in nanoseconds, for code that does not read the system tick. */
void ouncevk_profile_record_ns(enum ouncevk_profile_timer timer, uint64_t ns);

#ifdef __cplusplus
}
#endif

#endif
