/* SPDX-License-Identifier: MIT
 * The chunk reserve. See nvkmd_nvgpu_native_chunk_reserve.h. Original
 * OunceVK code.
 */
#include "nvkmd_nvgpu_native_chunk_reserve.h"
#include "nvkmd_nvgpu_native_chunked.h"
#include "nvkmd_nvgpu_native_device_owner.h"

#include "util/log.h"

#include <c11/threads.h>
#include <stdbool.h>
#include <stdint.h>

/* The thread adds no chunk while the process heap has less free memory
 * than this: the program needs the heap more than the reserve does. */
#define RESERVE_HEAP_FLOOR_B (UINT64_C(512) * 1024 * 1024)

static struct {
   /* Guards the fields below. Never held while the owner lock is taken. */
   mtx_t mutex;
   cnd_t changed;
   thrd_t thread;
   /* running: the thread exists. stop: it must end. pending: the pool was
    * reported below the reserve. */
   bool running, stop, pending;
} reserve;

static once_flag reserve_once = ONCE_FLAG_INIT;
static bool reserve_usable;

/* The mutex and the condition exist for the whole process, so that a wake
 * needs no check of whether the thread was ever started. */
static void
reserve_init_once(void)
{
   if (mtx_init(&reserve.mutex, mtx_plain) != thrd_success)
      return;
   if (cnd_init(&reserve.changed) != thrd_success) {
      mtx_destroy(&reserve.mutex);
      return;
   }
   reserve_usable = true;
}

static bool
reserve_stopping(void)
{
   mtx_lock(&reserve.mutex);
   const bool stop = reserve.stop;
   mtx_unlock(&reserve.mutex);
   return stop;
}

static bool
reserve_heap_allows(void)
{
   uint64_t available_B = 0;
   nvkmd_nvgpu_native_heap_query(NULL, &available_B);
   return available_B >= RESERVE_HEAP_FLOOR_B;
}

/* The number of chunks that the reserve lacks now. */
static uint32_t
reserve_missing(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!nvkmd_nvgpu_native_owner_lock(owner))
      return 0;
   const uint32_t missing = nvkmd_nvgpu_native_chunk_reserve_missing(owner);
   nvkmd_nvgpu_native_owner_unlock(owner);
   return missing;
}

/* Adds chunks until the reserve is full. It ends early when the thread
 * must stop, the heap is low or a registration fails. */
static void
reserve_fill(struct nvkmd_nvgpu_native_device_owner *owner)
{
   while (!reserve_stopping() && reserve_missing(owner) &&
          reserve_heap_allows()) {
      struct nvkmd_nvgpu_native_chunk *chunk =
         nvkmd_nvgpu_native_chunk_reserve_prepare();
      if (!chunk)
         return;
      if (!nvkmd_nvgpu_native_owner_lock(owner)) {
         nvkmd_nvgpu_native_chunk_reserve_discard(chunk);
         return;
      }
      /* The program may have freed memory during the clearing: the reserve
       * can be full now. */
      bool added = false;
      const bool wanted = nvkmd_nvgpu_native_chunk_reserve_missing(owner);
      if (wanted)
         added = nvkmd_nvgpu_native_chunk_reserve_add(owner, chunk);
      else
         nvkmd_nvgpu_native_chunk_reserve_discard(chunk);
      nvkmd_nvgpu_native_owner_unlock(owner);
      if (!added)
         return;
   }
}

static int
reserve_thread(void *argument)
{
   struct nvkmd_nvgpu_native_device_owner *owner = argument;
   mtx_lock(&reserve.mutex);
   while (!reserve.stop) {
      if (!reserve.pending) {
         cnd_wait(&reserve.changed, &reserve.mutex);
         continue;
      }
      reserve.pending = false;
      mtx_unlock(&reserve.mutex);
      reserve_fill(owner);
      mtx_lock(&reserve.mutex);
   }
   mtx_unlock(&reserve.mutex);
   return 0;
}

void
nvkmd_nvgpu_native_chunk_reserve_start(
   struct nvkmd_nvgpu_native_device_owner *owner)
{
   call_once(&reserve_once, reserve_init_once);
   if (!reserve_usable)
      return;
   mtx_lock(&reserve.mutex);
   if (!reserve.running) {
      reserve.stop = false;
      reserve.pending = false;
      if (thrd_create(&reserve.thread, reserve_thread, owner) == thrd_success)
         reserve.running = true;
      else
         mesa_logw("chunk reserve: no thread; chunks are registered when "
                   "an allocation needs them");
   }
   mtx_unlock(&reserve.mutex);
}

void
nvkmd_nvgpu_native_chunk_reserve_stop(void)
{
   call_once(&reserve_once, reserve_init_once);
   if (!reserve_usable)
      return;
   mtx_lock(&reserve.mutex);
   const bool running = reserve.running;
   if (running) {
      reserve.stop = true;
      cnd_signal(&reserve.changed);
   }
   mtx_unlock(&reserve.mutex);
   if (!running)
      return;
   thrd_join(reserve.thread, NULL);
   mtx_lock(&reserve.mutex);
   reserve.running = false;
   mtx_unlock(&reserve.mutex);
}

void
nvkmd_nvgpu_native_chunk_reserve_wake(void)
{
   call_once(&reserve_once, reserve_init_once);
   if (!reserve_usable)
      return;
   mtx_lock(&reserve.mutex);
   if (reserve.running && !reserve.stop) {
      reserve.pending = true;
      cnd_signal(&reserve.changed);
   }
   mtx_unlock(&reserve.mutex);
}
