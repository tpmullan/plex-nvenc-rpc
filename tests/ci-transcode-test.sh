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
# arbitrary. Confirmed 2026-07-16/17: a video-only `-f null -` smoke
# test (or even a segment-muxer test without concurrent audio) PASSES
# CLEANLY even against a plugin build that crashes production 100% of
# the time -- the real crash only reproduces with the actual
# combination PMS uses for real sessions: the HLS segment muxer
# (`-f ssegment`) together with a SECOND, concurrently-transcoded audio
# stream. A simpler test here would give a false pass and defeat the
# entire point of this gate. Do not simplify this command without
# re-validating against a known-bad build first.
#
# Fails (non-zero exit) if Plex Transcoder crashes, exits non-zero, or
# glue.c's own crash handler logs a CRASH: line -- any of which means
# this plugin build is not safe to promote for the currently running
# Plex image.
#
# Usage: plex-nvenc-ci-transcode-test.sh <plugin-so> <nvenc-helper> <stock-aac-decoder-so> <test-clip>
#   (paths to the freshly built libh264_nvenc_encoder.so, nvenc-helper,
#   the target image's own stock libaac_decoder.so -- needed alongside
#   our plugin since FFMPEG_EXTERNAL_LIBS only loads what's explicitly
#   listed, and a real stock AAC decoder plugin from the SAME Codecs/
#   <hash>-linux-aarch64/ directory this test's Plex image binary
#   expects -- and a small real test video with HEVC video + AAC audio,
#   e.g. sources/plex-nvenc-plugin/tests/testdata/test-clip-hevc.mkv)

PLUGIN_SO="$1"
NVENC_HELPER="$2"
STOCK_AAC_DECODER="$3"
TEST_CLIP="$4"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/out"

cp "$PLUGIN_SO" "$WORK/libh264_nvenc_encoder.so"
cp "$NVENC_HELPER" "$WORK/nvenc-helper"
chmod +x "$WORK/libh264_nvenc_encoder.so" "$WORK/nvenc-helper"

TRANSCODER="/usr/lib/plexmediaserver/Plex Transcoder"
LOGFILE="$WORK/nvenc-glue.log"

echo "==> Running real transcode (HEVC decode + libx264-hijacked GPU encode + concurrent audio + segment muxer)..."
STATUS=0
FFMPEG_EXTERNAL_LIBS="$WORK/libh264_nvenc_encoder.so:$STOCK_AAC_DECODER" \
  "$TRANSCODER" \
    -codec:0 hevc -codec:1 aac -y -i "$TEST_CLIP" \
    -start_at_zero -copyts -fps_mode cfr -t 2.5 \
    -filter_complex "[0:0]scale=w=640:h=360:force_divisible_by=4[0];[0]format=pix_fmts=yuv420p|nv12[1]" -map "[1]" \
    -codec:0 libx264 -crf:0 23 -maxrate:0 1000k -bufsize:0 2000k -r:0 25 -preset:0 veryfast \
    -x264opts:0 "subme=2:me_range=4:rc_lookahead=20:me=hex" -force_key_frames:0 "expr:gte(t,n_forced*8)" \
    -filter_complex "[0:1] aresample=async=1:ochl=stereo:rematrix_maxval=0.000000dB:osr=48000[2]" -map "[2]" \
    -codec:1 libopus -b:1 128k \
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
