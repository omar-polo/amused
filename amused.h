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

	IMSG_CTL_PLAY,
	IMSG_CTL_TOGGLE_PLAY,
	IMSG_CTL_PAUSE,
	IMSG_CTL_STOP,
	IMSG_CTL_RESTART,
	IMSG_CTL_ADD,
	IMSG_CTL_FLUSH,
	IMSG_CTL_SHOW,

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
};

struct ctl_command;

struct parse_result {
	enum actions		 action;
	char			**files;
	struct ctl_command	*ctl;
};

struct ctl_command {
	const char		*name;
	enum actions		 action;
	int			(*main)(struct parse_result *, int, char **);
	const char		*usage;
	int			 has_pledge;
};

/* amused.c */
void		imsg_event_add(struct imsgev *iev);
int		imsg_compose_event(struct imsgev *, uint16_t, uint32_t,
		    pid_t, int, const void *, uint16_t);
int		main_play_song(const char *);
void		main_enqueue(struct imsgev *, struct imsg *);

/* ctl.c */
__dead void	usage(void);
__dead void	ctl(int, char **);

/* player.c */
int	player_setrate(int);
void	player_senderr(void);
void	player_sendeof(void);
int	player_shouldstop(void);
int	player(int, int);

void	play_oggvorbis(int fd);
void	play_mp3(int fd);
void	play_flac(int fd);
void	play_opus(int fd);

#endif
