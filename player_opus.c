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

#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <opusfile.h>

#include "log.h"
#include "player.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

int
play_opus(int fd, const char **errstr)
{
	static int16_t pcm[BUFSIZ];
	static uint8_t out[BUFSIZ * 2];
	OggOpusFile *of;
	void *f;
	int64_t seek = -1;
	int r, ret = 0;
	OpusFileCallbacks cb = {NULL, NULL, NULL, NULL};
	int i, li, prev_li = -1, duration_set = 0;

	if ((f = op_fdopen(&cb, fd, "r")) == NULL) {
		*errstr = "fdopen failed";
		close(fd);
		return -1;
	}

	of = op_open_callbacks(f, &cb, NULL, 0, &r);
	if (of == NULL) {
		fclose(f);
		return -1;
	}

	for (;;) {
		if (seek != -1) {
			r = op_pcm_seek(of, seek);
			if (r != 0)
				break;
			player_setpos(seek);
		}

		/* NB: will downmix multichannels files into two channels */
		r = op_read_stereo(of, pcm, nitems(pcm));
		if (r == OP_HOLE) /* corrupt file segment? */
			continue;
		if (r < 0) {
			*errstr = "opus decoding error";
			ret = -1;
			break;
		}
		if (r == 0)
			break; /* eof */

		li = op_current_link(of);
		if (li != prev_li) {
			const OpusHead *head;

			prev_li = li;
			head = op_head(of, li);
			if (head->input_sample_rate &&
			    player_setup(16, head->input_sample_rate, 2) == -1)
				fatal("player_setup");

			if (!duration_set) {
				duration_set = 1;
				player_setduration(op_pcm_total(of, -1));
			}
		}

		for (i = 0; i < 2*r; ++i) {
			out[2*i+0] = pcm[i] & 0xFF;
			out[2*i+1] = (pcm[i] >> 8) & 0xFF;
		}

		if (!play(out, 4*r, &seek)) {
			ret = 1;
			break;
		}
	}

	op_free(of);
	return ret;
}
