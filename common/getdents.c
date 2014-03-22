/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * Copyright (C) 1993, 95, 96, 97, 98, 99,2000,2001 Free Software Foundation, Inc.
 * This file is based almost entirely on getdents.c in the GNU C Library.
 */

#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <linux/posix_types.h>

/* For Linux we need a special version of this file since the
   definition of `struct dirent' is not the same for the kernel and
   the libc.  There is one additional field which might be introduced
   in the kernel structure in the future.

   Here is the kernel definition of `struct dirent' as of 2.1.20:  */

struct kernel_dirent
  {
    long int d_ino;
    __kernel_off_t d_off;
    unsigned short int d_reclen;
    char d_name[256];
  };

struct kernel_dirent64
  {
    uint64_t		d_ino;
    int64_t		d_off;
    unsigned short int	d_reclen;
    unsigned char	d_type;
    char		d_name[256];
  };

#define DIRENT_SET_DP_INO(dp, value) (dp)->d_ino = (value)

#define __set_errno(e) (errno = (e))

int __have_no_getdents64;

#ifdef __NR_getdents64
# define __SYS_GETDENTS64(fd, buf, len) (syscall(SYS_getdents64, fd, buf, len))
#else
# define __SYS_GETDENTS64(fd, buf, len) ({ __set_errno(ENOSYS); -1; })
#endif

#ifdef __NR_getdents
# define __SYS_GETDENTS(fd, buf, len) (syscall(SYS_getdents, fd, buf, len))
#else
# define __SYS_GETDENTS(fd, buf, len) ({ __set_errno(ENOSYS); -1; })
#endif

/* The problem here is that we cannot simply read the next NBYTES
   bytes.  We need to take the additional field into account.  We use
   some heuristic.  Assuming the directory contains names with 14
   characters on average we can compute an estimated number of entries
   which fit in the buffer.  Taking this number allows us to specify a
   reasonable number of bytes to read.  If we should be wrong, we can
   reset the file descriptor.  In practice the kernel is limiting the
   amount of data returned much more then the reduced buffer size.  */
int
getdents_wrap (int fd, char *buf, size_t nbytes)
{
  struct dirent *dp;
  off64_t last_offset = -1;
  ssize_t retval;

  if (!__have_no_getdents64)
    {
      int saved_errno = errno;
      char *kbuf = buf;
      size_t kbytes = nbytes;
      if (offsetof (struct dirent, d_name)
	  < offsetof (struct kernel_dirent64, d_name)
	  && nbytes <= sizeof (struct dirent))
	{
	  kbytes = nbytes + offsetof (struct kernel_dirent64, d_name)
		   - offsetof (struct dirent, d_name);
	  kbuf = alloca(kbytes);
	}
      retval = __SYS_GETDENTS64(fd, kbuf, kbytes);
      if (retval != -1)
	{
	  struct kernel_dirent64 *kdp;
	  const size_t size_diff = (offsetof (struct kernel_dirent64, d_name)
				    - offsetof (struct dirent, d_name));

	  /* If the structure returned by the kernel is identical to what we
	     need, don't do any conversions.  */
	  if (offsetof (struct dirent, d_name)
	      == offsetof (struct kernel_dirent64, d_name)
	      && sizeof (dp->d_ino) == sizeof (kdp->d_ino)
	      && sizeof (dp->d_off) == sizeof (kdp->d_off))
	    return retval;

	  dp = (struct dirent *)buf;
	  kdp = (struct kernel_dirent64 *) kbuf;
	  while ((char *) kdp < kbuf + retval)
	    {
	      const size_t alignment = __alignof__ (struct dirent);
	      /* Since kdp->d_reclen is already aligned for the kernel
		 structure this may compute a value that is bigger
		 than necessary.  */
	      size_t old_reclen = kdp->d_reclen;
	      size_t new_reclen = ((old_reclen - size_diff + alignment - 1)
				  & ~(alignment - 1));
	      uint64_t d_ino = kdp->d_ino;
	      int64_t d_off = kdp->d_off;
	      unsigned char d_type = kdp->d_type;

	      DIRENT_SET_DP_INO (dp, d_ino);
	      dp->d_off = d_off;
	      if ((sizeof (dp->d_ino) != sizeof (kdp->d_ino)
		   && dp->d_ino != d_ino)
		  || (sizeof (dp->d_off) != sizeof (kdp->d_off)
		      && dp->d_off != d_off))
		{
		  /* Overflow.  If there was at least one entry
		     before this one, return them without error,
		     otherwise signal overflow.  */
		  if (last_offset != -1)
		    {
		      lseek64 (fd, last_offset, SEEK_SET);
		      return (char *) dp - buf;
		    }
		  __set_errno (EOVERFLOW);
		  return -1;
		}

	      last_offset = d_off;
	      dp->d_reclen = new_reclen;
	      dp->d_type = d_type;
	      memmove (dp->d_name, kdp->d_name,
		       old_reclen - offsetof (struct kernel_dirent64, d_name));

	      dp = (struct dirent *) ((char *) dp + new_reclen);
	      kdp = (struct kernel_dirent64 *) ((char *) kdp + old_reclen);
	    }

	  return (char *) dp - buf;
	}

      __set_errno (saved_errno);
      __have_no_getdents64 = 1;
    }

  /* fallback to getdents */
  {
    size_t red_nbytes;
    struct kernel_dirent *skdp, *kdp;
    const size_t size_diff = (offsetof (struct dirent, d_name)
			      - offsetof (struct kernel_dirent, d_name));

    red_nbytes = MIN (nbytes
		      - ((nbytes / (offsetof (struct dirent, d_name) + 14))
			 * size_diff),
		      nbytes - size_diff);

    dp = (struct dirent *) buf;
    skdp = kdp = alloca (red_nbytes);

    retval = __SYS_GETDENTS(fd, kdp, red_nbytes);
    if (retval == -1)
      return -1;

    while ((char *) kdp < (char *) skdp + retval)
      {
	const size_t alignment = __alignof__ (struct dirent);
	/* Since kdp->d_reclen is already aligned for the kernel structure
	   this may compute a value that is bigger than necessary.  */
	size_t new_reclen = ((kdp->d_reclen + size_diff + alignment - 1)
			     & ~(alignment - 1));
	if ((char *) dp + new_reclen > buf + nbytes)
	  {
	    /* Our heuristic failed.  We read too many entries.  Reset
	       the stream.  */
	    assert (last_offset != -1);
	    lseek64 (fd, last_offset, SEEK_SET);

	    if ((char *) dp == buf)
	      {
		/* The buffer the user passed in is too small to hold even
		   one entry.  */
		__set_errno (EINVAL);
		return -1;
	      }

	    break;
	  }

	last_offset = kdp->d_off;
	DIRENT_SET_DP_INO(dp, kdp->d_ino);
	dp->d_off = kdp->d_off;
	dp->d_reclen = new_reclen;
	dp->d_type = DT_UNKNOWN;
	memcpy (dp->d_name, kdp->d_name,
		kdp->d_reclen - offsetof (struct kernel_dirent, d_name));

	dp = (struct dirent *) ((char *) dp + new_reclen);
	kdp = (struct kernel_dirent *) (((char *) kdp) + kdp->d_reclen);
      }
    }

  return (char *) dp - buf;
}
