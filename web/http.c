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

#include <sys/uio.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufio.h"
#include "http.h"
#include "log.h"
#include "ws.h"
#include "xmalloc.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

int
http_init(struct client *clt, int fd)
{
	memset(clt, 0, sizeof(*clt));
	if (bufio_init(&clt->bio) == -1)
		return -1;
	bufio_set_fd(&clt->bio, fd);
	return 0;
}

int
http_parse(struct client *clt)
{
	struct buffer	*rbuf = &clt->bio.rbuf;
	struct request	*req = &clt->req;
	size_t		 len;
	uint8_t		*endln;
	char		*frag, *query, *http, *line;
	const char	*errstr, *m;

	while (!clt->reqdone) {
		endln = memmem(rbuf->buf, rbuf->len, "\r\n", 2);
		if (endln == NULL) {
			errno = EAGAIN;
			return -1;
		}

		line = rbuf->buf;
		if (endln == rbuf->buf)
			clt->reqdone = 1;

		len = endln - rbuf->buf + 2;
		while (len > 0 && (line[len - 1] == '\r' ||
		    line[len - 1] == '\n' || line[len - 1] == ' ' ||
		    line[len - 1] == '\t'))
			line[--len] = '\0';

		/* first line */
		if (clt->req.method == METHOD_UNKNOWN) {
			if (!strncmp("GET ", line, 4)) {
				req->method = METHOD_GET;
				line += 4;
			} else if (!strncmp("POST ", line, 5)) {
				req->method = METHOD_POST;
				line += 5;
			} else {
				errno = EINVAL;
				return -1;
			}

			if ((http = strchr(line, ' ')) == NULL)
				http = line;
			if (*http != '\0')
				*http++ = '\0';

			if ((query = strchr(line, '?')))
				*query = '\0';
			if ((frag = strchr(line, '#')))
				*frag = '\0';

			clt->req.path = xstrdup(line);

			if (!strcmp(http, "HTTP/1.0"))
				clt->req.version = HTTP_1_0;
			else if (!strcmp(http, "HTTP/1.1")) {
				clt->req.version = HTTP_1_1;
				clt->chunked = 1;
			} else {
				log_warnx("unknown http version %s", http);
				errno = EINVAL;
				return -1;
			}

			line = http;	/* so that no header below matches */
		}

		if (!strncasecmp(line, "Content-Length:", 15)) {
			line += 15;
			line += strspn(line, " \t");
			clt->req.clen = strtonum(line, 0, LONG_MAX,
			    &errstr);
			if (errstr) {
				log_warnx("content-length is %s: %s",
				    errstr, line);
				errno = EINVAL;
				return -1;
			}
		}

		if (!strncasecmp(line, "Connection:", 11)) {
			line += 11;
			line += strspn(line, " \t");
			if (!strcasecmp(line, "upgrade"))
				req->flags |= R_CONNUPGR;
		}

		if (!strncasecmp(line, "Upgrade:", 8)) {
			line += 8;
			line += strspn(line, " \t");
			if (!strcasecmp(line, "websocket"))
				req->flags |= R_UPGRADEWS;
		}

		if (!strncasecmp(line, "Sec-WebSocket-Version:", 22)) {
			line += 22;
			line += strspn(line, " \t");
			if (strcmp(line, "13") != 0) {
				log_warnx("unsupported websocket version %s",
				    line);
				errno = EINVAL;
				return -1;
			}
			req->flags |= R_WSVERSION;
		}

		if (!strncasecmp(line, "Sec-WebSocket-Key:", 18)) {
			line += 18;
			line += strspn(line, " \t");
			req->secret = xstrdup(line);
		}

		buf_drain(rbuf, endln - rbuf->buf + 2);
	}

	if (req->method == METHOD_GET)
		m = "GET";
	else if (req->method == METHOD_POST)
		m = "POST";
	else
		m = "unknown";
	log_debug("< %s %s HTTP/%s", m, req->path,
	    req->version == HTTP_1_1 ? "1.1" : "1.0");

	return 0;
}

int
http_read(struct client *clt)
{
	struct request *req = &clt->req;
	size_t	 left;
	size_t	 nr;

	if (req->clen > sizeof(clt->buf) - 1) {
		log_warnx("POST has more data then what can be accepted");
		return -1;
	}

	/* clients may have sent more data than advertised */
	if (req->clen < clt->len)
		left = 0;
	else
		left = req->clen - clt->len;

	if (left > 0) {
		nr = bufio_drain(&clt->bio, clt->buf + clt->len, left);
		clt->len += nr;
		if (nr < left) {
			errno = EAGAIN;
			return -1;
		}
	}

	clt->buf[clt->len] = '\0';
	while (clt->len > 0 && (clt->buf[clt->len - 1] == '\r' ||
	    (clt->buf[clt->len - 1] == '\n')))
		clt->buf[--clt->len] = '\0';

	return 0;
}

int
http_reply(struct client *clt, int code, const char *reason,
    const char *ctype)
{
	const char	*version, *location = NULL;
	char		 b32[32] = "";

	log_debug("> %d %s", code, reason);

	if (code == 101) {
		if (ws_accept_hdr(clt->req.secret, b32, sizeof(b32)) == -1) {
			clt->err = 1;
			return -1;
		}
		clt->chunked = 0;
	}

	if (code >= 300 && code < 400) {
		location = ctype;
		ctype = "text/html;charset=UTF-8";
	}

	version = "HTTP/1.1";
	if (clt->req.version == HTTP_1_0)
		version = "HTTP/1.0";

	if (bufio_compose_fmt(&clt->bio, "%s %d %s\r\n"
	    "Connection: close\r\n"
	    "Cache-Control: no-store\r\n",
	    version, code, reason) == -1)
		goto err;
	if (ctype != NULL &&
	    bufio_compose_fmt(&clt->bio, "Content-Type: %s\r\n", ctype) == -1)
		goto err;
	if (location != NULL &&
	    bufio_compose_fmt(&clt->bio, "Location: %s\r\n", location) == -1)
		goto err;
	if (clt->chunked && bufio_compose_str(&clt->bio,
	    "Transfer-Encoding: chunked\r\n") == -1)
		goto err;
	if (code == 101) {
		if (bufio_compose_fmt(&clt->bio, "Upgrade: websocket\r\n"
		    "Connection: Upgrade\r\n"
		    "Sec-WebSocket-Accept: %s\r\n", b32) == -1)
			goto err;
	}
	if (bufio_compose(&clt->bio, "\r\n", 2) == -1)
		goto err;

	bufio_set_chunked(&clt->bio, clt->chunked);

	if (location) {
		if (http_writes(clt, "<a href='") == -1 ||
		    http_htmlescape(clt, location) == -1 ||
		    http_writes(clt, "'>") == -1 ||
		    http_htmlescape(clt, reason) == -1 ||
		    http_writes(clt, "</a>") == -1)
			return -1;
	}

	return 0;

 err:
	clt->err = 1;
	return -1;
}

int
http_flush(struct client *clt)
{
	if (clt->err)
		return -1;

	if (clt->len == 0)
		return 0;

	if (bufio_compose(&clt->bio, clt->buf, clt->len) == -1) {
		clt->err = 1;
		return -1;
	}

	clt->len = 0;

	return 0;
}

int
http_write(struct client *clt, const char *d, size_t len)
{
	size_t		avail;

	if (clt->err)
		return -1;

	while (len > 0) {
		avail = sizeof(clt->buf) - clt->len;
		if (avail > len)
			avail = len;

		memcpy(clt->buf + clt->len, d, avail);
		clt->len += avail;
		len -= avail;
		d += avail;
		if (clt->len == sizeof(clt->buf)) {
			if (http_flush(clt) == -1)
				return -1;
		}
	}

	return 0;
}

int
http_writes(struct client *clt, const char *str)
{
	return http_write(clt, str, strlen(str));
}

int
http_fmt(struct client *clt, const char *fmt, ...)
{
	va_list	 ap;
	char	*str;
	int	 r;

	va_start(ap, fmt);
	r = vasprintf(&str, fmt, ap);
	va_end(ap);

	if (r == -1) {
		log_warn("vasprintf");
		clt->err = 1;
		return -1;
	}

	r = http_write(clt, str, r);
	free(str);
	return r;
}

int
http_urlescape(struct client *clt, const char *str)
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
				clt->err = 1;
				return -1;
			}
			if (http_write(clt, tmp, r) == -1)
				return -1;
		} else if (http_write(clt, str, 1) == -1)
			return -1;
	}

	return 0;
}

int
http_htmlescape(struct client *clt, const char *str)
{
	int r;

	for (; *str; ++str) {
		switch (*str) {
		case '<':
			r = http_writes(clt, "&lt;");
			break;
		case '>':
			r = http_writes(clt, "&gt;");
			break;
		case '&':
			r = http_writes(clt, "&gt;");
			break;
		case '"':
			r = http_writes(clt, "&quot;");
			break;
		case '\'':
			r = http_writes(clt, "&apos;");
			break;
		default:
			r = http_write(clt, str, 1);
			break;
		}

		if (r == -1)
			return -1;
	}

	return 0;
}

int
http_close(struct client *clt)
{
	if (clt->err)
		return -1;
	if (clt->len != 0 && http_flush(clt) == -1)
		return -1;
	if (bufio_compose(&clt->bio, NULL, 0) == -1)
		clt->err = 1;
	return (clt->err ? -1 : 0);
}

void
http_free(struct client *clt)
{
	free(clt->req.path);
	free(clt->req.secret);
	free(clt->req.ctype);
	free(clt->req.body);
	bufio_free(&clt->bio);
}
