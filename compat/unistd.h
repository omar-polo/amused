#include "config.h"
#include_next "unistd.h"

#if !HAVE_GETDTABLECOUNT
/* XXX: on linux it should be possible to inspect /proc/self/fd/ */
#define getdtablecount() (0)
#endif

#if !HAVE_GETDTABLESIZE
#define getdtablesize() (sysconf(_SC_OPEN_MAX))
#endif

#if !HAVE_OPTRESET
/* replace host' getopt with OpenBSD' one */
#define opterr		BSDopterr
#define optind		BSDoptind
#define optopt		BSDoptopt
#define optreset	BSDoptreset
#define optarg		BSDoptarg
#define getopt		BSDgetopt

extern int BSDopterr, BSDoptind, BSDoptopt, BSDoptreset;
extern char *BSDoptarg;
int	 BSDgetopt(int, char * const *, const char *);
#endif
