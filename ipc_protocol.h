/*
 * IPC protocol between the Plex plugin (glue.c, runs inside Plex
 * Transcoder's musl process) and nvenc-helper (separate glibc process
 * that does all real CUDA/NVENC work). Entirely our own design, no
 * dependency on Plex's build -- stable across Plex updates.
 *
 * Transport: a single AF_UNIX SOCK_STREAM fd, created via socketpair()
 * by glue.c before fork()+exec() and inherited by the helper on fd 3
 * (dup2'd there before exec). No filesystem socket path, no cleanup,
 * no race between helper startup and glue.c connecting.
 *
 * Framing: every message is
 *   [u32 LE total_len]  (length of everything after this field)
 *   [u8  type]          (IPC_MSG_*)
 *   [payload, total_len - 1 bytes]
 */
#ifndef NVENC_IPC_PROTOCOL_H
#define NVENC_IPC_PROTOCOL_H

#include <stdint.h>

#define IPC_HELPER_FD 3

enum {
    /* glue.c -> nvenc-helper (encode direction) */
    IPC_MSG_INIT       = 1,  /* struct ipc_init_req */
    IPC_MSG_FRAME       = 2, /* struct ipc_frame_hdr + raw planar frame data */
    IPC_MSG_FLUSH       = 3, /* no payload */
    IPC_MSG_SHUTDOWN    = 4, /* no payload */

    /* nvenc-helper -> glue.c (encode direction) */
    IPC_MSG_INIT_OK     = 5,  /* struct ipc_init_ok + extradata bytes */
    IPC_MSG_INIT_ERR    = 6,  /* payload: NUL-terminated error string */
    IPC_MSG_PACKET      = 7,  /* struct ipc_packet_hdr + encoded bytes */
    IPC_MSG_FRAME_DONE  = 8,  /* no payload -- all packets for this FRAME/FLUSH sent */
    IPC_MSG_ERR         = 9,  /* payload: NUL-terminated error string */

    /* glue.c -> nvenc-helper (decode direction) */
    IPC_MSG_DEC_INIT    = 10, /* struct ipc_dec_init_req */
    IPC_MSG_DEC_PACKET  = 11, /* struct ipc_dec_packet_hdr + compressed bytes; empty payload = flush */

    /* nvenc-helper -> glue.c (decode direction) */
    IPC_MSG_DEC_INIT_OK  = 12, /* no payload */
    IPC_MSG_DEC_INIT_ERR = 13, /* payload: NUL-terminated error string */
    IPC_MSG_DEC_FRAME    = 14, /* struct ipc_dec_frame_hdr + raw planar NV12 frame data */
    IPC_MSG_DEC_DONE     = 15, /* no payload -- all frames for this PACKET/flush sent */
};

/* codec_id: 0 = h264_nvenc, 1 = hevc_nvenc, 2 = libx264 (CPU fallback,
 * used when glue.c's "libx264" hijack registration can't get a GPU
 * encoder session -- see glue.c and README.md). crf/
 * preset/x264opts are only meaningful for codec_id 2, forwarded
 * as-is to the bundled real libx264 encoder for quality-faithful CPU
 * encoding matching what Plex actually requested. crf < 0 means
 * "not set, use bit_rate-based rate control instead". Followed by
 * `preset_len` bytes (preset string, not NUL-terminated on the wire)
 * then `x264opts_len` bytes (x264opts string, same). Zero length is
 * valid (means "not set"). */
struct ipc_init_req {
    uint32_t codec_id;
    uint32_t width;
    uint32_t height;
    int32_t  pix_fmt;      /* AVPixelFormat value from this FFmpeg build */
    int64_t  bit_rate;
    uint32_t gop_size;
    uint32_t framerate_num;
    uint32_t framerate_den;
    uint32_t timebase_num;
    uint32_t timebase_den;
    int64_t  rc_max_rate;   /* AVCodecContext.rc_max_rate, 0 = unset */
    int64_t  rc_buffer_size; /* AVCodecContext.rc_buffer_size, 0 = unset */
    float    crf;
    uint32_t preset_len;
    uint32_t x264opts_len;
    /* Whether the host AVCodecContext was opened with
     * AV_CODEC_FLAG_GLOBAL_HEADER (Plex passes "-flags +global_header"
     * on every real segment-muxer session). Sent as our own explicit
     * bit rather than forwarding the host's raw AVCodecContext.flags
     * value, since that bitmask's layout is defined by whichever
     * FFmpeg headers each side happens to be built against -- glue.c
     * (musl, Plex's patched build) and nvenc-helper (glibc, a plain
     * upstream FFmpeg checkout) are not guaranteed to agree on where
     * that bit lives, only on what it means. When set, the helper's
     * own internal encoder must be opened with the equivalent flag so
     * it actually produces extradata (SPS/PPS/VPS) for
     * IPC_MSG_INIT_OK to hand back -- see ipc_init_ok. */
    uint32_t global_header;
};

/* Followed by width*height + 2*(width/2 * height/2) bytes of planar
 * YUV420P data (Y plane, then U plane, then V plane, no padding --
 * glue.c must pack from the host AVFrame's strided planes). */
struct ipc_frame_hdr {
    int64_t pts;
};

/* Followed by `extradata_size` bytes of the helper's real encoder's
 * out-of-band SPS/PPS/VPS parameter sets (AVCodecContext.extradata
 * after avcodec_open2(), populated by the real encoder itself only
 * when opened with the global-header flag -- see ipc_init_req's
 * global_header field). glue.c must copy these into the HOST
 * AVCodecContext's own extradata/extradata_size before returning from
 * its .init() callback: any muxer run with "-flags +global_header"
 * (every real Plex segment-muxer session) reads the codec's extradata
 * while writing its own header/CodecPrivate immediately after open,
 * and dereferences it unconditionally when the encoder declares
 * AV_CODEC_CAP_* as a real, non-passthrough encoder -- confirmed via
 * a live crash (SIGSEGV, faulting address 0x10, PC inside
 * libavformat.so.60's segment/mkv header-writing code, immediately
 * after a real GPU init succeeded) against a build that never set
 * this. extradata_size == 0 is valid (e.g. HEVC/H264 profiles or
 * encoder configs that inline parameter sets per-keyframe instead) --
 * glue.c must not treat that as an error. */
struct ipc_init_ok {
    uint32_t extradata_size;
};

/* Followed by `size` bytes of encoded packet data. */
struct ipc_packet_hdr {
    int64_t pts;
    int64_t dts;
    uint32_t size;
    uint32_t flags;   /* AV_PKT_FLAG_* bits relevant to Plex (e.g. KEY) */
};

/* codec_id: 0 = h264_cuvid, 1 = hevc_cuvid. cuvid derives width/height/
 * pix_fmt from the bitstream itself, so no dimensions needed here.
 * Followed by `extradata_size` bytes of the host AVCodecContext's
 * extradata (SPS/PPS/VPS parameter sets, stored out-of-band by
 * MKV/MP4 containers in avcC/hvcC form) -- without this, cuvid never
 * sees the parameter sets and silently produces zero frames on any
 * real container-demuxed stream (only worked in isolated testing
 * against a bare elementary stream with inline parameter sets; see
 * docs/history.md). */
struct ipc_dec_init_req {
    uint32_t codec_id;
    uint32_t extradata_size;
};

/* Followed by `size` bytes of compressed packet data (size == 0 means
 * flush: send a NULL packet and drain remaining buffered frames). */
struct ipc_dec_packet_hdr {
    int64_t pts;
    uint32_t size;
};

/* Followed by width*height + 2*(width/2 * height/2) bytes of planar
 * NV12-converted-to-I420 data (Y plane, then U plane, then V plane, no
 * padding) -- the helper converts cuvid's native NV12 output to planar
 * I420 before sending, so glue.c never has to. */
struct ipc_dec_frame_hdr {
    int64_t pts;
    uint32_t width;
    uint32_t height;
};

#endif
