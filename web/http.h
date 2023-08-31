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
