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

#include "config.h"

#include <limits.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "log.h"
#include "xmalloc.h"

struct sio_hdl		*hdl;
struct sio_par		 par;
struct pollfd		*player_pfds;
static struct imsgbuf	*ibuf;

static int stopped = 1;
static int nextfd = -1;
static int64_t samples;
static int64_t duration;

volatile sig_atomic_t halted;

static void
player_signal_handler(int signo)
{
	halted = 1;
}

int
player_setup(unsigned int bits, unsigned int rate, unsigned int channels)
{
	int nfds, fpct;

	log_debug("%s: bits=%u, rate=%u, channels=%u", __func__,
	    bits, rate, channels);

	fpct = (rate*5)/100;

	/* don't stop if the parameters are the same */
	if (bits == par.bits && channels == par.pchan &&
	    par.rate - fpct <= rate && rate <= par.rate + fpct) {
		if (stopped)
			goto start;
		return 0;
	}

again:
	if (!stopped) {
		sio_stop(hdl);
		stopped = 1;
	}

	sio_initpar(&par);
	par.bits = bits;
	par.rate = rate;
	par.pchan = channels;
	if (!sio_setpar(hdl, &par)) {
		if (errno == EAGAIN) {
			nfds = sio_pollfd(hdl, player_pfds + 1, POLLOUT);
			if (poll(player_pfds + 1, nfds, INFTIM) == -1)
				fatal("poll");
			goto again;
		}
		log_warnx("invalid params (bits=%d, rate=%d, channels=%d",
		    bits, rate, channels);
		return -1;
	}
	if (!sio_getpar(hdl, &par)) {
		log_warnx("can't get params");
		return -1;
	}

	if (par.bits != bits || par.pchan != channels) {
		log_warnx("failed to set params");
		return -1;
	}

	/* TODO: check the sample rate? */

start:
	if (!sio_start(hdl)) {
		log_warn("sio_start");
		return -1;
	}
	stopped = 0;
	return 0;
}

void
player_setduration(int64_t d)
{
	int64_t seconds;

	duration = d;
	seconds = duration / par.rate;
	imsg_compose(ibuf, IMSG_LEN, 0, 0, -1, &seconds, sizeof(seconds));
	imsg_flush(ibuf);
}

static void
player_onmove(void *arg, int delta)
{
	static int64_t reported;
	int64_t sec;

	samples += delta;
	if (llabs(samples - reported) >= par.rate) {
		reported = samples;
		sec = samples / par.rate;

		imsg_compose(ibuf, IMSG_POS, 0, 0, -1, &sec, sizeof(sec));
		imsg_flush(ibuf);
	}
}

void
player_setpos(int64_t pos)
{
	samples = pos;
	player_onmove(NULL, 0);
}

/* process only one message */
static int
player_dispatch(int64_t *s, int wait)
{
	struct player_seek seek;
	struct pollfd	pfd;
	struct imsg	imsg;
	ssize_t		n;
	int		ret;

	if (halted != 0)
		return IMSG_STOP;

again:
	if ((n = imsg_get(ibuf, &imsg)) == -1)
		fatal("imsg_get");
	if (n == 0) {
		if (!wait)
			return -1;

		pfd.fd = ibuf->fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, INFTIM) == -1)
			fatal("poll");
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");
		goto again;
	}

	ret = imsg.hdr.type;
	switch (imsg.hdr.type) {
	case IMSG_PLAY:
		if (nextfd != -1)
			fatalx("track already enqueued");
		if ((nextfd = imsg.fd) == -1)
			fatalx("%s: got invalid file descriptor", __func__);
		log_debug("song enqueued");
		ret = IMSG_STOP;
		break;
	case IMSG_RESUME:
	case IMSG_PAUSE:
	case IMSG_STOP:
		break;
	case IMSG_CTL_SEEK:
		if (s == NULL)
			break;
		if (IMSG_DATA_SIZE(imsg) != sizeof(seek))
			fatalx("wrong size for seek ctl");
		memcpy(&seek, imsg.data, sizeof(seek));
		if (seek.percent) {
			*s = (double)seek.offset * (double)duration / 100.0;
		} else {
			*s = seek.offset * par.rate;
			if (seek.relative)
				*s += samples;
		}
		if (*s < 0)
			*s = 0;
		break;
	default:
		fatalx("unknown imsg %d", imsg.hdr.type);
	}

	imsg_free(&imsg);
	return ret;
}

static void
player_senderr(const char *errstr)
{
	size_t len = 0;

	if (errstr != NULL)
		len = strlen(errstr) + 1;

	imsg_compose(ibuf, IMSG_ERR, 0, 0, -1, errstr, len);
	imsg_flush(ibuf);
}

static void
player_sendeof(void)
{
	imsg_compose(ibuf, IMSG_EOF, 0, 0, -1, NULL, 0);
	imsg_flush(ibuf);
}

static int
player_playnext(const char **errstr)
{
	static char buf[512];
	ssize_t r;
	int fd = nextfd;

	assert(nextfd != -1);
	nextfd = -1;

	/* reset samples and set position to zero */
	samples = 0;
	imsg_compose(ibuf, IMSG_POS, 0, 0, -1, &samples, sizeof(samples));
	imsg_flush(ibuf);

	r = read(fd, buf, sizeof(buf));

	/* 8 byte is the larger magic number */
	if (r < 8) {
		*errstr = "read failed";
		goto err;
	}

	if (lseek(fd, 0, SEEK_SET) == -1) {
		*errstr = "lseek failed";
		goto err;
	}

	if (memcmp(buf, "fLaC", 4) == 0)
		return play_flac(fd, errstr);
	if (memcmp(buf, "ID3", 3) == 0 ||
	    memcmp(buf, "\xFF\xFB", 2) == 0)
		return play_mp3(fd, errstr);
	if (memmem(buf, r, "OpusHead", 8) != NULL)
		return play_opus(fd, errstr);
	if (memmem(buf, r, "OggS", 4) != NULL)
		return play_oggvorbis(fd, errstr);

	*errstr = "unknown file type";
err:
	close(fd);
	return -1;
}

static int
player_pause(int64_t *s)
{
	int r;

	r = player_dispatch(s, 1);
	return r == IMSG_RESUME || r == IMSG_CTL_SEEK;
}

static int
player_shouldstop(int64_t *s, int wait)
{
	switch (player_dispatch(s, wait)) {
	case IMSG_PAUSE:
		if (player_pause(s))
			break;
		/* fallthrough */
	case IMSG_STOP:
		return 1;
	}

	return 0;
}

int
play(const void *buf, size_t len, int64_t *s)
{
	size_t w;
	int nfds, revents, r, wait;

	*s = -1;
	while (len != 0) {
		nfds = sio_pollfd(hdl, player_pfds + 1, POLLOUT);
		r = poll(player_pfds, nfds + 1, INFTIM);
		if (r == -1)
			fatal("poll");

		wait = player_pfds[0].revents & (POLLHUP|POLLIN);
		if (player_shouldstop(s, wait)) {
			sio_flush(hdl);
			stopped = 1;
			return 0;
		}

		revents = sio_revents(hdl, player_pfds + 1);
		if (revents & POLLHUP) {
			if (errno == EAGAIN)
				continue;
			fatal("sndio hang-up");
		}
		if (revents & POLLOUT) {
			w = sio_write(hdl, buf, len);
			len -= w;
			buf += w;
		}
	}

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

	if ((hdl = sio_open(SIO_DEVANY, SIO_PLAY, 1)) == NULL)
		fatal("sio_open");

	sio_onmove(hdl, player_onmove, NULL);

	/* allocate one extra for imsg */
	player_pfds = calloc(sio_nfds(hdl) + 1, sizeof(*player_pfds));
	if (player_pfds == NULL)
		fatal("calloc");

	player_pfds[0].events = POLLIN;
	player_pfds[0].fd = 3;

	ibuf = xmalloc(sizeof(*ibuf));
	imsg_init(ibuf, 3);

	signal(SIGINT, player_signal_handler);
	signal(SIGTERM, player_signal_handler);

	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	if (pledge("stdio recvfd audio", NULL) == -1)
		fatal("pledge");

	while (!halted) {
		const char *errstr = NULL;

		while (nextfd == -1)
			player_dispatch(NULL, 1);

		r = player_playnext(&errstr);
		if (r == -1)
			player_senderr(errstr);
		if (r == 0)
			player_sendeof();
	}

	return 0;
}
