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

enum http_method {
	METHOD_UNKNOWN,
	METHOD_GET,
	METHOD_POST,
};

struct request {
	char	buf[BUFSIZ];
	size_t	len;

	char	*path;
	int	 method;
	char	*ctype;
	size_t	 clen;
};

struct reswriter {
	int	fd;
	int	err;
	int	chunked;
	char	buf[BUFSIZ];
	size_t	len;
};

int	http_parse(struct request *, int);
int	http_read(struct request *, int);
void	http_response_init(struct reswriter *, int);
int	http_reply(struct reswriter *, int, const char *, const char *);
int	http_flush(struct reswriter *);
int	http_write(struct reswriter *, const char *, size_t);
int	http_writes(struct reswriter *, const char *);
int	http_fmt(struct reswriter *, const char *, ...);
int	http_urlescape(struct reswriter *, const char *);
int	http_htmlescape(struct reswriter *, const char *);
int	http_close(struct reswriter *);
void	http_free_request(struct request *);
