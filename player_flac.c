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

#include <err.h>
#include <event.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <imsg.h>

#include <FLAC/stream_decoder.h>

#include "amused.h"
#include "log.h"

static FLAC__StreamDecoderWriteStatus
writecb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const int32_t * const *buffer, void *data)
{
	static uint8_t buf[BUFSIZ];
	int i;
	size_t len;

	for (i = 0, len = 0; i < frame->header.blocksize; ++i) {
		if (len+4 >= sizeof(buf)) {
			if (!play(buf, len))
				goto quit;
			len = 0;
		}

		buf[len++] = buffer[0][i] & 0xff;
		buf[len++] = (buffer[0][i] >> 8) & 0xff;

		buf[len++] = buffer[1][i] & 0xff;
		buf[len++] = (buffer[1][i] >> 8) & 0xff;
	}

	if (len != 0 && !play(buf, len))
		goto quit;

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
quit:
	return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

static void
metacb(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *meta,
    void *d)
{
	uint32_t sample_rate;
	int channels;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
		sample_rate = meta->data.stream_info.sample_rate;
		channels = meta->data.stream_info.channels;

		if (player_setup(16, sample_rate, channels) == -1)
			err(1, "player_setrate");
	}
}

static void
errcb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status,
    void *data)
{
	log_warnx("flac error: %s", FLAC__StreamDecoderErrorStatusString[status]);
}

int
play_flac(int fd)
{
	FILE *f;
	int s, ok = 1;
	const char *state;
	FLAC__StreamDecoder *decoder = NULL;
	FLAC__StreamDecoderInitStatus init_status;

	if ((f = fdopen(fd, "r")) == NULL)
		err(1, "fdopen");

	decoder = FLAC__stream_decoder_new();
	if (decoder == NULL)
		err(1, "flac stream decoder");

	FLAC__stream_decoder_set_md5_checking(decoder, 1);

	init_status = FLAC__stream_decoder_init_FILE(decoder, f, writecb,
	    metacb, errcb, NULL);
	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		errx(1, "flac decoder: %s",
		    FLAC__StreamDecoderInitStatusString[init_status]);

	ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
	s = FLAC__stream_decoder_get_state(decoder);

	FLAC__stream_decoder_delete(decoder);
	fclose(f);

	if (!ok && s != FLAC__STREAM_DECODER_ABORTED) {
		state = FLAC__StreamDecoderStateString[s];
		log_warnx("decoding failed; state: %s", state);
		return 1;
	}
	if (!ok)
		return -1;
	return 0;
}
