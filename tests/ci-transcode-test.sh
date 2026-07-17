#!/bin/sh
set -eu

# Runs inside a CI job whose `image:` is the target Plex image itself
# (GitLab CI overrides the entrypoint to run this script, so PMS's own
# init never runs -- deliberately not needed here: this test doesn't
# need a real running PMS or a seeded Codecs/<hash>-.../ directory, it
# invokes Plex Transcoder directly with FFMPEG_EXTERNAL_LIBS pointing
# straight at the freshly-built plugin, the same pattern used manually
# throughout this plugin's development -- see
# docs/plans/2026-07-02-plex-nvenc-transcoder.md).
#
# IMPORTANT: the exact command shape below is load-bearing, not
# arbitrary. Confirmed 2026-07-16/17: a plain `-f null -` smoke test
# PASSES CLEANLY against a plugin build that segfaults Plex Transcoder
# 100% of the time in real sessions (signal 11, small faulting
# address, immediately after libx264_hijack_init succeeds -- see the
# home-automation repo's plan doc for the full crash writeup). The
# real crash reproduces reliably once the HLS segment muxer
# (`-f ssegment` with `-individual_header_trailer 0 -flags
# +global_header`, matching PMS's own real invocation) is used --
# confirmed this is sufficient on its own; concurrent audio
# transcoding is NOT required to trigger it (tested both ways). Audio
# is deliberately left out of this test: reproducing the crash doesn't
# need it, and including it would require a stock audio-decoder plugin
# from a seeded Codecs/<hash>-.../ directory that a bare CI container
# doesn't have (PMS only creates that directory on its own first run,
# nowhere else in the image -- confirmed via `find` inside a running
# pod). Do not simplify this command to a plain `-f null -` test
# without re-validating against a known-bad build first -- that gives
# a false pass and defeats the entire point of this gate.
#
# Fails (non-zero exit) if Plex Transcoder crashes, exits non-zero, or
# glue.c's own crash handler logs a CRASH: line -- any of which means
# this plugin build is not safe to promote for the currently running
# Plex image.
#
# Usage: ci-transcode-test.sh <plugin-so> <nvenc-helper> <test-clip>
#   (paths to the freshly built libh264_nvenc_encoder.so, nvenc-helper,
#   and a small real HEVC test video --
#   sources/plex-nvenc-plugin/tests/testdata/test-clip-hevc.mkv)

PLUGIN_SO="$1"
NVENC_HELPER="$2"
TEST_CLIP="$3"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/out"

cp "$PLUGIN_SO" "$WORK/libh264_nvenc_encoder.so"
cp "$NVENC_HELPER" "$WORK/nvenc-helper"
chmod +x "$WORK/libh264_nvenc_encoder.so" "$WORK/nvenc-helper"

TRANSCODER="/usr/lib/plexmediaserver/Plex Transcoder"
LOGFILE="$WORK/nvenc-glue.log"

echo "==> Running real transcode (HEVC decode + libx264-hijacked GPU encode + HLS segment muxer)..."
STATUS=0
FFMPEG_EXTERNAL_LIBS="$WORK/libh264_nvenc_encoder.so" \
  "$TRANSCODER" \
    -codec:0 hevc -y -i "$TEST_CLIP" -t 2.5 -map 0:v:0 \
    -codec:0 libx264 -crf:0 23 -maxrate:0 1000k -bufsize:0 2000k -r:0 25 -preset:0 veryfast \
    -segment_format matroska -f ssegment -individual_header_trailer 0 -flags +global_header \
    -segment_time 8 -segment_list "$WORK/out/manifest.csv" -segment_list_type csv \
    "$WORK/out/media-%05d.ts" >"$WORK/transcoder.out" 2>&1 || STATUS=$?

echo "--- Plex Transcoder output ---"
cat "$WORK/transcoder.out"
echo "--- end output ---"

FAIL=0
if [ "$STATUS" -ne 0 ]; then
  echo "FAIL: Plex Transcoder exited non-zero/was killed by a signal ($STATUS)"
  FAIL=1
fi
if grep -q '^\[.*CRASH:' "$LOGFILE" 2>/dev/null; then
  echo "FAIL: crash detected in plugin log:"
  grep '^\[.*CRASH:' "$LOGFILE"
  FAIL=1
fi
if ! grep -q 'using GPU (h264_nvenc)' "$WORK/transcoder.out" "$LOGFILE" 2>/dev/null; then
  echo "FAIL: no confirmation the GPU path was actually exercised (never saw 'using GPU')"
  FAIL=1
fi
if [ ! -s "$WORK/out/media-00000.ts" ]; then
  echo "FAIL: no output segment file was produced"
  FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
  echo "==> Transcode test FAILED -- this plugin build is not safe for this Plex image."
  exit 1
fi

echo "==> Transcode test PASSED."
