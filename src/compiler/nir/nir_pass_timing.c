/*
 * Copyright © 2026 OunceVK contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Time spent in each NIR pass, for finding where shader compiles spend their
 * time. Set NIR_PASS_TIMING to a file path to turn it on. Every NIR_PASS and
 * NIR_LOOP_PASS then adds its wall time and a call to its pass's total, and
 * code can add named phases with nir_pass_timing_begin/end. A driver calls
 * nir_pass_timing_report after each compile: it appends the totals, largest
 * first, to the file and starts new totals. A pass's time includes the
 * passes that it runs itself. When the variable is not set, a pass costs one
 * check of a flag.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "nir.h"

#define NIR_PASS_TIMING_MAX 512

struct nir_pass_total {
   const char *name;
   uint64_t ns;
   uint32_t calls;
};

static simple_mtx_t timing_lock = SIMPLE_MTX_INITIALIZER;
static struct nir_pass_total totals[NIR_PASS_TIMING_MAX];
static unsigned total_count;
static uint64_t report_start_ns;
static unsigned report_index;

/* 0: not read yet, 1: off, 2: on. */
static int timing_state;
static const char *timing_path;

static bool
timing_enabled(void)
{
   if (likely(timing_state == 1))
      return false;
   if (timing_state == 0) {
      timing_path = getenv("NIR_PASS_TIMING");
      timing_state = timing_path && timing_path[0] ? 2 : 1;
   }
   return timing_state == 2;
}

uint64_t
nir_pass_timing_begin(void)
{
   return timing_enabled() ? os_time_get_nano() : 0;
}

void
nir_pass_timing_end(const char *name, uint64_t start_ns)
{
   if (!start_ns)
      return;
   const uint64_t ns = os_time_get_nano() - start_ns;

   simple_mtx_lock(&timing_lock);
   if (!report_start_ns)
      report_start_ns = start_ns;
   /* Pass names are string literals, so the same pass has the same pointer
    * at each call site; compare the text so that call sites share a total.
    */
   unsigned i;
   for (i = 0; i < total_count; i++) {
      if (totals[i].name == name || strcmp(totals[i].name, name) == 0)
         break;
   }
   if (i == total_count && total_count < NIR_PASS_TIMING_MAX)
      totals[total_count++] = (struct nir_pass_total){ name, 0, 0 };
   if (i < total_count) {
      totals[i].ns += ns;
      totals[i].calls++;
   }
   simple_mtx_unlock(&timing_lock);
}

static int
compare_totals(const void *a, const void *b)
{
   const struct nir_pass_total *x = a, *y = b;
   return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

void
nir_pass_timing_report(const char *label)
{
   if (!timing_enabled())
      return;

   simple_mtx_lock(&timing_lock);
   const uint64_t wall_ns = report_start_ns ? os_time_get_nano() - report_start_ns : 0;
   qsort(totals, total_count, sizeof(totals[0]), compare_totals);

   FILE *file = fopen(timing_path, "a");
   if (file) {
      fprintf(file, "== %u %s: %.1f ms from the first timed pass\n",
              report_index, label ? label : "", wall_ns / 1e6);
      for (unsigned i = 0; i < total_count; i++) {
         fprintf(file, "%9.2f ms %6u  %s\n", totals[i].ns / 1e6,
                 totals[i].calls, totals[i].name);
      }
      fclose(file);
   }

   report_index++;
   total_count = 0;
   report_start_ns = 0;
   simple_mtx_unlock(&timing_lock);
}
