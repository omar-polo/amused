# amused

amused is a music player.  It doesn't have any amazing functionalities
built-in, on the contrary: it's quite minimal (a fancy word to say
that does very little.)  It composes well, or aims to do so, with
other tools, find(1) in particular.

The main feature is that the process of decoding the audio from the
files is done in a sandboxed project that runs with `pledge("stdio
recvfd audio")`.  Oh, by the way, amused targets OpenBSD only: it
relies its make infrastructure to build, uses various cool stuff
from its libc and can output only to sndio.

(I *think* it's possible to compile it on other UNIX-like systems
too by providing shims for some non-portable functions -- hello
libbsd -- and assuming that sndio is available.  Oh, and that you
bundle a copy of imsg.c too)


## building

	$ make

it needs the following packages from ports:

 - flac
 - libmad
 - libvorbis
 - opusfile

Release tarballs installs into `/usr/local/`, git checkouts installs
into `~/bin` (idea and implementation stolen from got, thanks stsp!)


## usage

The fine man page has all nitty gritty details, but the TL;DR is

 - enqueue music with `amused add files...`
 - control the playback with `amused play|pause|toggle|stop` etc

Pro tip: amused plays well with find:

	find . -type f -iname \*.opus -exec amused add {} +

Well, for these kinds of things I wrote a wrapper around find called
walk that's very handy in combo with amused too!

	walk \*.opus ! amused add

(walk lives in my [dotfiles](//git.omarpolo.com/dotsnew))
