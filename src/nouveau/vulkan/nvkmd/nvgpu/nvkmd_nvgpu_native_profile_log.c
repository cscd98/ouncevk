/* SPDX-License-Identifier: MIT
 * The driver's profile log. Original OunceVK code.
 *
 * Each line covers one interval. The frames are the presents in it; the
 * other figures are per frame, from the device's counters
 * (nvkmd_nvgpu_native_submit_stats) and the profile timers:
 *
 *   submit   vkQueueSubmit calls, and the driver's time in them
 *   kick     GPU kicks (one service request each), entries per kick and the
 *            time in them
 *   wait     waits for a submission: resolved by reading memory, or
 *            blocking, and the time blocked
 *   acquire, present   swapchain calls
 *   alloc    vkAllocateMemory calls and their time
 *   service  GPU-service memory requests (map, unmap, kind change,
 *            register, unregister) and their time
 *   chunks   chunks registered (of them, by the reserve, ahead of an
 *            allocation) and released
 *   backing  the CPU steps of registering driver-owned memory: heap
 *            allocation, clearing, the SVC 0x80 mark and the change to
 *            CPU-uncached: total time in the interval and, in brackets,
 *            the longest single step
 *
 * "max" is the longest single call in the interval.
 */
#include "nvkmd_nvgpu_native_profile_log.h"
#include "nvkmd_nvgpu_native_device_owner.h"

#include "util/log.h"
#include "util/macros.h"

#include <c11/threads.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define PROFILE_LOG_CONFIG "sdmc:/config/ouncevk/profile-log.txt"
#define PROFILE_LOG_PATH "sdmc:/config/ouncevk/profile.log"
#define PROFILE_LOG_DEFAULT_MS 1000u
#define PROFILE_LOG_MIN_MS 100u
#define PROFILE_LOG_MAX_MS 60000u

/* One reading of the device's counters and the interval's timers. */
struct profile_sample {
   struct nvkmd_nvgpu_native_submit_stats stats;
   struct ouncevk_profile_timer_stats timers[OUNCEVK_PROFILE_TIMER_COUNT];
   uint64_t tick;
};

static struct {
   struct nvkmd_nvgpu_native_device_owner *owner;
   FILE *file;
   uint32_t interval_ms;
   thrd_t thread;
   mtx_t mutex;
   cnd_t stop_requested;
   bool running, stop;
   struct profile_sample previous;
   uint64_t start_tick;
} profile_log;

/* The interval from the configuration file, or 0 when there is no file. */
static uint32_t
profile_log_interval(void)
{
   FILE *config = fopen(PROFILE_LOG_CONFIG, "r");
   if (!config)
      return 0;
   unsigned interval = PROFILE_LOG_DEFAULT_MS;
   if (fscanf(config, "%u", &interval) != 1)
      interval = PROFILE_LOG_DEFAULT_MS;
   fclose(config);
   return CLAMP(interval, PROFILE_LOG_MIN_MS, PROFILE_LOG_MAX_MS);
}

/* Reads the counters, and takes the interval's timers. */
static void
profile_log_take_sample(struct profile_sample *sample)
{
   struct nvkmd_nvgpu_native_device_owner *owner = profile_log.owner;
   memset(sample, 0, sizeof(*sample));
   sample->tick = armGetSystemTick();
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return;
   sample->stats = owner->stats;
   memcpy(sample->timers, owner->log_timers, sizeof(sample->timers));
   memset(owner->log_timers, 0, sizeof(owner->log_timers));
   nvkmd_nvgpu_native_owner_unlock(owner);
}

static double
per_frame(uint64_t value, uint32_t frames)
{
   return frames ? (double)value / frames : (double)value;
}

static double
ms(uint64_t us)
{
   return (double)us / 1000.0;
}

/* Writes the line for the interval since the previous sample. */
static void
profile_log_write_line(const struct profile_sample *now)
{
   const struct profile_sample *before = &profile_log.previous;
   const struct nvkmd_nvgpu_native_submit_stats *a = &before->stats;
   const struct nvkmd_nvgpu_native_submit_stats *b = &now->stats;
   const struct ouncevk_profile_timer_stats *t = now->timers;
   const uint32_t frames = (uint32_t)t[OUNCEVK_PROFILE_PRESENT].count;
   const double seconds =
      (double)armTicksToNs(now->tick - profile_log.start_tick) / 1e9;
   const double interval_ms =
      (double)armTicksToNs(now->tick - before->tick) / 1e6;
   const uint32_t flushes = b->flushes - a->flushes;
   const uint32_t entries = b->flush_entries - a->flush_entries;
   const uint32_t polls = b->polls_reached - a->polls_reached;
   const uint32_t blocking = b->joins_blocking - a->joins_blocking;

#define TIMER(name) t[OUNCEVK_PROFILE_##name]
   uint64_t service_count = 0, service_us = 0, service_max_us = 0;
   for (unsigned i = OUNCEVK_PROFILE_SERVICE_MAP;
        i <= OUNCEVK_PROFILE_SERVICE_UNREGISTER; i++) {
      service_count += t[i].count;
      service_us += t[i].total_us;
      service_max_us = MAX2(service_max_us, t[i].max_us);
   }

   fprintf(profile_log.file,
           "%8.1f s %4u frames %6.2f ms/frame"
           " | submit %4.1f/f %5.2f ms/f max %5.2f"
           " | kick %4.1f/f %4.1f entries %5.2f ms/f max %5.2f"
           " | wait read %4.1f/f blocking %4.1f/f %5.2f ms/f max %5.2f"
           " | acquire %5.2f ms/f max %5.2f | present %5.2f ms/f max %5.2f"
           " | alloc %4.1f/f %5.2f ms/f max %5.2f"
           " | service %4.1f/f (map %llu unmap %llu kind %llu reg %llu"
           " unreg %llu) %5.2f ms/f max %5.2f"
           " | chunks +%u (%u ahead) -%u"
           " | backing ms heap %.2f (%.2f) clear %.2f (%.2f)"
           " mark %.2f (%.2f) uncache %.2f (%.2f)\n",
           seconds, frames, frames ? interval_ms / frames : 0.0,
           per_frame(TIMER(SUBMIT).count, frames),
           ms(per_frame(TIMER(SUBMIT).total_us, frames)),
           ms(TIMER(SUBMIT).max_us),
           per_frame(flushes, frames),
           flushes ? (double)entries / flushes : 0.0,
           ms(per_frame(TIMER(FLUSH).total_us, frames)),
           ms(TIMER(FLUSH).max_us),
           per_frame(polls, frames), per_frame(blocking, frames),
           ms(per_frame(TIMER(JOIN).total_us, frames)),
           ms(TIMER(JOIN).max_us),
           ms(per_frame(TIMER(ACQUIRE).total_us, frames)),
           ms(TIMER(ACQUIRE).max_us),
           ms(per_frame(TIMER(PRESENT).total_us, frames)),
           ms(TIMER(PRESENT).max_us),
           per_frame(TIMER(ALLOCATE).count, frames),
           ms(per_frame(TIMER(ALLOCATE).total_us, frames)),
           ms(TIMER(ALLOCATE).max_us),
           per_frame(service_count, frames),
           (unsigned long long)TIMER(SERVICE_MAP).count,
           (unsigned long long)TIMER(SERVICE_UNMAP).count,
           (unsigned long long)TIMER(SERVICE_KIND).count,
           (unsigned long long)TIMER(SERVICE_REGISTER).count,
           (unsigned long long)TIMER(SERVICE_UNREGISTER).count,
           ms(per_frame(service_us, frames)), ms(service_max_us),
           b->chunks_registered - a->chunks_registered,
           b->chunks_prepared - a->chunks_prepared,
           b->chunks_released - a->chunks_released,
           ms(TIMER(BACKING_HEAP).total_us), ms(TIMER(BACKING_HEAP).max_us),
           ms(TIMER(BACKING_CLEAR).total_us), ms(TIMER(BACKING_CLEAR).max_us),
           ms(TIMER(BACKING_MARK).total_us), ms(TIMER(BACKING_MARK).max_us),
           ms(TIMER(BACKING_UNCACHE).total_us),
           ms(TIMER(BACKING_UNCACHE).max_us));
#undef TIMER
   fflush(profile_log.file);
   profile_log.previous = *now;
}

static int
profile_log_thread(void *argument)
{
   (void)argument;
   mtx_lock(&profile_log.mutex);
   while (!profile_log.stop) {
      struct timespec deadline;
      timespec_get(&deadline, TIME_UTC);
      const uint64_t ns = (uint64_t)deadline.tv_nsec +
                          (uint64_t)profile_log.interval_ms * 1000000u;
      deadline.tv_sec += (time_t)(ns / 1000000000u);
      deadline.tv_nsec = (long)(ns % 1000000000u);
      while (!profile_log.stop &&
             cnd_timedwait(&profile_log.stop_requested, &profile_log.mutex,
                           &deadline) != thrd_timedout) {
      }
      if (profile_log.stop)
         break;
      mtx_unlock(&profile_log.mutex);
      struct profile_sample sample;
      profile_log_take_sample(&sample);
      profile_log_write_line(&sample);
      mtx_lock(&profile_log.mutex);
   }
   mtx_unlock(&profile_log.mutex);
   return 0;
}

void
nvkmd_nvgpu_native_profile_log_start(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (profile_log.running)
      return;
   const uint32_t interval_ms = profile_log_interval();
   if (!interval_ms)
      return;
   FILE *file = fopen(PROFILE_LOG_PATH, "w");
   if (!file) {
      mesa_logw("profile log: cannot open %s", PROFILE_LOG_PATH);
      return;
   }
   if (mtx_init(&profile_log.mutex, mtx_plain) != thrd_success) {
      fclose(file);
      return;
   }
   if (cnd_init(&profile_log.stop_requested) != thrd_success) {
      mtx_destroy(&profile_log.mutex);
      fclose(file);
      return;
   }
   profile_log.owner = owner;
   profile_log.file = file;
   profile_log.interval_ms = interval_ms;
   profile_log.stop = false;
   fprintf(file, "OunceVK profile log, %u ms per line; figures are per "
           "frame (presents), max is the longest single call\n",
           interval_ms);
   profile_log_take_sample(&profile_log.previous);
   profile_log.start_tick = profile_log.previous.tick;
   if (thrd_create(&profile_log.thread, profile_log_thread, NULL) !=
       thrd_success) {
      cnd_destroy(&profile_log.stop_requested);
      mtx_destroy(&profile_log.mutex);
      fclose(file);
      return;
   }
   profile_log.running = true;
   mesa_logi("profile log: writing %s every %u ms", PROFILE_LOG_PATH,
             interval_ms);
}

void
nvkmd_nvgpu_native_profile_log_stop(void)
{
   if (!profile_log.running)
      return;
   mtx_lock(&profile_log.mutex);
   profile_log.stop = true;
   cnd_signal(&profile_log.stop_requested);
   mtx_unlock(&profile_log.mutex);
   thrd_join(profile_log.thread, NULL);
   /* The last, partial interval. */
   struct profile_sample sample;
   profile_log_take_sample(&sample);
   profile_log_write_line(&sample);
   fclose(profile_log.file);
   cnd_destroy(&profile_log.stop_requested);
   mtx_destroy(&profile_log.mutex);
   memset(&profile_log, 0, sizeof(profile_log));
}
