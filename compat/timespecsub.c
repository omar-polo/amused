/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 */

#include "config.h"

#include <sys/time.h>

void
timespecsub(struct timespec *a, struct timespec *b, struct timespec *ret)
{
	ret->tv_sec = a->tv_sec - b->tv_sec;
	ret->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (ret->tv_nsec < 0) {
		ret->tv_sec--;
		ret->tv_nsec += 1000000000L;
	}
}
