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

#include <stdlib.h>
#include <syslog.h>

#include "log.h"
#include "xmalloc.h"
#include "playlist.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct playlist	playlist;
enum play_state	play_state;
int		repeat_one;
int		repeat_all = 1;
ssize_t		play_off = -1;

void
playlist_enqueue(const char *path)
{
	size_t newcap;

	if (playlist.len == playlist.cap) {
		newcap = MAX(16, playlist.cap * 1.5);
		playlist.songs = xrecallocarray(playlist.songs, playlist.cap,
		    newcap, sizeof(*playlist.songs));
		playlist.cap = newcap;
	}

	playlist.songs[playlist.len++] = xstrdup(path);
}

const char *
playlist_current(void)
{
	if (playlist.len == 0 || play_off == -1) {
		play_state = STATE_STOPPED;
		return NULL;
	}

	return playlist.songs[play_off];
}

const char *
playlist_advance(void)
{
	if (playlist.len == 0) {
		play_state = STATE_STOPPED;
		return NULL;
	}

	play_off++;
	if (play_off == playlist.len) {
		if (repeat_all)
			play_off = 0;
		else {
			play_state = STATE_STOPPED;
			play_off = -1;
			return NULL;
		}
	}

	play_state = STATE_PLAYING;
	return playlist.songs[play_off];
}

const char *
playlist_previous(void)
{
	if (playlist.len == 0) {
		play_state = STATE_STOPPED;
		return NULL;
	}

	play_off--;
	if (play_off < 0) {
		if (repeat_all)
			play_off = playlist.len - 1;
		else {
			play_state = STATE_STOPPED;
			play_off = -1;
			return NULL;
		}
	}

	play_state = STATE_PLAYING;
	return playlist.songs[play_off];
}

void
playlist_reset(void)
{
	play_off = -1;
}

void
playlist_truncate(void)
{
	size_t i;

	for (i = 0; i < playlist.len; ++i)
		free(playlist.songs[i]);
	free(playlist.songs);
	playlist.songs = NULL;

	playlist.len = 0;
	playlist.cap = 0;
	play_off = -1;
}

void
playlist_dropcurrent(void)
{
	size_t i;

	if (play_off == -1 || playlist.len == 0)
		return;

	free(playlist.songs[play_off]);

	playlist.len--;
	for (i = play_off; i < playlist.len; ++i)
		playlist.songs[i] = playlist.songs[i+1];

	playlist.songs[playlist.len] = NULL;
}
