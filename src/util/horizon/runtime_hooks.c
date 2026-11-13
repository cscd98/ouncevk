/* SPDX-License-Identifier: MIT */
/* The platform hook of runtime_hooks.h, on libounce's runtime. */
#include "runtime_hooks.h"

#include <switch.h>

uint64_t
ouncevk_clock_read_raw(void)
{
   return armGetSystemTick();
}
