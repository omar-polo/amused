#include "config.h"
#include_next "stdlib.h"

#if !HAVE_FREEZERO
void		 freezero(void *, size_t);
#endif

#if !HAVE_GETPROGNAME
const char	*getprogname(void);
#endif

#if !HAVE_RECALLOCARRAY
void		*recallocarray(void *, size_t, size_t, size_t);
#endif

#if !HAVE_SETPROCTITLE
void		 setproctitle(const char *, ...);
#endif

#if !HAVE_STRTONUM
long long	 strtonum(const char *, long long, long long, const char **);
#endif
