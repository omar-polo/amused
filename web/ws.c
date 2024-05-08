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

#include "config.h"

#include <sys/queue.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sha1.h>

#include "bufio.h"
#include "http.h"
#include "ws.h"

#define WS_GUID	"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int
tob64(unsigned char ch)
{
	if (ch < 26)
		return ('A' + ch);
	ch -= 26;
	if (ch < 26)
		return ('a' + ch);
	ch -= 26;
	if (ch < 10)
		return ('0' + ch);
	ch -= 10;
	if (ch == 0)
		return ('+');
	if (ch == 1)
		return ('/');
	errno = EINVAL;
	return (-1);
}

static int
b64encode(unsigned char *in, size_t ilen, char *out, size_t olen)
{
	int		 r;

#define SET(x) {				\
	if ((r = tob64((x) & 0x3F)) == -1)	\
		return (-1);			\
	*out++ = r;				\
}

	while (ilen > 0) {
		if (olen < 4) {
			errno = ENOSPC;
			return (-1);
		}
		olen -= 4;

		switch (ilen) {
		case 1:
			SET(in[0] >> 2);
			SET(in[0] << 4);
			*out++ = '=';
			*out++ = '=';
			ilen = 0;
			break;
		case 2:
			SET(in[0] >> 2);
			SET(in[0] << 4 | in[1] >> 4);
			SET(in[1] << 2);
			*out++ = '=';
			ilen = 0;
			break;
		default:
			SET(in[0] >> 2);
			SET(in[0] << 4 | in[1] >> 4);
			SET(in[1] << 2 | in[2] >> 6);
			SET(in[2]);
			ilen -= 3;
			in += 3;
			break;
		}
	}

#undef SET

	if (olen < 1) {
		errno = ENOSPC;
		return (-1);
	}
	*out = '\0';
	return (0);
}

int
ws_accept_hdr(const char *secret, char *out, size_t olen)
{
	SHA1_CTX	 ctx;
	uint8_t		 hash[SHA1_DIGEST_LENGTH];

	SHA1Init(&ctx);
	SHA1Update(&ctx, secret, strlen(secret));
	SHA1Update(&ctx, WS_GUID, strlen(WS_GUID));
	SHA1Final(hash, &ctx);

	return (b64encode(hash, sizeof(hash), out, olen));
}

int
ws_read(struct client *clt, int *type, size_t *len)
{
	struct buf	*rbuf = &clt->bio.rbuf;
	size_t		 i;
	uint32_t	 mask;
	uint8_t		 first, second, op, plen;

	*type = WST_UNKNOWN, *len = 0;

	if (rbuf->len < 2) {
		errno = EAGAIN;
		return (-1);
	}

	memcpy(&first, &rbuf->buf[0], sizeof(first));
	memcpy(&second, &rbuf->buf[1], sizeof(second));

	/* for the close message this doesn't seem to be the case... */
#if 0
	/* the reserved bits must be zero, don't care about FIN */
	if ((first & 0x0E) != 0) {
		errno = EINVAL;
		return (-1);
	}
#endif

	/* mask must be set for messages sent by the clients */
	if ((second >> 7) != 1) {
		errno = EINVAL;
		return (-1);
	}

	op = first & 0x0F;
	plen = second & 0x7F;

	/* don't support extended payload length for now */
	if (plen >= 126) {
		errno = E2BIG;
		return (-1);
	}

	*len = plen;

	switch (op) {
	case WST_CONT:
	case WST_TEXT:
	case WST_BINARY:
	case WST_CLOSE:
	case WST_PING:
		*type = op;
		break;
	}

	if (rbuf->len < sizeof(first) + sizeof(second) + sizeof(mask) + plen) {
		errno = EAGAIN;
		return (-1);
	}

	buf_drain(rbuf, 2); /* header */
	memcpy(&mask, rbuf->buf, sizeof(mask));
	buf_drain(rbuf, 4);

	/* decode the payload */
	for (i = 0; i < plen; ++i)
		rbuf->buf[i] ^= mask >> (8 * (i % 4));

	return (0);
}

int
ws_compose(struct client *clt, int type, const void *data, size_t len)
{
	struct bufio	*bio = &clt->bio;
	uint16_t	 extlen = 0;
	uint8_t		 first, second;

	first = (type & 0x0F) | 0x80;

	if (len < 126)
		second = len;
	else {
		second = 126;

		/*
		 * for the extended length, the most significant bit
		 * must be zero.  We could use the 64 bit field but
		 * it's a waste.
		 */
		if (len > 0x7FFF) {
			errno = ERANGE;
			return (-1);
		}
		extlen = htons(len);
	}

	if (bufio_compose(bio, &first, 1) == -1 ||
	    bufio_compose(bio, &second, 1) == -1)
		goto err;

	if (extlen != 0 && bufio_compose(bio, &extlen, sizeof(extlen)) == -1)
		goto err;

	if (bufio_compose(bio, data, len) == -1)
		goto err;

	return (0);

 err:
	clt->err = 1;
	return (-1);
}
