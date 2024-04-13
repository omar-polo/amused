/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
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

#include "config.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "amused.h"
#include "control.h"
#include "ev.h"
#include "log.h"
#include "playlist.h"
#include "xmalloc.h"

char		*csock = NULL;
int		 debug;
int		 verbose;
struct imsgev	*iev_player;

const char	*argv0;
pid_t		 player_pid;

enum amused_process {
	PROC_MAIN,
	PROC_PLAYER,
};

static __dead void
main_shutdown(void)
{
	pid_t	pid;
	int	status;

	/* close pipes. */
	msgbuf_clear(&iev_player->imsgbuf.w);
	close(iev_player->imsgbuf.fd);
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
main_sig_handler(int sig, int event, void *arg)
{
	/*
	 * Normal signal handler rules don't apply because ev.c
	 * decouples for us.
	 */

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		main_shutdown();
		break;
	default:
		fatalx("unexpected signal %d", sig);
	}
}

static void
main_dispatch_player(int sig, int event, void *d)
{
	char		*errstr;
	struct imsgev	*iev = d;
	struct imsgbuf	*imsgbuf = &iev->imsgbuf;
	struct imsg	 imsg;
	struct ibuf	 ibuf;
	size_t		 datalen;
	ssize_t		 n;
	int		 shut = 0;

	if (event & POLLIN) {
		if ((n = imsg_read(imsgbuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}
	if (event & POLLOUT) {
		if ((n = msgbuf_write(&imsgbuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(imsgbuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg_get_type(&imsg)) {
		case IMSG_POS:
			if (imsg_get_data(&imsg, &current_position,
			    sizeof(current_position)) == -1)
				fatalx("IMSG_POS: got wrong size");
			if (current_position < 0)
				current_position = -1;
			control_notify(IMSG_CTL_SEEK);
			break;
		case IMSG_LEN:
			if (imsg_get_data(&imsg, &current_duration,
			    sizeof(current_duration)) == -1)
				fatalx("IMSG_LEN: got wrong size");
			if (current_duration < 0)
				current_duration = -1;
			break;
		case IMSG_ERR:
			if (imsg_get_ibuf(&imsg, &ibuf) == -1 ||
			    (datalen = ibuf_size(&ibuf)) == 0)
				errstr = "unknown error";
			else {
				errstr = ibuf_data(&ibuf);
				errstr[datalen-1] = '\0';
			}
			log_warnx("%s; skipping %s", errstr, current_song);
			playlist_dropcurrent();
			main_playlist_advance();
			if (play_state == STATE_PLAYING)
				control_notify(IMSG_CTL_NEXT);
			else
				control_notify(IMSG_CTL_STOP);
			break;
		case IMSG_EOF:
			if (repeat_one && main_play_song(current_song))
				break;
			else if (repeat_one || consume)
				playlist_dropcurrent();
			main_playlist_advance();
			if (play_state == STATE_PLAYING)
				control_notify(IMSG_CTL_NEXT);
			else
				control_notify(IMSG_CTL_STOP);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg_get_type(&imsg));
			break;
		}
		imsg_free(&imsg);
	}

	if (shut)
		ev_break();
	else
		imsg_event_add(iev);
}

static pid_t
start_child(enum amused_process proc, int fd)
{
	const char	*argv[7];
	int		 argc = 0;
	pid_t		 pid;

	if (fd == -1 && debug)
		goto exec;

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

exec:
	argv[argc++] = argv0;

	switch (proc) {
	case PROC_MAIN:
		argv[argc++] = "-s";
		argv[argc++] = csock;
		argv[argc++] = "-Tm";
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
static __dead void
amused_main(void)
{
	int	 pipe_main2player[2];
	int	 control_fd, flags;

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);
	log_procinit("main");

	if (!debug && daemon(1, 0) == -1)
		fatal("daemon");

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_main2player) == -1)
		fatal("socketpair");

	if ((flags = fcntl(pipe_main2player[0], F_GETFL)) == -1 ||
	    fcntl(pipe_main2player[0], F_SETFL, flags | O_NONBLOCK) == -1 ||
	    (flags = fcntl(pipe_main2player[1], F_GETFL)) == -1 ||
	    fcntl(pipe_main2player[1], F_SETFL, flags | O_NONBLOCK) == -1)
		fatal("fcntl(O_NONBLOCK)");

	if (fcntl(pipe_main2player[0], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(pipe_main2player[1], F_SETFD, FD_CLOEXEC) == -1)
		fatal("fcntl(CLOEXEC)");

	player_pid = start_child(PROC_PLAYER, pipe_main2player[1]);

	if (ev_init() == -1)
		fatal("ev_init");

	signal(SIGHUP, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	ev_signal(SIGINT, main_sig_handler, NULL);
	ev_signal(SIGTERM, main_sig_handler, NULL);

	iev_player = xmalloc(sizeof(*iev_player));
	imsg_init(&iev_player->imsgbuf, pipe_main2player[0]);
	iev_player->handler = main_dispatch_player;
	iev_player->events = POLLIN;
	ev_add(iev_player->imsgbuf.fd, iev_player->events,
	    iev_player->handler, iev_player);

	if ((control_fd = control_init(csock)) == -1)
		fatal("control socket setup failed %s", csock);
	control_listen(control_fd);

	if (pledge("stdio rpath unix sendfd", NULL) == -1)
		fatal("pledge");

	log_info("startup");
	ev_loop();
	main_shutdown();
}

int
main(int argc, char **argv)
{
	int ch, proc = -1;

	log_init(1, LOG_DAEMON);	/* Log to stderr until daemonized */
	log_setverbose(1);

	argv0 = argv[0];
	if (argv0 == NULL)
		argv0 = "amused";

	while ((ch = getopt(argc, argv, "ds:T:v")) != -1) {
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
			case 'm':
				proc = PROC_MAIN;
				break;
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
		default:
			usage();
		}
	}
	argv += optind;
	argc -= optind;

	if (proc == PROC_MAIN)
		amused_main();
	if (proc == PROC_PLAYER)
		exit(player(debug, verbose));

	if (csock == NULL) {
		const char *tmpdir;

		if ((tmpdir = getenv("TMPDIR")) == NULL)
			tmpdir = "/tmp";

		xasprintf(&csock, "%s/amused-%d", tmpdir, getuid());
	}

	if (argc > 0)
		debug = 0;

	ctl(argc, argv);
}

void
spawn_daemon(void)
{
	start_child(PROC_MAIN, -1);
}

void
imsg_event_add(struct imsgev *iev)
{
	iev->events = POLLIN;
	if (iev->imsgbuf.w.queued)
		iev->events |= POLLOUT;

	ev_add(iev->imsgbuf.fd, iev->events, iev->handler, iev);
}

int
imsg_compose_event(struct imsgev *iev, uint16_t type, uint32_t peerid,
    pid_t pid, int fd, const void *data, uint16_t datalen)
{
	int ret;

	if ((ret = imsg_compose(&iev->imsgbuf, type, peerid, pid, fd, data,
	    datalen)) != -1)
		imsg_event_add(iev);

	return ret;
}

int
main_send_player(uint16_t type, int fd, const void *data, size_t len)
{
	return imsg_compose_event(iev_player, type, 0, 0, fd, data, len);
}

int
main_play_song(const char *path)
{
	struct stat sb;
	int fd;

	if ((fd = open(path, O_RDONLY)) == -1) {
		log_warn("open %s", path);
		return 0;
	}

	if (fstat(fd, &sb) == -1) {
		log_warn("failed to stat %s", path);
		close(fd);
		return 0;
	}

	if (!S_ISREG(sb.st_mode)) {
		log_info("skipping non-regular file: %s", path);
		close(fd);
		return 0;
	}

	play_state = STATE_PLAYING;
	main_send_player(IMSG_PLAY, fd, NULL, 0);
	return 1;
}

void
main_playlist_jump(struct imsgev *iev, struct imsg *imsg)
{
	char arg[PATH_MAX];
	const char *song;

	if (imsg_get_data(imsg, arg, sizeof(arg)) == -1) {
		main_senderr(iev, "wrong size");
		return;
	}

	if (arg[sizeof(arg)-1] != '\0') {
		main_senderr(iev, "data corrupted");
		return;
	}

	song = playlist_jump(arg);
	if (song == NULL) {
		main_senderr(iev, "not found");
		return;
	}

	control_notify(IMSG_CTL_JUMP);

	main_send_player(IMSG_STOP, -1, NULL, 0);
	if (!main_play_song(song)) {
		main_senderr(iev, "can't play");
		playlist_dropcurrent();
		main_playlist_advance();
		return;
	}

	main_send_status(iev);
}

void
main_playlist_resume(void)
{
	const char *song;

	if ((song = current_song) == NULL)
		song = playlist_advance();

	for (; song != NULL; song = playlist_advance()) {
		if (main_play_song(song))
			return;

		playlist_dropcurrent();
	}
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
main_senderr(struct imsgev *iev, const char *msg)
{
	imsg_compose_event(iev, IMSG_CTL_ERR, 0, 0, -1,
	    msg, strlen(msg)+1);
}

void
main_enqueue(int tx, struct playlist *px, struct imsgev *iev,
    struct imsg *imsg)
{
	char path[PATH_MAX];
	const char *err = NULL;

	if (imsg_get_data(imsg, path, sizeof(path)) == -1) {
		err = "data size mismatch";
		goto err;
	}

	if (path[sizeof(path)-1] != '\0') {
		err = "malformed data";
		goto err;
	}

	if (tx)
		playlist_push(px, path);
	else
		playlist_enqueue(path);
	imsg_compose_event(iev, IMSG_CTL_ADD, 0, 0, -1, path, sizeof(path));
	return;
err:
	main_senderr(iev, err);
}

void
main_send_playlist(struct imsgev *iev)
{
	struct player_status s;
	size_t i;

	for (i = 0; i < playlist.len; ++i) {
		memset(&s, 0, sizeof(s));
		strlcpy(s.path, playlist.songs[i], sizeof(s.path));
		s.status = play_off == i ? STATE_PLAYING : STATE_STOPPED;
		imsg_compose_event(iev, IMSG_CTL_SHOW, 0, 0, -1, &s,
		    sizeof(s));
	}

	imsg_compose_event(iev, IMSG_CTL_SHOW, 0, 0, -1, NULL, 0);
}

void
main_send_status(struct imsgev *iev)
{
	struct player_status s;

	memset(&s, 0, sizeof(s));

	if (current_song != NULL)
		strlcpy(s.path, current_song, sizeof(s.path));
	s.status = play_state;
	s.position = current_position;
	s.duration = current_duration;
	s.mode.repeat_all = repeat_all;
	s.mode.repeat_one = repeat_one;
	s.mode.consume = consume;

	imsg_compose_event(iev, IMSG_CTL_STATUS, 0, 0, -1, &s, sizeof(s));
}

void
main_seek(struct player_seek *s)
{
	switch (play_state) {
	case STATE_STOPPED:
		main_playlist_resume();
		break;
	case STATE_PLAYING:
		break;
	case STATE_PAUSED:
		play_state = STATE_PLAYING;
		break;
	}

	main_send_player(IMSG_CTL_SEEK, -1, s, sizeof(*s));
}
