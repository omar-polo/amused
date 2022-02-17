/*
 * Copyright (c) 2022 Omar Polo <op@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <event.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sndio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <imsg.h>

#include "amused.h"
#include "control.h"
#include "log.h"
#include "playlist.h"
#include "xmalloc.h"

struct sio_hdl	*hdl;
char		*csock = NULL;
int		 debug;
int		 verbose;
struct imsgev	*iev_player;

const char	*argv0;
pid_t		 player_pid;
struct event	 ev_sigint;
struct event	 ev_sigterm;
struct event	 ev_siginfo;

enum amused_process {
	PROC_MAIN,
	PROC_PLAYER,
};

__dead void
main_shutdown(void)
{
	pid_t	pid;
	int	status;

	/* close pipes. */
	msgbuf_clear(&iev_player->ibuf.w);
	close(iev_player->ibuf.fd);
	free(iev_player);

	log_debug("waiting for children to terminate");
	do {
		pid = wait(&status);
		if (pid == -1) {
			if (errno != EINTR && errno != ECHILD)
				fatal("wait");
		} else if (WIFSIGNALED(status))
			log_warnx("player terminated; signal %d",
			    WTERMSIG(status));
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	log_info("terminating");
	exit(0);
}

static void
main_status(void)
{
	const char *cur;

	switch (play_state) {
	case STATE_STOPPED:
		log_info("status: stopped");
		break;
	case STATE_PLAYING:
		log_info("status: playing");
		break;
	case STATE_PAUSED:
		log_info("status: paused");
		break;
	default:
		log_info("status: unknown");
		break;
	}

	if ((cur = playlist_current()) != NULL)
		log_info("playing %s", cur);
	else
		log_info("not playing anything");
}

static void
main_sig_handler(int sig, short event, void *arg)
{
	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		main_shutdown();
		break;
	case SIGINFO:
		main_status();
		break;
	default:
		fatalx("unexpected signal %d", sig);
	}
}

static void
main_dispatch_player(int sig, short event, void *d)
{
	struct imsgev	*iev = d;
	struct imsgbuf	*ibuf = &iev->ibuf;
	struct imsg	 imsg;
	ssize_t		 n;
	int		 shut = 0;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_ERR:
			playlist_dropcurrent();
			/* fallthrough */
		case IMSG_EOF:
			main_playlist_advance();
			break;

		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}

	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead.  Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

static pid_t
start_child(enum amused_process proc, int fd)
{
	const char	*argv[5];
	int		 argc = 0;
	pid_t		 pid;

	switch (pid = fork()) {
	case -1:
		fatal("cannot fork");
	case 0:
		break;
	default:
		close(fd);
		return pid;
	}

	if (fd != 3) {
		if (fd != -1 && dup2(fd, 3) == -1)
			fatal("cannot setup imsg fd");
	} else if (fcntl(F_SETFD, 0) == -1)
		fatal("cannot setup imsg fd");

	argv[argc++] = argv0;
	switch (proc) {
	case PROC_MAIN:
		break;
	case PROC_PLAYER:
		argv[argc++] = "-Tp";
		break;
	}

	if (debug)
		argv[argc++] = "-d";
	if (verbose)
		argv[argc++] = "-v";
	argv[argc++] = NULL;

	/* obnoxious casts */
	execvp(argv0, (char *const *)argv);
	fatal("execvp %s", argv0);
}

/* daemon main routine */
static __dead int
amused_main(void)
{
	int	 pipe_main2player[2];
	int	 control_fd;

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);
	log_procinit("main");

	if (!debug)
		daemon(1, 0);

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    PF_UNSPEC, pipe_main2player) == -1)
		fatal("socketpair");

	player_pid = start_child(PROC_PLAYER, pipe_main2player[1]);

	event_init();

	signal_set(&ev_sigint, SIGINT, main_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, main_sig_handler, NULL);
	signal_set(&ev_siginfo, SIGINFO, main_sig_handler, NULL);

	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_siginfo, NULL);

	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	iev_player = xmalloc(sizeof(*iev_player));
	imsg_init(&iev_player->ibuf, pipe_main2player[0]);
	iev_player->handler = main_dispatch_player;
	iev_player->events = EV_READ;
	event_set(&iev_player->ev, iev_player->ibuf.fd, iev_player->events,
	    iev_player->handler, iev_player);
	event_add(&iev_player->ev, NULL);

	if ((control_fd = control_init(csock)) == -1)
		fatal("control socket setup failed %s", csock);
	control_listen(control_fd);

	if (pledge("stdio rpath unix sendfd", NULL) == -1)
		fatal("pledge");

	log_info("startup");
	event_dispatch();
	main_shutdown();
}

int
main(int argc, char **argv)
{
	int ch, proc = PROC_MAIN;

	log_init(1, LOG_DAEMON);	/* Log to stderr until daemonized */
	log_setverbose(1);

	argv0 = argv[0];
	if (argv0 == NULL)
		argv0 = "amused";

	while ((ch = getopt(argc, argv, "ds:T:vV")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 's':
			free(csock);
			csock = xstrdup(optarg);
			break;
		case 'T':
			switch (*optarg) {
			case 'p':
				proc = PROC_PLAYER;
				break;
			default:
				usage();
			}
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("%s version %s\n", getprogname(),
			    AMUSED_VERSION);
			exit(0);
		default:
			usage();
		}
	}
	argv += optind;
	argc -= optind;

	if (proc == PROC_PLAYER)
		exit(player(debug, verbose));

	if (csock == NULL)
		xasprintf(&csock, "/tmp/amused-%d", getuid());

	if (argc == 0)
		amused_main();
	else
		ctl(argc, argv);
	return 0;
}

void
spawn_daemon(void)
{
	debug = 0;
	start_child(PROC_MAIN, -1);
}

void
imsg_event_add(struct imsgev *iev)
{
	iev->events = EV_READ;
	if (iev->ibuf.w.queued)
		iev->events |= EV_WRITE;

	event_del(&iev->ev);
	event_set(&iev->ev, iev->ibuf.fd, iev->events, iev->handler, iev);
	event_add(&iev->ev, NULL);
}

int
imsg_compose_event(struct imsgev *iev, uint16_t type, uint32_t peerid,
    pid_t pid, int fd, const void *data, uint16_t datalen)
{
	int ret;

	if ((ret = imsg_compose(&iev->ibuf, type, peerid, pid, fd, data,
	    datalen)) != -1)
		imsg_event_add(iev);

	return ret;
}

int
main_send_player(uint16_t type, int fd, const void *data, uint16_t datalen)
{
	return imsg_compose_event(iev_player, type, 0, 0, fd, data, datalen);
}

int
main_play_song(const char *song)
{
	char path[PATH_MAX] = { 0 };
	int fd;

	strlcpy(path, song, sizeof(path));
	if ((fd = open(path, O_RDONLY)) == -1) {
		log_warn("open %s", path);
		return 0;
	}

	play_state = STATE_PLAYING;
	imsg_compose_event(iev_player, IMSG_PLAY, 0, 0, fd,
	    path, sizeof(path));
	return 1;
}

void
main_playlist_advance(void)
{
	const char *song;

	for (;;) {
		song = playlist_advance();
		if (song == NULL)
			return;

		if (main_play_song(song))
			break;

		playlist_dropcurrent();
	}
}

void
main_playlist_previous(void)
{
	const char *song;

	for (;;) {
		song = playlist_previous();
		if (song == NULL)
			return;

		if (main_play_song(song))
			break;

		playlist_dropcurrent();
	}
}

void
main_restart_track(void)
{
	const char *song;

	song = playlist_current();
	if (song == NULL)
		return;

	if (main_play_song(song))
		return;

	playlist_dropcurrent();
	main_playlist_advance();
}

void
main_enqueue(struct imsgev *iev, struct imsg *imsg)
{
	size_t datalen;
	char path[PATH_MAX] = { 0 };
	const char *err = NULL;

	datalen = IMSG_DATA_SIZE(*imsg);
	if (datalen != sizeof(path)) {
		err = "data size mismatch";
		goto err;
	}

	memcpy(path, imsg->data, sizeof(path));
	if (path[datalen-1] != '\0') {
		err = "malformed data";
		goto err;
	}

	playlist_enqueue(path);
	imsg_compose_event(iev, IMSG_CTL_ADD, 0, 0, -1, path, sizeof(path));
	return;
err:
	imsg_compose_event(iev, IMSG_CTL_ERR, 0, 0, -1, err, strlen(err)+1);
}

void
main_send_playlist(struct imsgev *iev)
{
	char path[PATH_MAX];
	size_t i;

	for (i = 0; i < playlist.len; ++i) {
		strlcpy(path, playlist.songs[i], sizeof(path));
		imsg_compose_event(iev, IMSG_CTL_SHOW, 0, 0, -1, path,
		    sizeof(path));
	}

	imsg_compose_event(iev, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
}

void
main_send_status(struct imsgev *iev)
{
	struct player_status s;
	const char *song;

	memset(&s, 0, sizeof(s));

	song = playlist_current();
	if (song != NULL)
		strlcpy(s.path, song, sizeof(s.path));
	s.status = play_state;

	imsg_compose_event(iev, IMSG_CTL_STATUS, 0, 0, -1, &s, sizeof(s));
}
