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

enum http_version {
	HTTP_1_0,
	HTTP_1_1,
};

struct bufio;

struct request {
	char	*path;
	int	 method;
	int	 version;
	char	*ctype;
	char	*body;
	size_t	 clen;
};

struct client;
typedef void (*route_fn)(struct client *);

struct client {
	char		buf[1024];
	size_t		len;
	struct bufio	bio;
	struct request	req;
	int		err;
	int		chunked;
	int		reqdone;	/* done parsing the request */
	int		done;		/* done handling the client */
	route_fn	route;
};

int	http_init(struct client *, int);
int	http_parse(struct client *);
int	http_read(struct client *);
int	http_reply(struct client *, int, const char *, const char *);
int	http_flush(struct client *);
int	http_write(struct client *, const char *, size_t);
int	http_writes(struct client *, const char *);
int	http_fmt(struct client *, const char *, ...);
int	http_urlescape(struct client *, const char *);
int	http_htmlescape(struct client *, const char *);
int	http_close(struct client *);
void	http_free(struct client *);
