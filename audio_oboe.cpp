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

extern "C" {
#include <sys/socket.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
}

#include <oboe/Oboe.h>

extern "C" {
#include "audio.h"
#include "log.h"
}

#define ext extern "C"

static void		(*onmove_cb)(void *, int);
static int		 sp[2]; /* main, audio thread */
static pthread_t	 at;

static int		 bpf;
static unsigned int	 bits, rate, chan;
static oboe::AudioFormat fmt;
static char		 buf[BUFSIZ];
static size_t		 buflen;

static std::shared_ptr<oboe::AudioStream> stream;

static void *
aworker(void *d)
{
	unsigned int	 last_bits, last_rate, last_chan;
	ssize_t		 r;
	int		 sock = sp[1];
	char		 ch;

	stream = nullptr;
	last_bits = last_rate = last_chan = 0;

	log_info("%s: starting", __func__);
	for (;;) {
		ch = 1;
		if ((r = write(sock, &ch, 1)) == -1)
			fatal("write");
		if (r == 0)
			break;

		if ((r = read(sock, &ch, 1)) == -1)
			fatal("read");
		if (r == 0)
			break;

		if (bits != last_bits ||
		    rate != last_rate ||
		    chan != last_chan) {
			if (stream) {
				stream->close();
				stream = nullptr;
			}

			last_bits = bits;
			last_rate = rate;
			last_chan = chan;

			log_debug("setting bits=%d rate=%d chan=%d bpf=%d",
			    bits, rate, chan, bpf);

			oboe::AudioStreamBuilder streamBuilder;
			streamBuilder.setFormat(fmt);
			streamBuilder.setSampleRate(rate);
			streamBuilder.setChannelCount(chan);
			oboe::Result result = streamBuilder.openStream(stream);
			if (result != oboe::Result::OK)
				fatalx("Error opening stream %s",
				    oboe::convertToText(result));

			stream->requestStart();
		}

		// oboe works in terms of FRAMES not BYTES!
		unsigned int len = buflen / bpf;

		// XXX should be the timeout in nanoseconds...
		auto ret = stream->write(buf, len, 1000000000);
		if (!ret) {
			fatalx("write failed: %s",
			    oboe::convertToText(ret.error()));
		}
	}

	return nullptr;
}

ext int
audio_open(void (*cb)(void *, int))
{
	onmove_cb = cb;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1) {
		log_warn("socketpair");
		return (-1);
	}

	if (pthread_create(&at, NULL, aworker, NULL) == -1) {
		log_warn("pthread_create");
		return (-1);
	}

	return (0);
}

ext int
audio_setup(unsigned int p_bits, unsigned int p_rate, unsigned int p_chan,
    struct pollfd *pfds, int nfds)
{
	bits = p_bits;
	rate = p_rate;
	chan = p_chan;

	if (bits == 8) {
		log_warnx("would require a conversion layer...");
		return (-1);
	} else if (bits == 16) {
		bpf = 2;
		fmt = oboe::AudioFormat::I16;
	} else if (bits == 24) {
		bpf = 4;
		fmt = oboe::AudioFormat::I24;
	} else if (bits == 32) {
		// XXX not so sure...
		bpf = 4;
		fmt = oboe::AudioFormat::I24;
	} else {
		log_warnx("can't handle %d bits", bits);
		return (-1);
	}

	bpf *= chan;

	return (0);
}

ext int
audio_nfds(void)
{
	return 1;
}

ext int
audio_pollfd(struct pollfd *pfds, int nfds, int events)
{
	if (nfds != 1) {
		errno = EINVAL;
		return -1;
	}

	pfds[0].fd = sp[0];
	pfds[0].events = POLLIN;
	return (0);
}

ext int
audio_revents(struct pollfd *pfds, int nfds)
{
	if (nfds != 1) {
		log_warnx("%s: called with %d nfds", __func__, nfds);
		return 0;
	}

	/* don't need to check; if we're here the audio thread is ready */
	return POLLOUT;
}

ext size_t
audio_write(const void *data, size_t len)
{
	char	ch;
	ssize_t	r;

	if ((r = read(sp[0], &ch, 1)) == -1) {
		log_warn("oboe/%s: read failed", __func__);
		return 0;
	}
	if (r == 0)
		return 0;

	if (len > sizeof(buf))
		len = sizeof(buf);

	memcpy(buf, data, len);
	buflen = len;

	ch = 1;
	if ((r = write(sp[0], &ch, 1)) == -1) {
		log_warn("oboe/%s: write failed", __func__);
		return 0;
	}
	if (r == 0) {
		log_warnx("oboe/%s: write got EOF", __func__);
		return 0;
	}

	if (onmove_cb)
		onmove_cb(NULL, len / bpf);

	return len;
}

ext int
audio_flush(void)
{
	return 0; // XXX request flush
}

ext int
audio_stop(void)
{
	return 0; // XXX request stop
}
