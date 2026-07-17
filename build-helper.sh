#!/bin/bash
set -euxo pipefail

# Builds nvenc-helper: a plain glibc binary that does all real
# CUDA/NVENC/NVDEC/libx264 work. Runs on any modern Debian/Ubuntu
# aarch64 host with the NVIDIA driver installed -- no musl, no
# Plex-specific toolchain, no compat shims. See README.md for why this
# process boundary exists.
#
# Usage: ./build-helper.sh [output-dir]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "$(dirname "${1:-./dist}")" && pwd)/$(basename "${1:-./dist}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# CI job containers typically already run as root (no sudo installed,
# or nothing left to elevate to) -- only shell out to sudo when we're
# actually unprivileged, so this script works unmodified both for a
# local interactive run and inside CI.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

$SUDO apt-get update -qq
$SUDO apt-get install -y -qq build-essential git pkg-config yasm nasm ca-certificates

cd "$WORK"

# --- x264 (GPL, CPU fallback encoder) ---
git clone --depth 1 https://code.videolan.org/videolan/x264.git x264-src
cd x264-src
./configure --prefix=/usr/local --enable-static --enable-pic --disable-cli --disable-opencl
make -j"$(nproc)"
$SUDO make install
cd "$WORK"

# --- nv-codec-headers (NVENC/NVDEC API headers, MIT-style license) ---
git clone --depth 1 --branch n11.1.5.3 https://github.com/FFmpeg/nv-codec-headers.git
$SUDO make -C nv-codec-headers install PREFIX=/usr/local

# --- FFmpeg 6.1, NVENC + NVDEC + libx264 enabled ---
git clone --depth 1 --branch n6.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
cd ffmpeg-src
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
./configure \
  --prefix=/usr/local \
  --enable-static \
  --disable-shared \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --disable-network \
  --enable-nonfree \
  --enable-gpl \
  --enable-cuda \
  --enable-cuvid \
  --enable-nvenc \
  --enable-nvdec \
  --enable-libx264 \
  --extra-cflags=-fPIC \
  --extra-cflags=-I/usr/local/include \
  --extra-ldflags=-L/usr/local/lib
make -j"$(nproc)"

gcc -O2 -I. -I/usr/local/include -I"$SCRIPT_DIR" \
  "$SCRIPT_DIR/nvenc-helper.c" \
  libavcodec/libavcodec.a libavutil/libavutil.a libswresample/libswresample.a \
  -L/usr/local/lib -lx264 \
  -lm -lpthread -ldl -o "$OUT/nvenc-helper"

echo "Built $OUT/nvenc-helper"
