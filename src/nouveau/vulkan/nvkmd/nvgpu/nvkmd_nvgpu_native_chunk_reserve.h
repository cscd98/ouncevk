/* SPDX-License-Identifier: MIT
 * The chunk reserve: a thread that registers free chunks of GPU-only memory
 * ahead of the allocations that need them.
 *
 * A new chunk costs about 0.55 ms on the console (clearing 4 MiB, the
 * change to CPU-uncached, and the registration), so an allocation that
 * needs 49 new chunks took 34 ms on the thread that called
 * vkAllocateMemory. With the reserve, the pool of free chunks holds as many
 * chunks as the largest chunked allocation so far that had to register
 * one, up to 256 MiB. When an allocation takes chunks from the pool, the
 * thread fills it again. It clears each chunk without the owner lock and
 * takes the lock only for the registration. A free releases chunks only
 * when the pool holds more than the reserve plus 128 MiB.
 *
 * The thread adds no chunk while the process heap has less than
 * 512 MiB free, and a free then keeps 128 MiB of free chunks at most. When
 * the heap has no memory for the driver, every free chunk returns to it
 * and the reserve starts again from nothing.
 */
#ifndef NVKMD_NVGPU_NATIVE_CHUNK_RESERVE_H
#define NVKMD_NVGPU_NATIVE_CHUNK_RESERVE_H

#include <stdbool.h>

struct nvkmd_nvgpu_native_device_owner;

/* Whether the process heap has enough free memory for the reserve to keep
 * chunks ready. Needs no lock. */
bool nvkmd_nvgpu_native_chunk_reserve_heap_allows(void);

/* Starts the thread for the device. Call after the device opened. Does
 * nothing when the thread already runs. */
void nvkmd_nvgpu_native_chunk_reserve_start(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* Stops the thread. Call before the device closes, without the owner
 * lock. */
void nvkmd_nvgpu_native_chunk_reserve_stop(void);

/* Tells the thread that the pool is below the reserve. Any thread can call
 * it, also with the owner lock held. Does nothing when the thread does not
 * run. */
void nvkmd_nvgpu_native_chunk_reserve_wake(void);

#endif
