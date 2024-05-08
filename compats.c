#include "config.h"
#if !HAVE_FLOCK
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
#endif /* HAVE_FLOCK */
#if !HAVE_FREEZERO
#include <stdlib.h>
#include <string.h>

void
freezero(void *ptr, size_t len)
{
	if (ptr == NULL)
		return;
	memset(ptr, 0, len);
	free(ptr);
}
#endif /* HAVE_FREEZERO */
#if !HAVE_EXPLICIT_BZERO
/* OPENBSD ORIGINAL: lib/libc/string/explicit_bzero.c */
/*
 * Public domain.
 * Written by Ted Unangst
 */

#include <string.h>

/*
 * explicit_bzero - don't let the compiler optimize away bzero
 */

#if HAVE_MEMSET_S

void
explicit_bzero(void *p, size_t n)
{
	if (n == 0)
		return;
	(void)memset_s(p, n, 0, n);
}

#else /* HAVE_MEMSET_S */

#include <strings.h>

/*
 * Indirect memset through a volatile pointer to hopefully avoid
 * dead-store optimisation eliminating the call.
 */
static void (* volatile ssh_memset)(void *, int, size_t) =
    (void (*volatile)(void *, int, size_t))memset;

void
explicit_bzero(void *p, size_t n)
{
	if (n == 0)
		return;
	/*
	 * clang -fsanitize=memory needs to intercept memset-like functions
	 * to correctly detect memory initialisation. Make sure one is called
	 * directly since our indirection trick above sucessfully confuses it.
	 */
#if defined(__has_feature)
# if __has_feature(memory_sanitizer)
	memset(p, 0, n);
# endif
#endif

	ssh_memset(p, 0, n);
}

#endif /* HAVE_MEMSET_S */
#endif /* !HAVE_EXPLICIT_BZERO */
#if !HAVE_GETDTABLESIZE
/* public domain */
#include <unistd.h>

int
getdtablesize(void)
{
	return sysconf(_SC_OPEN_MAX);
}
#endif /* !HAVE_GETDTABLESIZE */
#if !HAVE_GETPROGNAME
/*
 * Copyright (c) 2016 Nicholas Marriott <nicholas.marriott@gmail.com>
 * Copyright (c) 2017 Kristaps Dzonsons <kristaps@bsd.lv>
 * Copyright (c) 2020 Stephen Gregoratto <dev@sgregoratto.me>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <errno.h>

#if HAVE_GETEXECNAME
#include <stdlib.h>
const char *
getprogname(void)
{
	return getexecname();
}
#elif HAVE_PROGRAM_INVOCATION_SHORT_NAME
const char *
getprogname(void)
{
	return (program_invocation_short_name);
}
#elif HAVE___PROGNAME
const char *
getprogname(void)
{
	extern char	*__progname;

	return (__progname);
}
#else
#warning No getprogname available.
const char *
getprogname(void)
{
	return ("amused");
}
#endif
#endif /* !HAVE_GETPROGNAME */
#if !HAVE_LIB_IMSG
/*	$OpenBSD: imsg-buffer.c,v 1.18 2023/12/12 15:47:41 claudio Exp $	*/
/*	$OpenBSD: imsg.c,v 1.23 2023/12/12 15:47:41 claudio Exp $	*/

/*
 * Copyright (c) 2023 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <limits.h>
#include <errno.h>
#include <endian.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "imsg.h"

static int	ibuf_realloc(struct ibuf *, size_t);
static void	ibuf_enqueue(struct msgbuf *, struct ibuf *);
static void	ibuf_dequeue(struct msgbuf *, struct ibuf *);
static void	msgbuf_drain(struct msgbuf *, size_t);

struct ibuf *
ibuf_open(size_t len)
{
	struct ibuf	*buf;

	if (len == 0) {
		errno = EINVAL;
		return (NULL);
	}
	if ((buf = calloc(1, sizeof(struct ibuf))) == NULL)
		return (NULL);
	if ((buf->buf = calloc(len, 1)) == NULL) {
		free(buf);
		return (NULL);
	}
	buf->size = buf->max = len;
	buf->fd = -1;

	return (buf);
}

struct ibuf *
ibuf_dynamic(size_t len, size_t max)
{
	struct ibuf	*buf;

	if (max == 0 || max < len) {
		errno = EINVAL;
		return (NULL);
	}

	if ((buf = calloc(1, sizeof(struct ibuf))) == NULL)
		return (NULL);
	if (len > 0) {
		if ((buf->buf = calloc(len, 1)) == NULL) {
			free(buf);
			return (NULL);
		}
	}
	buf->size = len;
	buf->max = max;
	buf->fd = -1;

	return (buf);
}

static int
ibuf_realloc(struct ibuf *buf, size_t len)
{
	unsigned char	*b;

	/* on static buffers max is eq size and so the following fails */
	if (len > SIZE_MAX - buf->wpos || buf->wpos + len > buf->max) {
		errno = ERANGE;
		return (-1);
	}

	b = recallocarray(buf->buf, buf->size, buf->wpos + len, 1);
	if (b == NULL)
		return (-1);
	buf->buf = b;
	buf->size = buf->wpos + len;

	return (0);
}

void *
ibuf_reserve(struct ibuf *buf, size_t len)
{
	void	*b;

	if (len > SIZE_MAX - buf->wpos || buf->max == 0) {
		errno = ERANGE;
		return (NULL);
	}

	if (buf->wpos + len > buf->size)
		if (ibuf_realloc(buf, len) == -1)
			return (NULL);

	b = buf->buf + buf->wpos;
	buf->wpos += len;
	return (b);
}

int
ibuf_add(struct ibuf *buf, const void *data, size_t len)
{
	void *b;

	if ((b = ibuf_reserve(buf, len)) == NULL)
		return (-1);

	memcpy(b, data, len);
	return (0);
}

int
ibuf_add_ibuf(struct ibuf *buf, const struct ibuf *from)
{
	return ibuf_add(buf, ibuf_data(from), ibuf_size(from));
}

/* remove after tree is converted */
int
ibuf_add_buf(struct ibuf *buf, const struct ibuf *from)
{
	return ibuf_add_ibuf(buf, from);
}

int
ibuf_add_n8(struct ibuf *buf, uint64_t value)
{
	uint8_t v;

	if (value > UINT8_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return ibuf_add(buf, &v, sizeof(v));
}

int
ibuf_add_n16(struct ibuf *buf, uint64_t value)
{
	uint16_t v;

	if (value > UINT16_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = htobe16(value);
	return ibuf_add(buf, &v, sizeof(v));
}

int
ibuf_add_n32(struct ibuf *buf, uint64_t value)
{
	uint32_t v;

	if (value > UINT32_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = htobe32(value);
	return ibuf_add(buf, &v, sizeof(v));
}

int
ibuf_add_n64(struct ibuf *buf, uint64_t value)
{
	value = htobe64(value);
	return ibuf_add(buf, &value, sizeof(value));
}

int
ibuf_add_h16(struct ibuf *buf, uint64_t value)
{
	uint16_t v;

	if (value > UINT16_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return ibuf_add(buf, &v, sizeof(v));
}

int
ibuf_add_h32(struct ibuf *buf, uint64_t value)
{
	uint32_t v;

	if (value > UINT32_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return ibuf_add(buf, &v, sizeof(v));
}

int
ibuf_add_h64(struct ibuf *buf, uint64_t value)
{
	return ibuf_add(buf, &value, sizeof(value));
}

int
ibuf_add_zero(struct ibuf *buf, size_t len)
{
	void *b;

	if ((b = ibuf_reserve(buf, len)) == NULL)
		return (-1);
	memset(b, 0, len);
	return (0);
}

void *
ibuf_seek(struct ibuf *buf, size_t pos, size_t len)
{
	/* only allow seeking between rpos and wpos */
	if (ibuf_size(buf) < pos || SIZE_MAX - pos < len ||
	    ibuf_size(buf) < pos + len) {
		errno = ERANGE;
		return (NULL);
	}

	return (buf->buf + buf->rpos + pos);
}

int
ibuf_set(struct ibuf *buf, size_t pos, const void *data, size_t len)
{
	void *b;

	if ((b = ibuf_seek(buf, pos, len)) == NULL)
		return (-1);

	memcpy(b, data, len);
	return (0);
}

int
ibuf_set_n8(struct ibuf *buf, size_t pos, uint64_t value)
{
	uint8_t v;

	if (value > UINT8_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return (ibuf_set(buf, pos, &v, sizeof(v)));
}

int
ibuf_set_n16(struct ibuf *buf, size_t pos, uint64_t value)
{
	uint16_t v;

	if (value > UINT16_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = htobe16(value);
	return (ibuf_set(buf, pos, &v, sizeof(v)));
}

int
ibuf_set_n32(struct ibuf *buf, size_t pos, uint64_t value)
{
	uint32_t v;

	if (value > UINT32_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = htobe32(value);
	return (ibuf_set(buf, pos, &v, sizeof(v)));
}

int
ibuf_set_n64(struct ibuf *buf, size_t pos, uint64_t value)
{
	value = htobe64(value);
	return (ibuf_set(buf, pos, &value, sizeof(value)));
}

int
ibuf_set_h16(struct ibuf *buf, size_t pos, uint64_t value)
{
	uint16_t v;

	if (value > UINT16_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return (ibuf_set(buf, pos, &v, sizeof(v)));
}

int
ibuf_set_h32(struct ibuf *buf, size_t pos, uint64_t value)
{
	uint32_t v;

	if (value > UINT32_MAX) {
		errno = EINVAL;
		return (-1);
	}
	v = value;
	return (ibuf_set(buf, pos, &v, sizeof(v)));
}

int
ibuf_set_h64(struct ibuf *buf, size_t pos, uint64_t value)
{
	return (ibuf_set(buf, pos, &value, sizeof(value)));
}

void *
ibuf_data(const struct ibuf *buf)
{
	return (buf->buf + buf->rpos);
}

size_t
ibuf_size(const struct ibuf *buf)
{
	return (buf->wpos - buf->rpos);
}

size_t
ibuf_left(const struct ibuf *buf)
{
	if (buf->max == 0)
		return (0);
	return (buf->max - buf->wpos);
}

int
ibuf_truncate(struct ibuf *buf, size_t len)
{
	if (ibuf_size(buf) >= len) {
		buf->wpos = buf->rpos + len;
		return (0);
	}
	if (buf->max == 0) {
		/* only allow to truncate down */
		errno = ERANGE;
		return (-1);
	}
	return ibuf_add_zero(buf, len - ibuf_size(buf));
}

void
ibuf_rewind(struct ibuf *buf)
{
	buf->rpos = 0;
}

void
ibuf_close(struct msgbuf *msgbuf, struct ibuf *buf)
{
	ibuf_enqueue(msgbuf, buf);
}

void
ibuf_from_buffer(struct ibuf *buf, void *data, size_t len)
{
	memset(buf, 0, sizeof(*buf));
	buf->buf = data;
	buf->size = buf->wpos = len;
	buf->fd = -1;
}

void
ibuf_from_ibuf(struct ibuf *buf, const struct ibuf *from)
{
	ibuf_from_buffer(buf, ibuf_data(from), ibuf_size(from));
}

int
ibuf_get(struct ibuf *buf, void *data, size_t len)
{
	if (ibuf_size(buf) < len) {
		errno = EBADMSG;
		return (-1);
	}

	memcpy(data, ibuf_data(buf), len);
	buf->rpos += len;
	return (0);
}

int
ibuf_get_ibuf(struct ibuf *buf, size_t len, struct ibuf *new)
{
	if (ibuf_size(buf) < len) {
		errno = EBADMSG;
		return (-1);
	}

	ibuf_from_buffer(new, ibuf_data(buf), len);
	buf->rpos += len;
	return (0);
}

int
ibuf_get_n8(struct ibuf *buf, uint8_t *value)
{
	return ibuf_get(buf, value, sizeof(*value));
}

int
ibuf_get_n16(struct ibuf *buf, uint16_t *value)
{
	int rv;

	rv = ibuf_get(buf, value, sizeof(*value));
	*value = be16toh(*value);
	return (rv);
}

int
ibuf_get_n32(struct ibuf *buf, uint32_t *value)
{
	int rv;

	rv = ibuf_get(buf, value, sizeof(*value));
	*value = be32toh(*value);
	return (rv);
}

int
ibuf_get_n64(struct ibuf *buf, uint64_t *value)
{
	int rv;

	rv = ibuf_get(buf, value, sizeof(*value));
	*value = be64toh(*value);
	return (rv);
}

int
ibuf_get_h16(struct ibuf *buf, uint16_t *value)
{
	return ibuf_get(buf, value, sizeof(*value));
}

int
ibuf_get_h32(struct ibuf *buf, uint32_t *value)
{
	return ibuf_get(buf, value, sizeof(*value));
}

int
ibuf_get_h64(struct ibuf *buf, uint64_t *value)
{
	return ibuf_get(buf, value, sizeof(*value));
}

int
ibuf_skip(struct ibuf *buf, size_t len)
{
	if (ibuf_size(buf) < len) {
		errno = EBADMSG;
		return (-1);
	}

	buf->rpos += len;
	return (0);
}

void
ibuf_free(struct ibuf *buf)
{
	if (buf == NULL)
		return;
	if (buf->max == 0)	/* if buf lives on the stack */
		abort();	/* abort before causing more harm */
	if (buf->fd != -1)
		close(buf->fd);
	freezero(buf->buf, buf->size);
	free(buf);
}

int
ibuf_fd_avail(struct ibuf *buf)
{
	return (buf->fd != -1);
}

int
ibuf_fd_get(struct ibuf *buf)
{
	int fd;

	fd = buf->fd;
	buf->fd = -1;
	return (fd);
}

void
ibuf_fd_set(struct ibuf *buf, int fd)
{
	if (buf->max == 0)	/* if buf lives on the stack */
		abort();	/* abort before causing more harm */
	if (buf->fd != -1)
		close(buf->fd);
	buf->fd = fd;
}

int
ibuf_write(struct msgbuf *msgbuf)
{
	struct iovec	 iov[IOV_MAX];
	struct ibuf	*buf;
	unsigned int	 i = 0;
	ssize_t	n;

	memset(&iov, 0, sizeof(iov));
	TAILQ_FOREACH(buf, &msgbuf->bufs, entry) {
		if (i >= IOV_MAX)
			break;
		iov[i].iov_base = ibuf_data(buf);
		iov[i].iov_len = ibuf_size(buf);
		i++;
	}

again:
	if ((n = writev(msgbuf->fd, iov, i)) == -1) {
		if (errno == EINTR)
			goto again;
		if (errno == ENOBUFS)
			errno = EAGAIN;
		return (-1);
	}

	if (n == 0) {			/* connection closed */
		errno = 0;
		return (0);
	}

	msgbuf_drain(msgbuf, n);

	return (1);
}

void
msgbuf_init(struct msgbuf *msgbuf)
{
	msgbuf->queued = 0;
	msgbuf->fd = -1;
	TAILQ_INIT(&msgbuf->bufs);
}

static void
msgbuf_drain(struct msgbuf *msgbuf, size_t n)
{
	struct ibuf	*buf, *next;

	for (buf = TAILQ_FIRST(&msgbuf->bufs); buf != NULL && n > 0;
	    buf = next) {
		next = TAILQ_NEXT(buf, entry);
		if (n >= ibuf_size(buf)) {
			n -= ibuf_size(buf);
			ibuf_dequeue(msgbuf, buf);
		} else {
			buf->rpos += n;
			n = 0;
		}
	}
}

void
msgbuf_clear(struct msgbuf *msgbuf)
{
	struct ibuf	*buf;

	while ((buf = TAILQ_FIRST(&msgbuf->bufs)) != NULL)
		ibuf_dequeue(msgbuf, buf);
}

int
msgbuf_write(struct msgbuf *msgbuf)
{
	struct iovec	 iov[IOV_MAX];
	struct ibuf	*buf, *buf0 = NULL;
	unsigned int	 i = 0;
	ssize_t		 n;
	struct msghdr	 msg;
	struct cmsghdr	*cmsg;
	union {
		struct cmsghdr	hdr;
		char		buf[CMSG_SPACE(sizeof(int))];
	} cmsgbuf;

	memset(&iov, 0, sizeof(iov));
	memset(&msg, 0, sizeof(msg));
	memset(&cmsgbuf, 0, sizeof(cmsgbuf));
	TAILQ_FOREACH(buf, &msgbuf->bufs, entry) {
		if (i >= IOV_MAX)
			break;
		if (i > 0 && buf->fd != -1)
			break;
		iov[i].iov_base = ibuf_data(buf);
		iov[i].iov_len = ibuf_size(buf);
		i++;
		if (buf->fd != -1)
			buf0 = buf;
	}

	msg.msg_iov = iov;
	msg.msg_iovlen = i;

	if (buf0 != NULL) {
		msg.msg_control = (caddr_t)&cmsgbuf.buf;
		msg.msg_controllen = sizeof(cmsgbuf.buf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		*(int *)CMSG_DATA(cmsg) = buf0->fd;
	}

again:
	if ((n = sendmsg(msgbuf->fd, &msg, 0)) == -1) {
		if (errno == EINTR)
			goto again;
		if (errno == ENOBUFS)
			errno = EAGAIN;
		return (-1);
	}

	if (n == 0) {			/* connection closed */
		errno = 0;
		return (0);
	}

	/*
	 * assumption: fd got sent if sendmsg sent anything
	 * this works because fds are passed one at a time
	 */
	if (buf0 != NULL) {
		close(buf0->fd);
		buf0->fd = -1;
	}

	msgbuf_drain(msgbuf, n);

	return (1);
}

uint32_t
msgbuf_queuelen(struct msgbuf *msgbuf)
{
	return (msgbuf->queued);
}

static void
ibuf_enqueue(struct msgbuf *msgbuf, struct ibuf *buf)
{
	if (buf->max == 0)	/* if buf lives on the stack */
		abort();	/* abort before causing more harm */
	TAILQ_INSERT_TAIL(&msgbuf->bufs, buf, entry);
	msgbuf->queued++;
}

static void
ibuf_dequeue(struct msgbuf *msgbuf, struct ibuf *buf)
{
	TAILQ_REMOVE(&msgbuf->bufs, buf, entry);
	msgbuf->queued--;
	ibuf_free(buf);
}

/* imsg.c */

struct imsg_fd {
	TAILQ_ENTRY(imsg_fd)	entry;
	int			fd;
};

int	 imsg_fd_overhead = 0;

static int	 imsg_dequeue_fd(struct imsgbuf *);

void
imsg_init(struct imsgbuf *imsgbuf, int fd)
{
	msgbuf_init(&imsgbuf->w);
	memset(&imsgbuf->r, 0, sizeof(imsgbuf->r));
	imsgbuf->fd = fd;
	imsgbuf->w.fd = fd;
	imsgbuf->pid = getpid();
	TAILQ_INIT(&imsgbuf->fds);
}

ssize_t
imsg_read(struct imsgbuf *imsgbuf)
{
	struct msghdr		 msg;
	struct cmsghdr		*cmsg;
	union {
		struct cmsghdr hdr;
		char	buf[CMSG_SPACE(sizeof(int) * 1)];
	} cmsgbuf;
	struct iovec		 iov;
	ssize_t			 n = -1;
	int			 fd;
	struct imsg_fd		*ifd;

	memset(&msg, 0, sizeof(msg));
	memset(&cmsgbuf, 0, sizeof(cmsgbuf));

	iov.iov_base = imsgbuf->r.buf + imsgbuf->r.wpos;
	iov.iov_len = sizeof(imsgbuf->r.buf) - imsgbuf->r.wpos;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &cmsgbuf.buf;
	msg.msg_controllen = sizeof(cmsgbuf.buf);

	if ((ifd = calloc(1, sizeof(struct imsg_fd))) == NULL)
		return (-1);

again:
	if (getdtablecount() + imsg_fd_overhead +
	    (int)((CMSG_SPACE(sizeof(int))-CMSG_SPACE(0))/sizeof(int))
	    >= getdtablesize()) {
		errno = EAGAIN;
		free(ifd);
		return (-1);
	}

	if ((n = recvmsg(imsgbuf->fd, &msg, 0)) == -1) {
		if (errno == EINTR)
			goto again;
		goto fail;
	}

	imsgbuf->r.wpos += n;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			int i;
			int j;

			/*
			 * We only accept one file descriptor.  Due to C
			 * padding rules, our control buffer might contain
			 * more than one fd, and we must close them.
			 */
			j = ((char *)cmsg + cmsg->cmsg_len -
			    (char *)CMSG_DATA(cmsg)) / sizeof(int);
			for (i = 0; i < j; i++) {
				fd = ((int *)CMSG_DATA(cmsg))[i];
				if (ifd != NULL) {
					ifd->fd = fd;
					TAILQ_INSERT_TAIL(&imsgbuf->fds, ifd,
					    entry);
					ifd = NULL;
				} else
					close(fd);
			}
		}
		/* we do not handle other ctl data level */
	}

fail:
	free(ifd);
	return (n);
}

ssize_t
imsg_get(struct imsgbuf *imsgbuf, struct imsg *imsg)
{
	struct imsg		 m;
	size_t			 av, left, datalen;

	av = imsgbuf->r.wpos;

	if (IMSG_HEADER_SIZE > av)
		return (0);

	memcpy(&m.hdr, imsgbuf->r.buf, sizeof(m.hdr));
	if (m.hdr.len < IMSG_HEADER_SIZE ||
	    m.hdr.len > MAX_IMSGSIZE) {
		errno = ERANGE;
		return (-1);
	}
	if (m.hdr.len > av)
		return (0);

	m.fd = -1;
	m.buf = NULL;
	m.data = NULL;

	datalen = m.hdr.len - IMSG_HEADER_SIZE;
	imsgbuf->r.rptr = imsgbuf->r.buf + IMSG_HEADER_SIZE;
	if (datalen != 0) {
		if ((m.buf = ibuf_open(datalen)) == NULL)
			return (-1);
		if (ibuf_add(m.buf, imsgbuf->r.rptr, datalen) == -1) {
			/* this should never fail */
			ibuf_free(m.buf);
			return (-1);
		}
		m.data = ibuf_data(m.buf);
	}

	if (m.hdr.flags & IMSGF_HASFD)
		m.fd = imsg_dequeue_fd(imsgbuf);

	if (m.hdr.len < av) {
		left = av - m.hdr.len;
		memmove(&imsgbuf->r.buf, imsgbuf->r.buf + m.hdr.len, left);
		imsgbuf->r.wpos = left;
	} else
		imsgbuf->r.wpos = 0;

	*imsg = m;
	return (datalen + IMSG_HEADER_SIZE);
}

int
imsg_get_ibuf(struct imsg *imsg, struct ibuf *ibuf)
{
	if (imsg->buf == NULL) {
		errno = EBADMSG;
		return (-1);
	}
	return ibuf_get_ibuf(imsg->buf, ibuf_size(imsg->buf), ibuf);
}

int
imsg_get_data(struct imsg *imsg, void *data, size_t len)
{
	if (len == 0) {
		errno = EINVAL;
		return (-1);
	}
	if (imsg->buf == NULL || ibuf_size(imsg->buf) != len) {
		errno = EBADMSG;
		return (-1);
	}
	return ibuf_get(imsg->buf, data, len);
}

int
imsg_get_fd(struct imsg *imsg)
{
	int fd = imsg->fd;

	imsg->fd = -1;
	return fd;
}

uint32_t
imsg_get_id(struct imsg *imsg)
{
	return (imsg->hdr.peerid);
}

size_t
imsg_get_len(struct imsg *imsg)
{
	if (imsg->buf == NULL)
		return 0;
	return ibuf_size(imsg->buf);
}

pid_t
imsg_get_pid(struct imsg *imsg)
{
	return (imsg->hdr.pid);
}

uint32_t
imsg_get_type(struct imsg *imsg)
{
	return (imsg->hdr.type);
}

int
imsg_compose(struct imsgbuf *imsgbuf, uint32_t type, uint32_t id, pid_t pid,
    int fd, const void *data, size_t datalen)
{
	struct ibuf	*wbuf;

	if ((wbuf = imsg_create(imsgbuf, type, id, pid, datalen)) == NULL)
		return (-1);

	if (imsg_add(wbuf, data, datalen) == -1)
		return (-1);

	ibuf_fd_set(wbuf, fd);
	imsg_close(imsgbuf, wbuf);

	return (1);
}

int
imsg_composev(struct imsgbuf *imsgbuf, uint32_t type, uint32_t id, pid_t pid,
    int fd, const struct iovec *iov, int iovcnt)
{
	struct ibuf	*wbuf;
	int		 i;
	size_t		 datalen = 0;

	for (i = 0; i < iovcnt; i++)
		datalen += iov[i].iov_len;

	if ((wbuf = imsg_create(imsgbuf, type, id, pid, datalen)) == NULL)
		return (-1);

	for (i = 0; i < iovcnt; i++)
		if (imsg_add(wbuf, iov[i].iov_base, iov[i].iov_len) == -1)
			return (-1);

	ibuf_fd_set(wbuf, fd);
	imsg_close(imsgbuf, wbuf);

	return (1);
}

/*
 * Enqueue imsg with payload from ibuf buf. fd passing is not possible 
 * with this function.
 */
int
imsg_compose_ibuf(struct imsgbuf *imsgbuf, uint32_t type, uint32_t id,
    pid_t pid, struct ibuf *buf)
{
	struct ibuf	*hdrbuf = NULL;
	struct imsg_hdr	 hdr;
	int save_errno;

	if (ibuf_size(buf) + IMSG_HEADER_SIZE > MAX_IMSGSIZE) {
		errno = ERANGE;
		goto fail;
	}

	hdr.type = type;
	hdr.len = ibuf_size(buf) + IMSG_HEADER_SIZE;
	hdr.flags = 0;
	hdr.peerid = id;
	if ((hdr.pid = pid) == 0)
		hdr.pid = imsgbuf->pid;

	if ((hdrbuf = ibuf_open(IMSG_HEADER_SIZE)) == NULL)
		goto fail;
	if (imsg_add(hdrbuf, &hdr, sizeof(hdr)) == -1)
		goto fail;

	ibuf_close(&imsgbuf->w, hdrbuf);
	ibuf_close(&imsgbuf->w, buf);
	return (1);

 fail:
	save_errno = errno;
	ibuf_free(buf);
	ibuf_free(hdrbuf);
	errno = save_errno;
	return (-1);
}

/*
 * Forward imsg to another channel. Any attached fd is closed.
 */
int
imsg_forward(struct imsgbuf *imsgbuf, struct imsg *msg)
{
	struct ibuf	*wbuf;
	size_t		 len = 0;

	if (msg->fd != -1) {
		close(msg->fd);
		msg->fd = -1;
	}

	if (msg->buf != NULL) {
		ibuf_rewind(msg->buf);
		len = ibuf_size(msg->buf);
	}

	if ((wbuf = imsg_create(imsgbuf, msg->hdr.type, msg->hdr.peerid,
	    msg->hdr.pid, len)) == NULL)
		return (-1);

	if (msg->buf != NULL) {
		if (ibuf_add_buf(wbuf, msg->buf) == -1) {
			ibuf_free(wbuf);
			return (-1);
		}
	}

	imsg_close(imsgbuf, wbuf);
	return (1);
}

struct ibuf *
imsg_create(struct imsgbuf *imsgbuf, uint32_t type, uint32_t id, pid_t pid,
    size_t datalen)
{
	struct ibuf	*wbuf;
	struct imsg_hdr	 hdr;

	datalen += IMSG_HEADER_SIZE;
	if (datalen > MAX_IMSGSIZE) {
		errno = ERANGE;
		return (NULL);
	}

	hdr.type = type;
	hdr.flags = 0;
	hdr.peerid = id;
	if ((hdr.pid = pid) == 0)
		hdr.pid = imsgbuf->pid;
	if ((wbuf = ibuf_dynamic(datalen, MAX_IMSGSIZE)) == NULL) {
		return (NULL);
	}
	if (imsg_add(wbuf, &hdr, sizeof(hdr)) == -1)
		return (NULL);

	return (wbuf);
}

int
imsg_add(struct ibuf *msg, const void *data, size_t datalen)
{
	if (datalen)
		if (ibuf_add(msg, data, datalen) == -1) {
			ibuf_free(msg);
			return (-1);
		}
	return (datalen);
}

void
imsg_close(struct imsgbuf *imsgbuf, struct ibuf *msg)
{
	struct imsg_hdr	*hdr;

	hdr = (struct imsg_hdr *)msg->buf;

	hdr->flags &= ~IMSGF_HASFD;
	if (ibuf_fd_avail(msg))
		hdr->flags |= IMSGF_HASFD;
	hdr->len = ibuf_size(msg);

	ibuf_close(&imsgbuf->w, msg);
}

void
imsg_free(struct imsg *imsg)
{
	ibuf_free(imsg->buf);
}

static int
imsg_dequeue_fd(struct imsgbuf *imsgbuf)
{
	int		 fd;
	struct imsg_fd	*ifd;

	if ((ifd = TAILQ_FIRST(&imsgbuf->fds)) == NULL)
		return (-1);

	fd = ifd->fd;
	TAILQ_REMOVE(&imsgbuf->fds, ifd, entry);
	free(ifd);

	return (fd);
}

int
imsg_flush(struct imsgbuf *imsgbuf)
{
	while (imsgbuf->w.queued)
		if (msgbuf_write(&imsgbuf->w) <= 0)
			return (-1);
	return (0);
}

void
imsg_clear(struct imsgbuf *imsgbuf)
{
	int	fd;

	msgbuf_clear(&imsgbuf->w);
	while ((fd = imsg_dequeue_fd(imsgbuf)) != -1)
		close(fd);
}
#endif /* HAVE_LIB_IMSG */
#if !HAVE_MEMMEM
/*-
 * Copyright (c) 2005 Pascal Gloor <pascal.gloor@spale.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Find the first occurrence of the byte string s in byte string l.
 */
void *
memmem(const void *l, size_t l_len, const void *s, size_t s_len)
{
	const char *cur, *last;
	const char *cl = l;
	const char *cs = s;

	/* a zero length needle should just return the haystack */
	if (l_len == 0)
		return (void *)cl;

	/* "s" must be smaller or equal to "l" */
	if (l_len < s_len)
		return NULL;

	/* special case where s_len == 1 */
	if (s_len == 1)
		return memchr(l, *cs, l_len);

	/* the last position where its possible to find "s" in "l" */
	last = cl + l_len - s_len;

	for (cur = cl; cur <= last; cur++)
		if (cur[0] == cs[0] && memcmp(cur, cs, s_len) == 0)
			return (void *)cur;

	return NULL;
}
#endif /* !HAVE_MEMMEM */
#if !HAVE_MEMRCHR
/*
 * Copyright (c) 2007 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <string.h>

/*
 * Reverse memchr()
 * Find the last occurrence of 'c' in the buffer 's' of size 'n'.
 */
void *
memrchr(const void *s, int c, size_t n)
{
    const unsigned char *cp;

    if (n != 0) {
        cp = (unsigned char *)s + n;
        do {
            if (*(--cp) == (unsigned char)c)
                return((void *)cp);
        } while (--n != 0);
    }
    return(NULL);
}
#endif /* !HAVE_MEMRCHR */
#if !HAVE_OPTRESET
/*
 * Copyright (c) 1987, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* OPENBSD ORIGINAL: lib/libc/stdlib/getopt.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int	BSDopterr = 1,		/* if error message should be printed */
	BSDoptind = 1,		/* index into parent argv vector */
	BSDoptopt,		/* character checked for validity */
	BSDoptreset;		/* reset getopt */
char	*BSDoptarg;		/* argument associated with option */

#define	BADCH	(int)'?'
#define	BADARG	(int)':'
#define	EMSG	""

/*
 * getopt --
 *	Parse argc/argv argument vector.
 */
int
BSDgetopt(int nargc, char *const *nargv, const char *ostr)
{
	static const char *place = EMSG;	/* option letter processing */
	char *oli;				/* option letter list index */

	if (ostr == NULL)
		return (-1);

	if (BSDoptreset || !*place) {		/* update scanning pointer */
		BSDoptreset = 0;
		if (BSDoptind >= nargc || *(place = nargv[BSDoptind]) != '-') {
			place = EMSG;
			return (-1);
		}
		if (place[1] && *++place == '-') {	/* found "--" */
			if (place[1])
				return (BADCH);
			++BSDoptind;
			place = EMSG;
			return (-1);
		}
	}					/* option letter okay? */
	if ((BSDoptopt = (int)*place++) == (int)':' ||
	    !(oli = strchr(ostr, BSDoptopt))) {
		/*
		 * if the user didn't specify '-' as an option,
		 * assume it means -1.
		 */
		if (BSDoptopt == (int)'-')
			return (-1);
		if (!*place)
			++BSDoptind;
		if (BSDopterr && *ostr != ':')
			(void)fprintf(stderr,
			    "%s: unknown option -- %c\n", getprogname(),
			    BSDoptopt);
		return (BADCH);
	}
	if (*++oli != ':') {			/* don't need argument */
		BSDoptarg = NULL;
		if (!*place)
			++BSDoptind;
	}
	else {					/* need an argument */
		if (*place)			/* no white space */
			BSDoptarg = (char *)place;
		else if (nargc <= ++BSDoptind) {	/* no arg */
			place = EMSG;
			if (*ostr == ':')
				return (BADARG);
			if (BSDopterr)
				(void)fprintf(stderr,
				    "%s: option requires an argument -- %c\n",
				    getprogname(), BSDoptopt);
			return (BADCH);
		}
		else				/* white space */
			BSDoptarg = nargv[BSDoptind];
		place = EMSG;
		++BSDoptind;
	}
	return (BSDoptopt);			/* dump back option letter */
}
#endif
#if !HAVE_REALLOCARRAY
/*
 * Copyright (c) 2008 Otto Moerbeek <otto@drijf.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * This is sqrt(SIZE_MAX+1), as s1*s2 <= SIZE_MAX
 * if both s1 < MUL_NO_OVERFLOW and s2 < MUL_NO_OVERFLOW
 */
#define MUL_NO_OVERFLOW	((size_t)1 << (sizeof(size_t) * 4))

void *
reallocarray(void *optr, size_t nmemb, size_t size)
{
	if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    nmemb > 0 && SIZE_MAX / nmemb < size) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(optr, size * nmemb);
}
#endif /* !HAVE_REALLOCARRAY */
#if !HAVE_RECALLOCARRAY
/*
 * Copyright (c) 2008, 2017 Otto Moerbeek <otto@drijf.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* OPENBSD ORIGINAL: lib/libc/stdlib/recallocarray.c */

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
 * This is sqrt(SIZE_MAX+1), as s1*s2 <= SIZE_MAX
 * if both s1 < MUL_NO_OVERFLOW and s2 < MUL_NO_OVERFLOW
 */
#define MUL_NO_OVERFLOW ((size_t)1 << (sizeof(size_t) * 4))

void *
recallocarray(void *ptr, size_t oldnmemb, size_t newnmemb, size_t size)
{
	size_t oldsize, newsize;
	void *newptr;

	if (ptr == NULL)
		return calloc(newnmemb, size);

	if ((newnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    newnmemb > 0 && SIZE_MAX / newnmemb < size) {
		errno = ENOMEM;
		return NULL;
	}
	newsize = newnmemb * size;

	if ((oldnmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    oldnmemb > 0 && SIZE_MAX / oldnmemb < size) {
		errno = EINVAL;
		return NULL;
	}
	oldsize = oldnmemb * size;
	
	/*
	 * Don't bother too much if we're shrinking just a bit,
	 * we do not shrink for series of small steps, oh well.
	 */
	if (newsize <= oldsize) {
		size_t d = oldsize - newsize;

		if (d < oldsize / 2 && d < (size_t)getpagesize()) {
			memset((char *)ptr + newsize, 0, d);
			return ptr;
		}
	}

	newptr = malloc(newsize);
	if (newptr == NULL)
		return NULL;

	if (newsize > oldsize) {
		memcpy(newptr, ptr, oldsize);
		memset((char *)newptr + oldsize, 0, newsize - oldsize);
	} else
		memcpy(newptr, ptr, newsize);

	explicit_bzero(ptr, oldsize);
	free(ptr);

	return newptr;
}
#endif /* !HAVE_RECALLOCARRAY */
#if !HAVE_SETPROCTITLE
/*
 * Copyright (c) 2016 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

# if HAVE_PR_SET_NAME
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
void
setproctitle(const char *fmt, ...)
{
	char	title[16], name[16], *cp;
	va_list	ap;
	int	used;

	va_start(ap, fmt);
	vsnprintf(title, sizeof(title), fmt, ap);
	va_end(ap);

	used = snprintf(name, sizeof(name), "%s: %s", getprogname(), title);
	if (used >= (int)sizeof(name)) {
		cp = strrchr(name, ' ');
		if (cp != NULL)
			*cp = '\0';
	}
	prctl(PR_SET_NAME, name);
}
# else
void
setproctitle(const char *fmt, ...)
{
	return;
}
# endif
#endif /* !HAVE_SETPROCTITLE */
#if !HAVE_STRLCAT
/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>

/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left).  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
size_t
strlcat(char *dst, const char *src, size_t siz)
{
	char *d = dst;
	const char *s = src;
	size_t n = siz;
	size_t dlen;

	/* Find the end of dst and adjust bytes left but don't go past end */
	while (n-- != 0 && *d != '\0')
		d++;
	dlen = d - dst;
	n = siz - dlen;

	if (n == 0)
		return(dlen + strlen(s));
	while (*s != '\0') {
		if (n != 1) {
			*d++ = *s;
			n--;
		}
		s++;
	}
	*d = '\0';

	return(dlen + (s - src));	/* count does not include NUL */
}
#endif /* !HAVE_STRLCAT */
#if !HAVE_STRLCPY
/*
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>

/*
 * Copy src to string dst of size siz.  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t
strlcpy(char *dst, const char *src, size_t siz)
{
	char *d = dst;
	const char *s = src;
	size_t n = siz;

	/* Copy as many bytes as will fit */
	if (n != 0) {
		while (--n != 0) {
			if ((*d++ = *s++) == '\0')
				break;
		}
	}

	/* Not enough room in dst, add NUL and traverse rest of src */
	if (n == 0) {
		if (siz != 0)
			*d = '\0';		/* NUL-terminate dst */
		while (*s++)
			;
	}

	return(s - src - 1);	/* count does not include NUL */
}
#endif /* !HAVE_STRLCPY */
#if !HAVE_STRNDUP
/*	$OpenBSD$	*/
/*
 * Copyright (c) 2010 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char *
strndup(const char *str, size_t maxlen)
{
	char *copy;
	size_t len;

	len = strnlen(str, maxlen);
	copy = malloc(len + 1);
	if (copy != NULL) {
		(void)memcpy(copy, str, len);
		copy[len] = '\0';
	}

	return copy;
}
#endif /* !HAVE_STRNDUP */
#if !HAVE_STRNLEN
/*	$OpenBSD$	*/

/*
 * Copyright (c) 2010 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>

size_t
strnlen(const char *str, size_t maxlen)
{
	const char *cp;

	for (cp = str; maxlen != 0 && *cp != '\0'; cp++, maxlen--)
		;

	return (size_t)(cp - str);
}
#endif /* !HAVE_STRNLEN */
#if !HAVE_STRTONUM
/*
 * Copyright (c) 2004 Ted Unangst and Todd Miller
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define	INVALID		1
#define	TOOSMALL	2
#define	TOOLARGE	3

long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	long long ll = 0;
	int error = 0;
	char *ep;
	struct errval {
		const char *errstr;
		int err;
	} ev[4] = {
		{ NULL,		0 },
		{ "invalid",	EINVAL },
		{ "too small",	ERANGE },
		{ "too large",	ERANGE },
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval) {
		error = INVALID;
	} else {
		ll = strtoll(numstr, &ep, 10);
		if (numstr == ep || *ep != '\0')
			error = INVALID;
		else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
			error = TOOSMALL;
		else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
			error = TOOLARGE;
	}
	if (errstrp != NULL)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;

	return (ll);
}
#endif /* !HAVE_STRTONUM */
#if !HAVE_TIMESPECSUB
#include <sys/time.h>
void
timespecsub(struct timespec *a, struct timespec *b, struct timespec *ret)
{
	ret->tv_sec = a->tv_sec - b->tv_sec;
	ret->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (ret->tv_nsec < 0) {
		ret->tv_sec--;
		ret->tv_nsec += 1000000000L;
	}
}
#endif /* !HAVE_TIMESPECSUB */
