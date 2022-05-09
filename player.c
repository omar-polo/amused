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
#include <sys/queue.h>
#include <sys/uio.h>

#include <limits.h>

#include <assert.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <imsg.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "log.h"
#include "xmalloc.h"

struct sio_hdl		*hdl;
static struct imsgbuf	*ibuf;

static int nextfd = -1;
static char nextpath[PATH_MAX];

volatile sig_atomic_t halted;

static void
player_signal_handler(int signo)
{
	halted = 1;
}

static void
audio_init(void)
{
	struct sio_par par;

	if ((hdl = sio_open(SIO_DEVANY, SIO_PLAY, 0)) == NULL)
		fatal("sio_open");

	sio_initpar(&par);
	par.bits = 16;
	par.appbufsz = 50 * par.rate / 1000;
	par.pchan = 2;
	if (!sio_setpar(hdl, &par) || !sio_getpar(hdl, &par))
		fatal("couldn't set audio params");
	if (par.bits != 16 || par.le != SIO_LE_NATIVE)
		fatalx("unsupported audio params");
	if (!sio_start(hdl))
		fatal("sio_start");
}

int
player_setup(int bits, int rate, int channels)
{
	struct sio_par par;

	log_debug("%s: bits=%d, rate=%d, channels=%d", __func__,
	    bits, rate, channels);

	sio_stop(hdl);

	sio_initpar(&par);
	par.bits = bits;
	par.rate = rate;
	par.pchan = channels;
	if (!sio_setpar(hdl, &par) || !sio_getpar(hdl, &par)) {
		log_warnx("invalid params (bits=%d, rate=%d, channels=%d",
		    bits, rate, channels);
		return -1;
	}

	if (par.bits != bits || par.pchan != channels) {
		log_warnx("failed to set params");
		return -1;
	}

	/* TODO: check the sample rate? */

	if (!sio_start(hdl)) {
		log_warn("sio_start");
		return -1;
	}
	return 0;
}

int
player_pendingimsg(void)
{
	struct pollfd pfd;
	int r;

	if (halted != 0)
		return 1;

	pfd.fd = ibuf->fd;
	pfd.events = POLLIN;

	r = poll(&pfd, 1, 0);
	if (r == -1)
		fatal("poll");
	return r;
}

void
player_enqueue(struct imsg *imsg)
{
	size_t datalen;

	if (nextfd != -1)
		fatalx("track already enqueued");

	datalen = IMSG_DATA_SIZE(*imsg);
	if (datalen != sizeof(nextpath))
		fatalx("%s: size mismatch", __func__);
	memcpy(nextpath, imsg->data, sizeof(nextpath));
	if (nextpath[datalen-1] != '\0')
		fatalx("%s: corrupted path", __func__);
	if ((nextfd = imsg->fd) == -1)
		fatalx("%s: got invalid file descriptor", __func__);
	log_debug("enqueued %s", nextpath);
}

/* process only one message */
int
player_dispatch(void)
{
	struct imsg	imsg;
	ssize_t		n;
	int		ret;

	if (halted != 0)
		return IMSG_STOP;

	if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
		fatalx("imsg_read");
	if (n == 0)
		fatalx("pipe closed");

	if ((n = imsg_get(ibuf, &imsg)) == -1)
		fatal("imsg_get");
	if (n == 0) /* no more messages */
		fatalx("expected at least a message");

	ret = imsg.hdr.type;
	switch (imsg.hdr.type) {
	case IMSG_PLAY:
		player_enqueue(&imsg);
		ret = IMSG_STOP;
		break;
	case IMSG_RESUME:
	case IMSG_PAUSE:
	case IMSG_STOP:
		break;
	default:
		fatalx("unknown imsg %d", imsg.hdr.type);
	}

	return ret;
}

void
player_senderr(void)
{
	imsg_compose(ibuf, IMSG_ERR, 0, 0, -1, NULL, 0);
	imsg_flush(ibuf);
}

void
player_sendeof(void)
{
	imsg_compose(ibuf, IMSG_EOF, 0, 0, -1, NULL, 0);
	imsg_flush(ibuf);
}

void
player_playnext(void)
{
	int fd = nextfd;
	int r;

	assert(nextfd != -1);
	nextfd = -1;

	/* XXX: use magic(5) for this, not file extensions */
	if (strstr(nextpath, ".ogg") != NULL)
		r = play_oggvorbis(fd);
	else if (strstr(nextpath, ".mp3") != NULL)
		r = play_mp3(fd);
	else if (strstr(nextpath, ".flac") != NULL)
		r = play_flac(fd);
	else if (strstr(nextpath, ".opus") != NULL)
		r = play_opus(fd);
	else {
		log_warnx("unknown file type for %s", nextpath);
		r = -1;
	}

	if (r == -1)
		player_senderr();
	else if (r == 0)
		player_sendeof();
}

int
player_pause(void)
{
	int r;

	r = player_dispatch();
	return r == IMSG_RESUME;
}

int
player_shouldstop(void)
{
	if (!player_pendingimsg())
		return 0;

	switch (player_dispatch()) {
	case IMSG_PAUSE:
		if (player_pause())
			break;
		/* fallthrough */
	case IMSG_STOP:
		return 1;
	}

	return 0;
}

int
play(const void *buf, size_t len)
{
	if (player_shouldstop())
		return 0;
	sio_write(hdl, buf, len);
	return 1;
}

int
player(int debug, int verbose)
{
	int flags;
	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	setproctitle("player");
	log_procinit("player");

#if 0
	{
		static int attached;

		while (!attached)
			sleep(1);
	}
#endif

	audio_init();

	/* mark fd as blocking i/o mode */
	if ((flags = fcntl(3, F_GETFL)) == -1)
		fatal("fcntl(F_GETFL)");
	if (fcntl(3, F_SETFL, flags & ~O_NONBLOCK) == -1)
		fatal("fcntl F_SETFL O_NONBLOCK");

	ibuf = xmalloc(sizeof(*ibuf));
	imsg_init(ibuf, 3);

	signal(SIGINT, player_signal_handler);
	signal(SIGTERM, player_signal_handler);

	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	if (pledge("stdio recvfd audio", NULL) == -1)
		fatal("pledge");

	while (!halted) {
		while (nextfd == -1)
			player_dispatch();
		player_playnext();
	}

	return 0;
}
