{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  # Build tools + libraries needed to build amused with the pipewire backend.
  nativeBuildInputs = [
    pkgs.gcc
    pkgs.gnumake
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.pipewire   # audio backend (--backend=pipewire)
    pkgs.flac       # decoder
    pkgs.mpg123     # decoder
    pkgs.opusfile   # decoder
    pkgs.libvorbis  # decoder
    pkgs.libmd      # amused-web on Linux
  ];

  shellHook = ''
    echo "amused dev shell. build: ./configure --backend=pipewire && make"
  '';
}
