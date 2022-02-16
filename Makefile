PROG=	amused
SRCS=	amused.c control.c log.c xmalloc.c player.c ctl.c playlist.c \
	player_flac.c player_mad.c player_opus.c player_oggvorbis.c

.include "amused-version.mk"

CPPFLAGS +=	-DAMUSED_VERSION=\"${AMUSED_VERSION}\" \
		-I/usr/local/include -I/usr/local/include/opus

LDADD =	-levent -lm -lsndio -lutil \
	-L/usr/local/lib -lmad -lvorbisfile -lopusfile -lFLAC
DPADD =	${LIBEVENT} ${LIBM} ${LIBSNDIO} ${LIBUTIL}

.if "${AMUSED_RELEASE}" == "Yes"
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
MANDIR ?= ${PREFIX}/man/man
.else
CFLAGS += -Werror -Wall -Wstrict-prototypes -Wunused-variable
PREFIX ?= ${HOME}
BINDIR ?= ${PREFIX}/bin
BINOWN ?= ${USER}
.if !defined(BINGRP)
BINGRP != id -g -n
.endif
DEBUG = -O0 -g
.endif

release: clean
	sed -i -e 's/_RELEASE=No/_RELEASE=Yes/' amused-version.mk
	${MAKE} dist
	sed -i -e 's/_RELEASE=Yes/_RELEASE=No/' amused-version.mk

dist: clean
	mkdir /tmp/amused-${AMUSED_VERSION}
	pax -rw * /tmp/amused-${AMUSED_VERSION}
	find /tmp/amused-${AMUSED_VERSION} -type d -name obj -delete
	rm /tmp/amused-${AMUSED_VERSION}/amused-dist.txt
	tar -C /tmp -zcf amused-${AMUSED_VERSION}.tar.gz amused-${AMUSED_VERSION}
	rm -rf /tmp/amused-${AMUSED_VERSION}
	tar -ztf amused-${AMUSED_VERSION}.tar.gz | \
		sed -e 's/^amused-${AMUSED_VERSION}//' | \
		sort > amused-dist.txt.new
	diff -u amused-dist.txt{,.new}
	rm amused-dist.txt.new

.include <bsd.prog.mk>
