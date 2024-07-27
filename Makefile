.PHONY: all songmeta web clean distclean \
	install install-amused install-songmeta install-web

VERSION =	0.16
PROG =		amused
DISTNAME =	${PROG}-${VERSION}

SOURCES =	amused.c \
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

OBJS =		${SOURCES:.c=.o} audio_${BACKEND}.o ${COBJS:%=compat/%}

HEADERS =	amused.h \
		audio.h \
		control.h \
		ev.h \
		log.h \
		player.h \
		playlist.h \
		xmalloc.h

DISTFILES =	CHANGES \
		LICENSE \
		Makefile \
		README.md \
		amused.1 \
		configure \
		configure.local.example \
		tests.c \
		${HEADERS} \
		${SOURCES} \
		audio_alsa.c \
		audio_ao.c \
		audio_oboe.cpp \
		audio_oss.cpp \
		audio_sndio.c

TOPDIR =	.

include config.mk

all: ${PROGS}

config.mk config.h: configure tests.c
	@echo "$@ is out of date; please run ./configure"
	@exit 1

# -- targets --

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_IMSG} \
		${LDADD_DECODERS} ${LDADD_LIB_SOCKET} ${LDADD_BACKEND}

songmeta:
	${MAKE} -C songmeta

web:
	${MAKE} -C web

clean:
	rm -f ${OBJS} ${OBJS:.o=.d} ${PROG}
	-${MAKE} -C songmeta clean
	-${MAKE} -C web clean

distclean: clean
	rm -f config.mk config.h config.h.old config.log config.log.old

install:
	${MAKE} ${PROGS:%=install-%}

install-amused:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} amused.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-songmeta:
	${MAKE} -C songmeta install

install-web:
	${MAKE} -C web install

install-local: amused songmeta web
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin
	${MAKE} -C songmeta install-local
	${MAKE} -C web install-local

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

.c.o:
	${CC} -I. -Icompat ${CFLAGS} -DBUFIO_WITHOUT_TLS -c $< -o $@

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
	${MAKE} -C compat DESTDIR=${PWD}/.dist/${DISTNAME}/compat dist
	${MAKE} -C songmeta DESTDIR=${PWD}/.dist/${DISTNAME}/songmeta dist
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
