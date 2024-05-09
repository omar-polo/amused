/*
 * Copyright (c) 2023 Omar Polo <op@omarpolo.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/socket.h>

#include <ao/ao.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "log.h"

static void		(*onmove_cb)(void *, int);
static int		 sp[2]; /* main, audio thread */
static pthread_t	 at;

static int		 bpf;
static ao_sample_format	 fmt;
static char		 buf[BUFSIZ];
static size_t		 buflen;

static void *
aworker(void *d)
{
	ao_sample_format f;
	ao_device	*device = NULL;
	ssize_t		 r;
	int		 sock = sp[1];
	char		 ch;

	memset(&f, 0, sizeof(f));

	log_info("%s: starting", __func__);

	for (;;) {
		ch = 1;
		if ((r = write(sock, &ch, 1)) == -1)
			fatal("write");
		if (r == 0)
			break;

		if ((r = read(sock, &ch, 1)) == -1)
			fatal("read");
		if (r == 0)
			break;

		if (memcmp(&fmt, &f, sizeof(f)) != 0) {
			if (device != NULL)
				ao_close(device);
			device = ao_open_live(ao_default_driver_id(),
			    &fmt, NULL);
			if (device == NULL) {
				switch (errno) {
				case AO_ENODRIVER:
					log_warnx("ao: no driver found");
					break;
				case AO_ENOTLIVE:
					log_warnx("ao: not a live device");
					break;
				case AO_EBADOPTION:
					log_warnx("ao: bad option(s)");
					break;
				case AO_EOPENDEVICE:
					log_warnx("ao: failed to open device");
					break;
				case AO_EFAIL:
				default:
					log_warnx("ao: failed opening driver");
					break;
				}
				errno = EINVAL;
				break;
			}
			log_info("%s: device (re)opened", __func__);
			memcpy(&f, &fmt, sizeof(f));
		}

		if (ao_play(device, buf, buflen) == 0) {
			log_warnx("ao_play failed");
			break;
		}
	}

	log_info("quitting audio thread");
	close(sock);
	return NULL;
}

int
audio_open(void (*cb)(void *, int))
{
	ao_initialize();
	onmove_cb = cb;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1) {
		log_warn("socketpair");
		return (-1);
	}

	if (pthread_create(&at, NULL, aworker, NULL) == -1) {
		log_warn("pthread_create");
		return (-1);
	}

	return 0;
}

int
audio_setup(unsigned int bits, unsigned int rate, unsigned int channels,
    struct pollfd *pfds, int nfds)
{
	fmt.bits = bits;
	fmt.rate = rate;
	fmt.channels = channels;
	fmt.byte_format = AO_FMT_NATIVE;
	fmt.matrix = NULL;

	if (bits == 8)
		bpf = 1;
	else if (bits == 16)
		bpf = 2;
	else if (bits == 24 || bits == 32)
		bpf = 4;
	else {
		log_warnx("can't handle %d bits", bits);
		return -1;
	}

	return 0;
}

int
audio_nfds(void)
{
	return 1;
}

int
audio_pollfd(struct pollfd *pfds, int nfds, int events)
{
	if (nfds != 1) {
		errno = EINVAL;
		return -1;
	}

	pfds[0].fd = sp[0];
	pfds[0].events = POLLIN;
	return 0;
}

int
audio_revents(struct pollfd *pfds, int nfds)
{
	if (nfds != 1) {
		log_warnx("%s: called with nfds=%d", __func__, nfds);
		return 0;
	}

	/* don't need to check; if we're here the audio thread is ready */
	return POLLOUT;
}

size_t
audio_write(const void *data, size_t len)
{
	char	 ch;
	ssize_t	 r;

	if ((r = read(sp[0], &ch, 1)) == -1) {
		log_warn("ao/%s: read failed", __func__);
		return 0;
	}
	if (r == 0)
		return 0;

	if (len > sizeof(buf))
		len = sizeof(buf);

	memcpy(buf, data, len);
	buflen = len;

	ch = 1;
	if ((r = write(sp[0], &ch, 1)) == -1) {
		log_warn("ao/%s: write failed", __func__);
		return 0;
	}
	if (r == 0) {
		log_warnx("ao/%s: write got EOF", __func__);
		return 0;
	}

	if (onmove_cb)
		onmove_cb(NULL, len / bpf);

	return len;
}

int
audio_flush(void)
{
	return 0;
}

int
audio_stop(void)
{
	return 0;
}
