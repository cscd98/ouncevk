/* SPDX-License-Identifier: MIT
 *
 * POSIX functions that Mesa needs and that newlib and libounce do not define.
 * The values describe Horizon, the Switch 2 operating system.
 */

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#define POSIX_SHIMS_PAGE_SIZE 0x1000

/* Horizon has no users or groups. Each identity is 0. */
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

static long
total_memory_pages(void)
{
   u64 total_size = 0;

   if (R_FAILED(svcGetInfo(&total_size, InfoType_TotalMemorySize,
                           CUR_PROCESS_HANDLE, 0)))
      return -1;

   return (long)(total_size / POSIX_SHIMS_PAGE_SIZE);
}

static long
allowed_core_count(void)
{
   u64 core_mask = 0;

   if (R_FAILED(svcGetInfo(&core_mask, InfoType_CoreMask,
                           CUR_PROCESS_HANDLE, 0)))
      return -1;

   /* A process with no allowed cores is not valid. */
   if (core_mask == 0)
      return -1;

   return __builtin_popcountll(core_mask);
}

long
sysconf(int name)
{
   switch (name) {
   case _SC_PAGESIZE:
      return POSIX_SHIMS_PAGE_SIZE;
   case _SC_PHYS_PAGES:
      return total_memory_pages();
   case _SC_NPROCESSORS_CONF:
   case _SC_NPROCESSORS_ONLN:
      return allowed_core_count();
   default:
      errno = EINVAL;
      return -1;
   }
}

/* Horizon has no POSIX signals. The signal mask is always empty. */
int
pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
   (void)how;
   (void)set;

   if (oldset != NULL)
      sigemptyset(oldset);

   return 0;
}

/* The disk cache refers to the functions below. On Horizon it uses only
 * its own store (disk_cache_store.c), which needs none of them. The
 * packaged library keeps them local, so programs do not see them. */

/* One process at a time uses a file, so a lock always succeeds. */
int
flock(int fd, int operation)
{
   (void)fd;
   (void)operation;
   return 0;
}

int
dirfd(DIR *dir)
{
   (void)dir;
   errno = ENOSYS;
   return -1;
}

int
fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
   (void)dirfd;
   (void)path;
   (void)buf;
   (void)flags;
   errno = ENOSYS;
   return -1;
}

/* Horizon has no user database. */
int
getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t size,
           struct passwd **result)
{
   (void)uid;
   (void)pwd;
   (void)buf;
   (void)size;
   *result = NULL;
   return ENOENT;
}
