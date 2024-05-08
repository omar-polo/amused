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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ogg.h"
#include "log.h"
#include "songmeta.h"

int
opus_match(struct ogg *ogg)
{
	uint8_t		 hdr[8], v;

	if (ogg_read(ogg, hdr, sizeof(hdr)) != sizeof(hdr))
		return (-1);
	if (memcmp(hdr, "OpusHead", 8) != 0)
		return (-1);

	ogg_use_current_stream(ogg);

	if (ogg_read(ogg, &v, 1) != 1)
		return (-1);
	if (v < 1 || v > 2) {
		log_warnx("unsupported opus version %d", v);
		return (-1);
	}

	/* skip the rest of the identification header */
	if (ogg_skip_page(ogg) == -1)
		return (-1);

	/* now there should be the optional tag section */
	if (ogg_read(ogg, hdr, sizeof(hdr)) != sizeof(hdr))
		return (-1);
	if (memcmp(hdr, "OpusTags", 8) != 0)
		return (-1);

	return (0);
}

int
opus_dump(struct ogg *ogg, const char *name, const char *filter)
{
	static char	 buf[2048]; /* should be enough... */
	char		*v;
	uint32_t	 i, n, l, len;

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

		/*
		 * XXX should probably ignore R128_TRACK_GAIN and
		 * R128_ALBUM_GAIN.
		 */

		printf("%s = %s\n", buf, v);
	}


	return (-1);
}
