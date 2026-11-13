/* SPDX-License-Identifier: MIT */

/*
 * Stub <dlfcn.h> for Horizon.
 * Horizon has no dynamic loader. Every call reports that loading is not
 * available.
 */

#ifndef SWITCH_CROSS_DLFCN_H
#define SWITCH_CROSS_DLFCN_H

#include <stddef.h>

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100

#ifdef __cplusplus
extern "C" {
#endif

static inline void *dlopen(const char *file, int mode)
{
    (void)file;
    (void)mode;
    return NULL;
}

static inline void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    return NULL;
}

static inline int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

static inline char *dlerror(void)
{
    return (char *)"dynamic loading is not available on Horizon";
}

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_CROSS_DLFCN_H */
