/*
 * Copyright (c) 2023 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2014 Reyk Floeter <reyk@openbsd.org>
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

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "bufio.h"
#include "ev.h"
#include "http.h"
#include "log.h"
#include "playlist.h"
#include "xmalloc.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

#define FORM_URLENCODED		"application/x-www-form-urlencoded"

#define ICON_REPEAT_ALL		"🔁"
#define ICON_REPEAT_ONE		"🔂"
#define ICON_PREV		"⏮"
#define ICON_NEXT		"⏭"
#define ICON_STOP		"⏹"
#define ICON_PAUSE		"⏸"
#define ICON_TOGGLE		"⏯"
#define ICON_PLAY		"⏵"

static struct imsgbuf		 ibuf;
static const char		*prefix = "";
static size_t			 prefixlen;

const char *head = "<!doctype html>"
	"<html>"
	"<head>"
	"<meta name='viewport' content='width=device-width, initial-scale=1'/>"
	"<title>Amused Web</title>"
	"<link rel='stylesheet' href='/style.css?v=0'>"
	"</style>"
	"</head>"
	"<body>";

const char *css = 	"*{box-sizing:border-box}"
	"html,body{"
	" padding: 0;"
	" border: 0;"
	" margin: 0;"
	"}"
	"main{"
	" display: flex;"
	" flex-direction: column;"
	"}"
	"button{cursor:pointer}"
	".searchbox{"
	" position: sticky;"
	" top: 0;"
	"}"
	".searchbox input{"
	" width: 100%;"
	" padding: 9px;"
	"}"
	".playlist-wrapper{min-height:80vh}"
	".playlist{"
	" list-style: none;"
	" padding: 0;"
	" margin: 0;"
	"}"
	".playlist button{"
	" font-family: monospace;"
	" text-align: left;"
	" width: 100%;"
	" padding: 5px;"
	" border: 0;"
	" background: transparent;"
	" transition: background-color .25s ease-in-out;"
	"}"
	".playlist button::before{"
	" content: \"\";"
	" width: 2ch;"
	" display: inline-block;"
	"}"
	".playlist button:hover{"
	" background-color: #dfdddd;"
	"}"
	".playlist #current button{"
	" font-weight: bold;"
	"}"
	".playlist #current button::before{"
	" content: \"→ \";"
	" font-weight: bold;"
	"}"
	".controls{"
	" position: sticky;"
	" width: 100%;"
	" max-width: 800px;"
	" margin: 0 auto;"
	" bottom: 0;"
	" background-color: white;"
	" background: #3d3d3d;"
	" color: white;"
	" border-radius: 10px 10px 0 0;"
	" padding: 10px;"
	" text-align: center;"
	" order: 2;"
	"}"
	".controls p{"
	" margin: .4rem;"
	"}"
	".controls a{"
	" color: white;"
	"}"
	".controls .status{"
	" font-size: 0.9rem;"
	"}"
	".controls button{"
	" margin: 5px;"
	" padding: 5px 20px;"
	"}"
	".mode-active{"
	" color: #0064ff;"
	"}";

const char *js =
	"function cur(e) {"
	" if (e) {e.preventDefault()}"
	" let cur = document.querySelector('#current');"
	" if (cur) {cur.scrollIntoView(); window.scrollBy(0, -100);}"
	"}"
	"cur();"
	"document.querySelector('.controls a').addEventListener('click',cur)";

const char *foot = "<script src='/app.js?v=0'></script></body></html>";

static int
dial(const char *sock)
{
	struct sockaddr_un	 sa;
	size_t			 len;
	int			 s;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	len = strlcpy(sa.sun_path, sock, sizeof(sa.sun_path));
	if (len >= sizeof(sa.sun_path))
		err(1, "path too long: %s", sock);

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		err(1, "socket");
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		err(1, "failed to connect to %s", sock);

	return s;
}


/*
 * Adapted from usr.sbin/httpd/httpd.c' url_decode.
 */
static int
url_decode(char *url)
{
	char*p, *q;
	char hex[3] = {0};
	unsigned long x;

	p = q = url;
	while (*p != '\0') {
		switch (*p) {
		case '%':
			/* Encoding character is followed by two hex chars */
			if (!isxdigit((unsigned char)p[1]) ||
			    !isxdigit((unsigned char)p[2]) ||
			    (p[1] == '0' && p[2] == '0'))
				return (-1);

			hex[0] = p[1];
			hex[1] = p[2];

			/*
			 * We don't have to validate "hex" because it is
			 * guaranteed to include two hex chars followed
			 * by NUL.
			 */
			x = strtoul(hex, NULL, 16);
			*q = (char)x;
			p += 2;
			break;
		case '+':
			*q = ' ';
			break;
		default:
			*q = *p;
			break;
		}
		p++;
		q++;
	}
	*q = '\0';

	return (0);
}

static void
unexpected_imsg(struct imsg *imsg, const char *expected)
{
	const char	 *msg;
	size_t		 datalen;

	if (imsg->hdr.type != IMSG_CTL_ERR) {
		log_warnx("got event %d while expecting %s",
		    imsg->hdr.type, expected);
		return;
	}

	datalen = IMSG_DATA_SIZE(*imsg);
	msg = imsg->data;
	if (datalen == 0 || msg[datalen - 1] != '\0')
		fatalx("malformed error message");
	log_warnx("failure: %s", msg);
}

static void
route_notfound(struct client *clt)
{
	if (http_reply(clt, 404, "Not Found", "text/plain") == -1 ||
	    http_writes(clt, "Page not found\n") == -1)
		return;
}

static void
render_playlist(struct client *clt)
{
	struct imsg		 imsg;
	struct player_status	 ps;
	ssize_t			 n;
	const char		*p;
	int			 current, done;

	imsg_compose(&ibuf, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
	imsg_flush(&ibuf);

	http_writes(clt, "<section class='playlist-wrapper'>");
	http_writes(clt, "<form action=jump method=post"
	    " enctype='"FORM_URLENCODED"'>");
	http_writes(clt, "<ul class=playlist>");

	done = 0;
	while (!done) {
		if ((n = imsg_read(&ibuf)) == -1)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");

		for (;;) {
			if ((n = imsg_get(&ibuf, &imsg)) == -1)
				fatal("imsg_get");
			if (n == 0)
				break;

			if (imsg.hdr.type != IMSG_CTL_SHOW) {
				unexpected_imsg(&imsg, "IMSG_CTL_SHOW");
				imsg_free(&imsg);
				continue;
			}

			if (IMSG_DATA_SIZE(imsg) == 0) {
				done = 1;
				break;
			}

			if (IMSG_DATA_SIZE(imsg) != sizeof(ps))
				fatalx("wrong size for seek ctl");
			memcpy(&ps, imsg.data, sizeof(ps));
			if (ps.path[sizeof(ps.path) - 1] != '\0')
				fatalx("received corrupted data");

			current = ps.status == STATE_PLAYING;

			p = ps.path;
			if (!strncmp(p, prefix, prefixlen))
				p += prefixlen;

			http_fmt(clt, "<li%s>",
			    current ? " id=current" : "");
			http_writes(clt,
			    "<button type=submit name=jump value=\"");
			http_htmlescape(clt, ps.path);
			http_writes(clt, "\">");
			http_htmlescape(clt, p);
			http_writes(clt, "</button></li>");

			imsg_free(&imsg);
		}
	}

	http_writes(clt, "</ul>");
	http_writes(clt, "</form>");
	http_writes(clt, "</section>");
}

static void
render_controls(struct client *clt)
{
	struct imsg		 imsg;
	struct player_status	 ps;
	ssize_t			 n;
	const char		*oc, *ac, *p;
	int			 playing;

	imsg_compose(&ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
	imsg_flush(&ibuf);

	if ((n = imsg_read(&ibuf)) == -1)
		fatal("imsg_read");
	if (n == 0)
		fatalx("pipe closed");

	if ((n = imsg_get(&ibuf, &imsg)) == -1)
		fatal("imsg_get");
	if (n == 0)
		return;

	if (imsg.hdr.type != IMSG_CTL_STATUS) {
		unexpected_imsg(&imsg, "IMSG_CTL_STATUS");
		goto done;
	}
	if (IMSG_DATA_SIZE(imsg) != sizeof(ps))
		fatalx("wrong size for IMSG_CTL_STATUS");
	memcpy(&ps, imsg.data, sizeof(ps));
	if (ps.path[sizeof(ps.path) - 1] != '\0')
		fatalx("received corrupted data");

	ac = ps.mode.repeat_all ? " class='mode-active'" : "";
	oc = ps.mode.repeat_one ? " class='mode-active'" : "";
	playing = ps.status == STATE_PLAYING;

	if ((p = strrchr(ps.path, '/')) != NULL)
		p++;
	else
		p = ps.path;

	if (http_writes(clt, "<section class=controls>") == -1 ||
	    http_writes(clt, "<p><a href='#current'>") == -1 ||
	    http_htmlescape(clt, p) == -1 ||
	    http_writes(clt, "</a></p>") == -1 ||
	    http_writes(clt, "<form action=ctrls method=post"
		" enctype='"FORM_URLENCODED"'>") == -1 ||
	    http_writes(clt, "<button type=submit name=ctl value=prev>"
		ICON_PREV"</button>") == -1 ||
	    http_fmt(clt, "<button type=submit name=ctl value=%s>"
		"%s</button>", playing ? "pause" : "play",
		playing ? ICON_PAUSE : ICON_PLAY) == -1 ||
	    http_writes(clt, "<button type=submit name=ctl value=next>"
		ICON_NEXT"</button>") == -1 ||
	    http_writes(clt, "</form>") == -1 ||
	    http_writes(clt, "<form action=mode method=post"
		" enctype='"FORM_URLENCODED"'>") == -1 ||
	    http_fmt(clt, "<button%s type=submit name=mode value=all>"
		ICON_REPEAT_ALL"</button>", ac) == -1 ||
	    http_fmt(clt, "<button%s type=submit name=mode value=one>"
		ICON_REPEAT_ONE"</button>", oc) == -1 ||
	    http_writes(clt, "</form>") == -1 ||
	    http_writes(clt, "</section>") == -1)
		return;

 done:
	imsg_free(&imsg);
}

static void
route_home(struct client *clt)
{
	if (http_reply(clt, 200, "OK", "text/html;charset=UTF-8") == -1)
		return;

	if (http_write(clt, head, strlen(head)) == -1)
		return;

	if (http_writes(clt, "<main>") == -1)
		return;

	if (http_writes(clt, "<section class=searchbox>"
	    "<input type=search name=filter aria-label='Filter playlist'"
	    " placeholder='Filter playlist' id=search />"
	    "</section>") == -1)
		return;

	render_controls(clt);
	render_playlist(clt);

	if (http_writes(clt, "</main>") == -1)
		return;

	http_write(clt, foot, strlen(foot));
}

static void
route_jump(struct client *clt)
{
	struct imsg		 imsg;
	struct player_status	 ps;
	ssize_t			 n;
	char			 path[PATH_MAX];
	char			*form, *field;
	int			 found = 0;

	form = clt->buf;
	while ((field = strsep(&form, "&")) != NULL) {
		if (url_decode(field) == -1)
			goto badreq;

		if (strncmp(field, "jump=", 5) != 0)
			continue;
		field += 5;
		found = 1;

		memset(&path, 0, sizeof(path));
		if (strlcpy(path, field, sizeof(path)) >= sizeof(path))
			goto badreq;

		log_warnx("path is %s", path);
		imsg_compose(&ibuf, IMSG_CTL_JUMP, 0, 0, -1,
		    path, sizeof(path));
		imsg_flush(&ibuf);

		if ((n = imsg_read(&ibuf)) == -1)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");

		for (;;) {
			if ((n = imsg_get(&ibuf, &imsg)) == -1)
				fatal("imsg_get");
			if (n == 0)
				break;

			if (imsg.hdr.type != IMSG_CTL_STATUS) {
				unexpected_imsg(&imsg, "IMSG_CTL_STATUS");
				imsg_free(&imsg);
				continue;
			}

			if (IMSG_DATA_SIZE(imsg) != sizeof(ps))
				fatalx("data size mismatch");
			memcpy(&ps, imsg.data, sizeof(ps));
			if (ps.path[sizeof(ps.path) - 1] != '\0')
				fatalx("received corrupted data");
			log_debug("jumped to %s", ps.path);
		}

		break;
	}

	if (!found)
		goto badreq;

	http_reply(clt, 302, "See Other", "/");
	return;

 badreq:
	http_reply(clt, 400, "Bad Request", "text/plain");
	http_writes(clt, "Bad Request.\n");
}

static void
route_controls(struct client *clt)
{
	char		*form, *field;
	int		 cmd, found = 0;

	form = clt->buf;
	while ((field = strsep(&form, "&")) != NULL) {
		if (url_decode(field) == -1)
			goto badreq;

		if (strncmp(field, "ctl=", 4) != 0)
			continue;
		field += 4;
		found = 1;

		if (!strcmp(field, "play"))
			cmd = IMSG_CTL_PLAY;
		else if (!strcmp(field, "pause"))
			cmd = IMSG_CTL_PAUSE;
		else if (!strcmp(field, "next"))
			cmd = IMSG_CTL_NEXT;
		else if (!strcmp(field, "prev"))
			cmd = IMSG_CTL_PREV;
		else
			goto badreq;

		imsg_compose(&ibuf, cmd, 0, 0, -1, NULL, 0);
		imsg_flush(&ibuf);
		break;
	}

	if (!found)
		goto badreq;

	http_reply(clt, 302, "See Other", "/");
	return;

 badreq:
	http_reply(clt, 400, "Bad Request", "text/plain");
	http_writes(clt, "Bad Request.\n");
}

static void
route_mode(struct client *clt)
{
	char			*form, *field;
	int			 found = 0;
	ssize_t			 n;
	struct player_status	 ps;
	struct player_mode	 pm;
	struct imsg		 imsg;

	pm.repeat_one = pm.repeat_all = pm.consume = MODE_UNDEF;

	form = clt->buf;
	while ((field = strsep(&form, "&")) != NULL) {
		if (url_decode(field) == -1)
			goto badreq;

		if (strncmp(field, "mode=", 5) != 0)
			continue;
		field += 5;
		found = 1;

		if (!strcmp(field, "all"))
			pm.repeat_all = MODE_TOGGLE;
		else if (!strcmp(field, "one"))
			pm.repeat_one = MODE_TOGGLE;
		else
			goto badreq;

		imsg_compose(&ibuf, IMSG_CTL_MODE, 0, 0, -1, &pm, sizeof(pm));
		imsg_flush(&ibuf);

		if ((n = imsg_read(&ibuf)) == -1)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");

		for (;;) {
			if ((n = imsg_get(&ibuf, &imsg)) == -1)
				fatal("imsg_get");
			if (n == 0)
				break;

			if (imsg.hdr.type != IMSG_CTL_STATUS) {
				unexpected_imsg(&imsg, "IMSG_CTL_STATUS");
				imsg_free(&imsg);
				continue;
			}

			if (IMSG_DATA_SIZE(imsg) != sizeof(ps))
				fatalx("data size mismatch");
			memcpy(&ps, imsg.data, sizeof(ps));
			if (ps.path[sizeof(ps.path) - 1] != '\0')
				fatalx("received corrupted data");
		}

		break;
	}

	if (!found)
		goto badreq;

	http_reply(clt, 302, "See Other", "/");
	return;

 badreq:
	http_reply(clt, 400, "Bad Request", "text/plain");
	http_writes(clt, "Bad Request.\n");
}

static void
route_assets(struct client *clt)
{
	if (!strcmp(clt->req.path, "/style.css")) {
		http_reply(clt, 200, "OK", "text/css");
		http_write(clt, css, strlen(css));
		return;
	}

	if (!strcmp(clt->req.path, "/app.js")) {
		http_reply(clt, 200, "OK", "application/javascript");
		http_write(clt, js, strlen(js));
		return;
	}

	route_notfound(clt);
}

static void
route_dispatch(struct client *clt)
{
	static const struct route {
		int		 method;
		const char	*path;
		route_fn	 route;
	} routes[] = {
		{ METHOD_GET,	"/",		&route_home },
		{ METHOD_POST,	"/jump",	&route_jump },
		{ METHOD_POST,	"/ctrls",	&route_controls },
		{ METHOD_POST,	"/mode",	&route_mode },

		{ METHOD_GET,	"/style.css",	&route_assets },
		{ METHOD_GET,	"/app.js",	&route_assets },

		{ METHOD_GET,	"*",		&route_notfound },
		{ METHOD_POST,	"*",		&route_notfound },
	};
	struct request *req = &clt->req;
	size_t i;

	if ((req->method != METHOD_GET && req->method != METHOD_POST) ||
	    (req->ctype != NULL && strcmp(req->ctype, FORM_URLENCODED) != 0) ||
	    req->path == NULL) {
		http_reply(clt, 400, "Bad Request", NULL);
		return;
	}

	for (i = 0; i < nitems(routes); ++i) {
		if (req->method != routes[i].method ||
		    fnmatch(routes[i].path, req->path, 0) != 0)
			continue;
		clt->done = 1; /* assume with one round is done */
		clt->route = routes[i].route;
		clt->route(clt);
		if (clt->done)
			http_close(clt);
		return;
	}
}

static void
client_ev(int fd, int ev, void *d)
{
	struct client	*clt = d;

	if (ev & (POLLIN|POLLHUP)) {
		if (bufio_read(&clt->bio) == -1 && errno != EAGAIN) {
			log_warn("bufio_read");
			goto err;
		}
	}

	if (ev & POLLOUT) {
		if (bufio_write(&clt->bio) == -1 && errno != EAGAIN) {
			log_warn("bufio_read");
			goto err;
		}
	}

	if (clt->route == NULL) {
		if (http_parse(clt) == -1) {
			if (errno == EAGAIN)
				goto again;
			log_warnx("HTTP parse request failed");
			goto err;
		}
		if (clt->req.method == METHOD_POST &&
		    http_read(clt) == -1) {
			if (errno == EAGAIN)
				goto again;
			log_warnx("failed to read POST data");
			goto err;
		}
		route_dispatch(clt);
		goto again;
	}

	if (!clt->done)
		clt->route(clt);

 again:
	ev = bufio_pollev(&clt->bio);
	if (ev == POLLIN && clt->done) {
		goto err; /* done with this client */
	}

	ev_add(fd, ev, client_ev, clt);
	return;

 err:
	ev_del(fd);
	http_free(clt);
}

static void
web_accept(int psock, int ev, void *d)
{
	struct client	*clt;
	int		 sock;

	if ((sock = accept(psock, NULL, NULL)) == -1) {
		warn("accept");
		return;
	}
	clt = xcalloc(1, sizeof(*clt));
	if ((clt = calloc(1, sizeof(*clt))) == NULL ||
	    http_init(clt, sock) == -1) {
		log_warn("failed to initialize client");
		free(clt);
		close(sock);
		return;
	}

	client_ev(sock, POLLIN, clt);
	return;
}

void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-v] [-s sock] [-t prefix] [[host] port]\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	struct addrinfo	 hints, *res, *res0;
	const char	*cause = NULL;
	const char	*host = NULL;
	const char	*port = "9090";
	char		*sock = NULL;
	size_t		 nsock, error, save_errno;
	int		 ch, v, amused_sock, fd;
	int		 verbose = 0;

	setlocale(LC_ALL, NULL);

	log_init(1, LOG_DAEMON);

	if (pledge("stdio rpath unix inet dns", NULL) == -1)
		err(1, "pledge");

	while ((ch = getopt(argc, argv, "s:t:v")) != -1) {
		switch (ch) {
		case 's':
			sock = optarg;
			break;
		case 't':
			prefix = optarg;
			prefixlen = strlen(prefix);
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 1)
		port = argv[0];
	if (argc == 2) {
		host = argv[0];
		port = argv[1];
	}
	if (argc > 2)
		usage();

	log_setverbose(verbose);

	if (sock == NULL)
		xasprintf(&sock, "/tmp/amused-%d", getuid());

	signal(SIGPIPE, SIG_IGN);

	if (ev_init() == -1)
		fatal("ev_init");

	amused_sock = dial(sock);
	imsg_init(&ibuf, amused_sock);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		errx(1, "%s", gai_strerror(error));

	nsock = 0;
	for (res = res0; res; res = res->ai_next) {
		fd = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
		if (fd == -1) {
			cause = "socket";
			continue;
		}

		v = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		    &v, sizeof(v)) == -1)
			fatal("setsockopt(SO_REUSEADDR)");

		if (bind(fd, res->ai_addr, res->ai_addrlen) == -1) {
			cause = "bind";
			save_errno = errno;
			close(fd);
			errno = save_errno;
			continue;
		}

		if (listen(fd, 5) == -1)
			err(1, "listen");

		if (ev_add(fd, POLLIN, web_accept, NULL) == -1)
			fatal("ev_add");
		nsock++;
	}
	if (nsock == 0)
		err(1, "%s", cause);
	freeaddrinfo(res0);

	if (pledge("stdio inet", NULL) == -1)
		err(1, "pledge");

	log_info("starting");
	ev_loop();
	return (1);
}
