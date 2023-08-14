/*
 * Copyright (c) 2023 Omar Polo <op@omarpolo.com>
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

#include "config.h"

#include <sys/uio.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http.h"
#include "log.h"
#include "xmalloc.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

static int
writeall(struct reswriter *res, const char *buf, size_t buflen)
{
	ssize_t	 nw;
	size_t	 off;

	for (off = 0; off < buflen; off += nw)
		if ((nw = write(res->fd, buf + off, buflen - off)) == 0 ||
		    nw == -1) {
			if (nw == 0)
				log_warnx("Unexpected EOF");
			else
				log_warn("write");
			res->err = 1;
			return -1;
		}

	return 0;
}

int
http_parse(struct request *req, int fd)
{
	ssize_t		 nr;
	size_t		 avail, len;
	int		 done = 0, first = 1;
	char		*s, *t, *line, *endln;
	const char	*errstr, *m;

	memset(req, 0, sizeof(*req));

	while (!done) {
		if (req->len == sizeof(req->buf)) {
			log_warnx("not enough space");
			return -1;
		}

		avail = sizeof(req->buf) - req->len;
		nr = read(fd, req->buf + req->len, avail);
		if (nr <= 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			if (nr == 0)
				log_warnx("Unexpected EOF");
			else
				log_warn("read");
			return -1;
		}
		req->len += nr;

		while ((endln = memmem(req->buf, req->len, "\r\n", 2))) {
			line = req->buf;
			if (endln == req->buf)
				done = 1;

			len = endln - req->buf + 2;
			while (len > 0 && (line[len - 1] == '\r' ||
			    line[len - 1] == '\n' || line[len - 1] == ' ' ||
			    line[len - 1] == '\t'))
				line[--len] = '\0';

			if (first) {
				first = 0;
				if (!strncmp("GET ", line, 4)) {
					req->method = METHOD_GET;
					s = line + 4;
				} else if (!strncmp("POST ", line, 5)) {
					req->method = METHOD_POST;
					s = line + 5;
				}

				t = strchr(s, ' ');
				if (t == NULL)
					t = s;
				if (*t != '\0')
					*t++ = '\0';
				req->path = xstrdup(s);
				if (strcmp("HTTP/1.0", t) != 0 &&
				    strcmp("HTTP/1.1", t) != 0) {
					log_warnx("unknown http version: %s",
					    t);
					return -1;
				}
			}

			if (!strncasecmp(line, "Content-Length:", 15)) {
				line += 15;
				line += strspn(line, " \t");
				req->clen = strtonum(line, 0, LONG_MAX,
				    &errstr);
				if (errstr != NULL) {
					log_warnx("content-length is %s: %s",
					    errstr, line);
					return -1;
				}
			}

			len = endln - req->buf + 2;
			memmove(req->buf, req->buf + len, req->len - len);
			req->len -= len;
			if (done)
				break;
		}
	}

	if (req->method == METHOD_GET)
		m = "GET";
	else if (req->method == METHOD_POST)
		m = "POST";
	else
		m = "unknown";
	log_debug("< %s %s", m, req->path);

	return 0;
}

int
http_read(struct request *req, int fd)
{
	size_t	 left;
	ssize_t	 nr;

	/* drop \r\n */
	if (req->len > 2)
		req->len -= 2;

	if (req->clen > sizeof(req->buf) - 1)
		return -1;
	if (req->len == req->clen) {
		req->buf[req->len] = '\0';
		return 0;
	}
	if (req->len > req->clen) {
		log_warnx("got more data than what advertised! (%zu vs %zu)",
		    req->len, req->clen);
		return -1;
	}

	left = req->clen - req->len;
	while (left > 0) {
		nr = read(fd, req->buf + req->len, left);
		if (nr <= 0) {
			if (nr == -1 && errno == EAGAIN)
				continue;
			if (nr == 0)
				log_warnx("Unexpected EOF");
			else
				log_warn("read");
			return -1;
		}
		req->len += nr;
		left -= nr;
	}

	req->buf[req->len] = '\0';
	return 0;
}

void
http_response_init(struct reswriter *res, int fd)
{
	memset(res, 0, sizeof(*res));
	res->fd = fd;
}

int
http_reply(struct reswriter *res, int code, const char *reason,
    const char *ctype)
{
	const char	*location = NULL;
	int		 r;

	res->len = 0;	/* discard any leftover from reading */
	res->chunked = ctype != NULL;

	log_debug("> %d %s", code, reason);

	if (code >= 300 && code < 400) {
		location = ctype;
		ctype = NULL;
	}

	r = snprintf(res->buf, sizeof(res->buf), "HTTP/1.1 %d %s\r\n"
	    "Connection: close\r\n"
	    "Cache-Control: no-store\r\n"
	    "%s%s%s"
	    "%s%s%s"
	    "%s"
	    "\r\n",
	    code, reason,
	    ctype == NULL ? "" : "Content-Type: ",
	    ctype == NULL ? "" : ctype,
	    ctype == NULL ? "" : "\r\n",
	    location == NULL ? "" : "Location: ",
	    location == NULL ? "" : location,
	    location == NULL ? "" : "\r\n",
	    ctype == NULL ? "" : "Transfer-Encoding: chunked\r\n");
	if (r < 0 || (size_t)r >= sizeof(res->buf))
		return -1;

	return writeall(res, res->buf, r);
}

int
http_flush(struct reswriter *res)
{
	struct iovec	 iov[3];
	char		 buf[64];
	ssize_t		 nw;
	size_t		 i, tot;
	int		 r;

	if (res->err)
		return -1;

	if (res->len == 0)
		return 0;

	r = snprintf(buf, sizeof(buf), "%zx\r\n", res->len);
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf failed");
		res->err = 1;
		return -1;
	}

	memset(iov, 0, sizeof(iov));

	iov[0].iov_base = buf;
	iov[0].iov_len = r;

	iov[1].iov_base = res->buf;
	iov[1].iov_len = res->len;

	iov[2].iov_base = "\r\n";
	iov[2].iov_len = 2;

	tot = iov[0].iov_len + iov[1].iov_len + iov[2].iov_len;
	while (tot > 0) {
		nw = writev(res->fd, iov, nitems(iov));
		if (nw <= 0) {
			if (nw == -1 && errno == EAGAIN)
				continue;
			if (nw == 0)
				log_warnx("Unexpected EOF");
			else
				log_warn("writev");
			res->err = 1;
			return -1;
		}

		tot -= nw;
		for (i = 0; i < nitems(iov); ++i) {
			if (nw < iov[i].iov_len) {
				iov[i].iov_base += nw;
				iov[i].iov_len -= nw;
				break;
			}
			nw -= iov[i].iov_len;
			iov[i].iov_len = 0;
		}
	}

	res->len = 0;
	return 0;
}

int
http_write(struct reswriter *res, const char *d, size_t len)
{
	size_t		avail;

	if (res->err)
		return -1;

	while (len > 0) {
		avail = sizeof(res->buf) - res->len;
		if (avail > len)
			avail = len;

		memcpy(res->buf + res->len, d, avail);
		res->len += avail;
		len -= avail;
		d += avail;
		if (res->len == sizeof(res->buf)) {
			if (http_flush(res) == -1)
				return -1;
		}
	}

	return 0;
}

int
http_writes(struct reswriter *res, const char *str)
{
	return http_write(res, str, strlen(str));
}

int
http_fmt(struct reswriter *res, const char *fmt, ...)
{
	va_list	 ap;
	char	*str;
	int	 r;

	va_start(ap, fmt);
	r = vasprintf(&str, fmt, ap);
	va_end(ap);

	if (r == -1) {
		log_warn("vasprintf");
		res->err = 1;
		return -1;
	}

	r = http_write(res, str, r);
	free(str);
	return r;
}

int
http_urlescape(struct reswriter *res, const char *str)
{
	int	 r;
	char	 tmp[4];

	for (; *str; ++str) {
		if (iscntrl((unsigned char)*str) ||
		    isspace((unsigned char)*str) ||
		    *str == '\'' || *str == '"' || *str == '\\') {
			r = snprintf(tmp, sizeof(tmp), "%%%2X",
			    (unsigned char)*str);
			if (r < 0 || (size_t)r >= sizeof(tmp)) {
				log_warn("snprintf failed");
				res->err = 1;
				return -1;
			}
			if (http_write(res, tmp, r) == -1)
				return -1;
		} else if (http_write(res, str, 1) == -1)
			return -1;
	}

	return 0;
}

int
http_htmlescape(struct reswriter *res, const char *str)
{
	int r;

	for (; *str; ++str) {
		switch (*str) {
		case '<':
			r = http_writes(res, "&lt;");
			break;
		case '>':
			r = http_writes(res, "&gt;");
			break;
		case '&':
			r = http_writes(res, "&gt;");
			break;
		case '"':
			r = http_writes(res, "&quot;");
			break;
		case '\'':
			r = http_writes(res, "&apos;");
			break;
		default:
			r = http_write(res, str, 1);
			break;
		}

		if (r == -1)
			return -1;
	}

	return 0;
}

int
http_close(struct reswriter *res)
{
	if (!res->chunked)
		return 0;

	return writeall(res, "0\r\n\r\n", 5);
}

void
http_free_request(struct request *req)
{
	free(req->path);
	free(req->ctype);
}
