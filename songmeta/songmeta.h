/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

extern int printraw;

enum {
	ENC_GUESS,
	ENC_UTF8,
};

struct ogg;

/* flac.c */
int	 flac_dump(FILE *, const char *, const char *);

/* id3v1.c */
int	 id3v1_dump(int, const char *, const char *);

/* id3v2.c */
int	 id3v2_dump(int, const char *, const char *);

/* opus.c */
int	 opus_match(struct ogg *);
int	 opus_dump(struct ogg *, const char *, const char *);

/* vorbis.c */
int	 vorbis_match(struct ogg *);
int	 vorbis_dump(struct ogg *, const char *, const char *);

/* songmeta.c */
int	 matchfield(const char *, const char *);
int	 printfield(const char *, const char *, const char *, int,
	    const char *);
int	 readprintfield(const char *, const char *, const char *,
	    int, int, off_t);

/* text.c */
void	 mlprint(const char *);
