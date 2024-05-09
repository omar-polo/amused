#include "config.h"
#include_next "sys/time.h"

#if !HAVE_TIMESPECSUB
struct timespec;
void timespecsub(struct timespec *, struct timespec *, struct timespec *);
#endif
