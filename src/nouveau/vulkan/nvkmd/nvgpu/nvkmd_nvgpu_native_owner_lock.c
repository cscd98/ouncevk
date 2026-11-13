/* SPDX-License-Identifier: MIT */
/* The recursive lock of the native device owner. It is in its own file so
 * that a host stress test can build it without the service code. It is a
 * plain C11 mutex plus a holder identity. The threads of the native runtime
 * give plain C11 mutexes (as the progress domain uses), but not
 * mtx_recursive. The identity is the IPC TLS page of the calling thread,
 * which is unique per thread. Only the holder ever compares equal, so an
 * unlocked read of the holder is safe.
 */
#include "nvkmd_nvgpu_native_device_owner.h"

#include "c11/threads.h"
#include "util/cnd_monotonic.h"

#include <stdatomic.h>
#include <stdlib.h>

struct nvkmd_nvgpu_native_owner_lock {
   mtx_t mutex;
   struct u_cnd_monotonic joined; /* signalled when a blocking join ends */
   _Atomic(const void *) holder;
   uint32_t depth;
};

bool
nvkmd_nvgpu_native_owner_lock_init(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || owner->lock_ready)
      return false;
   /* Freed by nvkmd_nvgpu_native_owner_lock_finish when the owner resets. */
   struct nvkmd_nvgpu_native_owner_lock *lock = calloc(1, sizeof(*lock));
   if (!lock || mtx_init(&lock->mutex, mtx_plain) != thrd_success) {
      free(lock);
      return false;
   }
   if (u_cnd_monotonic_init(&lock->joined) != thrd_success) {
      mtx_destroy(&lock->mutex);
      free(lock);
      return false;
   }
   owner->lock = lock;
   owner->lock_ready = true;
   return true;
}

void
nvkmd_nvgpu_native_owner_lock_finish(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || !owner->lock_ready)
      return;
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   u_cnd_monotonic_destroy(&lock->joined);
   mtx_destroy(&lock->mutex);
   free(lock);
   owner->lock = NULL;
   owner->lock_ready = false;
}

bool
nvkmd_nvgpu_native_owner_lock(struct nvkmd_nvgpu_native_device_owner *owner)
{
   if (!owner || owner->self != owner || !owner->lock_ready)
      return false;
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   const void *self = armGetTls();
   if (atomic_load_explicit(&lock->holder, memory_order_relaxed) == self) {
      lock->depth++;
      return true;
   }
   if (mtx_lock(&lock->mutex) != thrd_success)
      return false;
   atomic_store_explicit(&lock->holder, self, memory_order_relaxed);
   lock->depth = 1;
   return true;
}

void
nvkmd_nvgpu_native_owner_unlock(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   if (--lock->depth)
      return;
   atomic_store_explicit(&lock->holder, NULL, memory_order_relaxed);
   mtx_unlock(&lock->mutex);
}

uint32_t
nvkmd_nvgpu_native_owner_suspend(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   const uint32_t depth = lock->depth;
   lock->depth = 0;
   atomic_store_explicit(&lock->holder, NULL, memory_order_relaxed);
   mtx_unlock(&lock->mutex);
   return depth;
}

void
nvkmd_nvgpu_native_owner_resume(struct nvkmd_nvgpu_native_device_owner *owner,
                                uint32_t depth)
{
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   mtx_lock(&lock->mutex);
   atomic_store_explicit(&lock->holder, armGetTls(),
                         memory_order_relaxed);
   lock->depth = depth;
}

void
nvkmd_nvgpu_native_owner_wait_join(struct nvkmd_nvgpu_native_device_owner *owner)
{
   struct nvkmd_nvgpu_native_owner_lock *lock = owner->lock;
   const uint32_t depth = lock->depth;
   owner->stats.join_waits++;
   lock->depth = 0;
   atomic_store_explicit(&lock->holder, NULL, memory_order_relaxed);
   while (owner->joining)
      u_cnd_monotonic_wait(&lock->joined, &lock->mutex);
   atomic_store_explicit(&lock->holder, armGetTls(),
                         memory_order_relaxed);
   lock->depth = depth;
}

void
nvkmd_nvgpu_native_owner_join_done(struct nvkmd_nvgpu_native_device_owner *owner)
{
   u_cnd_monotonic_broadcast(&owner->lock->joined);
}
