/* SPDX-License-Identifier: MIT */

/*
 * Stub <sys/mman.h> for Horizon.
 * Horizon has no POSIX memory mapping. Every call fails.
 */

#ifndef SWITCH_CROSS_SYS_MMAN_H
#define SWITCH_CROSS_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#ifdef __cplusplus
extern "C" {
#endif

static inline void *mmap(void *address, size_t length, int protection,
                         int flags, int fd, off_t offset)
{
    (void)address;
    (void)length;
    (void)protection;
    (void)flags;
    (void)fd;
    (void)offset;
    return MAP_FAILED;
}

static inline int munmap(void *address, size_t length)
{
    (void)address;
    (void)length;
    return -1;
}

static inline int mprotect(void *address, size_t length, int protection)
{
    (void)address;
    (void)length;
    (void)protection;
    return -1;
}

static inline int msync(void *address, size_t length, int flags)
{
    (void)address;
    (void)length;
    (void)flags;
    return -1;
}

static inline int madvise(void *address, size_t length, int advice)
{
    (void)address;
    (void)length;
    (void)advice;
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_CROSS_SYS_MMAN_H */
