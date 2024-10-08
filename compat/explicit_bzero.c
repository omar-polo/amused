/*
 * Public domain.
 * Written by Ted Unangst
 */

#include "config.h"

#include <string.h>

/*
 * explicit_bzero - don't let the compiler optimize away bzero
 */

#if HAVE_MEMSET_S

void
explicit_bzero(void *p, size_t n)
{
	if (n == 0)
		return;
	(void)memset_s(p, n, 0, n);
}

#else /* HAVE_MEMSET_S */

#include <strings.h>

/*
 * Indirect memset through a volatile pointer to hopefully avoid
 * dead-store optimisation eliminating the call.
 */
static void (* volatile ssh_memset)(void *, int, size_t) =
    (void (*volatile)(void *, int, size_t))memset;

void
explicit_bzero(void *p, size_t n)
{
	if (n == 0)
		return;
	/*
	 * clang -fsanitize=memory needs to intercept memset-like functions
	 * to correctly detect memory initialisation. Make sure one is called
	 * directly since our indirection trick above successfully confuses it.
	 */
#if defined(__has_feature)
# if __has_feature(memory_sanitizer)
	memset(p, 0, n);
# endif
#endif

	ssh_memset(p, 0, n);
}

#endif /* HAVE_MEMSET_S */
