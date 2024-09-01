/*
 * Copyright (c) 2024 Omar Polo <op@omarpolo.com>
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

#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include "audio.h"
#include "log.h"

static void			(*onmove_cb)(void *, int);
static int			 bpf;
static int			 cur_bits, cur_rate, cur_chans;
static int			 audiofd = -1;

int
audio_open(void (*cb)(void *, int))
{
	onmove_cb = cb;
	if ((audiofd = open("/dev/dsp", O_WRONLY)) == -1)
		return -1;
	return 0;
}

int
audio_setup(unsigned int bits, unsigned int rate, unsigned int channels,
    struct pollfd *pfds, int nfds)
{
	int		 fmt, forig;
	int		 corig, rorig;
	int		 fpct;

	fpct = (rate * 5) / 100;
	if (audiofd != -1) {
		if (bits == cur_bits && channels == cur_chans &&
		    cur_rate - fpct <= rate && rate <= cur_rate + fpct)
			return 0;
	}

	close(audiofd);
	if (audio_open(onmove_cb) == -1)
		return -1;

	if (bits == 8) {
		fmt = AFMT_S8;
		bpf = 1;
	} else if (bits == 16) {
		fmt = AFMT_S16_NE;
		bpf = 2;
	} else if (bits == 24) {
		fmt = AFMT_S24_NE;
		bpf = 4;
	} else if (bits == 32) {
		fmt = AFMT_S32_NE;
		bpf = 4;
	} else {
		log_warnx("can't handle %d bits", bits);
		return -1;
	}
	bpf *= channels;

	forig = fmt;
	if (ioctl(audiofd, SNDCTL_DSP_SETFMT, &fmt) == -1) {
		log_warn("couldn't set the format");
		return -1;
	}
	if (forig != fmt) {
		errno = ENODEV;
		return -1;
	}

	corig = channels;
	if (ioctl(audiofd, SNDCTL_DSP_CHANNELS, &channels) == -1) {
		log_warn("couldn't set the channels");
		return -1;
	}
	if (corig != channels) {
		errno = ENODEV;
		return -1;
	}

	rorig = rate;
	if (ioctl(audiofd, SNDCTL_DSP_SPEED, &rate) == -1) {
		log_warn("couldn't set the rate");
		return -1;
	}
	if (rorig - fpct > rate || rate > rorig + fpct) {
		errno = ENODEV;
		return -1;
	}

	cur_bits = bits;
	cur_rate = rate;
	cur_chans = channels;
	return 0;
}

int
audio_nfds(void)
{
	return 1;
}

int
audio_pollfd(struct pollfd *pfds, int nfds, int events)
{
	if (nfds != 1) {
		errno = EINVAL;
		return -1;
	}

	pfds[0].fd = audiofd;
	pfds[0].events = POLLOUT;
	return 0;
}

int
audio_revents(struct pollfd *pfds, int nfds)
{
	if (nfds != 1) {
		log_warnx("%s: called with nfds=%d", __func__, nfds);
		return 0;
	}

	/* don't need to check, if we're here we can write */
	return POLLOUT;
}

size_t
audio_write(const void *buf, size_t len)
{
	ssize_t r;

	r = write(audiofd, buf, len);
	if (r == -1) {
		log_warn("oss write");
		return 0;
	}

	if (onmove_cb)
		onmove_cb(NULL, r / bpf);

	return r;
}

int
audio_flush(void)
{
	if (ioctl(audiofd, SNDCTL_DSP_POST) == -1)
		return -1;
	return (0);
}

int
audio_stop(void)
{
	return 0;
}
