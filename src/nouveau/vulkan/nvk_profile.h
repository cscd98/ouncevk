/*
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_PROFILE_H
#define NVK_PROFILE_H 1

#include <ouncevk/profile.h>

/* Adds a time to the driver profile (ouncevk/profile.h). The profile is
 * part of the native backend, so the call does nothing in a build without
 * it. */
static inline void
nvk_profile_record_ns(enum ouncevk_profile_timer timer, uint64_t ns)
{
#ifdef NVK_NATIVE_BACKING_ENABLED
   ouncevk_profile_record_ns(timer, ns);
#else
   (void)timer;
   (void)ns;
#endif
}

#endif
