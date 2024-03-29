.PHONY: all web clean distclean install install-web

VERSION =	0.15
PROG =		amused
DISTNAME =	${PROG}-${VERSION}

SOURCES =	amused.c \
		compats.c \
		control.c \
		ctl.c \
		ev.c \
		log.c \
		player.c \
		player_123.c \
		player_flac.c \
		player_oggvorbis.c \
		player_opus.c \
		playlist.c \
		xmalloc.c

OBJS =		${SOURCES:.c=.o} audio_${BACKEND}.o

HEADERS =	amused.h \
		control.h \
		ev.h \
		log.h \
		playlist.h \
		xmalloc.h

DISTFILES =	CHANGES \
		LICENSE \
		Makefile \
		README.md \
		amused.1 \
		configure \
		configure.local.example \
		endian.h \
		imsg.h \
		queue.h \
		tests.c \
		${HEADERS} \
		${SOURCES} \
		audio_alsa.c \
		audio_ao.c \
		audio_oboe.cpp \
		audio_sndio.c

all: ${PROG}

Makefile.configure config.h: configure tests.c
	@echo "$@ is out of date; please run ./configure"
	@exit 1

include Makefile.configure

# -- targets --

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_IMSG} \
		${LDADD_DECODERS} ${LDADD_LIB_SOCKET} ${LDADD_BACKEND}

web:
	${MAKE} -C web

clean:
	rm -f ${OBJS} ${OBJS:.o=.d} ${PROG}
	-${MAKE} -C web clean

distclean: clean
	rm -f Makefile.configure config.h config.h.old config.log config.log.old

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} amused.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-web:
	${MAKE} -C web install

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin
	${MAKE} -C web install-local

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

# --- maintainer targets ---

dist: ${DISTNAME}.sha256

${DISTNAME}.sha256: ${DISTNAME}.tar.gz
	sha256 ${DISTNAME}.tar.gz > $@

${DISTNAME}.tar.gz: ${DISTFILES}
	mkdir -p .dist/${DISTNAME}
	${INSTALL} -m 0644 ${DISTFILES} .dist/${DISTNAME}
	cd .dist/${DISTNAME} && chmod 755 configure
	cd .dist/${DISTNAME} && cp -R ../../contrib . && \
		chmod 755 contrib/amused-monitor
	${MAKE} -C web DESTDIR=${PWD}/.dist/${DISTNAME}/web dist
	cd .dist && tar zcf ../$@ ${DISTNAME}
	rm -rf .dist/

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include amused.d
-include audio_alsa.d
-include audio_ao.d
-include audio_oboe.d
-include audio_sndio.d
-include compats.d
-include control.d
-include ctl.d
-include ev.d
-include log.d
-include player.d
-include player_123.d
-include player_flac.d
-include player_oggvorbis.d
-include player_opus.d
-include playlist.d
-include xmalloc.d
