/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ev.h"

#ifndef timespecsub
static void
timespecsub(struct timespec *a, struct timespec *b, struct timespec *ret)
{
	ret->tv_sec = a->tv_sec - b->tv_sec;
	ret->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (ret->tv_nsec < 0) {
		ret->tv_sec--;
		ret->tv_nsec += 1000000000L;
	}
}
#endif

struct evcb {
	void		(*cb)(int, int, void *);
	void		*udata;
};

struct evbase {
	size_t		 len;

	struct pollfd	*pfds;
	size_t		 pfdlen;

	struct evcb	*cbs;
	size_t		 cblen;

	int		 sigpipe[2];
	struct evcb	 sigcb;

	int		 timeout;
	struct evcb	 toutcb;
};

static struct evbase	*base;
static int		 ev_stop;

static int
ev_resize(size_t len)
{
	void	*t;
	size_t	 i;

	t = recallocarray(base->pfds, base->pfdlen, len, sizeof(*base->pfds));
	if (t == NULL)
		return -1;
	base->pfds = t;
	base->pfdlen = len;

	for (i = base->len; i < len; ++i)
		base->pfds[i].fd = -1;

	t = recallocarray(base->cbs, base->cblen, len, sizeof(*base->cbs));
	if (t == NULL)
		return -1;
	base->cbs = t;
	base->cblen = len;

	base->len = len;
	return 0;
}

int
ev_init(void)
{
	if (base != NULL) {
		errno = EINVAL;
		return -1;
	}

	if ((base = calloc(1, sizeof(*base))) == NULL)
		return -1;

	base->sigpipe[0] = -1;
	base->sigpipe[1] = -1;
	base->timeout = INFTIM;

	if (ev_resize(16) == -1) {
		free(base->pfds);
		free(base->cbs);
		free(base);
		base = NULL;
		return -1;
	}

	return 0;
}

int
ev_add(int fd, int ev, void (*cb)(int, int, void *), void *udata)
{
	if (fd >= base->len) {
		if (ev_resize(fd + 1) == -1)
			return -1;
	}

	base->pfds[fd].fd = fd;
	base->pfds[fd].events = ev;

	base->cbs[fd].cb = cb;
	base->cbs[fd].udata = udata;

	return 0;
}

static void
ev_sigcatch(int signo)
{
	unsigned char	 s;
	int		 err;

	err = errno;

	/*
	 * We should be able to write up to PIPE_BUF bytes without
	 * blocking.
	 */
	s = signo;
	(void) write(base->sigpipe[1], &s, sizeof(s));

	errno = err;
}

static void
ev_sigdispatch(int fd, int ev, void *data)
{
	unsigned char	 signo;

	if (read(fd, &signo, sizeof(signo)) != sizeof(signo))
		return;

	base->sigcb.cb(signo, 0, base->sigcb.udata);
}

int
ev_signal(int sig, void (*cb)(int, int, void *), void *udata)
{
	if (base->sigpipe[0] == -1) {
		if (pipe2(base->sigpipe, O_NONBLOCK) == -1)
			return -1;
		if (ev_add(base->sigpipe[0], POLLIN, ev_sigdispatch, NULL)
		    == -1)
			return -1;
	}

	base->sigcb.cb = cb;
	base->sigcb.udata = udata;

	signal(sig, ev_sigcatch);
	return 0;
}

int
ev_timer(const struct timeval *tv, void (*cb)(int, int, void*), void *udata)
{
	base->timeout = INFTIM;
	if (tv) {
		base->timeout = tv->tv_sec * 1000;
		base->timeout += tv->tv_usec / 1000;
	}

	base->toutcb.cb = cb;
	base->toutcb.udata = udata;

	return 0;
}

int
ev_timer_pending(void)
{
	return base->timeout != INFTIM;
}

int
ev_del(int fd)
{
	if (fd >= base->len) {
		errno = ERANGE;
		return -1;
	}

	base->pfds[fd].fd = -1;
	base->pfds[fd].events = 0;

	base->cbs[fd].cb = NULL;
	base->cbs[fd].udata = NULL;

	return 0;
}

int
ev_loop(void)
{
	struct timespec	 elapsed, beg, end;
	int		 n, em;
	size_t		 i;

	while (!ev_stop) {
		clock_gettime(CLOCK_MONOTONIC, &beg);
		if ((n = poll(base->pfds, base->len, base->timeout)) == -1) {
			if (errno != EINTR)
				return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &end);

		timespecsub(&end, &beg, &elapsed);
		em = elapsed.tv_sec * 1000 + elapsed.tv_nsec / 1000000;
		if (base->timeout != INFTIM) {
			if (base->timeout - em < 0 || n == 0) {
				base->timeout = INFTIM;
				base->toutcb.cb(-1, 0, base->toutcb.udata);
			} else
				base->timeout -= em;
		}

		for (i = 0; i < base->len && n > 0 && !ev_stop; ++i) {
			if (base->pfds[i].fd == -1)
				continue;
			if (base->pfds[i].revents & (POLLIN|POLLOUT|POLLHUP)) {
				n--;
				base->cbs[i].cb(base->pfds[i].fd,
				    base->pfds[i].revents,
				    base->cbs[i].udata);
			}
		}
	}

	return 0;
}

void
ev_break(void)
{
	ev_stop = 1;
}
