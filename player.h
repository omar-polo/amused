/*
 * Copyright (c) 2022, 2023 Omar Polo <op@omarpolo.com>
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

#ifndef AMUSED_BUFSIZ
#define AMUSED_BUFSIZ (16 * 1024)
#endif

int	player_setup(unsigned int, unsigned int, unsigned int);
void	player_setduration(int64_t);
void	player_setpos(int64_t);
int	play(const void *, size_t, int64_t *);
int	player(int, int);

int	play_oggvorbis(int, const char **);
int	play_mp3(int, const char **);
int	play_flac(int, const char **);
int	play_opus(int, const char **);
