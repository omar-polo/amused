.PHONY: all clean

PROG =		songmeta
SRCS =		songmeta.c flac.c id3v1.c id3v2.c ogg.c opus.c text.c \
		vorbis.c ../log.c

OBJS =		${SRCS:.c=.o} ${COBJS:=../compat/%}

DISTFILES =	Makefile songmeta.c flac.c id3v1.c id3v2.c ogg.c opus.c \
		text.c vorbis.c

all: ${PROG}

../Makefile.configure ../config.h: ../configure ../tests.c
	@echo "$@ is out of date; please run ../configure"
	@exit 1

include ../Makefile.configure

# --- targets ---

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_IMSG} \
		${LDADD_LIB_SOCKET}

clean:
	rm -f ${OBJS} ${OBJS:.o=.d} ${PROG}

distclean: clean

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}${BINDIR}
#	${INSTALL_MAN} songmeta.1 ${DESTDIR}${MANDIR}/man1/${PROG}.1

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/${PROG}.1

.c.o:
	${CC} -I.. -I../compat ${CFLAGS} -c $< -o $@

# --- maintainer targets ---

dist:
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/

# --- dependency management ---

# these .d files are produced during the first build if the compiler
# supports it.

-include flac.d
-include id3v1.d
-include id3v2.d
-include ogg.d
-include opus.d
-include songmeta.d
-include text.d
-include vorbis.d
-include ../compats.d
-include ../log.d
