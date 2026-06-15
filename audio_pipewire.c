/*
 * Copyright (c) 2025 Omar Polo <op@omarpolo.com>
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

#include <sys/socket.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "audio.h"
#include "log.h"

/*
 * Same trick as the libao backend: the blocking audio API is driven by
 * a dedicated thread (here PipeWire's own pw_thread_loop) and we talk to
 * it over a socketpair with single-byte tokens.  The audio side writes a
 * byte when it's ready for more samples, blocks reading a byte back, then
 * plays whatever the player put in `buf`.  This lets the player's poll(2)
 * loop wait on a single fd (sp[0]) just like with any other backend.
 */

static void			(*onmove_cb)(void *, int);
static int			 sp[2]; /* main, audio thread */

static struct pw_thread_loop	*loop;
static struct pw_stream		*stream;

static int			 bpf;
static enum spa_audio_format	 format;
static unsigned int		 sample_rate;
static unsigned int		 channels;

static char			 buf[BUFSIZ];
static size_t			 buflen;

static void
on_process(void *userdata)
{
	struct pw_buffer	*b;
	struct spa_buffer	*spa_buf;
	uint8_t			*dst;
	size_t			 n;
	ssize_t			 r;
	char			 ch;

	/* tell the player we're ready for more data */
	ch = 1;
	if ((r = write(sp[1], &ch, 1)) == -1)
		fatal("write");
	if (r == 0)
		return;		/* main side closed */

	/* block until the player has filled buf */
	if ((r = read(sp[1], &ch, 1)) == -1)
		fatal("read");
	if (r == 0)
		return;

	if ((b = pw_stream_dequeue_buffer(stream)) == NULL) {
		log_warnx("pipewire: out of buffers");
		return;
	}

	spa_buf = b->buffer;
	if ((dst = spa_buf->datas[0].data) == NULL) {
		pw_stream_queue_buffer(stream, b);
		return;
	}

	n = buflen;
	if (n > spa_buf->datas[0].maxsize)
		n = spa_buf->datas[0].maxsize;
	memcpy(dst, buf, n);

	spa_buf->datas[0].chunk->offset = 0;
	spa_buf->datas[0].chunk->stride = bpf;
	spa_buf->datas[0].chunk->size = n;

	pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

int
audio_open(void (*cb)(void *, int))
{
	pw_init(NULL, NULL);
	onmove_cb = cb;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1) {
		log_warn("socketpair");
		return -1;
	}

	loop = pw_thread_loop_new("amused-pipewire", NULL);
	if (loop == NULL) {
		log_warn("pw_thread_loop_new");
		return -1;
	}

	if (pw_thread_loop_start(loop) < 0) {
		log_warn("pw_thread_loop_start");
		return -1;
	}

	return 0;
}

int
audio_setup(unsigned int bits, unsigned int rate, unsigned int chans,
    struct pollfd *pfds, int nfds)
{
	const struct spa_pod	*params[1];
	uint8_t			 podbuf[1024];
	struct spa_pod_builder	 b;
	enum spa_audio_format	 fmt;

	/* PCM is little-endian native, signed (8-bit being unsigned). */
	switch (bits) {
	case 8:
		fmt = SPA_AUDIO_FORMAT_U8;
		bpf = 1;
		break;
	case 16:
		fmt = SPA_AUDIO_FORMAT_S16;
		bpf = 2;
		break;
	case 24:
		/* decoders emit 24-bit samples in 32-bit containers */
		fmt = SPA_AUDIO_FORMAT_S24_32;
		bpf = 4;
		break;
	case 32:
		fmt = SPA_AUDIO_FORMAT_S32;
		bpf = 4;
		break;
	default:
		log_warnx("can't handle %d bits", bits);
		return -1;
	}
	bpf *= chans;

	/* nothing changed, keep the running stream */
	if (stream != NULL && fmt == format && rate == sample_rate &&
	    chans == channels)
		return 0;

	format = fmt;
	sample_rate = rate;
	channels = chans;

	pw_thread_loop_lock(loop);

	if (stream != NULL) {
		pw_stream_destroy(stream);
		stream = NULL;
	}

	stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "amused",
	    pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music",
		NULL),
	    &stream_events, NULL);
	if (stream == NULL) {
		pw_thread_loop_unlock(loop);
		log_warn("pw_stream_new_simple");
		return -1;
	}

	b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
	    &SPA_AUDIO_INFO_RAW_INIT(
		.format = format,
		.rate = sample_rate,
		.channels = channels));

	if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
	    params, 1) < 0) {
		pw_stream_destroy(stream);
		stream = NULL;
		pw_thread_loop_unlock(loop);
		log_warn("pw_stream_connect");
		return -1;
	}

	pw_thread_loop_unlock(loop);
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

	pfds[0].fd = sp[0];
	pfds[0].events = POLLIN;
	return 0;
}

int
audio_revents(struct pollfd *pfds, int nfds)
{
	if (nfds != 1) {
		log_warnx("%s: called with nfds=%d", __func__, nfds);
		return 0;
	}

	/* if we're here the audio thread is ready for more data */
	return POLLOUT;
}

size_t
audio_write(const void *data, size_t len)
{
	char	 ch;
	ssize_t	 r;

	if ((r = read(sp[0], &ch, 1)) == -1) {
		log_warn("pipewire/%s: read failed", __func__);
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
		log_warn("pipewire/%s: write failed", __func__);
		return 0;
	}
	if (r == 0) {
		log_warnx("pipewire/%s: write got EOF", __func__);
		return 0;
	}

	if (onmove_cb)
		onmove_cb(NULL, len / bpf);

	return len;
}

int
audio_flush(void)
{
	return 0;
}

int
audio_stop(void)
{
	return 0;
}
