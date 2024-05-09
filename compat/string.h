#include "config.h"
#include_next "string.h"

#if !HAVE_EXPLICIT_BZERO
void	 explicit_bzero(void *, size_t);
#endif

#if !HAVE_MEMMEM
void	*memmem(const void *, size_t, const void *, size_t);
#endif

#if !HAVE_MEMRCHR
void	*memrchr(const void *, int, size_t);
#endif

#if !HAVE_STRLCAT
size_t	 strlcat(char *, const char *, size_t);
#endif

#if !HAVE_STRLCPY
size_t	 strlcpy(char *, const char *, size_t);
#endif
