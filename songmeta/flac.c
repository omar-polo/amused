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
 * FLAC metadata handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "songmeta.h"

int
flac_dump(FILE *fp, const char *name, const char *filter)
{
	uint8_t		 btype;
	uint8_t		 magic[4], len[4];
	size_t		 r, i, n, blen;
	int		 last = 0;

	if ((r = fread(magic, 1, sizeof(magic), fp)) != sizeof(magic))
		return (-1);

	if (memcmp(magic, "fLaC", 4) != 0) {
		log_warnx("not a flac file %s", name);
		return (-1);
	}

	while (!last) {
		if (fread(&btype, 1, 1, fp) != 1)
			return (-1);
		last = btype & 0x80;

		if (fread(len, 1, 3, fp) != 3)
			return (-1);

		blen = (len[0] << 16)|(len[1] << 8)|len[2];

		if ((btype & 0x07) != 0x04) {
			//printf("skipping %zu bytes of block type %d\n",
			//    blen, (btype & 0x07));
			/* not a vorbis comment, skip... */
			if (fseeko(fp, blen, SEEK_CUR) == -1)
				return (-1);
			continue;
		}

		if (fread(len, 1, 4, fp) != 4)
			return (-1);

		/* The vorbis comment has little-endian integers */

		blen = (len[3] << 24)|(len[2] << 16)|(len[1] << 8)|len[0];
		/* skip the vendor string comment */
		if (fseeko(fp, blen, SEEK_CUR) == -1)
			return (-1);

		if (fread(len, 1, 4, fp) != 4)
			return (-1);
		n = (len[3] << 24)|(len[2] << 16)|(len[1] << 8)|len[0];

		for (i = 0; i < n; ++i) {
			char *m, *v;

			if (fread(len, 1, 4, fp) != 4)
				return (-1);
			blen = (len[3] << 24)|(len[2] << 16)|(len[1] << 8)|len[0];

			if ((m = malloc(blen + 1)) == NULL)
				fatal("malloc");

			if (fread(m, 1, blen, fp) != blen) {
				free(m);
				return (-1);
			}
			m[blen] = '\0';

			if ((v = strchr(m, '=')) == NULL) {
				log_warnx("missing field name!");
				free(m);
				return (-1);
			}

			*v++ = '\0';
			printf("%s = %s\n", m, v);

			free(m);
		}
	}

	return (0);
}
