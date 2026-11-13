/*
 * Copyright © 2026 OunceVK contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Shader disk cache for Horizon (Nintendo Switch homebrew).
 *
 * Horizon has no mmap() and no file locks, so this backend cannot use Mesa's
 * own cache layouts. It supplies disk_cache's blob callbacks instead. It
 * stores the entries in one append-only file per process on the SD card.
 */

#ifndef DISK_CACHE_STORE_H
#define DISK_CACHE_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct disk_cache;

/* The largest value that the store keeps. disk_cache.c reads entries back
 * into a buffer of this size.
 */
#define DISK_CACHE_STORE_MAX_BLOB (256 * 1024)

/* Opens the process's store, or takes another reference to it, and installs
 * the blob callbacks on the cache. On failure nothing is installed and the
 * cache behaves as disabled. The first opener's driver keys select the
 * store file; max_size limits its length in bytes.
 */
void
disk_cache_store_init(struct disk_cache *cache, uint64_t max_size,
                      const void *driver_keys, size_t driver_keys_size);

/* Drops a reference that a successful disk_cache_store_init took. The last
 * reference closes the store.
 */
void
disk_cache_store_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* DISK_CACHE_STORE_H */
