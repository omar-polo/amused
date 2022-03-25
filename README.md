# amused

amused is a music player.  It doesn't have any amazing functionalities
built-in, on the contrary: it's quite minimal (a fancy word to say that
does very little.)  It composes well, or aims to do so, with other tools
thought.

The main feature is that audio decoding runs in a sandboxed process
under `pledge("stdio recvfd audio")`.  Oh, by the way, amused targets
OpenBSD only: it relies its make infrastructure to build, uses various
cool stuff from its libc and can output only to sndio.

(I *think* it's possible to compile it on other UNIX-like systems too by
providing shims for some non-portable functions -- hello libbsd -- and
assuming that sndio is available.  And bundling a copy of imsg.c too)


## building

	$ make

it needs the following packages from ports:

 - flac
 - libmpg123
 - libvorbis
 - opusfile

Release tarballs installs into `/usr/local/`, git checkouts installs
into `~/bin` (idea and implementation stolen from got, thanks stsp!)

It'll be available on OpenBSD starting with 7.1


## usage

The fine man page has all nitty gritty details, but the TL;DR is

 - enqueue music with `amused add files...`
 - control the playback with `amused play|pause|toggle|stop` etc

amused tries to be usable in composition with other more familiar tools
instead of providing everything itself.  For instance, there isn't a
command to remove an item from the playlist, or shuffle it; instead,
standard UNIX tools can be used:

	$ amused show | grep -vi kobayashi | amused load
	$ amused show | sort -R | amused load
	$ amused show | sort | uniq | amused load

It also doesn't provide any means to manage a music collection.  It
plays nice with find(1) however:

	find . -type f -iname \*.opus -exec amused add {} +

I wrote a bit more about the background of amused [in a blog
post](https://www.omarpolo.com/post/amused.html).
