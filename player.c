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
	if (nextfd != -1)
		fatalx("track already enqueued");

	if ((nextfd = imsg->fd) == -1)
		fatalx("%s: got invalid file descriptor", __func__);
	log_debug("song enqueued");
}

/* process only one message */
int
player_dispatch(void)
{
	struct pollfd	pfd;
	struct imsg	imsg;
	ssize_t		n;
	int		ret;

	if (halted != 0)
		return IMSG_STOP;

	pfd.fd = ibuf->fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, INFTIM) == -1)
		fatal("poll");

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

int
player_playnext(void)
{
	static char buf[512];
	ssize_t r;
	int fd = nextfd;

	assert(nextfd != -1);
	nextfd = -1;

	r = read(fd, buf, sizeof(buf));

	/* 8 byte is the larger magic number */
	if (r < 8) {
		log_warn("read failed");
		goto err;
	}

	if (lseek(fd, 0, SEEK_SET) == -1) {
		log_warn("lseek failed");
		goto err;
	}

	if (memcmp(buf, "fLaC", 4) == 0)
		return play_flac(fd);
	if (memcmp(buf, "ID3", 3) == 0 ||
	    memcmp(buf, "\xFF\xFB", 2) == 0)
		return play_mp3(fd);
	if (memmem(buf, r, "OpusHead", 8) != NULL)
		return play_opus(fd);
	if (memmem(buf, r, "OggS", 4) != NULL)
		return play_oggvorbis(fd);

	log_warnx("unknown file type");
err:
	close(fd);
	return -1;
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
	int r;

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

		r = player_playnext();
		if (r == -1)
			player_senderr();
		if (r == 0)
			player_sendeof();
	}

	return 0;
}
