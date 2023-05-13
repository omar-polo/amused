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

#include <alsa/asoundlib.h>

#include <limits.h>

#include "amused.h"
#include "log.h"

static snd_pcm_t	*pcm;
static size_t		 bpf;
static void		(*onmove_cb)(void *, int);

int
audio_open(void (*cb)(void *, int))
{
	const char	*device = "default";
	int		 err;

	err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK,
	    SND_PCM_NONBLOCK);
	if (err < 0) {
		log_warnx("playback open error: %s", snd_strerror(err));
		return -1;
	}

	onmove_cb = cb;
	return 0;
}

int
audio_setup(unsigned int bits, unsigned int rate, unsigned int channels,
    struct pollfd *pfds, int nfds)
{
	int			 err;
	snd_pcm_format_t	 fmt;

	if (bits == 8) {
		fmt = SND_PCM_FORMAT_S8;
		bpf = 1;
	} else if (bits == 16) {
		fmt = SND_PCM_FORMAT_S16;
		bpf = 2;
	} else if (bits == 24) {
		fmt = SND_PCM_FORMAT_S24;
		bpf = 4;
	} else if (bits == 32) {
		fmt = SND_PCM_FORMAT_S32;
		bpf = 4;
	} else {
		log_warnx("can't handle %d bits", bits);
		return -1;
	}
	
	bpf *= channels;

	err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
	    channels, rate, 1, 500000 /* 0.5s */);
	if (err < 0) {
		log_warnx("invalid params: %s", snd_strerror(err));
		return -1;
	}

	err = snd_pcm_prepare(pcm);
	if (err < 0) {
		log_warnx("snd_pcm_prepare failed: %s", snd_strerror(err));
		return -1;
	}

	return 0;
}

int
audio_nfds(void)
{
	return snd_pcm_poll_descriptors_count(pcm);
}

int
audio_pollfd(struct pollfd *pfds, int nfds, int events)
{
	return snd_pcm_poll_descriptors(pcm, pfds, nfds);
}

int
audio_revents(struct pollfd *pfds, int nfds)
{
	int			 err;
	unsigned short		 revents;

	err = snd_pcm_poll_descriptors_revents(pcm, pfds, nfds, &revents);
	if (err < 0) {
		log_warnx("snd revents failure: %s", snd_strerror(err));
		return 0;
	}

	return revents;
}

size_t
audio_write(const void *buf, size_t len)
{
	snd_pcm_sframes_t	avail, ret;

	/*
	 * snd_pcm_writei works in terms of FRAMES, not BYTES!
	 */
	len /= bpf;

	avail = snd_pcm_avail_update(pcm);
	if (avail < 0) {
		if (avail == -EPIPE) {
			log_debug("alsa xrun occurred");
			snd_pcm_recover(pcm, -EPIPE, 1);
			return 0;
		}
		log_warnx("snd_pcm_avail_update failure: %s",
		    snd_strerror(avail));
		return 0;
	}

	if (len > avail)
		len = avail;

	ret = snd_pcm_writei(pcm, buf, len);
	if (ret < 0) {
		log_warnx("snd_pcm_writei failed: %s", snd_strerror(ret));
		return 0;
	}
	if (onmove_cb)
		onmove_cb(NULL, ret);
	return ret * bpf;
}

int
audio_flush(void)
{
	int			err;

	err = snd_pcm_drop(pcm);
	if (err < 0) {
		log_warnx("snd_pcm_drop: %s", snd_strerror(err));
		return -1;
	}

	return 0;
}

int
audio_stop(void)
{
	int			err;

	err = snd_pcm_drain(pcm);
	if (err < 0) {
		log_warnx("snd_pcm_drain: %s", snd_strerror(err));
		return -1;
	}

	return 0;
}
