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
 * Ogg file-format handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ogg.h"

struct ogg {
	FILE		*fp;
	const char	*name;
	int		 firstpkt;
	uint32_t	 sn;
	int		 chained;	/* use only the sn stream */
	size_t		 plen;
};

static int
readpkt(struct ogg *ogg)
{
	uint32_t	 sn;
	uint8_t		 magic[4], ssv, htype, ps, b;
	size_t		 r;
	int		 i;

 again:
	ogg->plen = 0;
	if ((r = fread(magic, 1, sizeof(magic), ogg->fp)) != sizeof(magic))
		return (-1);

	if (memcmp(magic, "OggS", 4) != 0) {
		log_warnx("not an ogg file: %s", ogg->name);
		return (-1);
	}

	if (fread(&ssv, 1, 1, ogg->fp) != 1 ||
	    fread(&htype, 1, 1, ogg->fp) != 1)
		return (-1);

	/* skip the absolute granule position */
	if (fseeko(ogg->fp, 8, SEEK_CUR) == -1)
		return (-1);

	/* the serial number of the stream */
	if (fread(&sn, 1, sizeof(sn), ogg->fp) != sizeof(ogg->sn))
		return (-1);
	sn = le32toh(sn);

	/* ignore sequence number and crc32 for now */
	if (fseeko(ogg->fp, 4 + 4, SEEK_CUR) == -1)
		return (-1);

	if (fread(&ps, 1, 1, ogg->fp) != 1)
		return (-1);

	ogg->plen = 0;
	for (i = 0; i < ps; ++i) {
		if (fread(&b, 1, 1, ogg->fp) != 1)
			return (-1);
		ogg->plen += b;
	}

	/* found some data without a suitable stream */
	if (!ogg->chained && !(htype & 0x02))
		return (-1);

	if (ogg->chained && ogg->sn != sn) {
		/* not "our" stream */
		if (fseeko(ogg->fp, ogg->plen, SEEK_CUR) == -1)
			return (-1);
		ogg->plen = 0;
		goto again;
	}

	ogg->sn = sn;
	return (0);
}

struct ogg *
ogg_open(FILE *fp, const char *name)
{
	struct ogg	*ogg;

	ogg = calloc(1, sizeof(*ogg));
	if (ogg == NULL)
		return (NULL);

	ogg->fp = fp;
	ogg->name = name;
	ogg->firstpkt = 1;

	if (readpkt(ogg) == -1) {
		free(ogg);
		return (NULL);
	}

	return (ogg);
}

size_t
ogg_read(struct ogg *ogg, void *buf, size_t len)
{
	size_t	 r;

	if (len == 0)
		return (0);

	if (ogg->plen == 0 && readpkt(ogg) == -1)
		return (0);

	if (len > ogg->plen)
		len = ogg->plen;

	r = fread(buf, 1, len, ogg->fp);
	ogg->plen -= r;
	return (r);
}

int
ogg_seek(struct ogg *ogg, off_t n)
{
	/* not implemented */
	if (n < 0)
		return (-1);

	while (n > 0) {
		if (ogg->plen == 0 && readpkt(ogg) == -1)
			return (-1);

		if (n >= ogg->plen) {
			if (fseeko(ogg->fp, ogg->plen, SEEK_CUR) == -1)
				return (-1);
			n -= ogg->plen;
			ogg->plen = 0;
			continue;
		}

		if (fseeko(ogg->fp, n, SEEK_CUR) == -1)
			return (-1);
		ogg->plen -= n;
		break;
	}

	return (0);
}

int
ogg_skip_page(struct ogg *ogg)
{
	return (ogg_seek(ogg, ogg->plen));
}

void
ogg_use_current_stream(struct ogg *ogg)
{
	ogg->chained = 1;
}

int
ogg_rewind(struct ogg *ogg)
{
	if (fseeko(ogg->fp, 0, SEEK_SET) == -1)
		return (-1);

	ogg->plen = 0;
	ogg->firstpkt = 1;
	ogg->chained = 0;
	return (0);
}

void
ogg_close(struct ogg *ogg)
{
	/* it's up to the caller to close ogg->fp */
	free(ogg);
}
