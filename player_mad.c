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

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <limits.h>
#include <sndio.h>
#include <stdio.h>
#include <stdint.h>
#include <imsg.h>
#include <unistd.h>

#include <mad.h>

#include "amused.h"
#include "log.h"

struct mad_stream mad_stream;
struct mad_frame mad_frame;
struct mad_synth mad_synth;

struct buffer {
	const void *start;
	size_t length;
	int sample_rate;
	int channels;
};

static enum mad_flow
input(void *d, struct mad_stream *stream)
{
	struct buffer *buffer = d;

	if (buffer->length == 0)
		return MAD_FLOW_STOP;

	mad_stream_buffer(stream, buffer->start, buffer->length);
	buffer->length = 0;
	buffer->sample_rate = 0;
	buffer->channels = 0;
	return MAD_FLOW_CONTINUE;
}

/* scale a mad sample to 16 bits */
static inline int
scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample -= MAD_F_ONE;

	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static enum mad_flow
output(void *data, const struct mad_header *header, struct mad_pcm *pcm)
{
	static uint8_t buf[BUFSIZ];
	size_t len;
	struct buffer *buffer = data;
	int nsamples, i;
	uint16_t sample;
	const mad_fixed_t *leftch, *rightch;

	if (player_shouldstop())
		return MAD_FLOW_STOP;

	nsamples = pcm->length;
	leftch = pcm->samples[0];
	rightch = pcm->samples[1];

	if (buffer->sample_rate != pcm->samplerate ||
	    buffer->channels != pcm->channels) {
		buffer->sample_rate = pcm->samplerate;
		buffer->channels = pcm->channels;
		if (player_setup(16, pcm->samplerate, pcm->channels) == -1)
			err(1, "player_setrate");
	}

	for (i = 0, len = 0; i < nsamples; ++i) {
		if (len+4 >= sizeof(buf)) {
			sio_write(hdl, buf, len);
			len = 0;
		}

		sample = scale(*leftch++);
		buf[len++] = sample & 0xff;
		buf[len++] = (sample >> 8) & 0xff;

		if (pcm->channels == 2) {
			sample = scale(*rightch++);
			buf[len++] = sample & 0xff;
			buf[len++] = (sample >> 8) & 0xff;
		}
	}

	if (len != 0)
		sio_write(hdl, buf, len);

	return MAD_FLOW_CONTINUE;
}

static enum mad_flow
error(void *d, struct mad_stream *stream, struct mad_frame *frame)
{
	struct buffer *buffer = d;

	/*
	 * most of the decoding errors are actually ID3 tags.  Since
	 * they're common, this has a lower priority to avoid spamming
	 * syslog.
	 */
	log_debug("decoding error 0x%04x (%s) at byte offset %zu",
	    stream->error, mad_stream_errorstr(stream),
	    stream->this_frame - (const unsigned char *)buffer->start);

	return MAD_FLOW_CONTINUE;
}

static int
decode(void *m, size_t len)
{
	struct buffer buffer;
	struct mad_decoder decoder;
	int result;

	/* initialize our private message structure; */
	buffer.start = m;
	buffer.length = len;

	/* configure input, output and error functions */
	mad_decoder_init(&decoder, &buffer, input, 0 /* header */,
	    0 /* filter */, output, error, 0 /* message */);

	/* start decoding */
	result = mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);

	/* release the decoder */
	mad_decoder_finish(&decoder);

	return result;
}

void
play_mp3(int fd)
{
	struct stat stat;
	void *m;

	if (fstat(fd, &stat) == -1) {
		log_warn("fstat");
		goto end;
	}

	m = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) {
		log_warn("map failed");
		goto end;
	}

	decode(m, stat.st_size);
	munmap(m, stat.st_size);

end:
	close(fd);
}
