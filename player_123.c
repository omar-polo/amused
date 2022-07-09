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

#include <sys/mman.h>
#include <sys/stat.h>

#include <limits.h>
#include <unistd.h>

#include <mpg123.h>

#include "amused.h"
#include "log.h"

static int
setup(mpg123_handle *mh)
{
	long	rate;
	int	chan, enc;

	if (mpg123_getformat(mh, &rate, &chan, &enc) != MPG123_OK) {
		log_warnx("mpg123_getformat failed");
		return 0;
	}

	if (player_setup(mpg123_encsize(enc) * 8, rate, chan) == -1)
		err(1, "player_setup");

	return 1;
}

int
play_mp3(int fd, const char **errstr)
{
	static char	 buf[4096];
	size_t		 len;
	mpg123_handle	*mh;
	int64_t		 seek = -1;
	int		 err, ret = -1;

	if ((mh = mpg123_new(NULL, NULL)) == NULL)
		fatal("mpg123_new");

	if (mpg123_open_fd(mh, fd) != MPG123_OK) {
		*errstr = "mpg123_open_fd failed";
		close(fd);
		return -1;
	}

	if (!setup(mh))
		goto done;

	player_setduration(mpg123_length(mh));

	for (;;) {
		if (seek != -1) {
			seek = mpg123_seek(mh, seek, SEEK_SET);
			if (seek < 0) {
				ret = 0;
				break;
			}
			player_setpos(seek);
		}

		err = mpg123_read(mh, buf, sizeof(buf), &len);
		switch (err) {
		case MPG123_DONE:
			ret = 0;
			goto done;
		case MPG123_NEW_FORMAT:
			if (!setup(mh))
				goto done;
			break;
		case MPG123_OK:
			if (!play(buf, len, &seek)) {
				ret = 1;
				goto done;
			}
			break;
		default:
			log_warnx("skipping mp3 decoding error");
			break;
		}
	}

done:
	mpg123_delete(mh);
	close(fd);
	return ret;
}
