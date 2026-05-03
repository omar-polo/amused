/*
 * Copyright (c) 2026 Omar Polo <op@omarpolo.com>
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

#include <endian.h>
#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "player.h"

int
play_wav(int fd, const char **errstr)
{
	static uint8_t buf[AMUSED_BUFSIZ];
	char hdr[36], tag[8];
	size_t toread;
	ssize_t r, start, pos, tot;
	int64_t seek = -1;
	uint32_t freq;
	int audiofmt, nchan, bps, frame;

	if ((r = read(fd, hdr, sizeof(hdr))) == -1) {
		*errstr = "read failed";
		close(fd);
		return -1;
	}

	if (r != sizeof(hdr)) {
		*errstr = "short read";
		close(fd);
		return -1;
	}

	if (memcmp(&hdr[0], "RIFF", 4) != 0 ||
	    memcmp(&hdr[8], "WAVE", 4) != 0 ||
	    memcmp(&hdr[12], "fmt ", 4) != 0) {
		*errstr = "not a supported WAV file";
		close(fd);
		return -1;
	}

	audiofmt = (hdr[21] << 8) | hdr[20];
	if (audiofmt != 1) { /* PCM */
		*errstr = "unsupported audio format";
		close(fd);
		return -1;
	}

	nchan = (hdr[23] << 8) | hdr[22];

	memcpy(&freq, &hdr[24], 4);
	freq = le32toh(freq);

	bps = ((hdr[35] << 8) | hdr[34]);

	if (player_setup(bps, freq, nchan) == -1)
		fatal("player_setup");

	/* search for the "data" segment */
	for (;;) {
		if ((r = read(fd, tag, sizeof(tag))) != sizeof(tag)) {
			*errstr = "short read, file truncated?";
			close(fd);
			return -1;
		}

		memcpy(&tot, &tag[4], 4);
		tot = le32toh(tot);
		if (memcmp(tag, "data", 4) == 0)
			break;

		if (lseek(fd, tot, SEEK_CUR) == -1) {
			*errstr = "seek to skip section failed";
			close(fd);
			return -1;
		}
	}

	frame = nchan * (bps / 8);
	if (tot < frame) {
		*errstr = "data section too small!";
		close(fd);
		return -1;
	}

	player_setduration(tot / frame);

	if ((start = lseek(fd, 0, SEEK_CUR)) == -1) {
		*errstr = "lseek failed";
		close(fd);
		return -1;
	}

	pos = 0;
	while (pos < tot) {
		if (seek != -1) {
			ssize_t dest = seek * frame;
			if (dest >= tot) {
				dest = tot - frame;
				seek = dest / frame;
			}

			if (lseek(fd, start + dest, SEEK_SET) == -1) {
				*errstr = "seeking failed";
				close(fd);
				return -1;
			}
			player_setpos(seek);
			pos = seek * frame;
		}

		if ((toread = tot - pos) > sizeof(buf))
			toread = sizeof(buf);

		if ((r = read(fd, buf, toread)) == -1) {
			*errstr = "read failed";
			close(fd);
			return -1;
		}

		if (r == 0) {
			*errstr = "early EOF, file truncated?";
			close(fd);
			return -1;
		}

		pos += r;
		if (!play(buf, r, &seek)) {
			close(fd);
			return 1;
		}
	}

	close(fd);
	return 0;
}
