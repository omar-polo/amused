DISTFILES =	endian.h explicit_bzero.c fcntl.h flock.c freezero.c \
		getopt.c getprogname.c imsg-buffer.c imsg.c memmem.c \
		memrchr.c recallocarray.c setproctitle.c sha1.c stdlib.h \
		string.h strlcat.c strlcpy.c strtonum.c timespecsub.c \
		unistd.h \
		Makefile

all:
	false

dist:
	mkdir -p ${DESTDIR}/
	${INSTALL} -m 0644 ${DISTFILES} ${DESTDIR}/
	mkdir -p ${DESTDIR}/imsg
	${INSTALL} -m 0644 imsg/imsg.h ${DESTDIR}/imsg
	mkdir -p ${DESTDIR}/queue/sys
	${INSTALL} -m 0644 queue/sys/queue.h ${DESTDIR}/queue/sys
	mkdir -p ${DESTDIR}/sha1
	${INSTALL} -m 0644 sha1/sha1.h ${DESTDIR}/sha1
	mkdir -p ${DESTDIR}/sys
	${INSTALL} -m 0644 sys/time.h ${DESTDIR}/sys
