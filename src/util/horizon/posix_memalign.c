/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include <errno.h>
#include <malloc.h>
#include <reent.h>

/* posix_memalign on newlib's _memalign_r. _memalign_r checks the size and
 * keeps the block metadata when it splits a block. It runs under newlib's
 * recursive malloc lock. The native CPU heap, its owner and its lock are
 * outside this wrapper.
 */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
   if (alignment < sizeof(void *) || alignment % sizeof(void *) != 0 ||
       (alignment & (alignment - 1)) != 0)
      return EINVAL;

   struct _reent *r = __getreent();
   const int saved_errno = r->_errno;
   void *allocation = _memalign_r(r, alignment, size);
   r->_errno = saved_errno;
   if (allocation == NULL)
      return ENOMEM;
   *memptr = allocation;
   return 0;
}
