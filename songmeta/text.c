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

#include "../config.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "songmeta.h"

/*
 * The idea is to try to decode the text as UTF-8; if it fails assume
 * it's ISO Latin.  Some strings in ISO Latin may be decoded correctly
 * as UTF-8 (since both are a superset of ASCII).  In every case, this
 * is just a "best effort" for when we don't have other clues about
 * the format.
 */
static int
u8decode(const char *s, int print)
{
	wchar_t	 wc;
	int	 len;

	for (; *s != '\0'; s += len) {
		if ((len = mbtowc(&wc, s, MB_CUR_MAX)) == -1) {
			(void)mbtowc(NULL, NULL, MB_CUR_MAX);
			return (0);
		}
		if (print) {
			if (!printraw && wcwidth(wc) == -1)
				putchar('?');
			else
				fwrite(s, 1, len, stdout);
		}
	}

	return (1);
}

/* Print a string that may be encoded as ISO Latin. */
void
mlprint(const char *s)
{
	wchar_t	 wc;

	if (u8decode(s, 0)) {
		u8decode(s, 1);
		return;
	}

	/* let's hope it's ISO Latin */

	while (*s) {
		/* the first 256 UNICODE codepoints map 1:1 ISO Latin */
		wc = *s++;
		if (!printraw && wcwidth(wc) == -1)
			putchar('?');
		else
			fwprintf(stdout, L"%c", wc);
	}
}
