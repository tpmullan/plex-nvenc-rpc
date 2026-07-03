# Build and validation history

## The plugin-loading interface

Plex Transcoder sets `FFMPEG_EXTERNAL_LIBS` to a colon-separated list
of `.so` paths before spawning a transcode. For each path, it loads
the library and calls a single exported symbol:

```c
int av_init_library(void **ctx, unsigned int log_level);
```

`ctx` is a small array of host-provided callbacks, at fixed indices:

- `ctx[2]` — a log callback
- `ctx[3]` — `av_version_info()`, the host's FFmpeg build-hash string
- `ctx[4]` — `avcodec_version()`, the host's packed FFmpeg ABI version
- `ctx[5]` — a registration function: call it with a pointer to a
  fully-populated `FFCodec` struct (the same internal type FFmpeg's
  own codecs use) to add that codec to the live, in-process codec
  table

A correct plugin checks `ctx[4]()` and `ctx[3]()` against the exact
build it was compiled for (a plain `strcmp`, no cryptographic
verification of any kind) before calling `ctx[5]()` with each codec it
wants to register. There is no signature or manifest check anywhere in
this path — any `.so` placed in the right directory and returning `0`
from `av_init_library` gets its codecs live.

## Why an in-process plugin doesn't work for NVENC/NVDEC

Plex Transcoder on aarch64 is linked against its own bundled musl
libc, not the host glibc. The NVIDIA driver's userspace libraries
(`libcuda.so.1`, `libnvidia-encode.so.1`) are glibc binaries. A `.so`
loaded into a running process doesn't bring its own dynamic linker —
whatever linker is already resident for that process handles every
subsequent `dlopen()` too — so a plugin loaded into Plex Transcoder's
musl process is stuck using musl's loader for the NVIDIA libraries as
well, even though those libraries themselves are glibc-linked.

This mostly works far enough to be misleading. Musl's loader resolves
most of the needed symbols and gets a fair way into real NVIDIA driver
initialization before hitting problems that are specific to the
musl/glibc boundary rather than to NVENC/NVDEC itself:

- Some glibc-versioned symbols (`fcntl64@GLIBC_2.28`) simply aren't
  present in musl at all — a hard, immediate load failure.
- `__getauxval` and `dlvsym` aren't implemented by musl; both are easy
  to shim (read `/proc/self/auxv` directly; fall back to plain
  `dlsym`), but they're just the first two, not the last.
- musl's loader does an independent bare-name search for every
  `NEEDED` entry in a library it's loading — it does not, unlike
  glibc, reuse an already-loaded library by matching `SONAME`. NVIDIA
  driver libraries live under a multiarch path musl doesn't search by
  default, requiring absolute paths patched into the library that
  looks them up, plus rewriting one `NEEDED` entry (a copy, not
  NVIDIA's original file) to an absolute path too.
- Deepest one: `gnu_get_libc_version()` is a glibc-only introspection
  call with no musl equivalent. Unlike the earlier gaps, an unresolved
  reference to it doesn't fail at load time — it leaves a null
  function pointer that only crashes when the driver actually calls
  it, mid-initialization, well after the plugin has already loaded and
  registered successfully. Shimming it (returning a plausible glibc
  version string) moves the crash further into initialization rather
  than resolving it — at that point, the next-nearest crash was inside
  the driver's own internal per-device state initialization, with no
  further musl-side symbol gap to fill.

Every fix in that list is a real, working fix for the specific gap it
addresses. What doesn't work is generalizing from "we found and fixed
N musl/glibc gaps" to "musl/glibc compatibility is achievable this
way" — the number of remaining gaps isn't knowable in advance, and the
last one found was already inside NVIDIA's own closed-source
initialization logic, not resolvable with a userspace shim at all.
This is what motivated the process-boundary design instead: a
*process's* dynamic linker is fixed at exec() time by that process's
own `PT_INTERP`, so a genuinely separate process gets a real glibc
loader, unconditionally — a `.so` cannot get this by any means, since
it's just data interpreted by whichever linker is already running.

## Getting real decode/encode data through the boundary

With the CUDA/NVENC/NVDEC work moved into `nvenc-helper` (a normal
glibc program with no compatibility concerns at all), the remaining
work was almost entirely about the IPC boundary and getting FFmpeg's
own option/queue conventions right, not about NVIDIA or musl at all:

- **Missing codec extradata.** The first hardware-decode attempt
  against real container-demuxed content (MKV) produced zero frames
  and no error, despite identical code succeeding against a bare
  elementary stream in isolated testing. The difference: MKV/MP4 store
  a codec's SPS/PPS/VPS parameter sets out-of-band, in the container's
  `extradata`, not inline in the packet stream — and that value simply
  wasn't being forwarded across the IPC boundary at all. Once
  `nvenc-helper`'s decoder init received the real `extradata`, decode
  started working immediately.
- **10-bit content.** Real HEVC media is very often 10-bit even when
  visually indistinguishable from 8-bit; NVDEC returns `P010LE` frames
  for that content instead of `NV12`. The IPC frame format is fixed at
  8-bit `YUV420P`, so `nvenc-helper` downsamples (takes the high byte
  of each 16-bit little-endian sample) before sending. This is a
  deliberate simplification, not a limitation of NVDEC itself — see
  "Known limitations" in the main README.
- **A queue-draining protocol bug.** The IPC helper processes one
  request fully (draining every ready result plus a terminal "done"
  marker) before reading the next one — a naive client that sends a
  new frame on every call without first fully draining the previous
  response leaves an ever-growing backlog in the socket buffers, which
  works fine on a short test clip and would eventually deadlock a real
  multi-thousand-frame session. Fixed by having the plugin side buffer
  results locally and only issue new work once the previous response
  is fully drained.
- **`AVOption`/private-data layout.** FFmpeg's convention for any codec
  that exposes tunable options (`crf`, `preset`, etc.) is that the
  first field of its private data struct must literally be the
  `AVClass*` the host writes there before the codec's own init runs —
  a fixed, documented convention, not something specific to any one
  codec's implementation. Missing it produces a low-level assertion
  failure the first time the host validates a freshly-opened codec.

## Registering under Plex's own default encoder name (`libx264`)

Every registered codec above (`h264_nvenc`, `hevc_nvenc`, `h264`,
`hevc`, `h264_nvdec`, `hevc_nvdec`) worked correctly under direct
invocation from the moment each was added. What was not straightforward
was getting a real, automatic, server-driven Plex transcode session to
actually *choose* to use them.

Live testing (with Plex's own debug logging enabled) showed the server
logging a line matching `"hardware transcoding: enabled, but no
hardware decode accelerator found"` on every session, even after real
hardware decode was demonstrably active (confirmed independently via
GPU process/memory listings during the session) — a genuine mismatch
between what Plex's own capability check believed and what was
actually happening. A number of targeted attempts to satisfy that
check directly (matching real hardware-codec capability flags exactly,
adding the same hardware-config declarations real hardware decoders
use, adding an additional decoder registration under a
`<codec>_nvdec`-style name) did not change the outcome for the
*encoder* selection specifically, even once decode-side behavior did
shift.

What did consistently work, independent of any of that: Plex
Transcoder's own command line always requests decode by the plain
codec name (`-codec:0 hevc`), and whichever plugin is registered under
that exact name is what actually runs — a mechanism completely
decoupled from whatever Plex's own capability check believes,
demonstrated repeatedly across many real sessions once the plain-name
registrations were in place. Encode is different: the server explicitly
writes either `-codec:0 libx264` or `-codec:0 h264_nvenc` into the
command line itself, as its own decision, before Transcoder even
starts — there's no generic name for Transcoder's own resolution to
land on. Since that decision consistently came back `libx264` in every
real session tested regardless of what capability signals were added,
registering under the name `libx264` itself — with a policy that
always attempts real GPU encode first and only falls back to a
genuine, bundled CPU `libx264` encode if the GPU attempt fails — is
what actually gets a real, automatic session onto the GPU. Confirmed
against multiple real, unmodified Plex sessions: GPU process/memory
listings during playback showed real hardware decode and encode
running simultaneously, sustained, with no errors.

## Quality/bitrate silently dropped on the GPU path

Getting the GPU path selected turned out not to be the whole story.
Live sessions ran on the GPU, at the correct resolution, but at a
severely bitrate-starved, visually degraded quality — easy to mistake
for a resolution bug at first, since the symptom (blocky, low-detail
video) looks similar, but the actual encoded resolution was always
correct; only the bitrate was collapsing.

Root cause: Plex requests quality via `-crf:0 N` (constant rate
factor), not an explicit target bitrate. The `libx264` hijack's GPU
attempt was forwarding a hardcoded "not set" sentinel instead of the
real requested value, so it never reached the encoder — NVENC then
fell back to its own bare-minimum default bitrate (independent of
resolution), regardless of what Plex actually asked for.

Fixing that hijack path alone didn't fully resolve it, because of a
second, more fundamental issue: live testing showed Plex's own
internal codec resolution sometimes opens the plain `h264_nvenc`/
`hevc_nvenc` registrations *directly* — bypassing the `libx264` hijack
entirely — even while the literal Transcoder command line still reads
`-codec:0 libx264 -crf:0 N`. Since those two registrations had no
`priv_class` (no AVOption table at all), any `-crf` value targeting
them had nowhere to be written by the host's own generic option
handling and was silently dropped before the plugin's own code ever
ran — confirmed by adding an unbuffered raw syscall write at the very
first line of both codecs' init callbacks and observing it never fire
for a real session, while manually invoking `-codec:0 h264_nvenc
-crf:0 N` directly reproduced the identical silent-drop behavior.
Fixed by giving `h264_nvenc`/`hevc_nvenc` a real `priv_class` with
their own `crf`/`preset` options, translated to NVENC's own scales
(`rc=vbr`+`cq` for quality, `p1`..`p7` for preset) inside
`nvenc-helper`, so the request lands correctly regardless of which of
the two registrations actually ends up handling a given session.

A third, unrelated wrinkle compounded the debugging: this plugin is
delivered onto Plex's container via three separate Kubernetes
`subPath` bind mounts of one underlying file (see the main README's
"Installing" section). `subPath` bind mounts are pinned to the inode
present at container start — overwriting the underlying file in place
does not propagate to the mounted copies until the container restarts,
even though a fresh read of the "real" path shows the new content
immediately. Every fix during this investigation had to be followed by
a full pod restart, not just a file update, for it to actually take
effect in the running session — a build/deploy step easy to miss since
nothing about it fails loudly.
