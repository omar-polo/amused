.PHONY: all clean

PROG =		amused-mpris2

SOURCES =	mpris2.c ../log.c ../xmalloc.c

OBJS =		${SOURCES:.c=.o} ${COBJS:%=../compat/%}

DISTFILES =	Makefile amused-mpris2.1 mpris2.c

TOPDIR =	..

all: ${PROG}

../config.mk ../config.h: ../configure ../tests.c
	@echo "$@ is out of date; please run ../configure"
	@exit 1

include ../config.mk

# --- targets ---

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_IMSG} \
		${LDADD_LIB_GLIB} ${LDADD_LIB_SOCKET} ${LDADD_LIB_MD}

clean:
	rm -f ${OBJS} ${OBJS:.o=.d} ${PROG}

distclean: clean

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} amused-mpris2.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

.c.o:
	${CC} -I../ -I../compat ${CFLAGS} -c $< -o $@

# --- maintainer targets ---

dist:
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include mpris.d
-include ../log.d
-include ../xmalloc.d
