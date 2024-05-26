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

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "log.h"
#include "ogg.h"
#include "songmeta.h"

int printraw;

static void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-r] [-g field] files...\n",
	    getprogname());
	exit(1);
}

int
matchfield(const char *field, const char *filter)
{
	if (filter == NULL)
		return (1);
	return (strcasecmp(field, filter) == 0);
}

int
printfield(const char *field, const char *filter, const char *fname,
    int enc, const char *str)
{
	if (!matchfield(field, filter))
		return (0);

	if (filter == NULL) {
		printf("%s:\t", fname);
		if (strlen(fname) < 8)
			printf("\t");
	}

	if (enc == ENC_GUESS) {
		mlprint(str);
		puts("");
	} else
		printf("%s\n", str);

	return (0);
}

int
readprintfield(const char *field, const char *filter, const char *fname,
    int enc, int fd, off_t len)
{
	static char	 buf[BUFSIZ + 1];
	size_t		 n;
	ssize_t		 r;

	if (!matchfield(field, filter))
		return (0);

	if (filter == NULL) {
		printf("%s:\t", fname);
		if (strlen(fname) < 8)
			printf("\t");
	}

	while (len > 0) {
		if ((n = len) > sizeof(buf) - 1)
			n = sizeof(buf) - 1;

		if ((r = read(fd, buf, n)) == -1) {
			log_warn("read");
			return (-1);
		}
		if (r == 0) {
			log_warnx("unexpected EOF");
			return (-1);
		}
		buf[r] = '\0';

		if (enc == ENC_GUESS)
			mlprint(buf);
		else
			fwrite(buf, 1, r, stdout);

		len -= r;
	}

	puts("");
	return (0);
}

static int
dofile(FILE *fp, const char *name, const char *filter)
{
	static char	 buf[512];
	struct ogg	*ogg;
	size_t		 r, ret = -1;

	if ((r = fread(buf, 1, sizeof(buf), fp)) < 8) {
		log_warn("failed to read %s", name);
		return (-1);
	}

	if (fseek(fp, 0, SEEK_SET) == -1) {
		log_warn("fseek failed in %s", name);
		return (-1);
	}

	if (memcmp(buf, "fLaC", 4) == 0)
		return flac_dump(fp, name, filter);

	if (memcmp(buf, "ID3", 3) == 0)
		return id3v2_dump(fileno(fp), name, filter);

	/* maybe it's an ogg file */
	if ((ogg = ogg_open(fp, name)) != NULL) {
		if (vorbis_match(ogg) != -1) {
			ret = vorbis_dump(ogg, name, filter);
			ogg_close(ogg);
			return (ret);
		}

		if (ogg_rewind(ogg) == -1) {
			log_warn("I/O error on %s", name);
			ogg_close(ogg);
			return (-1);
		}

		if (opus_match(ogg) != -1) {
			ret = opus_dump(ogg, name, filter);
			ogg_close(ogg);
			return (ret);
		}
		ogg_close(ogg);
	}

	if (ferror(fp)) {
		log_warn("I/O error on %s", name);
		return (-1);
	}

	/* TODO: id3v1? */

	log_warnx("unknown file format: %s", name);
	return (-1);
}

int
main(int argc, char **argv)
{
	const char	*filter = NULL;
	FILE		*fp;
	int		 ch;
	int		 ret = 0;

	if (pledge("stdio rpath", NULL) == -1)
		fatal("pledge");

	log_init(1, LOG_USER);

	while ((ch = getopt(argc, argv, "g:r")) != -1) {
		switch (ch) {
		case 'g':
			filter = optarg;
			break;
		case 'r':
			printraw = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0)
		usage();

	for (; *argv; ++argv) {
		if ((fp = fopen(*argv, "r")) == NULL) {
			log_warn("can't open %s", *argv);
			ret = 1;
			continue;
		}

		if (argc != 1)
			printf("=> %s\n", *argv);
		if (dofile(fp, *argv, filter) == -1)
			ret = 1;

		fclose(fp);
	}

	return (ret);
}
