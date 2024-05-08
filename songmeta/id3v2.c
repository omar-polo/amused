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

/*
 * ID3v2(.4.0) handling.
 */

#include <sys/stat.h>

#include <ctype.h>
#include <endian.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "songmeta.h"

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

#define ID3v2_HDR_SIZE		10
#define ID3v2_FRAME_SIZE	10

#define F_UNSYNC	0x80
#define F_EXTHDR	0x40
#define F_EXPIND	0x20
#define F_FOOTER	0x10

static const struct fnmap {
	const char	*id;
	const char	*name;
	const char	*pretty;
} map[] = {
	/* AENC Audio encryption */
	/* APIC Attached picture */
	/* ASPI Audio seek point index */

	{ "COMM",	"comment",	"Comment" },
	/* COMR Commercial frame */

	/* ENCR Encryption method registration */
	/* EQU2 Equalisation (2) */
	/* ETCO Event time codes */

	/* GEOB General encapsulated object */
	/* GRID Group identification registration */

	/* LINK Linked information */

	/* MCDI Music CD identifier */
	/* MLLT MPEG location lookup table */

	/* OWNE Ownership frame */

	/* PRIV Private frame */
	/* PCNT Play counter */
	/* POPM Popularimeter */
	/* POSS Position synchronisation frame */

	/* RBUF Recommended buffer size */
	/* RVA2 Relative volume adjustment (2) */
	/* RVRB Reverb */

	/* SEEK Seek frame */
	/* SIGN Signature frame */
	/* SYLT Synchronised lyric text */
	/* SYTC Synchronised tempo codes */

	{ "TALB",	"album",		"Album" },
	{ "TBPM",	"bpm",			"beats per minute" },
	{ "TCOM",	"composer",		"Composer" },
	{ "TCON",	"content-type",		"Content type" },
	{ "TCOP",	"copyright-message",	"Copyright message" },
	{ "TDEN",	"encoding-time",	"Encoding time" },
	{ "TDLY",	"playlist-delay",	"Playlist delay" },
	{ "TDOR",	"original-release-time","Original release time" },
	{ "TDRC",	"recording-time",	"Recording time" },
	{ "TDRL",	"release-time",		"Release time" },
	{ "TDTG",	"tagging-time",		"Tagging time" },
	{ "TENC",	"encoded-by",		"Encoded by" },
	{ "TEXT",	"lyricist",		"Lyricist/Text writer" },
	{ "TFLT",	"file-type",		"File type" },
	{ "TIPL",	"involved-people",	"Involved people list" },
	{ "TIT1",	"content-group-description", "Content group description" },
	{ "TIT2",	"title",		"Title" },
	{ "TIT3",	"subtitle",		"Subtitle" },
	{ "TKEY",	"initial-key",		"Initial key" },
	{ "TLAN",	"language",		"Language" },
	{ "TLEN",	"length",		"Length" },
	{ "TMCL",	"musician",		"Musician credits list" },
	{ "TMED",	"media-type",		"Media type" },
	{ "TMOO",	"mood",			"Mood" },
	{ "TOAL",	"original-title",	"Original album/movie/show title" },
	{ "TOFN",	"original-filename",	"Original filename" },
	{ "TOLY",	"original-lyricist",	"Original lyricist(s)/text writer(s)" },
	{ "TOPE",	"original-artist",	"Original artist(s)/performer(s)" },
	{ "TOWN",	"licensee",		"File owner/licensee" },
	{ "TPE1",	"lead-performer",	"Lead performer(s)/Soloist(s)" },
	{ "TPE2",	"band",			"Band/orchestra/accompaniment" },
	{ "TPE3",	"conductor",		"Conductor/performer refinement" },
	{ "TPE4",	"interpreted-by",	"Interpreted, remixed, or otherwise modified by" },
	{ "TPOS",	"part",			"Part of a set" },
	{ "TPRO",	"notice",		"Produced notice" },
	{ "TPUB",	"publisher",		"Publisher" },
	{ "TRCK",	"track",		"Track number/Position in set" },
	{ "TRSN",	"radio-name",		"Internet radio station name" },
	{ "TRSO",	"radio-owner",		"Internet radio station owner" },
	{ "TSOA",	"album-order",		"Album sort order" },
	{ "TSOP",	"performer-order",	"Performer sort order" },
	{ "TSOT",	"title-order",		"Title sort order" },
	{ "TSRC",	"isrc",			"ISRC (international standard recording code)" },
	{ "TSSE",	"encoder",		"Software/Hardware and settings used for encoding" },
	{ "TSST",	"subtitle",		"Set subtitle" },
	/* TXXX user defined text information frame */

	/* UFID Unique file identifier */
	/* USER Terms of use */
	/* USLT Unsynchronised lyric/text transcription */

	/* WCOM Commercial information */
	/* WCOP Copyright/legal information */
	/* WOAF Official audio file webpage */
	/* WOAR Official artist/performer webpage */
	/* WOAS Official audio source webpage */
	/* WORS Official internet radio station homepage */
	/* WPAY Payment */
	/* WPUB Publishers official webpage */
	/* WXXX User defined URL link frame */
};

static int
mapcmp(const void *k, const void *e)
{
	const struct fnmap *f = e;

	return (memcmp(k, f->id, 4));
}

static uint32_t
fromss32(uint32_t x)
{
	uint8_t		y[4];

	memcpy(y, &x, sizeof(x));
	return (y[0] << 21) | (y[1] << 14) | (y[2] << 7) | y[3];
}

int
id3v2_dump(int fd, const char *name, const char *filter)
{
	struct fnmap	*f;
	char		 hdr[ID3v2_HDR_SIZE];
	char		*s;
	ssize_t		 r;
	uint8_t		 flags;
	uint32_t	 size, fsize;

	if ((r = read(fd, hdr, sizeof(hdr))) == -1 ||
	    r != ID3v2_HDR_SIZE) {
		warn("read failed: %s", name);
		return (-1);
	}

	s = hdr;
	if (strncmp(s, "ID3", 3))
		goto bad;
	s += 3;

	if (s[0] != 0x04 && s[1] != 0x00)
		goto bad;
	s += 2;

	flags = s[0];
	if ((s[0] & 0x0F) != 0)
		goto bad;
	s += 1;

#ifdef DEBUG
	const char *sep = "";
	printf("flags:\t");
	if (flags & F_UNSYNC) printf("%sunsync", sep), sep = ",";
	if (flags & F_EXTHDR) printf("%sexthdr", sep), sep = ",";
	if (flags & F_EXPIND) printf("%sexpind", sep), sep = ",";
	if (flags & F_FOOTER) printf("%sfooter", sep), sep = ",";
	printf(" (0x%x)\n", flags);
#endif

	if (flags & F_EXPIND) {
		warnx("don't know how to handle the extended header yet.");
		return (-1);
	}

	memcpy(&size, s, 4);
	size = fromss32(size);

	while (size > 0) {
		if ((r = read(fd, hdr, sizeof(hdr))) == -1 ||
		    r != ID3v2_FRAME_SIZE) {
			warn("read failed: %s", name);
			return (-1);
		}

		memcpy(&fsize, hdr + 4, sizeof(fsize));
		fsize = fromss32(fsize);
		if (fsize == 0)
			break;	/* XXX padding?? */
		if (fsize + 10 > size) {
			warnx("bad frame length (%d vs %d)", fsize, size);
			return (-1);
		}
		size -= fsize + 10;

		f = bsearch(hdr, map, nitems(map), sizeof(map[0]), mapcmp);
		if (f == NULL) {
			if (lseek(fd, fsize, SEEK_CUR) == -1) {
				warn("lseek");
				return (-1);
			}
			continue;
		}

		/* XXX skip encoding for now */
		lseek(fd, SEEK_CUR, 1);
		fsize--; /* XXX */

		if (readprintfield(f->name, filter, f->pretty,
		    ENC_UTF8, fd, fsize) == -1)
			return (-1);
	}

	return (0);

 bad:
	warnx("bad ID3v2 section");
	return (-1);
}
