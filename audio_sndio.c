/*
 * Copyright (c) 2023 Omar Polo <op@omarpolo.com>
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

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sndio.h>
#include <stdio.h>

#include "amused.h"
#include "log.h"

static struct sio_hdl		*hdl;
static struct sio_par		 par;
static int			 stopped = 1;

int
audio_open(void (*onmove_cb)(void *, int))
{
	if ((hdl = sio_open(SIO_DEVANY, SIO_PLAY, 1)) == NULL)
		return -1;

	sio_onmove(hdl, onmove_cb, NULL);
	return 0;
}

int
audio_setup(unsigned int bits, unsigned int rate, unsigned int channels,
    struct pollfd *pfds)
{
	int		 nfds, fpct;

	fpct = (rate * 5) / 100;

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
			nfds = sio_pollfd(hdl, pfds, POLLOUT);
			if (poll(pfds, nfds, INFTIM) == -1)
				fatal("poll");
			goto again;
		}
		log_warnx("invalid params (bits=%u, rate=%u, channels=%u)",
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

	/* TODO: check sample rate? */

 start:
	if (!sio_start(hdl)) {
		log_warn("sio_start");
		return -1;
	}
	stopped = 0;
	return 0;
}

int
audio_nfds(void)
{
	return sio_nfds(hdl);
}

int
audio_pollfd(struct pollfd *pfds, int events)
{
	return sio_pollfd(hdl, pfds, events);
}

int
audio_revents(struct pollfd *pfds)
{
	return sio_revents(hdl, pfds);
}

size_t
audio_write(const void *buf, size_t len)
{
	return sio_write(hdl, buf, len);
}

int
audio_flush(void)
{
	stopped = 1;
	return sio_flush(hdl);
}

int
audio_stop(void)
{
	stopped = 1;
	return sio_stop(hdl);
}
