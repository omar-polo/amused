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

#include <endian.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ogg.h"
#include "songmeta.h"

int
vorbis_match(struct ogg *ogg)
{
	uint8_t		 hdr[7]; /* packet type + "vorbis" */

	if (ogg_read(ogg, hdr, 7) != 7)
		return (-1);

	if (memcmp(hdr + 1, "vorbis", 6) != 0)
		return (-1);

	ogg_use_current_stream(ogg);

	/*
	 * vorbis version (4 bytes)
	 * channels (1 byte)
	 * sample rate (4 bytes)
	 * bitrate max/nominal/min (4 bytes each)
	 * blocksize_{0,1} (1 byte)
	 * framing flag (1 bit -- rounded to 1 byte)
	 */
	/* XXX check that the framing bit is 1? */
	if (ogg_seek(ogg, +23) == -1)
		return (-1);

	return (0);
}

int
vorbis_dump(struct ogg *ogg, const char *name, const char *filter)
{
	static char	 buf[2048]; /* should be enough... */
	char		*v;
	uint32_t	 i, n, l, len;
	uint8_t		 pktype, hdr[7];

	if (ogg_read(ogg, hdr, 7) != 7)
		return (-1);
	if (memcmp(hdr + 1, "vorbis", 6) != 0)
		return (-1);

	pktype = hdr[0];
	if (pktype != 3) /* metadata */
		return (-1);

	if (ogg_read(ogg, &len, sizeof(len)) != sizeof(len))
		return (-1);
	len = le32toh(len);
	if (ogg_seek(ogg, +len) == -1)
		return (-1);

	if (ogg_read(ogg, &n, sizeof(n)) != sizeof(n))
		return (-1);
	n = le32toh(n);

	for (i = 0; i < n; ++i) {
		if (ogg_read(ogg, &len, sizeof(len)) != sizeof(len))
			return (-1);
		len = le32toh(len);

		l = len;
		if (l >= sizeof(buf))
			l = len - 1;
		len -= l;

		if (ogg_read(ogg, buf, l) != l ||
		    ogg_seek(ogg, +len) == -1)
			return (-1);
		buf[l] = '\0';

		if ((v = strchr(buf, '=')) == NULL)
			return (-1);
		*v++ = '\0';

		printf("%s = %s\n", buf, v);
	}
			
	return (0);
}
