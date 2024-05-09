#include "config.h"
#include_next "fcntl.h"

#if !HAVE_FLOCK
int	 flock(int, int);
#endif
