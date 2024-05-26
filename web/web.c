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

#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <imsg.h>
#include <limits.h>
#include <locale.h>
#include <netdb.h>
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
#include "ws.h"
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

static struct clthead		 clients;
static struct imsgbuf		 imsgbuf;
static struct playlist		 playlist_tmp;
static struct player_status	 player_status;
static uint64_t			 position, duration;

static void client_ev(int, int, void *);

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
	"var ws;"
	"let pos=0, dur=0;"
	"const playlist=document.querySelector('.playlist');"
	"function cur(e) {"
	" if (e) {e.preventDefault()}"
	" let cur = document.querySelector('#current');"
	" if (cur) {cur.scrollIntoView(); window.scrollBy(0, -100);}"
	"};"
	"function b(x){return x=='on'};"
	"function c(p, c){"
	" const l=document.createElement('li');"
	" if(c){l.id='current'};"
	" const b=document.createElement('button');"
	" b.type='submit'; b.name='jump'; b.value=p;"
	" b.innerText=p;"
	" l.appendChild(b);"
	" playlist.appendChild(l);"
	"}"
	"function d(t){"
	" const [, type, payload] = t.split(/^(.):(.*)$/);"
	" if (type=='s'){"
	"  let s=payload.split(' ');"
	"  pos=s[0], dur=s[1];"
	" } else if (type=='S') {"
	"  const btn=document.querySelector('#toggle');"
	"  if (payload=='playing') {"
	"   btn.innerHTML='"ICON_PAUSE"';"
	"   btn.value='pause';"
	"  } else {"
	"   btn.innerHTML='"ICON_PLAY"';"
	"   btn.value='play';"
	"  }"
	" } else if (type=='r') {"
	"  const btn=document.querySelector('#rone');"
	"  btn.className=b(payload)?'mode-active':'';"
	" } else if (type=='R') {"
	"  const btn=document.querySelector('#rall');"
	"  btn.className=b(payload)?'mode-active':'';"
	" } else if (type=='c') {"
	/* consume */
	" } else if (type=='x') {"
	"  playlist.innerHTML='';"
	" } else if (type=='X') {"
	"  dofilt();" /* done with the list */
	" } else if (type=='A') {"
	"  c(payload, true);"
	" } else if (type=='a') {"
	"  c(payload, false);"
	" } else if (type=='C') {"
	"  const t=document.querySelector('.controls>p>a');"
	"  t.innerText = payload.replace(/.*\\//, '');"
	"  cur();"
	" } else {"
	"  console.log('unknown:',t);"
	" }"
	"};"
	"function w(){"
	" ws = new WebSocket((location.protocol=='http:'?'ws://':'wss://')"
	"  + location.host + '/ws');"
	" ws.addEventListener('open', () => console.log('ws: connected'));"
	" ws.addEventListener('close', () => {"
	"  alert('Websocket closed.  The interface won\\'t update itself.'"
	"   + ' Please refresh the page');"
	" });"
	" ws.addEventListener('message', e => d(e.data))"
	"};"
	"w();"
	"cur();"
	"document.querySelector('.controls a').addEventListener('click',cur);"
	"document.querySelectorAll('form').forEach(f => {"
	" f.action='/a/'+f.getAttribute('action');"
	" f.addEventListener('submit', e => {"
	"  e.preventDefault();"
	"  const fd = new FormData(f);"
	"  if (e.submitter && e.submitter.value && e.submitter.value != '')"
	"   fd.append(e.submitter.name, e.submitter.value);"
	"  fetch(f.action, {"
	"   method:'POST',"
	"   body: new URLSearchParams(fd)"
	"  })"
	"  .catch(x => console.log('failed to submit form:', x));"
	" });"
	"});"
	"const sb = document.createElement('section');"
	"sb.className = 'searchbox';"
	"const filter = document.createElement('input');"
	"filter.type = 'search';"
	"filter.setAttribute('aria-label', 'Filter Playlist');"
	"filter.placeholder = 'Filter Playlist';"
	"sb.append(filter);"
	"document.querySelector('main').prepend(sb);"
	"function dofilt() {"
	" let t = filter.value.toLowerCase();"
	" document.querySelectorAll('.playlist li').forEach(e => {"
	"  if (e.querySelector('button').value.toLowerCase().indexOf(t) == -1)"
	"    e.setAttribute('hidden', 'true');"
	"  else"
	"    e.removeAttribute('hidden');"
	" });"
	"};"
	"function dbc(fn, wait) {"
	" let tout;"
	" return function() {"
	"  let later = () => {tout = null; fn()};"
	"  clearTimeout(tout);"
	"  if (!tout) fn();"
	"  tout = setTimeout(later, wait);"
	" };"
	"};"
	"filter.addEventListener('input', dbc(dofilt, 400));"
	;

const char *foot = "<script src='/app.js?v=0'></script></body></html>";

static inline int
bio_ev(struct bufio *bio)
{
	int	 ret, ev;

	ret = 0;
	ev = bufio_ev(bio);
	if (ev & BUFIO_WANT_READ)
		ret |= EV_READ;
	if (ev & BUFIO_WANT_WRITE)
		ret |= EV_WRITE;
	return ret;
}

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
		fatalx("path too long: %s", sock);

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		fatal("socket");
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		fatal("failed to connect to %s", sock);

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

static int
dispatch_event(const char *msg)
{
	struct client	*clt;
	size_t		 len;
	int		 ret = 0;

	len = strlen(msg);
	TAILQ_FOREACH(clt, &clients, clients) {
		if (!clt->ws || clt->done || clt->err)
			continue;

		if (ws_compose(clt, WST_TEXT, msg, len) == -1)
			ret = -1;

		ev_add(clt->bio.fd, EV_READ|EV_WRITE, client_ev, clt);
	}

	return (ret);
}

static int
dispatch_event_status(void)
{
	const char	*status;
	char		 buf[PATH_MAX + 2];
	int		 r;

	switch (player_status.status) {
	case STATE_STOPPED: status = "stopped"; break;
	case STATE_PLAYING: status = "playing"; break;
	case STATE_PAUSED:  status = "paused";  break;
	default: status = "unknown";
	}

	r = snprintf(buf, sizeof(buf), "S:%s", status);
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf");
		return -1;
	}
	dispatch_event(buf);

	r = snprintf(buf, sizeof(buf), "r:%s",
	    player_status.mode.repeat_one == MODE_ON ? "on" : "off");
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf");
		return -1;
	}
	dispatch_event(buf);

	r = snprintf(buf, sizeof(buf), "R:%s",
	    player_status.mode.repeat_all == MODE_ON ? "on" : "off");
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf");
		return -1;
	}
	dispatch_event(buf);

	r = snprintf(buf, sizeof(buf), "c:%s",
	    player_status.mode.consume == MODE_ON ? "on" : "off");
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf");
		return -1;
	}
	dispatch_event(buf);

	r = snprintf(buf, sizeof(buf), "C:%s", player_status.path);
	if (r < 0 || (size_t)r >= sizeof(buf)) {
		log_warn("snprintf");
		return -1;
	}
	dispatch_event(buf);

	return 0;
}

static int
dispatch_event_track(struct player_status *ps)
{
	char		 p[PATH_MAX + 2];
	int		 r;

	r = snprintf(p, sizeof(p), "%c:%s",
	    ps->status == STATE_PLAYING ? 'A' : 'a', ps->path);
	if (r < 0 || (size_t)r >= sizeof(p))
		return (-1);

	return dispatch_event(p);
}

static void
imsg_dispatch(int fd, int ev, void *d)
{
	static ssize_t		 off;
	static int		 off_found;
	char			 seekmsg[128];
	struct imsg		 imsg;
	struct ibuf		 ibuf;
	struct player_status	 ps;
	struct player_event	 event;
	const char		*msg;
	ssize_t			 n;
	size_t			 datalen;
	int			 r;

	if (ev & EV_READ) {
		if ((n = imsg_read(&imsgbuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");
	}
	if (ev & EV_WRITE) {
		if ((n = msgbuf_write(&imsgbuf.w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)
			fatalx("pipe closed");
	}

	for (;;) {
		if ((n = imsg_get(&imsgbuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)
			break;

		datalen = IMSG_DATA_SIZE(imsg);

		switch (imsg_get_type(&imsg)) {
		case IMSG_CTL_ERR:
			if (imsg_get_ibuf(&imsg, &ibuf) == -1 ||
			    (datalen = ibuf_size(&ibuf)) == 0 ||
			    (msg = ibuf_data(&ibuf)) == NULL ||
			    msg[datalen - 1] != '\0')
				fatalx("malformed error message");
			log_warnx("error: %s", msg);
			break;

		case IMSG_CTL_ADD:
			playlist_free(&playlist_tmp);
			imsg_compose(&imsgbuf, IMSG_CTL_SHOW, 0, 0, -1,
			    NULL, 0);
			break;

		case IMSG_CTL_MONITOR:
			if (imsg_get_data(&imsg, &event, sizeof(event)) == -1)
				fatalx("corrupted IMSG_CTL_MONITOR");
			switch (event.event) {
			case IMSG_CTL_PLAY:
			case IMSG_CTL_PAUSE:
			case IMSG_CTL_STOP:
			case IMSG_CTL_MODE:
				imsg_compose(&imsgbuf, IMSG_CTL_STATUS, 0, 0,
				    -1, NULL, 0);
				break;

			case IMSG_CTL_NEXT:
			case IMSG_CTL_PREV:
			case IMSG_CTL_JUMP:
			case IMSG_CTL_COMMIT:
				imsg_compose(&imsgbuf, IMSG_CTL_SHOW, 0, 0, -1,
				    NULL, 0);
				imsg_compose(&imsgbuf, IMSG_CTL_STATUS, 0, 0,
				    -1, NULL, 0);
				break;

			case IMSG_CTL_SEEK:
				position = event.position;
				duration = event.duration;
				r = snprintf(seekmsg, sizeof(seekmsg),
				    "s:%lld %lld", (long long)position,
				    (long long)duration);
				if (r < 0 || (size_t)r >= sizeof(seekmsg)) {
					log_warn("snprintf failed");
					break;
				}
				dispatch_event(seekmsg);
				break;

			default:
				log_debug("ignoring event %d", event.event);
				break;
			}
			break;

		case IMSG_CTL_SHOW:
			if (imsg_get_len(&imsg) == 0) {
				if (playlist_tmp.len == 0) {
					dispatch_event("x:");
					off = -1;
				} else if (playlist_tmp.len == off)
					off = -1;
				dispatch_event("X:");
				playlist_swap(&playlist_tmp, off);
				memset(&playlist_tmp, 0, sizeof(playlist_tmp));
				off = 0;
				off_found = 0;
				break;
			}
			if (imsg_get_data(&imsg, &ps, sizeof(ps)) == -1)
				fatalx("corrupted IMSG_CTL_SHOW");
			if (ps.path[sizeof(ps.path) - 1] != '\0')
				fatalx("corrupted IMSG_CTL_SHOW");
			if (playlist_tmp.len == 0)
				dispatch_event("x:");
			dispatch_event_track(&ps);
			playlist_push(&playlist_tmp, ps.path);
			if (ps.status == STATE_PLAYING)
				off_found = 1;
			if (!off_found)
				off++;
			break;

		case IMSG_CTL_STATUS:
			if (imsg_get_data(&imsg, &player_status,
			    sizeof(player_status)) == -1)
				fatalx("corrupted IMSG_CTL_STATUS");
			if (player_status.path[sizeof(player_status.path) - 1]
			    != '\0')
				fatalx("corrupted IMSG_CTL_STATUS");
			dispatch_event_status();
			break;
		}
	}

	ev = EV_READ;
	if (imsgbuf.w.queued)
		ev |= EV_WRITE;
	ev_add(fd, ev, imsg_dispatch, NULL);
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
	ssize_t			 i;
	const char		*path;
	int			 current;

	http_writes(clt, "<section class='playlist-wrapper'>");
	http_writes(clt, "<form action=jump method=post"
	    " enctype='"FORM_URLENCODED"'>");
	http_writes(clt, "<ul class=playlist>");

	for (i = 0; i < playlist.len; ++i) {
		current = play_off == i;

		path = playlist.songs[i];

		http_fmt(clt, "<li%s>", current ? " id=current" : "");
		http_writes(clt, "<button type=submit name=jump value=\"");
		http_htmlescape(clt, path);
		http_writes(clt, "\">");
		http_htmlescape(clt, path);
		http_writes(clt, "</button></li>");
	}

	http_writes(clt, "</ul>");
	http_writes(clt, "</form>");
	http_writes(clt, "</section>");
}

static void
render_controls(struct client *clt)
{
	const char		*oc, *ac, *p;
	int			 playing;

	ac = player_status.mode.repeat_all ? " class='mode-active'" : "";
	oc = player_status.mode.repeat_one ? " class='mode-active'" : "";
	playing = player_status.status == STATE_PLAYING;

	if ((p = strrchr(player_status.path, '/')) != NULL)
		p++;
	else
		p = player_status.path;

	if (http_writes(clt, "<section class=controls>") == -1 ||
	    http_writes(clt, "<p><a href='#current'>") == -1 ||
	    http_htmlescape(clt, p) == -1 ||
	    http_writes(clt, "</a></p>") == -1 ||
	    http_writes(clt, "<form action=ctrls method=post"
		" enctype='"FORM_URLENCODED"'>") == -1 ||
	    http_writes(clt, "<button type=submit name=ctl value=prev>"
		ICON_PREV"</button>") == -1 ||
	    http_fmt(clt, "<button id='toggle' type=submit name=ctl value=%s>"
		"%s</button>", playing ? "pause" : "play",
		playing ? ICON_PAUSE : ICON_PLAY) == -1 ||
	    http_writes(clt, "<button type=submit name=ctl value=next>"
		ICON_NEXT"</button>") == -1 ||
	    http_writes(clt, "</form>") == -1 ||
	    http_writes(clt, "<form action=mode method=post"
		" enctype='"FORM_URLENCODED"'>") == -1 ||
	    http_fmt(clt, "<button%s id=rall type=submit name=mode value=all>"
		ICON_REPEAT_ALL"</button>", ac) == -1 ||
	    http_fmt(clt, "<button%s id=rone type=submit name=mode value=one>"
		ICON_REPEAT_ONE"</button>", oc) == -1 ||
	    http_writes(clt, "</form>") == -1 ||
	    http_writes(clt, "</section>") == -1)
		return;
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

	render_controls(clt);
	render_playlist(clt);

	if (http_writes(clt, "</main>") == -1)
		return;

	http_write(clt, foot, strlen(foot));
}

static void
route_jump(struct client *clt)
{
	char			 path[PATH_MAX];
	char			*form, *field;
	int			 found = 0;

	http_postdata(clt, &form, NULL);
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

		imsg_compose(&imsgbuf, IMSG_CTL_JUMP, 0, 0, -1,
		    path, sizeof(path));
		ev_add(imsgbuf.w.fd, EV_READ|EV_WRITE, imsg_dispatch, NULL);
		break;
	}

	if (!found)
		goto badreq;

	if (!strncmp(clt->req.path, "/a/", 2))
		http_reply(clt, 200, "OK", "text/plain");
	else
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

	http_postdata(clt, &form, NULL);
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

		imsg_compose(&imsgbuf, cmd, 0, 0, -1, NULL, 0);
		imsg_flush(&imsgbuf);
		break;
	}

	if (!found)
		goto badreq;

	if (!strncmp(clt->req.path, "/a/", 2))
		http_reply(clt, 200, "OK", "text/plain");
	else
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
	struct player_mode	 pm;

	pm.repeat_one = pm.repeat_all = pm.consume = MODE_UNDEF;

	http_postdata(clt, &form, NULL);
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

		imsg_compose(&imsgbuf, IMSG_CTL_MODE, 0, 0, -1,
		    &pm, sizeof(pm));
		ev_add(imsgbuf.w.fd, EV_READ|EV_WRITE, imsg_dispatch, NULL);
		break;
	}

	if (!found)
		goto badreq;

	if (!strncmp(clt->req.path, "/a/", 2))
		http_reply(clt, 200, "OK", "text/plain");
	else
		http_reply(clt, 302, "See Other", "/");
	return;

 badreq:
	http_reply(clt, 400, "Bad Request", "text/plain");
	http_writes(clt, "Bad Request.\n");
}

static void
route_handle_ws(struct client *clt)
{
	struct buf	*rbuf = &clt->bio.rbuf;
	int		 type;
	size_t		 len;

	if (ws_read(clt, &type, &len) == -1) {
		if (errno != EAGAIN) {
			log_warn("ws_read");
			clt->done = 1;
		}
		return;
	}

	switch (type) {
	case WST_PING:
		ws_compose(clt, WST_PONG, rbuf->buf, len);
		break;
	case WST_TEXT:
		/* log_info("<<< %.*s", (int)len, rbuf->buf); */
		break;
	case WST_CLOSE:
		/* TODO send a close too (ack) */
		clt->done = 1;
		break;
	default:
		log_info("got unexpected ws frame type 0x%02x", type);
		break;
	}

	buf_drain(rbuf, len);
}

static void
route_init_ws(struct client *clt)
{
	if (!(clt->req.flags & (R_CONNUPGR|R_UPGRADEWS|R_WSVERSION)) ||
	    clt->req.secret == NULL) {
		http_reply(clt, 400, "Bad Request", "text/plain");
		http_writes(clt, "Invalid websocket handshake.\r\n");
		return;
	}

	clt->ws = 1;
	clt->done = 0;
	clt->route = route_handle_ws;
	http_reply(clt, 101, "Switching Protocols", NULL);
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

		{ METHOD_POST,	"/a/jump",	&route_jump },
		{ METHOD_POST,	"/a/ctrls",	&route_controls },
		{ METHOD_POST,	"/a/mode",	&route_mode },

		{ METHOD_GET,	"/ws",		&route_init_ws },

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
	ssize_t		 ret;

	if (ev & EV_READ) {
		if ((ret = bufio_read(&clt->bio)) == -1 && errno != EAGAIN) {
			log_warn("bufio_read");
			goto err;
		}
		if (ret == 0)
			goto err;
	}

	if (ev & EV_WRITE) {
		if ((ret = bufio_write(&clt->bio)) == -1 && errno != EAGAIN) {
			log_warn("bufio_write");
			goto err;
		}
		if (ret == 0)
			goto err;
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

	if (!clt->done && !clt->err)
		clt->route(clt);

 again:
	ev = bio_ev(&clt->bio);
	if (ev == EV_READ && (clt->done || clt->err)) {
		goto err; /* done with this client */
	}

	ev_add(fd, ev, client_ev, clt);
	return;

 err:
	ev_del(fd);
	TAILQ_REMOVE(&clients, clt, clients);
	http_free(clt);
}

static void
web_accept(int psock, int ev, void *d)
{
	struct client	*clt;
	int		 sock;

	if ((sock = accept(psock, NULL, NULL)) == -1) {
		log_warn("accept");
		return;
	}
	if ((clt = calloc(1, sizeof(*clt))) == NULL ||
	    http_init(clt, sock) == -1) {
		log_warn("failed to initialize client");
		free(clt);
		close(sock);
		return;
	}

	TAILQ_INSERT_TAIL(&clients, clt, clients);

	client_ev(sock, EV_READ, clt);
	return;
}

void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-v] [-s sock] [[host] port]\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	struct addrinfo	 hints, *res, *res0;
	const char	*cause = NULL;
	const char	*host = "localhost";
	const char	*port = "9090";
	char		*sock = NULL;
	size_t		 nsock, error, save_errno;
	int		 ch, v, amused_sock, fd;
	int		 verbose = 0;

	TAILQ_INIT(&clients);
	setlocale(LC_ALL, NULL);

	log_init(1, LOG_DAEMON);

	if (pledge("stdio rpath unix inet dns", NULL) == -1)
		fatal("pledge");

	while ((ch = getopt(argc, argv, "s:v")) != -1) {
		switch (ch) {
		case 's':
			sock = optarg;
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

	if (!strcmp(host, "*"))
		host = NULL;

	log_setverbose(verbose);

	if (sock == NULL) {
		const char *tmpdir;

		if ((tmpdir = getenv("TMPDIR")) == NULL)
			tmpdir = "/tmp";

		xasprintf(&sock, "%s/amused-%d", tmpdir, getuid());
	}

	signal(SIGPIPE, SIG_IGN);

	if (ev_init() == -1)
		fatal("ev_init");

	amused_sock = dial(sock);
	imsg_init(&imsgbuf, amused_sock);
	imsg_compose(&imsgbuf, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
	imsg_compose(&imsgbuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
	imsg_compose(&imsgbuf, IMSG_CTL_MONITOR, 0, 0, -1, NULL, 0);
	ev_add(amused_sock, EV_READ|EV_WRITE, imsg_dispatch, NULL);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, port, &hints, &res0);
	if (error)
		fatal("%s", gai_strerror(error));

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
			fatal("listen");

		if (ev_add(fd, EV_READ, web_accept, NULL) == -1)
			fatal("ev_add");
		nsock++;
	}
	if (nsock == 0)
		fatal("%s", cause);
	freeaddrinfo(res0);

	if (pledge("stdio inet", NULL) == -1)
		fatal("pledge");

	log_info("listening on %s:%s", host ? host : "*", port);
	ev_loop();
	return (1);
}
