# plex-nvenc-rpc

Hardware-accelerated (NVIDIA NVENC/NVDEC) transcoding for Plex Media
Server on **aarch64**, where Plex does not currently ship an
NVENC-capable transcoder build.

Tested on: NVIDIA T600, aarch64 host, `lscr.io/linuxserver/plex`
container image, Plex Media Server `1.43.2.10687-563d026ea`,
transcoder build hash `c75335c-a7cfb6836f3ed63280a7eb83`. Real,
validated hardware decode (H.264 and 10-bit HEVC) and hardware encode
(H.264 and HEVC) against real media, including a full video+audio
transcode session driven by an unmodified, real Plex Media Server.

## Why this exists

Plex's per-platform `Plex Transcoder` binary supports an external
codec-plugin mechanism: it reads the `FFMPEG_EXTERNAL_LIBS`
environment variable (a `:`-separated list of `.so` paths), loads each
one, and calls a single exported function,
`av_init_library(void **ctx, unsigned int log_level)`, letting the
plugin register additional encoders/decoders into the process's live
FFmpeg codec table. This is a real, general-purpose interoperability
interface — the same mechanism Plex's own bundled codec plugins
(`libh264_decoder.so`, `liblibx264_encoder.so`, etc.) are built on.

No NVENC/NVDEC-capable codec plugin is currently shipped for
`linux-aarch64`, even though the NVIDIA driver, CUDA runtime, and
NVENC/NVDEC hardware are all present and fully functional on capable
aarch64 hosts (confirmed directly: `cuInit()`, `NvEncodeAPI`, and a
full `libx264`/`h264_cuvid`/`h264_nvenc` round-trip all work cleanly
under plain glibc on this hardware). This project is a plugin built
against that same interface, filling that gap.

## Architecture

Plex's own `Plex Transcoder` binary on aarch64 is linked against a
**musl libc** it bundles itself (`ld-musl-aarch64.so.1`), not the host
OS's glibc. NVIDIA's userspace driver libraries (`libcuda.so.1`,
`libnvidia-encode.so.1`, etc.) are glibc binaries. A plugin `.so`
doesn't carry its own dynamic linker — whichever linker loaded the
host process services every later `dlopen()` call too, including ones
made by a plugin — so a plugin loaded into Plex Transcoder's musl
process cannot reliably load and run NVIDIA's glibc driver libraries
in-process. (We got a long way down that path before concluding it
doesn't converge: see `docs/history.md` for the specific musl/glibc
incompatibilities hit and fixed one at a time.)

The fix is a process boundary, not a compatibility shim:

```
Plex Transcoder (musl)                    nvenc-helper (glibc)
  |                                          |
  |  libh264_nvenc_encoder.so (glue.c)       |  real FFmpeg 6.1, built
  |  registers h264_nvenc/hevc_nvenc/        |  with --enable-nvenc
  |  h264_nvdec/hevc_nvdec/libx264,          |  --enable-nvdec
  |  fork()+exec()s nvenc-helper,            |  --enable-libx264
  |  talks to it over a socketpair()  <----> |  (real libcuda.so.1,
  |  using a small length-prefixed           |  libnvidia-encode.so.1,
  |  framing protocol (ipc_protocol.h)       |  all glibc, all native)
```

`glue.c` is deliberately tiny (~800 lines, no FFmpeg linkage, no CUDA
code, no `dlopen` of any NVIDIA library) — it only uses FFmpeg's public
struct/type *definitions* as headers so the compiler lays out its
`FFCodec` entries identically to Plex's own build, and speaks the
`av_init_library` ABI plus our own IPC protocol. All the real
encode/decode work — and every NVIDIA/CUDA/musl-vs-glibc concern —
lives entirely in `nvenc-helper`, a completely ordinary glibc program.
This also makes future Plex version updates cheap: `glue.c`'s
`EXPECTED_BUILD_HASH` constant is normally the only thing that needs
to change (a few-second recompile, since it doesn't link FFmpeg at
all), and `nvenc-helper` essentially never needs to change since it
has zero dependency on Plex's build.

### Registered codecs

- `h264_nvenc` / `hevc_nvenc` — hardware encoders, real NVENC. Expose a
  `crf` and `preset` option, translated to NVENC's own quality/preset
  scales (`rc=vbr`/`cq` for `crf`; `p1`..`p7` for `preset`) — necessary
  because Plex's own internal codec resolution can open these directly
  (bypassing the `libx264` hijack below) even when the command line
  itself says `-codec:0 libx264`, so a real `priv_class` is what lets
  the requested quality/speed settings land anywhere at all instead of
  being silently dropped before encoding starts.
- `h264` / `hevc` (decode) — hardware decoders, real NVDEC (`cuvid`).
  Registered under the plain codec name because Plex Transcoder always
  requests decode by the generic codec name; whichever plugin is
  registered under that name wins.
- `h264_nvdec` / `hevc_nvdec` — the same decoders, under an additional
  name. Included because Plex Media Server's own hardware-transcode
  capability check looks for a decoder registered under a
  `<codec>_nvdec`-style name before it will emit hardware-encoder
  flags for a session; it does not appear to check whether the plain
  `h264`/`hevc` decoder is itself hardware-capable.
- `libx264` — **hijacked**. Plex Media Server's own hardware-transcode
  capability check (internal to Plex Media Server, not part of this
  project) did not treat this plugin's encoders as available for real,
  server-driven transcode sessions, even after every capability signal
  we could find was satisfied (`AV_CODEC_CAP_HARDWARE`, `hw_configs`,
  the `_nvdec`/`_nvenc` naming convention above). Plex Media Server
  explicitly writes `-codec:0 libx264` into the Transcoder command
  line as its own decision, made before Transcoder even starts — so it
  was the only remaining lever. This registration always attempts a
  real GPU encode first (forwarding `crf`/`preset`, translated the same
  way as the plain `h264_nvenc`/`hevc_nvenc` registrations above) and
  transparently falls back to a genuine, bundled `libx264` CPU encode
  (forwarding the same `crf`/`preset`/`x264opts` values Plex actually
  requested, with full option fidelity since it's genuinely libx264) if
  the GPU attempt fails for any reason, so it does not force GPU
  encoding unconditionally.

See `docs/history.md` for the full build/debug history, including the
`av_init_library` interface this plugin implements against.

## Building

Two independent builds, two different libc targets:

```sh
# nvenc-helper: plain glibc, run on a real aarch64 Linux host with the
# NVIDIA driver installed (needs sudo for apt-get/make install).
./build-helper.sh ./dist

# libh264_nvenc_encoder.so: musl, easiest via Docker.
docker run --rm -v "$PWD:/src" -w /src alpine:3.19 sh build-plugin.sh ./dist
```

Both scripts write into `./dist`.

## Installing

Drop both files into Plex's per-build codec directory (find the exact
path via `find / -iname 'liblibx264_encoder.so' 2>/dev/null` inside
your Plex container — it looks like
`Plex Media Server/Codecs/<hash>-linux-aarch64/`):

```sh
cp dist/libh264_nvenc_encoder.so dist/nvenc-helper /path/to/Codecs/<hash>-linux-aarch64/
```

That alone gets you working `h264_nvenc`/`hevc_nvenc`/`h264_nvdec`/
`hevc_nvdec` encoders/decoders, usable by anything that requests them
explicitly (e.g. `Plex Transcoder -c:v h264_nvenc ...` by hand, or any
tool that lets you pick the encoder name directly).

**To get real, automatic Plex-driven sessions using the GPU** (not
just manual invocation), the plain `h264`/`hevc`/`libx264` names need
to *win* over Plex's own same-named plugins, and Plex has its own
codec self-healing that silently restores files it expects to find
missing — a plain file-drop doesn't survive that. The robust way is a
**read-only bind mount** of `libh264_nvenc_encoder.so` over each of
`libh264_decoder.so`, `libhevc_decoder.so`, and
`liblibx264_encoder.so` in that same directory (all four names point
at the identical file — it registers all seven codecs regardless of
which path loads it). See `k8s-example/deployment-snippet.yaml` for a
worked Kubernetes example; the same idea applies to a Docker Compose
bind mount.

**Back up the original three files first.** If anything goes wrong,
removing the bind mounts restores stock behavior instantly.

## Status / known limitations

- Extensively tested against real 10-bit HEVC Blu-ray-remux content:
  hardware decode, hardware encode, and the `libx264` GPU-first/
  CPU-fallback routing, including a full video+audio session with a
  real, unmodified Plex Media Server. See `docs/history.md` for the
  detailed validation log.
- `nvenc-helper` currently assumes 8-bit `YUV420P` output over the IPC
  boundary; 10-bit `P010LE` source frames are downsampled to 8-bit
  before being sent (full-fidelity 10-bit passthrough is not
  implemented).
- If `Plex Transcoder` exits abnormally (not a clean codec close), its
  forked `nvenc-helper` child can be orphaned, holding GPU memory until
  manually killed. Not yet hardened with a supervising reaper.
- The `libx264` hijack's GPU-vs-CPU decision is "always prefer GPU,
  fall back to CPU only if GPU init fails" — no awareness of e.g.
  concurrent session limits or wanting to reserve GPU capacity for
  something else.
- Tested against exactly the Plex build/version noted at the top of
  this file. `EXPECTED_BUILD_HASH` in `glue.c` will reject a build
  hash mismatch outright (fails safe) rather than risk an ABI
  mismatch.

## Legal

This project contains no Plex source code. `nvenc-helper.c` and
`glue.c` are original code, built against Plex's own external
codec-plugin loading behavior (the same interface Plex's own bundled
codec plugins use), licensed GPLv2 (see `LICENSE`) since `nvenc-helper`
statically links FFmpeg built with `--enable-gpl` for `libx264`.

This is not affiliated with, endorsed by, or supported by Plex Inc.
Use at your own risk against your own Plex installation.
