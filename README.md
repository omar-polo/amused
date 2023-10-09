# amused

amused is a music player.  It doesn't have any amazing features
built-in, on the contrary: it's quite minimal (a fancy word to say
that does very little.)  It composes well, or aims to do so, with
other tools though.

The main feature is that audio decoding runs in a sandboxed process
under `pledge("stdio recvfd audio")` (on OpenBSD at least.)

It's available on the OpenBSD port tree starting from 7.1


## Building

The dependencies are:

 - flac
 - libmpg123
 - libvorbis
 - opusfile
 - libsndio or libasound (ALSA) or libao
 - libmd (optional; needed by amused-web on linux and Mac)

Then, to build:

	$ ./configure
	$ make
	# make install # eventually

To compile the web control interface, amused-web, run:

	$ make web
	# make install-web # eventually

The build can be customized by passing arguments to the configure
script or by using a `configure.local` file; see `./configure -h`
and [`configure.local.example`](configure.local.example) for more
information.

For each library the `configure` script first tries to see if they're
available without any extra flags, then tries again with some
hard-coded flags (e.g. `-lFLAC` for flac) and finally resorts to
pkg-config if available.  pkg-config auto-detection can be disable by
passing `PKG_CONFIG=false` (or the empty string)

For Linux users with libbsd installed, the configure script can be
instructed to use libbsd exclusively as follows:

	$ CFLAGS="$(pkg-config --cflags libbsd-overlay)" \
		./configure LDFLAGS="$(pkg-config --libs libbsd-overlay)"

To force the use of one specific audio backend and not simply the first
one found, pass `--backend` as:

	$ ./configure --backend=alsa # or sndio, or ao


## Usage

The fine man page has all nitty gritty details, but the TL;DR is

 - enqueue music with `amused add files...` or `amused load <playlist`
 - control the playback with `amused play|pause|toggle|stop`
 - check the status with `amused status` and the current playlist with
   `amused show`

amused tries to be usable in composition with other more familiar tools
instead of providing everything itself.  For instance, there isn't a
command to remove an item from the playlist, or shuffle it; instead,
standard UNIX tools can be used:

	$ amused show | grep -vi kobayashi | amused load
	$ amused show | sort -R | amused load
	$ amused show | sort | uniq | amused load

It also doesn't provide any means to manage a music collection.  It
plays nice with find(1) however:

	$ find . | amused load

Non-music files found in the playlist are automatically skipped and
removed, so there's no harm in loading everything under a certain
directory.

I wrote a bit more about the background of amused [in a blog
post](https://www.omarpolo.com/post/amused.html).
