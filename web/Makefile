.PHONY: all clean

PROG =		amused-web

SOURCES =	web.c bufio.c http.c ws.c \
		../ev.c ../log.c ../playlist.c ../xmalloc.c

OBJS =		${SOURCES:.c=.o} ${COBJS:%=../compat/%}

DISTFILES =	Makefile amused-web.1 bufio.c bufio.h http.c http.h \
		web.c ws.c ws.h

TOPDIR =	..

all: ${PROG}

../Makefile.configure ../config.h: ../configure ../tests.c
	@echo "$@ is out of date; please run ../configure"
	@exit 1

include ../Makefile.configure

# --- targets ---

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_IMSG} \
		${LDADD_LIB_MD} ${LDADD_LIB_SOCKET}

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
	${CC} -I.. -I../compat ${CFLAGS} -DBUFIO_WITHOUT_TLS -c $< -o $@

# --- maintainer targets ---

dist:
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include bufio.d
-include http.d
-include web.d
-include ws.d
-include ../ev.d
-include ../log.d
-include ../playlist.d
-include ../xmalloc.d
