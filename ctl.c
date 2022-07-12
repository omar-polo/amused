/*
 * Copyright (c) 2022 Omar Polo <op@openbsd.org>
 * Copyright (c) 2015 Theo de Raadt <deraadt@openbsd.org>
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
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "log.h"
#include "playlist.h"
#include "xmalloc.h"

static struct imsgbuf	*ibuf;
char			 cwd[PATH_MAX];

static int	ctl_noarg(struct parse_result *, int, char **);
static int	ctl_add(struct parse_result *, int, char **);
static int	ctl_show(struct parse_result *, int, char **);
static int	ctl_load(struct parse_result *, int, char **);
static int	ctl_jump(struct parse_result *, int, char **);
static int	ctl_repeat(struct parse_result *, int, char **);
static int	ctl_monitor(struct parse_result *, int, char **);
static int	ctl_seek(struct parse_result *, int, char **);
static int	ctl_status(struct parse_result *, int, char **);

struct ctl_command ctl_commands[] = {
	{ "add",	ADD,		ctl_add,	"files...", 0 },
	{ "flush",	FLUSH,		ctl_noarg,	"", 0 },
	{ "jump",	JUMP,		ctl_jump,	"pattern", 0 },
	{ "load",	LOAD,		ctl_load,	"[file]", 1 },
	{ "monitor",	MONITOR,	ctl_monitor,	"[events]", 0 },
	{ "next",	NEXT,		ctl_noarg,	"", 0 },
	{ "pause",	PAUSE,		ctl_noarg,	"", 0 },
	{ "play",	PLAY,		ctl_noarg,	"", 0 },
	{ "prev",	PREV,		ctl_noarg,	"", 0 },
	{ "repeat",	REPEAT,		ctl_repeat,	"one|all on|off", 0 },
	{ "restart",	RESTART,	ctl_noarg,	"", 0 },
	{ "seek",	SEEK,		ctl_seek,	"[+-]time[%]", 0 },
	{ "show",	SHOW,		ctl_show,	"[-p]", 0 },
	{ "status",	STATUS,		ctl_status,	"[-f fmt]", 0 },
	{ "stop",	STOP,		ctl_noarg,	"", 0 },
	{ "toggle",	TOGGLE,		ctl_noarg,	"", 0 },
	{ NULL, 0, NULL, NULL, 0 },
};

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-dv] [-s socket]\n", getprogname());
	exit(1);
}

static __dead void
ctl_usage(struct ctl_command *ctl)
{
	fprintf(stderr, "usage: %s [-v] [-s socket] %s %s\n", getprogname(),
	    ctl->name, ctl->usage);
	exit(1);
}

/* based on canonpath from kern_pledge.c */
static int
canonpath(const char *input, char *buf, size_t bufsize)
{
	const char *p;
	char *q, path[PATH_MAX];

	if (input[0] != '/') {
		if (snprintf(path, sizeof(path), "%s/%s", cwd, input)
		    >= sizeof(path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		input = path;
	}

	p = input;
	q = buf;
	while (*p && (q - buf < bufsize)) {
		if (p[0] == '/' && (p[1] == '/' || p[1] == '\0')) {
			p += 1;

		} else if (p[0] == '/' && p[1] == '.' &&
		    (p[2] == '/' || p[2] == '\0')) {
			p += 2;

		} else if (p[0] == '/' && p[1] == '.' && p[2] == '.' &&
		    (p[3] == '/' || p[3] == '\0')) {
			p += 3;
			if (q != buf)	/* "/../" at start of buf */
				while (*--q != '/')
					continue;

		} else {
			*q++ = *p++;
		}
	}
	if ((*p == '\0') && (q - buf < bufsize)) {
		*q = 0;
		return 0;
	} else {
		errno = ENAMETOOLONG;
		return -1;
	}
}

static int
parse(struct parse_result *res, int argc, char **argv)
{
	struct ctl_command	*ctl = NULL;
	const char		*argv0;
	int			 i, status;

	if ((argv0 = argv[0]) == NULL)
		argv0 = "status";

	for (i = 0; ctl_commands[i].name != NULL; ++i) {
		if (strncmp(ctl_commands[i].name, argv0, strlen(argv0))
		    == 0) {
			if (ctl != NULL) {
				fprintf(stderr,
				    "ambiguous argument: %s\n", argv0);
				usage();
			}
			ctl = &ctl_commands[i];
		}
	}

	if (ctl == NULL) {
		fprintf(stderr, "unknown argument: %s\n", argv[0]);
		usage();
	}

	res->action = ctl->action;
	res->ctl = ctl;

	if (!ctl->has_pledge) {
		/* pledge(2) default if command doesn't have its own */
		if (pledge("stdio", NULL) == -1)
			fatal("pledge");
	}

	status = ctl->main(res, argc, argv);
	close(ibuf->fd);
	free(ibuf);
	return status;
}

static int
load_files(struct parse_result *res, int *ret)
{
	FILE		*f;
	const char	*file;
	char		*line = NULL;
	char		 path[PATH_MAX];
	size_t		 linesize = 0, i = 0;
	ssize_t		 linelen, curr = -1;

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
		if (!strncmp(file, "> ", 2)) {
			file += 2;
			curr = i;
		} else if (!strncmp(file, "  ", 2))
			file += 2;

		memset(path, 0, sizeof(path));
		if (canonpath(file, path, sizeof(path)) == -1) {
			log_warn("canonpath %s", file);
			continue;
		}

		i++;
		imsg_compose(ibuf, IMSG_CTL_ADD, 0, 0, -1,
		    path, sizeof(path));
	}

	free(line);
	if (ferror(f))
		fatal("getline");
	fclose(f);

	if (i == 0) {
		*ret = 1;
		return 1;
	}

	imsg_compose(ibuf, IMSG_CTL_COMMIT, 0, 0, -1,
	    &curr, sizeof(curr));
	imsg_flush(ibuf);
	return 0;
}

static const char *
imsg_strerror(struct imsg *imsg)
{
	size_t datalen;
	const char *msg;

	datalen = IMSG_DATA_SIZE(*imsg);
	msg = imsg->data;
	if (datalen == 0 || msg[datalen-1] != '\0')
		fatalx("malformed error message");

	return msg;
}

static const char *
event_name(int type)
{
	switch (type) {
	case IMSG_CTL_PLAY:
		return "play";
	case IMSG_CTL_PAUSE:
		return "pause";
	case IMSG_CTL_STOP:
		return "stop";
	case IMSG_CTL_NEXT:
		return "next";
	case IMSG_CTL_PREV:
		return "prev";
	case IMSG_CTL_JUMP:
		return "jump";
	case IMSG_CTL_ADD:
		return "add";
	case IMSG_CTL_COMMIT:
		return "load";
	case IMSG_CTL_SEEK:
		return "seek";
	default:
		return "unknown";
	}
}

static void
print_time(const char *label, int64_t seconds, const char *suffx)
{
	int hours, minutes;

	if (seconds < 0)
		seconds = 0;

	hours = seconds / 3600;
	seconds -= hours * 3600;

	minutes = seconds / 60;
	seconds -= minutes * 60;

	printf("%s", label);
	if (hours != 0)
		printf("%02d:", hours);
	printf("%02d:%02lld%s", minutes, (long long)seconds, suffx);
}

static void
print_status(struct player_status *ps, const char *spec)
{
	const char *status;
	double percent;
	char *dup, *tmp, *tok;

	if (ps->status == STATE_STOPPED)
		status = "stopped";
	else if (ps->status == STATE_PLAYING)
		status = "playing";
	else if (ps->status == STATE_PAUSED)
		status = "paused";
	else
		status = "unknown";

	percent = 100.0 * (double)ps->position / (double)ps->duration;

	tmp = dup = xstrdup(spec);
	while ((tok = strsep(&tmp, ",")) != NULL) {
		if (*tok == '\0')
			continue;

		if (!strcmp(tok, "path")) {
			puts(ps->path);
		} else if (!strcmp(tok, "repeat:oneline")) {
			printf("repeat one:%s ",
			    ps->rp.repeat_one ? "on" : "off");
			printf("all:%s\n", ps->rp.repeat_all ? "on" : "off");
		} else if (!strcmp(tok, "repeat")) {
			printf("repeat one %s\n",
			    ps->rp.repeat_one ? "on" : "off");
			printf("repeat all %s\n",
			    ps->rp.repeat_all ? "on" : "off");
		} else if (!strcmp(tok, "status")) {
			printf("%s %s\n", status, ps->path);
		} else if (!strcmp(tok, "time:oneline")) {
			print_time("time ", ps->position, " / ");
			print_time("", ps->duration, "\n");
		} else if (!strcmp(tok, "time:percentage")) {
			printf("position %.2f%%\n", percent);
		} else if (!strcmp(tok, "time:raw")) {
			printf("position %lld\n", (long long)ps->position);
			printf("duration %lld\n", (long long)ps->duration);
		} else if (!strcmp(tok, "time")) {
			print_time("position ", ps->position, "\n");
			print_time("duration ", ps->duration, "\n");
		}
	}

	free(dup);
}

static int
ctlaction(struct parse_result *res)
{
	char path[PATH_MAX];
	struct imsg imsg;
	struct player_status ps;
	size_t datalen;
	ssize_t n;
	int i, type, ret = 0, done = 1;

	switch (res->action) {
	case PLAY:
		imsg_compose(ibuf, IMSG_CTL_PLAY, 0, 0, -1, NULL, 0);
		if (verbose) {
			imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1,
			    NULL, 0);
			done = 0;
		}
		break;
	case PAUSE:
		imsg_compose(ibuf, IMSG_CTL_PAUSE, 0, 0, -1, NULL, 0);
		break;
	case TOGGLE:
		imsg_compose(ibuf, IMSG_CTL_TOGGLE_PLAY, 0, 0, -1, NULL, 0);
		if (verbose) {
			imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1,
			    NULL, 0);
			done = 0;
		}
		break;
	case STOP:
		imsg_compose(ibuf, IMSG_CTL_STOP, 0, 0, -1, NULL, 0);
		break;
	case ADD:
		done = 0;
		for (i = 0; res->files[i] != NULL; ++i) {
			memset(path, 0, sizeof(path));
			if (canonpath(res->files[i], path, sizeof(path))
			    == -1) {
				log_warn("canonpath %s", res->files[i]);
				continue;
			}

			imsg_compose(ibuf, IMSG_CTL_ADD, 0, 0, -1,
			    path, sizeof(path));
		}
		ret = i == 0;
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
		imsg_compose(ibuf, IMSG_CTL_NEXT, 0, 0, -1, NULL, 0);
		if (verbose) {
			imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1,
			    NULL, 0);
			done = 0;
		}
		break;
	case PREV:
		imsg_compose(ibuf, IMSG_CTL_PREV, 0, 0, -1, NULL, 0);
		if (verbose) {
			imsg_compose(ibuf, IMSG_CTL_STATUS, 0, 0, -1,
			    NULL, 0);
			done = 0;
		}
		break;
	case LOAD:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_BEGIN, 0, 0, -1, NULL, 0);
		break;
	case JUMP:
		done = 0;
		memset(path, 0, sizeof(path));
		strlcpy(path, res->file, sizeof(path));
		imsg_compose(ibuf, IMSG_CTL_JUMP, 0, 0, -1,
		    path, sizeof(path));
		break;
	case REPEAT:
		imsg_compose(ibuf, IMSG_CTL_REPEAT, 0, 0, -1,
		    &res->rep, sizeof(res->rep));
		break;
	case MONITOR:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_MONITOR, 0, 0, -1,
		    NULL, 0);
		break;
	case RESTART:
		memset(&res->seek, 0, sizeof(res->seek));
		/* fallthrough */
	case SEEK:
		imsg_compose(ibuf, IMSG_CTL_SEEK, 0, 0, -1, &res->seek,
		    sizeof(res->seek));
		break;
	case NONE:
		/* action not expected */
		fatalx("invalid action %u", res->action);
		break;
	}

	if (ret != 0)
		goto end;

	imsg_flush(ibuf);

	i = 0;
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

			if (imsg.hdr.type == IMSG_CTL_ERR) {
				log_warnx("%s: %s", res->ctl->name,
				    imsg_strerror(&imsg));
				ret = 1;
				done = 1;
				break;
			}

			datalen = IMSG_DATA_SIZE(imsg);

			switch (res->action) {
			case ADD:
				if (res->files[i] == NULL)
					fatalx("received more replies than "
					    "files enqueued.");

				if (imsg.hdr.type == IMSG_CTL_ADD)
					log_debug("enqueued %s", res->files[i]);
				else
					fatalx("invalid message %d",
					    imsg.hdr.type);
				i++;
				done = res->files[i] == NULL;
				break;
			case SHOW:
				if (datalen == 0) {
					done = 1;
					break;
				}
				if (datalen != sizeof(ps))
					fatalx("data size mismatch");
				memcpy(&ps, imsg.data, sizeof(ps));
				if (ps.path[sizeof(ps.path) - 1] != '\0')
					fatalx("received corrupted data");
				if (res->pretty) {
					char c = ' ';
					if (ps.status == STATE_PLAYING)
						c = '>';
					printf("%c ", c);
				}
				puts(ps.path);
				break;
			case PLAY:
			case TOGGLE:
			case RESTART:
			case STATUS:
			case NEXT:
			case PREV:
			case JUMP:
				if (imsg.hdr.type != IMSG_CTL_STATUS)
					fatalx("invalid message %d",
					    imsg.hdr.type);

				if (datalen != sizeof(ps))
					fatalx("data size mismatch");
				memcpy(&ps, imsg.data, sizeof(ps));
				if (ps.path[sizeof(ps.path) - 1] != '\0')
					fatalx("received corrupted data");

				print_status(&ps, res->status_format);
				done = 1;
				break;
			case LOAD:
				if (imsg.hdr.type == IMSG_CTL_ADD)
					break;
				if (imsg.hdr.type == IMSG_CTL_COMMIT) {
					done = 1;
					break;
				}

				if (imsg.hdr.type != IMSG_CTL_BEGIN)
					fatalx("invalid message %d",
					    imsg.hdr.type);

				load_files(res, &ret);
				break;
			case MONITOR:
				if (imsg.hdr.type != IMSG_CTL_MONITOR)
					fatalx("invalid message %d",
					    imsg.hdr.type);

				if (datalen != sizeof(type))
					fatalx("data size mismatch");

				memcpy(&type, imsg.data, sizeof(type));
				if (type < 0 || type > IMSG__LAST)
					fatalx("received corrupted data");

				if (!res->monitor[type])
					break;

				puts(event_name(type));
				fflush(stdout);
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

static int
ctl_noarg(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc > 0)
		ctl_usage(res->ctl);

	return ctlaction(res);
}

static int
ctl_add(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc == 0)
		ctl_usage(res->ctl);
	res->files = argv;

	return ctlaction(res);
}

static int
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

static int
ctl_load(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc == 0)
		res->file = NULL;
	else if (argc == 1)
		res->file = argv[0];
	else
		ctl_usage(res->ctl);

	if (pledge("stdio rpath", NULL) == -1)
		fatal("pledge");

	return ctlaction(res);
}

static int
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

static int
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
ctl_monitor(struct parse_result *res, int argc, char **argv)
{
	int ch, n = 0;
	const char *events;
	char *dup, *tmp, *tok;

	while ((ch = getopt(argc, argv, "")) != -1)
		ctl_usage(res->ctl);
	argc -= optind;
	argv += optind;

	if (argc > 1)
		ctl_usage(res->ctl);

	events = "play,pause,stop,next,prev,jump,repeat,add,load,seek";
	if (argc == 1)
		events = *argv;

	tmp = dup = xstrdup(events);
	while ((tok = strsep(&tmp, ",")) != NULL) {
		if (*tok == '\0')
			continue;

		n++;
		if (!strcmp(tok, "play"))
			res->monitor[IMSG_CTL_PLAY] = 1;
		else if (!strcmp(tok, "pause"))
			res->monitor[IMSG_CTL_PAUSE] = 1;
		else if (!strcmp(tok, "stop"))
			res->monitor[IMSG_CTL_STOP] = 1;
		else if (!strcmp(tok, "next"))
			res->monitor[IMSG_CTL_NEXT] = 1;
		else if (!strcmp(tok, "prev"))
			res->monitor[IMSG_CTL_PREV] = 1;
		else if (!strcmp(tok, "jump"))
			res->monitor[IMSG_CTL_JUMP] = 1;
		else if (!strcmp(tok, "repeat"))
			res->monitor[IMSG_CTL_REPEAT] = 1;
		else if (!strcmp(tok, "add"))
			res->monitor[IMSG_CTL_ADD] = 1;
		else if (!strcmp(tok, "load"))
			res->monitor[IMSG_CTL_COMMIT] = 1;
		else if (!strcmp(tok, "seek"))
			res->monitor[IMSG_CTL_SEEK] = 1;
		else {
			log_warnx("unknown event \"%s\"", tok);
			n--;
		}
	}

	free(dup);
	if (n == 0)
		ctl_usage(res->ctl);
	return ctlaction(res);
}

static int
ctl_seek(struct parse_result *res, int argc, char **argv)
{
	const char *n;
	char *ep;
	int hours = 0, minutes = 0, seconds = 0;
	int sign = 1;

	if (argc > 0) {
		/* skip the command name */
		argc--;
		argv++;
	}

	if (argc > 0 && !strcmp(*argv, "--")) {
		argc--;
		argv++;
	}

	if (argc != 1)
		ctl_usage(res->ctl);

	n = *argv;
	if (*n == '-' || *n == '+')
		res->seek.relative = 1;
	if (*n == '-') {
		n++;
		sign = -1;
	}

	seconds = strtol(n, &ep, 10);
	if (n[0] == '\0' ||
	    (*ep != '\0' && *ep != ':' && *ep != '%') ||
	    (*ep == '%' && ep[1] != '\0'))
		fatalx("invalid offset: %s", argv[0]);
	if (*ep == '\0' || *ep == '%') {
		res->seek.percent = *ep == '%';
		goto done;
	}

	n = ++ep;
	minutes = seconds;
	seconds = strtol(n, &ep, 10);
	if (n[0] == '\0' || (*ep != '\0' && *ep != ':'))
		fatalx("invalid offset: %s", argv[0]);
	if (*ep == '\0')
		goto done;

	n = ++ep;
	hours = minutes;
	minutes = seconds;
	seconds = strtol(n, &ep, 10);
	if (n[0] == '\0' || *ep != '\0')
		fatalx("invalid offset: %s", argv[0]);

done:
	res->seek.offset = sign * (hours * 3600 + minutes * 60 + seconds);
	return ctlaction(res);
}

static int
ctl_status(struct parse_result *res, int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "f:")) != -1) {
		switch (ch) {
		case 'f':
			res->status_format = optarg;
			break;
		default:
			ctl_usage(res->ctl);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0)
		ctl_usage(res->ctl);

	return ctlaction(res);
}

static int
ctl_get_lock(const char *lockfile)
{
	int lockfd;

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return -1;
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return -1;
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nop */;
		close(lockfd);
		return -2;
	}
	log_debug("flock succeeded");

	return lockfd;
}

static int
ctl_connect(void)
{
	struct timespec		 ts = { 0, 50000000 }; /* 0.05 seconds */
	struct sockaddr_un	 sa;
	size_t			 size;
	int			 fd, lockfd = -1, locked = 0, spawned = 0;
	int			 attempt = 0;
	char			*lockfile = NULL;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, csock, sizeof(sa.sun_path));
	if (size >= sizeof(sa.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

retry:
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		log_debug("connection failed: %s", strerror(errno));
		if (errno != ECONNREFUSED && errno != ENOENT)
			goto failed;
		if (attempt++ == 100)
			goto failed;
		close(fd);

		if (!locked) {
			xasprintf(&lockfile, "%s.lock", csock);
			if ((lockfd = ctl_get_lock(lockfile)) < 0) {
				log_debug("didn't get the lock (%d)", lockfd);

				free(lockfile);
				lockfile = NULL;

				if (lockfd == -1)
					goto retry;
			}

			/*
			 * Always retry at least once, even if we got
			 * the lock, because another client could have
			 * taken the lock, started the server and released
			 * the lock between our connect() and flock()
			 */
			locked = 1;
			goto retry;
		}

		if (!spawned) {
			log_debug("spawning the daemon");
			spawn_daemon();
			spawned = 1;
		}

		nanosleep(&ts, NULL);
		goto retry;
	}

	if (locked && lockfd >= 0) {
		unlink(lockfile);
		free(lockfile);
		close(lockfd);
	}
	return fd;

failed:
	if (locked) {
		free(lockfile);
		close(lockfd);
	}
	close(fd);
	return -1;
}

__dead void
ctl(int argc, char **argv)
{
	struct parse_result res;
	const char *fmt;
	int ctl_sock;

	memset(&res, 0, sizeof(res));
	if ((fmt = getenv("AMUSED_STATUS_FORMAT")) == NULL)
		fmt = "status,time,repeat";
	res.status_format = fmt;

	log_init(1, LOG_DAEMON);
	log_setverbose(verbose);

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		fatal("getcwd");

	if ((ctl_sock = ctl_connect()) == -1)
		fatal("can't connect");

	if (ctl_sock == -1)
		fatalx("failed to connect to the daemon");

	ibuf = xmalloc(sizeof(*ibuf));
	imsg_init(ibuf, ctl_sock);

	optreset = 1;
	optind = 1;

	exit(parse(&res, argc, argv));
}
