/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

/*
 * flock(2) emulation on top of fcntl advisory locks.  This is "good
 * enough" for amused, not a _real_ emulation.  flock and fcntl locks
 * have subtly different behaviours!
 */
int
flock(int fd, int op)
{
	struct flock l;
	int cmd;

	memset(&l, 0, sizeof(l));
	l.l_whence = SEEK_SET;

	if (op & LOCK_SH)
		l.l_type = F_RDLCK;
	else if (op & LOCK_EX)
		l.l_type = F_WRLCK;
	else {
		errno = EINVAL;
		return -1;
	}

	cmd = F_SETLKW;
	if (op & LOCK_NB)
		cmd = F_SETLK;

	return fcntl(fd, cmd, &l);
}
