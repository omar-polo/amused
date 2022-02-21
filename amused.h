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

#ifndef AMUSED_H
#define AMUSED_H

extern struct sio_hdl	*hdl;
extern char		*csock;
extern int		 debug;
extern int		 verbose;
extern int		 playing;
extern struct imsgev	*iev_player;

#define IMSG_DATA_SIZE(imsg)	((imsg).hdr.len - IMSG_HEADER_SIZE)

enum imsg_type {
	IMSG_PLAY,		/* fd + filename */
	IMSG_RESUME,
	IMSG_PAUSE,
	IMSG_STOP,
	IMSG_EOF,
	IMSG_ERR,

	IMSG_CTL_PLAY,		/* with optional filename */
	IMSG_CTL_TOGGLE_PLAY,
	IMSG_CTL_PAUSE,
	IMSG_CTL_STOP,
	IMSG_CTL_RESTART,
	IMSG_CTL_FLUSH,
	IMSG_CTL_SHOW,
	IMSG_CTL_STATUS,
	IMSG_CTL_NEXT,
	IMSG_CTL_PREV,
	IMSG_CTL_JUMP,
	IMSG_CTL_REPEAT,	/* struct player_repeat */

	IMSG_CTL_BEGIN,
	IMSG_CTL_ADD,		/* path to a file */
	IMSG_CTL_COMMIT,

	IMSG_CTL_MONITOR,

	IMSG_CTL_ERR,
};

struct imsgev {
	struct imsgbuf	 ibuf;
	void		(*handler)(int, short, void *);
	struct event	 ev;
	short		 events;
};

enum actions {
	NONE,
	PLAY,
	PAUSE,
	TOGGLE,
	STOP,
	RESTART,
	ADD,
	FLUSH,
	SHOW,
	STATUS,
	PREV,
	NEXT,
	LOAD,
	JUMP,
	REPEAT,
};

struct ctl_command;

struct player_repeat {
	int	repeat_one;
	int	repeat_all;
};

struct player_status {
	char			path[PATH_MAX];
	int			status;
	struct player_repeat	rp;
};

struct parse_result {
	enum actions		 action;
	char			**files;
	const char		*file;
	int			 pretty;
	struct player_repeat	 rep;
	struct ctl_command	*ctl;
};

struct ctl_command {
	const char		*name;
	enum actions		 action;
	int			(*main)(struct parse_result *, int, char **);
	const char		*usage;
	int			 has_pledge;
};

struct playlist;

/* amused.c */
void		spawn_daemon(void);
void		imsg_event_add(struct imsgev *iev);
int		imsg_compose_event(struct imsgev *, uint16_t, uint32_t,
		    pid_t, int, const void *, uint16_t);
int		main_send_player(uint16_t, int, const void *, uint16_t);
int		main_play_song(const char *);
void		main_playlist_jump(struct imsgev *, struct imsg *);
void		main_playlist_resume(void);
void		main_playlist_advance(void);
void		main_playlist_previous(void);
void		main_restart_track(void);
void		main_senderr(struct imsgev *, const char *);
void		main_enqueue(int, struct playlist *, struct imsgev *, struct imsg *);
void		main_send_playlist(struct imsgev *);
void		main_send_status(struct imsgev *);

/* ctl.c */
__dead void	usage(void);
__dead void	ctl(int, char **);

/* player.c */
int	player_setup(int, int, int);
void	player_senderr(void);
void	player_sendeof(void);
int	player_shouldstop(void);
int	player(int, int);

void	play_oggvorbis(int fd);
void	play_mp3(int fd);
void	play_flac(int fd);
void	play_opus(int fd);

#endif
