.PHONY: all clean distclean install

VERSION =	0.10

PROG =		amused

SOURCES =	amused.c \
		compats.c \
		control.c \
		ctl.c \
		log.c \
		player.c \
		player_123.c \
		player_flac.c \
		player_oggvorbis.c \
		player_opus.c \
		playlist.c \
		xmalloc.c

OBJS =		${SOURCES:.c=.o}

HEADERS =	amused.h \
		control.h \
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
		imsg.h \
		queue.h \
		tests.c \
		${HEADERS} \
		${SOURCES}

all: ${PROG}

Makefile.configure config.h: configure tests.c
	@echo "$@ is out of date; please run ./configure"
	@exit 1

include Makefile.configure

# -- targets --

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD}

clean:
	rm -f ${OBJS} ${PROG}

distclean: clean
	rm -f Makefile.configure config.h config.h.old config.log config.log.old

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} amused.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

# --- maintainer targets ---

dist: ${PROG}-${VERSION}.sha256

${PROG}-${VERSION}.sha256: ${PROG}-${VERSION}.tar.gz
	sha256 ${PROG}-${VERSION}.tar.gz > $@

${PROG}-${VERSION}.tar.gz: ${DISTFILES}
	mkdir -p .dist/${PROG}-${VERSION}
	${INSTALL} -m 0644 ${DISTFILES} .dist/${PROG}-${VERSION}
	cd .dist/${PROG}-${VERSION} && chmod 755 configure
	cd .dist && tar zcf ../$@ ${PROG}-${VERSION}
	rm -rf .dist/

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include amused.d
-include compats.d
-include control.d
-include ctl.d
-include log.d
-include player.d
-include player_123.d
-include player_flac.d
-include player_oggvorbis.d
-include player_opus.d
-include playlist.d
-include xmalloc.d
