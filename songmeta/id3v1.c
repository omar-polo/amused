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
 * ID3v1 and 1.1 handling.
 */

#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "songmeta.h"

#define ID3v1_SIZE	128

int
id3v1_dump(int fd, const char *name, const char *filter)
{
	struct stat	 sb;
	char		*s, *e, id3[ID3v1_SIZE];
	char		 buf[5]; /* wide enough for YYYY + NUL */
	ssize_t		 r;
	off_t		 off;

	if (fstat(fd, &sb) == -1) {
		warn("fstat %s", name);
		return (-1);
	}

	if (sb.st_size < ID3v1_SIZE) {
		warnx("no id3 section found in %s", name);
		return (-1);
	}
	off = sb.st_size - ID3v1_SIZE;
	r = pread(fd, id3, ID3v1_SIZE, off);
	if (r == -1 || r != ID3v1_SIZE) {
		warn("failed to read id3 section in %s", name);
		return (-1);
	}

	s = id3;
	if (strncmp(s, "TAG", 3) != 0)
		goto bad;
	s += 3;

	if (memchr(s, '\0', 30) == NULL)
		goto bad;
	if (*s)
		printfield("title", filter, "Title", 1, s);
	else if (filter != NULL && matchfield("title", filter))
		return (-1);
	s += 30;

	if (memchr(s, '\0', 30) == NULL)
		goto bad;
	if (*s)
		printfield("artist", filter, "Artist", 1, s);
	else if (filter != NULL && matchfield("artist", filter))
		return (-1);
	s += 30;

	if (memchr(s, '\0', 30) == NULL)
		goto bad;
	if (*s)
		printfield("album", filter, "Album", 1, s);
	else if (filter != NULL && matchfield("album", filter))
		return (-1);
	s += 30;

	if (!isdigit((unsigned char)s[0]) ||
	    !isdigit((unsigned char)s[1]) ||
	    !isdigit((unsigned char)s[2]) ||
	    !isdigit((unsigned char)s[3]))
		goto bad;
	memcpy(buf, s, 4);
	buf[4] = '\0';
	printfield("year", filter, "Year", 0, buf);
	s += 4;

	if ((e = memchr(s, '\0', 30)) == NULL)
		goto bad;
	s += strspn(s, " \t");
	if (*s)
		printfield("comment", filter, "Comment", 1, s);
	else if (filter != NULL && matchfield("comment", filter))
		return (-1);

	/* ID3v1.1: track number is inside the comment space */

	if (s[28] == '\0' && s[29] != '\0') {
		snprintf(buf, sizeof(buf), "%d", (unsigned int)s[29]);
		printfield("track", filter, "Track #", 0, s);
	} else if (filter != NULL && matchfield("track", filter))
		return (-1);

	return (0);

 bad:
	warnx("bad id3 section in %s", name);
	return (-1);
}
