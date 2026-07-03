#!/bin/sh
set -eux

# Builds libh264_nvenc_encoder.so: the Plex codec plugin itself. Must
# be built with a musl toolchain to match Plex Transcoder's own
# bundled musl libc on aarch64 (a glibc build fails to dlopen under
# Plex at all -- unresolved GLIBC-versioned symbols -- regardless of
# what the plugin does at runtime). This script assumes an Alpine
# Linux build environment (`docker run --rm -v $PWD:/src -w /src
# alpine:3.19 sh build-plugin.sh` works well).
#
# Usage: ./build-plugin.sh [output-dir]

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="$(mkdir -p "${1:-./dist}" && cd "${1:-./dist}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

apk add --no-cache build-base git

cd "$WORK"
# Only need FFmpeg's headers + a generated config.h (matching this
# build's feature set) for glue.c's type/struct definitions -- nothing
# here gets linked, see README.md's "Why glue.c stays this small"
# section.
git clone --depth 1 --branch n6.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
cd ffmpeg-src
./configure --disable-doc --disable-programs

gcc -shared -fPIC -O2 -I. -I"$SCRIPT_DIR" \
  -Wl,-rpath,'$ORIGIN' \
  -o "$OUT/libh264_nvenc_encoder.so" \
  "$SCRIPT_DIR/glue.c" -ldl

echo "Built $OUT/libh264_nvenc_encoder.so"
