/*
 * Copyright (c) 2022 Omar Polo <op@openbsd.org>
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
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/un.h>

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <imsg.h>

#include "amused.h"
#include "log.h"
#include "playlist.h"
#include "xmalloc.h"

static struct imsgbuf *ibuf;

int	ctl_noarg(struct parse_result *, int, char **);
int	ctl_add(struct parse_result *, int, char **);
int	ctl_show(struct parse_result *, int, char **);
int	ctl_load(struct parse_result *, int, char **);
int	ctl_jump(struct parse_result *, int, char **);
int	ctl_repeat(struct parse_result *, int, char **);

struct ctl_command ctl_commands[] = {
	{ "play",	PLAY,		ctl_noarg,	"" },
	{ "pause",	PAUSE,		ctl_noarg,	"" },
	{ "toggle",	TOGGLE,		ctl_noarg,	"" },
	{ "stop",	STOP,		ctl_noarg,	"" },
	{ "restart",	RESTART,	ctl_noarg,	"" },
	{ "add",	ADD,		ctl_add,	"files...", 1 },
	{ "flush",	FLUSH,		ctl_noarg,	"" },
	{ "show",	SHOW,		ctl_show,	"[-p]" },
	{ "status",	STATUS,		ctl_noarg,	"" },
	{ "next",	NEXT,		ctl_noarg,	"" },
	{ "prev",	PREV,		ctl_noarg,	"" },
	{ "load",	LOAD,		ctl_load,	"[file]", 1 },
	{ "jump",	JUMP,		ctl_jump,	"pattern" },
	{ "repeat",	REPEAT,		ctl_repeat,	"one|all on|off" },
	{ NULL },
};

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-dv] [-s socket]\n", getprogname());
	fprintf(stderr, "%s version %s\n", getprogname(), AMUSED_VERSION);
	exit(1);
}

static __dead void
ctl_usage(struct ctl_command *ctl)
{
	fprintf(stderr, "usage: %s [-v] [-s socket] %s %s\n", getprogname(),
	    ctl->name, ctl->usage);
	exit(1);
}

static int
parse(int argc, char **argv)
{
	struct ctl_command	*ctl = NULL;
	struct parse_result	 res;
	int			 i, status;

	memset(&res, 0, sizeof(res));

	for (i = 0; ctl_commands[i].name != NULL; ++i) {
		if (strncmp(ctl_commands[i].name, argv[0], strlen(argv[0]))
		    == 0) {
			if (ctl != NULL) {
				fprintf(stderr,
				    "ambiguous argument: %s\n", argv[0]);
				usage();
			}
			ctl = &ctl_commands[i];
		}
	}

	if (ctl == NULL) {
		fprintf(stderr, "unknown argument: %s\n", argv[0]);
		usage();
	}

	res.action = ctl->action;
	res.ctl = ctl;

	if (!ctl->has_pledge) {
		/* pledge(2) default if command doesn't have its own */
		if (pledge("stdio", NULL) == -1)
			fatal("pledge");
	}

	status = ctl->main(&res, argc, argv);
	close(ibuf->fd);
	free(ibuf);
	return status;
}

static int
enqueue_tracks(char **files)
{
	char res[PATH_MAX];
	int enq = 0;

	for (; *files != NULL; ++files) {
		memset(&res, 0, sizeof(res));
		if (realpath(*files, res) == NULL) {
			log_warn("realpath %s", *files);
			continue;
		}

		imsg_compose(ibuf, IMSG_CTL_ADD, 0, 0, -1,
		    res, sizeof(res));
		enq++;
	}

	return enq == 0;
}

static int
jump_req(const char *arg)
{
	char path[PATH_MAX];

	memset(path, 0, sizeof(path));
	strlcpy(path, arg, sizeof(path));
	imsg_compose(ibuf, IMSG_CTL_JUMP, 0, 0, -1, path, sizeof(path));
	return 0;
}

static void
print_error_message(const char *prfx, struct imsg *imsg)
{
	size_t datalen;
	char *msg;

	datalen = IMSG_DATA_SIZE(*imsg);
	if ((msg = calloc(1, datalen)) == NULL)
		fatal("calloc %zu", datalen);
	memcpy(msg, imsg->data, datalen);
	if (datalen == 0 || msg[datalen-1] != '\0')
		fatalx("malformed error message");

	log_warnx("%s: %s", prfx, msg);
	free(msg);
}

static int
show_add(struct imsg *imsg, int *ret, char ***files)
{
	if (**files == NULL) {
		log_warnx("received more replies than file sent");
		*ret = 1;
		return 1;
	}

	if (imsg->hdr.type == IMSG_CTL_ERR)
		print_error_message(**files, imsg);
	else if (imsg->hdr.type == IMSG_CTL_ADD)
		log_debug("enqueued %s", **files);
	else
		fatalx("got invalid message %d", imsg->hdr.type);

	(*files)++;
	return (**files) == NULL;
}

static int
show_complete(struct parse_result *res, struct imsg *imsg, int *ret)
{
	struct player_status s;
	size_t datalen;

	if (imsg->hdr.type == IMSG_CTL_ERR) {
		print_error_message("show failed", imsg);
		*ret = 1;
		return 1;
	}

	datalen = IMSG_DATA_SIZE(*imsg);
	if (datalen == 0)
		return 1;

	if (datalen != sizeof(s))
		fatalx("%s: data size mismatch", __func__);
	memcpy(&s, imsg->data, sizeof(s));
	if (s.path[sizeof(s.path)-1] != '\0')
		fatalx("%s: data corrupted?", __func__);

	if (res->pretty)
		printf("%c ", s.status == STATE_PLAYING ? '>' : ' ');
	printf("%s\n", s.path);
	return 0;
}

static int
show_status(struct imsg *imsg, int *ret)
{
	struct player_status s;
	size_t datalen;

	if (imsg->hdr.type == IMSG_CTL_ERR) {
		print_error_message("show failed", imsg);
		*ret = 1;
		return 1;
	}

	if (imsg->hdr.type != IMSG_CTL_STATUS)
		fatalx("%s: got wrong reply", __func__);

	datalen = IMSG_DATA_SIZE(*imsg);
	if (datalen != sizeof(s))
		fatalx("%s: data size mismatch", __func__);
	memcpy(&s, imsg->data, sizeof(s));
	if (s.path[sizeof(s.path)-1] != '\0')
		fatalx("%s: data corrupted?", __func__);

	switch (s.status) {
	case STATE_STOPPED:
		printf("stopped ");
		break;
	case STATE_PLAYING:
		printf("playing ");
		break;
	case STATE_PAUSED:
		printf("paused ");
		break;
	default:
		printf("unknown ");
		break;
	}

	printf("%s\n", s.path);
	printf("repeat one %s -- repeat all %s\n",
	    s.rp.repeat_one ? "on" : "off",
	    s.rp.repeat_all ? "on" : "off");
	return 1;
}

static int
show_load(struct parse_result *res, struct imsg *imsg, int *ret)
{
	FILE		*f;
	const char	*file;
	char		*line = NULL;
	char		 path[PATH_MAX];
	size_t		 linesize = 0;
	ssize_t		 linelen;
	int		 any = 0;

	if (imsg->hdr.type == IMSG_CTL_ERR) {
		print_error_message("load failed", imsg);
		*ret = 1;
		return 1;
	}

	if (imsg->hdr.type == IMSG_CTL_ADD)
		return 0;

	if (imsg->hdr.type == IMSG_CTL_COMMIT)
		return 1;

	if (imsg->hdr.type != IMSG_CTL_BEGIN)
		fatalx("got unexpected message %d", imsg->hdr.type);

	if (res->file == NULL)
		f = stdin;
	else if ((f = fopen(res->file, "r")) == NULL) {
		log_warn("can't open %s", res->file);
		*ret = 1;
		return 1;
	}

	while ((linelen = getline(&line, &linesize, f)) != -1) {
		if (linelen == 0)
			continue;
		line[linelen-1] = '\0';
		file = line;
		if (file[0] == '>' && file[1] == ' ')
			file += 2;
		if (file[0] == ' ' && file[1] == ' ')
			file += 2;

		memset(&path, 0, sizeof(path));
		if (realpath(file, path) == NULL) {
			log_warn("realpath %s", file);
			continue;
		}

		any++;
		imsg_compose(ibuf, IMSG_CTL_ADD, 0, 0, -1,
		    path, sizeof(path));
	}

	free(line);
	if (ferror(f))
		fatal("getline");
	fclose(f);

	if (!any) {
		*ret = 1;
		return 1;
	}

	imsg_compose(ibuf, IMSG_CTL_COMMIT, 0, 0, -1,
	    NULL, 0);
	imsg_flush(ibuf);
	return 0;
}

static int
ctlaction(struct parse_result *res)
{
	struct imsg imsg;
	ssize_t n;
	int ret = 0, done = 1;
	char **files;

	switch (res->action) {
	case PLAY:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_PLAY, 0, 0, -1, NULL, 0);
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case PAUSE:
		imsg_compose(ibuf, IMSG_CTL_PAUSE, 0, 0, -1, NULL, 0);
		break;
	case TOGGLE:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_TOGGLE_PLAY, 0, 0, -1, NULL, 0);
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case STOP:
		imsg_compose(ibuf, IMSG_CTL_STOP, 0, 0, -1, NULL, 0);
		break;
	case RESTART:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_RESTART, 0, 0, -1, NULL, 0);
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case ADD:
		done = 0;
		files = res->files;
		ret = enqueue_tracks(res->files);
		break;
	case FLUSH:
		imsg_compose(ibuf, IMSG_CTL_FLUSH, 0, 0, -1, NULL, 0);
		break;
	case SHOW:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
		break;
	case STATUS:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case NEXT:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_NEXT, 0, 0, -1, NULL, 0);
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case PREV:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_PREV, 0, 0, -1, NULL, 0);
		imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
		break;
	case LOAD:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_BEGIN, 0, 0, -1, NULL, 0);
		break;
	case JUMP:
		done = 0;
		ret = jump_req(res->file);
		break;
	case REPEAT:
		imsg_compose(ibuf, IMSG_CTL_REPEAT, 0, 0, -1,
		    &res->rep, sizeof(res->rep));
		break;
	case NONE:
		/* action not expected */
		fatalx("invalid action %u", res->action);
		break;
	}

	if (ret != 0)
		goto end;

	imsg_flush(ibuf);

	while (!done) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatalx("imsg_read error");
		if (n == 0)
			fatalx("pipe closed");

		while (!done) {
			if ((n = imsg_get(ibuf, &imsg)) == -1)
				fatalx("imsg_get error");
			if (n == 0)
				break;

			switch (res->action) {
			case ADD:
				done = show_add(&imsg, &ret, &files);
				break;
			case SHOW:
				done = show_complete(res, &imsg, &ret);
				break;
			case PLAY:
			case TOGGLE:
			case RESTART:
			case STATUS:
			case NEXT:
			case PREV:
			case JUMP:
				done = show_status(&imsg, &ret);
				break;
			case LOAD:
				done = show_load(res, &imsg, &ret);
				break;
			default:
				done = 1;
				break;
			}

			imsg_free(&imsg);
		}
	}

end:
	return ret;
}

int
ctl_noarg(struct parse_result *res, int argc, char **argv)
{
	if (argc != 1)
		ctl_usage(res->ctl);
	return ctlaction(res);
}

int
ctl_add(struct parse_result *res, int argc, char **argv)
{
	if (argc < 2)
		ctl_usage(res->ctl);
	res->files = argv+1;

	if (pledge("stdio rpath", NULL) == -1)
		fatal("pledge");

	return ctlaction(res);
}

int
ctl_show(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "p")) != -1) {
		switch (ch) {
		case 'p':
			res->pretty = 1;
			break;
		default:
			ctl_usage(res->ctl);
		}
	}

	return ctlaction(res);
}

int
ctl_load(struct parse_result *res, int argc, char **argv)
{
	if (argc < 2)
		res->file = NULL;
	else if (argc == 2)
		res->file = argv[1];
	else
		ctl_usage(res->ctl);

	if (pledge("stdio rpath", NULL) == -1)
		fatal("pledge");

	return ctlaction(res);
}

int
ctl_jump(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc != 1)
		ctl_usage(res->ctl);

	res->file = argv[0];
	return ctlaction(res);
}

int
ctl_repeat(struct parse_result *res, int argc, char **argv)
{
	int ch, b;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc != 2)
		ctl_usage(res->ctl);

	if (!strcmp(argv[1], "on"))
		b = 1;
	else if (!strcmp(argv[1], "off"))
		b = 0;
	else
		ctl_usage(res->ctl);

	res->rep.repeat_one = -1;
	res->rep.repeat_all = -1;
	if (!strcmp(argv[0], "one"))
		res->rep.repeat_one = b;
	else if (!strcmp(argv[0], "all"))
		res->rep.repeat_all = b;
	else
		ctl_usage(res->ctl);

	return ctlaction(res);
}

static int
sockconn(void)
{
	struct sockaddr_un	sun;
	int			sock, saved_errno;

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		fatal("socket");

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, csock, sizeof(sun.sun_path));
	if (connect(sock, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	return sock;
}

__dead void
ctl(int argc, char **argv)
{
	int ctl_sock, i = 0;

	log_init(1, LOG_DAEMON);
	log_setverbose(verbose);

	do {
		struct timespec	ts = { 0, 50000000 }; /* 0.05 seconds */

		if ((ctl_sock = sockconn()) != -1)
			break;
		if (errno != ENOENT && errno != ECONNREFUSED)
			fatal("connect %s", csock);

		if (i == 0)
			spawn_daemon();

		nanosleep(&ts, NULL);
	} while (++i < 20);

	if (ctl_sock == -1)
		fatalx("failed to connect to the daemon");

	ibuf = xmalloc(sizeof(*ibuf));
	imsg_init(ibuf, ctl_sock);

	optreset = 1;
	optind = 1;

	exit(parse(argc, argv));
}
