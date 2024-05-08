/*
 * Copyright (c) 2022, 2023 Omar Polo <op@omarpolo.com>
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
#include <imsg.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "audio.h"
#include "log.h"
#include "player.h"
#include "xmalloc.h"

struct pollfd		*player_pfds;
int			 player_nfds;
static struct imsgbuf	*imsgbuf;

static int nextfd = -1;
static int64_t samples;
static int64_t duration;
static unsigned int current_rate;

volatile sig_atomic_t halted;

static void
player_signal_handler(int signo)
{
	halted = 1;
}

int
player_setup(unsigned int bits, unsigned int rate, unsigned int channels)
{
	log_debug("%s: bits=%u, rate=%u, channels=%u", __func__,
	    bits, rate, channels);

	current_rate = rate;
	return audio_setup(bits, rate, channels, player_pfds + 1, player_nfds);
}

void
player_setduration(int64_t d)
{
	int64_t seconds;

	duration = d;
	seconds = duration / current_rate;
	imsg_compose(imsgbuf, IMSG_LEN, 0, 0, -1, &seconds, sizeof(seconds));
	imsg_flush(imsgbuf);
}

static void
player_onmove(void *arg, int delta)
{
	static int64_t reported;
	int64_t sec;

	samples += delta;
	if (llabs(samples - reported) >= current_rate) {
		reported = samples;
		sec = samples / current_rate;

		imsg_compose(imsgbuf, IMSG_POS, 0, 0, -1, &sec, sizeof(sec));
		imsg_flush(imsgbuf);
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
	if ((n = imsg_get(imsgbuf, &imsg)) == -1)
		fatal("imsg_get");
	if (n == 0) {
		if (!wait)
			return -1;

		pfd.fd = imsgbuf->fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, INFTIM) == -1)
			fatal("poll");
		if ((n = imsg_read(imsgbuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read");
		if (n == 0)
			fatalx("pipe closed");
		goto again;
	}

	ret = imsg_get_type(&imsg);
	switch (ret) {
	case IMSG_PLAY:
		if (nextfd != -1)
			fatalx("track already enqueued");
		if ((nextfd = imsg_get_fd(&imsg)) == -1)
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
		if (imsg_get_data(&imsg, &seek, sizeof(seek)) == -1)
			fatalx("wrong size for seek ctl");
		if (seek.percent)
			*s = (double)seek.offset * (double)duration / 100.0;
		else
			*s = seek.offset * current_rate;
		if (seek.relative)
			*s += samples;
		if (*s < 0)
			*s = 0;
		break;
	default:
		fatalx("unknown imsg %d", ret);
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

	imsg_compose(imsgbuf, IMSG_ERR, 0, 0, -1, errstr, len);
	imsg_flush(imsgbuf);
}

static void
player_sendeof(void)
{
	imsg_compose(imsgbuf, IMSG_EOF, 0, 0, -1, NULL, 0);
	imsg_flush(imsgbuf);
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
	imsg_compose(imsgbuf, IMSG_POS, 0, 0, -1, &samples, sizeof(samples));
	imsg_flush(imsgbuf);

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
	int revents, r, wait;

	*s = -1;
	while (len != 0) {
		audio_pollfd(player_pfds + 1, player_nfds, POLLOUT);
		r = poll(player_pfds, player_nfds + 1, INFTIM);
		if (r == -1)
			fatal("poll");

		wait = player_pfds[0].revents & (POLLHUP|POLLIN);
		if (player_shouldstop(s, wait)) {
			audio_flush();
			return 0;
		}

		revents = audio_revents(player_pfds + 1, player_nfds);
		if (revents & POLLHUP) {
			if (errno == EAGAIN)
				continue;
			fatal("audio hang-up");
		}
		if (revents & POLLOUT) {
			w = audio_write(buf, len);
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

	if (audio_open(player_onmove) == -1)
		fatal("audio_open");

	if ((player_nfds = audio_nfds()) <= 0)
		fatal("audio_nfds: invalid number of file descriptors: %d",
		    player_nfds);

	/* allocate one extra for imsg */
	player_pfds = calloc(player_nfds + 1, sizeof(*player_pfds));
	if (player_pfds == NULL)
		fatal("calloc");

	player_pfds[0].events = POLLIN;
	player_pfds[0].fd = 3;

	imsgbuf = xmalloc(sizeof(*imsgbuf));
	imsg_init(imsgbuf, 3);

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
