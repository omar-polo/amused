/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
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

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <FLAC/stream_decoder.h>

#include "log.h"
#include "player.h"

struct write_args {
	FLAC__StreamDecoder *decoder;
	int seek_failed;
};

static int
sample_seek(struct write_args *wa, int64_t seek)
{
	int ok;

	ok = FLAC__stream_decoder_seek_absolute(wa->decoder, seek);
	if (ok)
		player_setpos(seek);
	else
		wa->seek_failed = 1;
	return ok;
}

static FLAC__StreamDecoderWriteStatus
writecb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const int32_t * const *src, void *data)
{
	struct write_args *wa = data;
	static uint8_t buf[BUFSIZ];
	int64_t seek;
	int c, i, bps, chans;
	size_t len;

	bps = frame->header.bits_per_sample;
	chans = frame->header.channels;

	for (i = 0, len = 0; i < frame->header.blocksize; ++i) {
		if (len + 4*chans >= sizeof(buf)) {
			if (!play(buf, len, &seek))
				goto quit;
			if (seek != -1) {
				if (sample_seek(wa, seek))
					break;
				else
					goto quit;
			}
			len = 0;
		}

		for (c = 0; c < chans; ++c) {
			switch (bps) {
			case 8:
				buf[len++] = src[c][i] & 0xff;
				break;
			case 16:
				buf[len++] = src[c][i] & 0xff;
				buf[len++] = (src[c][i] >> 8) & 0xff;
				break;
			case 24:
			case 32:
				buf[len++] = src[c][i] & 0xff;
				buf[len++] = (src[c][i] >> 8) & 0xff;
				buf[len++] = (src[c][i] >> 16) & 0xff;
				buf[len++] = (src[c][i] >> 24) & 0xff;
				break;
			default:
				log_warnx("unsupported flac bps=%d", bps);
				goto quit;
			}
		}
	}

	if (len != 0 && !play(buf, len, &seek))
		goto quit;

	if (seek != -1 && !sample_seek(wa, seek))
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
	int channels, bits;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
		bits = meta->data.stream_info.bits_per_sample;
		sample_rate = meta->data.stream_info.sample_rate;
		channels = meta->data.stream_info.channels;

		if (player_setup(bits, sample_rate, channels) == -1)
			fatal("player_setup");

		player_setduration(meta->data.stream_info.total_samples);
	}
}

static void
errcb(const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status, void *data)
{
	log_warnx("flac error: %s",
	    FLAC__StreamDecoderErrorStatusString[status]);
}

int
play_flac(int fd, const char **errstr)
{
	FILE *f;
	struct write_args wa;
	int s, ok = 1;
	FLAC__StreamDecoder *decoder = NULL;
	FLAC__StreamDecoderInitStatus init_status;

	if ((f = fdopen(fd, "r")) == NULL) {
		*errstr = "fdopen failed";
		close(fd);
		return -1;
	}

	decoder = FLAC__stream_decoder_new();
	if (decoder == NULL) {
		*errstr = "FLAC__stream_decoder_new() failed";
		fclose(f);
		return -1;
	}

	FLAC__stream_decoder_set_md5_checking(decoder, 1);

	memset(&wa, 0, sizeof(wa));
	wa.decoder = decoder;

	init_status = FLAC__stream_decoder_init_FILE(decoder, f, writecb,
	    metacb, errcb, &wa);
	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		fatalx("flac decoder: %s",
		    FLAC__StreamDecoderInitStatusString[init_status]);

	ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);

	s = FLAC__stream_decoder_get_state(decoder);
	FLAC__stream_decoder_delete(decoder);
	fclose(f);

	if (s == FLAC__STREAM_DECODER_ABORTED && !wa.seek_failed)
		return 1;
	else if (!ok && !wa.seek_failed) {
		*errstr = "flac decoding error";
		return -1;
	} else
		return 0;
}
