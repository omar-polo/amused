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
#include "xmalloc.h"

static struct imsgbuf *ibuf;

int	ctl_noarg(struct parse_result *, int, char **);
int	ctl_add(struct parse_result *, int, char **);

struct ctl_command ctl_commands[] = {
	{ "play",	PLAY,		ctl_noarg,	"" },
	{ "pause",	PAUSE,		ctl_noarg,	"" },
	{ "toggle",	TOGGLE,		ctl_noarg,	"" },
	{ "stop",	STOP,		ctl_noarg,	"" },
	{ "restart",	RESTART,	ctl_noarg,	"" },
	{ "add",	ADD,		ctl_add,	"files...", 1 },
	{ "flush",	FLUSH,		ctl_noarg,	"" },
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
show_complete(struct imsg *imsg, int *ret)
{
	size_t datalen;
	char path[PATH_MAX];

	datalen = IMSG_DATA_SIZE(*imsg);
	if (datalen == 0)
		return 1;

	if (datalen != sizeof(path))
		fatalx("%s: data size mismatch", __func__);
	memcpy(path, imsg->data, sizeof(path));
	if (path[datalen-1] != '\0')
		fatalx("%s: data corrupted?", __func__);

	printf("%s\n", path);
	return 0;
}

static int
ctlaction(struct parse_result *res)
{
	struct imsg imsg;
	ssize_t n;
	int ret = 0, done = 1;

	switch (res->action) {
	case PLAY:
		imsg_compose(ibuf, IMSG_CTL_PLAY, 0, 0, -1, NULL, 0);
		break;
	case PAUSE:
		imsg_compose(ibuf, IMSG_CTL_PAUSE, 0, 0, -1, NULL, 0);
		break;
	case TOGGLE:
		imsg_compose(ibuf, IMSG_CTL_TOGGLE_PLAY, 0, 0, -1, NULL, 0);
		break;
	case STOP:
		imsg_compose(ibuf, IMSG_CTL_STOP, 0, 0, -1, NULL, 0);
		break;
	case RESTART:
		imsg_compose(ibuf, IMSG_CTL_RESTART, 0, 0, -1, NULL, 0);
		break;
	case ADD:
		ret = enqueue_tracks(res->files);
		break;
	case FLUSH:
		imsg_compose(ibuf, IMSG_CTL_FLUSH, 0, 0, -1, NULL, 0);
		break;
	case SHOW:
		done = 0;
		imsg_compose(ibuf, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
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
			case SHOW:
				done = show_complete(&imsg, &ret);
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
	return ctlaction(res);
}

__dead void
ctl(int argc, char **argv)
{
	struct sockaddr_un	 sun;
	int			 ctl_sock;

	log_init(1, LOG_DAEMON);
	log_setverbose(verbose);

	if ((ctl_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		fatal("socket");

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, csock, sizeof(sun.sun_path));

	if (connect(ctl_sock, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		fatal("connect %s", csock);

	ibuf = xmalloc(sizeof(*ibuf));
	imsg_init(ibuf, ctl_sock);

	optreset = 1;
	optind = 1;

	exit(parse(argc, argv));
}
