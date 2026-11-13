/* SPDX-License-Identifier: MIT */
/* The platform hook that Mesa's Horizon code calls: the CPU tick counter.
 * runtime_hooks.c implements it on libounce.
 */
#ifndef UTIL_HORIZON_RUNTIME_HOOKS_H
#define UTIL_HORIZON_RUNTIME_HOOKS_H

#include <stdint.h>

/* The CPU tick counter runs at 31.25 MHz: one tick is 32 ns. */
#define OUNCEVK_CLOCK_NS_PER_TICK UINT64_C(32)

/* The current value of the CPU tick counter. */
uint64_t ouncevk_clock_read_raw(void);

#endif
