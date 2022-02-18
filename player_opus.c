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
#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <sndio.h>
#include <stdio.h>
#include <stdint.h>
#include <imsg.h>
#include <unistd.h>

#include <opusfile.h>

#include "amused.h"
#include "log.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

void
play_opus(int fd)
{
	static uint16_t pcm[BUFSIZ];
	static uint8_t out[BUFSIZ * 2];
	OggOpusFile *of;
	void *f;
	int ret;
	OpusFileCallbacks cb = {NULL, NULL, NULL, NULL};
	int i, li, prev_li = -1;

	if ((f = op_fdopen(&cb, fd, "r")) == NULL)
		err(1, "fdopen");

	of = op_open_callbacks(f, &cb, NULL, 0, &ret);
	if (of == NULL) {
		close(fd);
		return;
	}

	for (;;) {
		if (player_shouldstop())
			break;

		/* NB: will downmix multichannels files into two channels */
		ret = op_read_stereo(of, pcm, nitems(pcm));
		if (ret == OP_HOLE) /* corrupt file segment? */
			continue;
		if (ret < 0) {
			log_warnx("error %d decoding file", ret);
			break;
		}
		if (ret == 0)
			break; /* eof */

		li = op_current_link(of);
		if (li != prev_li) {
			const OpusHead *head;

			prev_li = li;

			head = op_head(of, li);
			if (head->input_sample_rate &&
			    player_setup(head->input_sample_rate, 2) == -1)
				err(1, "player_setrate");
		}

		for (i = 0; i < 2*ret; ++i) {
			out[2*i+0] = pcm[i] & 0xFF;
			out[2*i+1] = (pcm[i] >> 8) & 0xFF;
		}
		sio_write(hdl, out, 4*ret);
	}

	op_free(of);
}
