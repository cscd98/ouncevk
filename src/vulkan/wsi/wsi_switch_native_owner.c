/* SPDX-License-Identifier: MIT
 * VI2 release dependencies and acquire deadlines for the native WSI.
 */
#include "wsi_switch_native_owner.h"

static uint64_t
read_le(const uint8_t *p, unsigned count)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < count; i++)
      value |= (uint64_t)p[i] << (8 * i);
   return value;
}

uint32_t
wsn_dependency_token(const struct wsn_dependency *d)
{
   return (uint32_t)read_le(d->bytes, 4);
}

uint8_t
wsn_dependency_mode(const struct wsn_dependency *d)
{
   return d->bytes[4];
}

uint64_t
wsn_dependency_threshold(const struct wsn_dependency *d)
{
   return read_le(d->bytes + 8, 8);
}

bool
wsn_dependency_semantic_empty(const struct wsn_dependency *d)
{
   return wsn_dependency_token(d) == 0 && wsn_dependency_mode(d) == 0 &&
          wsn_dependency_threshold(d) == 0;
}

bool
wsn_dependency_canonical(const struct wsn_dependency *d)
{
   /* A strict OunceVK policy: mode 0 or 1 and zero padding bytes. */
   return d->bytes[4] <= 1 && d->bytes[5] == 0 &&
          d->bytes[6] == 0 && d->bytes[7] == 0;
}

struct wsn_deadline
wsn_deadline_make(uint64_t now, uint64_t timeout)
{
   return (struct wsn_deadline) {
      .infinite = timeout == UINT64_MAX,
      .end = timeout > UINT64_MAX - now ? UINT64_MAX : now + timeout,
   };
}

uint64_t
wsn_deadline_remaining(struct wsn_deadline deadline, uint64_t now)
{
   if (deadline.infinite)
      return UINT64_MAX;
   return now >= deadline.end ? 0 : deadline.end - now;
}
