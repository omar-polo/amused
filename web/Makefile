.PHONY: all clean

PROG =		amused-web

SOURCES =	web.c http.c ../log.c ../playlist.c ../xmalloc.c

OBJS =		${SOURCES:.c=.o}

DISTFILES =	Makefile amused-web.1 http.c web.h

all: ${PROG}

../Makefile.configure ../config.h: ../configure ../tests.c
	@echo "$@ is out of date; please run ../configure"
	@exit 1

include ../Makefile.configure

# --- targets ---

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD}

clean:
	rm -f ${OBJS} ${OBJS:.o=.d} ${PROG}

distclean: clean

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} amused-web.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

.c.o:
	${CC} ${CFLAGS} -I../ -c $< -o $@

# --- maintainer targets ---

dist:
	mkdir -p ${DESTDIR}/web
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/web

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include http.d
-include web.d
-include ../log.d
-include ../playlist.d
-include ../xmalloc.d
