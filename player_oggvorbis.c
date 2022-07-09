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

#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include "amused.h"
#include "log.h"

#ifndef nitems
#define nitems(x) (sizeof(x)/sizeof(x[0]))
#endif

int
play_oggvorbis(int fd, const char **errstr)
{
	static char pcmout[4096];
	FILE *f;
	OggVorbis_File vf;
	vorbis_info *vi;
	int64_t seek = -1;
	int current_section, ret = 0;

	if ((f = fdopen(fd, "r")) == NULL) {
		*errstr = "fdopen failed";
		close(fd);
		return -1;
	}

	if (ov_open_callbacks(f, &vf, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0) {
		*errstr = "input is not an Ogg bitstream";
		ret = -1;
		goto end;
	}

	/*
	 * we could extract some tags by looping over the NULL
	 * terminated array returned by ov_comment(&vf, -1), see
	 * previous revision of this file.
	 */
	vi = ov_info(&vf, -1);
	if (player_setup(16, vi->rate, vi->channels) == -1)
		err(1, "player_setup");

	player_setduration(ov_time_total(&vf, -1) * vi->rate);

	for (;;) {
		long r;

		if (seek != -1) {
			r = ov_pcm_seek(&vf, seek);
			if (r != 0)
				break;
			player_setpos(seek);
		}

		r = ov_read(&vf, pcmout, sizeof(pcmout), 0, 2, 1,
		    &current_section);
		if (r == 0)
			break;
		else if (r > 0) {
			/* TODO: deal with sample rate changes */
			if (!play(pcmout, r, &seek)) {
				ret = 1;
				break;
			}
		}
	}

	ov_clear(&vf);

end:
	fclose(f);
	return ret;
}
