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

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "log.h"
#include "xmalloc.h"
#include "playlist.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct playlist	 playlist;
enum play_state	 play_state;
int		 repeat_one;
int		 repeat_all = 1;
ssize_t		 play_off = -1;
const char	*current_song;

static void
setsong(ssize_t i)
{
	free((char *)current_song);
	if (i == -1)
		current_song = NULL;
	else
		current_song = xstrdup(playlist.songs[i]);
}

void
playlist_swap(struct playlist *p)
{
	ssize_t i = -1;

	if (play_off != -1) {
		/* try to adjust play_off to match the same song */
		for (i = 0; i < p->len; ++i) {
			if (!strcmp(playlist.songs[play_off], p->songs[i]))
				break;
		}
		/* try to match one song before */
		if (i == p->len && play_off >= 1)
			for (i = 0; i < p->len; ++i)
				if (!strcmp(playlist.songs[play_off-1],
				    p->songs[i]))
					break;
		/* or one song after */
		if (i == p->len && play_off < playlist.len-1)
			for (i = 0; i < p->len; ++i)
				if (!strcmp(playlist.songs[play_off+1],
				    p->songs[i]))
					break;
		if (i == p->len)
			i = -1;
	}

	playlist_truncate();

	if (i != -1)
		play_off = i;

	playlist.len = p->len;
	playlist.cap = p->cap;
	playlist.songs = p->songs;
}

void
playlist_push(struct playlist *playlist, const char *path)
{
	size_t newcap;

	if (playlist->len == playlist->cap) {
		newcap = MAX(16, playlist->cap * 1.5);
		playlist->songs = xrecallocarray(playlist->songs,
		    playlist->cap, newcap, sizeof(*playlist->songs));
		playlist->cap = newcap;
	}

	playlist->songs[playlist->len++] = xstrdup(path);
}

void
playlist_enqueue(const char *path)
{
	playlist_push(&playlist, path);
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
			setsong(play_off);
			return NULL;
		}
	}

	setsong(play_off);
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
			setsong(play_off);
			return NULL;
		}
	}

	setsong(play_off);
	play_state = STATE_PLAYING;
	return playlist.songs[play_off];
}

void
playlist_reset(void)
{
	play_off = -1;
}

void
playlist_free(struct playlist *playlist)
{
	size_t i;

	for (i = 0; i < playlist->len; ++i)
		free(playlist->songs[i]);
	free(playlist->songs);
	playlist->songs = NULL;

	playlist->len = 0;
	playlist->cap = 0;
}

void
playlist_truncate(void)
{
	playlist_free(&playlist);
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

const char *
playlist_jump(const char *arg)
{
	size_t i;
	regex_t re;

	if (regcomp(&re, arg, REG_ICASE | REG_NOSUB) != 0)
		return NULL;

	for (i = 0; i < playlist.len; ++i) {
		if (regexec(&re, playlist.songs[i], 0, NULL, 0) == 0)
			break;
	}
	regfree(&re);

	if (i == playlist.len)
		return NULL;

	play_state = STATE_PLAYING;
	play_off = i;
	setsong(play_off);
	return playlist.songs[i];
}
