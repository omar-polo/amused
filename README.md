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
 - libmd (optional; needed for amused-web on linux and Mac)

Then, to build:

	$ ./configure
	$ make
	# make install # eventually

To include the metadata extractor utility, songmeta, use:

	$ ./configure --with-songmeta

To include the DBus control interface for amused for MPRIS2 support, use:

	$ ./configure --with-mpris2

`amused-mpris2` requires glib2.

To disable amused-web:

	$ ./configure --without-web

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
command to remove an item from the playlist, or sort it; instead,
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

amused-web is a simple web interface to control the player.  It opens an
HTTP server on localhost:

	$ amused-web

I wrote a bit more about the background of amused [in a blog
post](https://www.omarpolo.com/post/amused.html).


## Building on Android (termux) -- Experimental

amused can be built on android using the [oboe][oboe] backend,
although this has only been tested so far under [termux][termux].
First, oboe needs to be built locally.  Then build amused with:

	$ ./configure BACKEND=oboe \
		CXXFLAGS="-I /path/to/oboe/include" \
		LDADD="/path/to/liboboe.a"
	[...]
	$ make

tip: use `termux-setup-storage` to access the android storage in
`~/storage`.

amused-web works and can be used to control the playback, but as amused
doesn't respond to the events (calls, headsets buttons, other apps
playing music, etc...) it's not particularly pleasing to use.

`contrib/amused-termux-notification` shows a persistent notification
with the song file name and buttons to control the playback, making
slightly more nicer to use it.

[oboe]: https://github.com/google/oboe/
[termux]: https://termux.dev/en/


## License

Amused is free software.  All the code is distributed under a
BSD-style license or is Public Domain, with the only exception of a
part of amused-mpris2 which is under the GPLv2 or later.  Since
amused-mpris2 is an optional, separate executable, it's the only one
with the GPL restrictions.
