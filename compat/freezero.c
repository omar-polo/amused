/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

void
freezero(void *ptr, size_t len)
{
	if (ptr == NULL)
		return;
	memset(ptr, 0, len);
	free(ptr);
}
