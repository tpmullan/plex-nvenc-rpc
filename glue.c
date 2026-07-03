/*
 * Plex external-codec plugin: registers h264_nvenc/hevc_nvenc (encode)
 * and h264/hevc (decode, hardware-accelerated) with Plex Transcoder's
 * live codec registry, via the av_init_library ABI described in
 * docs/history.md. Deliberately minimal: this file never touches
 * CUDA, never dlopens an NVIDIA library, and links nothing from
 * FFmpeg -- it only uses FFmpeg 6.1's real struct/type definitions as
 * headers, so the compiler lays out our FFCodec identically to Plex's
 * own build.
 *
 * All real encode/decode work happens in a separate nvenc-helper
 * process (plain glibc, no musl compat concerns) reached over a
 * socketpair + fork/exec, per ipc_protocol.h -- see the main README's
 * "Architecture" section for why. Each AVCodecContext gets its own
 * forked helper -- a real transcode session opens one decoder and one
 * encoder context, so this means two helper processes per session,
 * kept fully independent.
 *
 * Must be built with an Alpine/musl toolchain (matching Plex
 * Transcoder's own bundled ld-musl-aarch64.so.1) -- a glibc build fails
 * to dlopen under Plex at all (unresolved GLIBC-versioned symbols),
 * independent of anything this file does at runtime.
 *
 * IMPORTANT: the decoders here register under the plain codec names
 * "h264"/"hevc" (matching AV_CODEC_ID_H264/HEVC), the same names Plex's
 * own liblibh264_decoder.so/libhevc_decoder.so use, because that's
 * what's needed for automatic codec selection to find them. See
 * docs/history.md for the resulting name-collision risk (which plugin
 * wins depends on directory-enumeration order, which we don't control)
 * and how it was tested before live deployment.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_internal.h>
#include <libavcodec/hwconfig.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "ipc_protocol.h"
#include "ipc_io.h"

/* This build's Plex ffmpeg-patch identity, confirmed 2026-07-02.
 * The ONLY thing that needs to change on a Plex FFmpeg-base bump. */
#define EXPECTED_BUILD_HASH "c75335c-a7cfb6836f3ed63280a7eb83"
#define EXPECTED_AVCODEC_VERSION 0x3c1f66u /* packed 60.31.102, avcodec_version() */

/* Path to nvenc-helper, shipped alongside this .so in the same
 * Codecs/<hash>-linux-<arch>/ directory. */
#define HELPER_RELATIVE_NAME "nvenc-helper"

static const enum AVPixelFormat nvenc_pix_fmts[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE,
};

/* Advertises CUDA hwaccel capability on our decoders, matching real
 * FFmpeg cuvid decoders' hw_configs exactly (device_type, methods,
 * pix_fmt). Plex's own hardware-transcode capability check looks at
 * this via avcodec_get_hw_config() as one signal of whether a decoder
 * counts as a "hardware decode accelerator" -- see docs/history.md.
 * .hwaccel = NULL because we don't participate in FFmpeg's hwaccel
 * negotiation machinery at all -- we always hand back plain
 * system-memory YUV420P frames over IPC, never an actual AV_PIX_FMT_CUDA
 * frame. This intentionally diverges from strict FFmpeg hw_configs
 * semantics (which ties the entry to genuinely producing that pix_fmt)
 * purely to satisfy that capability check. */
static const AVCodecHWConfigInternal *const nvdec_glue_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = AV_PIX_FMT_CUDA,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                           AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
            .device_type = AV_HWDEVICE_TYPE_CUDA,
        },
        .hwaccel = NULL,
    },
    NULL,
};

static char *helper_path(void) {
    /* Dl_info gives us this .so's own path; the helper binary lives
     * next to it. Avoids hardcoding the Codecs/<hash>-.../ directory. */
    static char path[4096];
    Dl_info info;
    if (dladdr((void *)helper_path, &info) && info.dli_fname) {
        char *dir = strdup(info.dli_fname);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        snprintf(path, sizeof(path), "%s/%s", slash ? dir : ".", HELPER_RELATIVE_NAME);
        free(dir);
        return path;
    }
    snprintf(path, sizeof(path), "./%s", HELPER_RELATIVE_NAME);
    return path;
}

static int spawn_helper(int *out_fd, pid_t *out_pid) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        close(sv[0]);
        dup2(sv[1], IPC_HELPER_FD);
        if (sv[1] != IPC_HELPER_FD) close(sv[1]);
        char *path = helper_path();
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    *out_fd = sv[0];
    *out_pid = pid;
    return 0;
}

static void reap_helper(int fd, pid_t pid) {
    if (pid > 0) {
        ipc_send_msg(fd, IPC_MSG_SHUTDOWN, NULL, 0);
        close(fd);
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ===================== Encode side ===================== */

struct pending_packet {
    struct pending_packet *next;
    int64_t pts, dts;
    uint32_t flags;
    uint32_t size;
    uint8_t data[];
};

/* `class` must be the literal first field, even though our plain
 * h264_nvenc/hevc_nvenc encoders leave priv_class unset (NULL, unused
 * here) -- this is FFmpeg's own required convention for any priv_data
 * that might have priv_class set: the host writes the AVClass pointer
 * at offset 0 of priv_data whenever priv_class != NULL, regardless of
 * our own struct's intended layout. Keeping it here (rather than only
 * in libx264_hijack_state below) means both this struct and anything
 * that embeds it as a first member share one consistent offset
 * layout, so nvenc_glue_encode()/pop_packet()/etc. work unmodified
 * for both. Found the hard way, 2026-07-02: without this, embedding
 * this struct as libx264_hijack_state's first member corrupted `fd`
 * with the AVClass pointer the host writes there. */
struct nvenc_glue_enc_state {
    const AVClass *class;
    int fd;
    pid_t helper_pid;
    struct pending_packet *head, *tail;
};

/* Plain h264_nvenc/hevc_nvenc need a real priv_class so a -crf option
 * targeting them has somewhere to land. Without one (priv_data_size ==
 * 0, no priv_class), the host's generic AVOption-dictionary
 * application in avcodec_open2() has nothing to apply to and silently
 * drops any -crf value before our init() ever runs -- discovered when
 * Plex's own internal codec resolution turned out to open these
 * directly (bypassing the "libx264" hijack's own crf-carrying
 * priv_data) even when the command line said "-codec:0 libx264
 * -crf:0 N"; see docs/history.md. */
struct nvenc_direct_enc_state {
    struct nvenc_glue_enc_state base;
    float crf;
    char *preset;
    char *x264opts;
};

#define NVENC_DIRECT_OFFSET(field) offsetof(struct nvenc_direct_enc_state, field)
static const AVOption nvenc_direct_options[] = {
    { "crf", "constant rate factor (mapped to nvenc's cq quality mode)",
      NVENC_DIRECT_OFFSET(crf), AV_OPT_TYPE_FLOAT, { .dbl = -1 }, -1, 51, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { "preset", "x264-style preset name (translated to an nvenc preset -- see nvenc-helper.c's x264_preset_to_nvenc)",
      NVENC_DIRECT_OFFSET(preset), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { "x264opts", "accepted for symmetry with the libx264 hijack -- no nvenc equivalent, ignored on the GPU path",
      NVENC_DIRECT_OFFSET(x264opts), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { NULL }
};
static const AVClass nvenc_direct_class = {
    .class_name = "nvenc_direct",
    .item_name = av_default_item_name,
    .option = nvenc_direct_options,
    .version = LIBAVUTIL_VERSION_INT,
};

static void enqueue_packet(struct nvenc_glue_enc_state *st, const struct ipc_packet_hdr *hdr, const uint8_t *data) {
    struct pending_packet *p = av_malloc(sizeof(*p) + hdr->size);
    if (!p) return;
    p->next = NULL;
    p->pts = hdr->pts;
    p->dts = hdr->dts;
    p->flags = hdr->flags;
    p->size = hdr->size;
    memcpy(p->data, data, hdr->size);
    if (st->tail) st->tail->next = p; else st->head = p;
    st->tail = p;
}

static int pop_packet(struct nvenc_glue_enc_state *st, AVPacket *avpkt) {
    struct pending_packet *p = st->head;
    if (!p) return 0;
    st->head = p->next;
    if (!st->head) st->tail = NULL;
    if (av_new_packet(avpkt, (int)p->size) == 0) {
        memcpy(avpkt->data, p->data, p->size);
        avpkt->pts = p->pts;
        avpkt->dts = p->dts;
        avpkt->flags = (int)p->flags;
    }
    av_free(p);
    return 1;
}

/* Reads messages until FRAME_DONE, buffering every PACKET locally.
 * Always fully drains one request's response before the caller sends
 * the next -- the helper is fully synchronous (one request handled to
 * completion, including its terminal DONE marker, before it reads the
 * next), so partial draining would leave a growing backlog in the
 * socket buffers on long streams. */
static int drain_enc_into_queue(AVCodecContext *avctx, struct nvenc_glue_enc_state *st) {
    while (1) {
        uint8_t type; uint32_t plen;
        if (ipc_recv_header(st->fd, &type, &plen) < 0) {
            av_log(avctx, AV_LOG_ERROR, "nvenc-glue: helper connection lost\n");
            return AVERROR(EIO);
        }
        if (type == IPC_MSG_FRAME_DONE) {
            ipc_recv_payload(st->fd, NULL, 0);
            return 0;
        }
        if (type == IPC_MSG_PACKET) {
            uint8_t *pbuf = av_malloc(plen);
            if (!pbuf || ipc_recv_payload(st->fd, pbuf, plen) < 0) {
                av_freep(&pbuf);
                return AVERROR(EIO);
            }
            struct ipc_packet_hdr phdr;
            memcpy(&phdr, pbuf, sizeof(phdr));
            enqueue_packet(st, &phdr, pbuf + sizeof(phdr));
            av_freep(&pbuf);
            continue;
        }
        if (type == IPC_MSG_ERR) {
            char errbuf[256] = {0};
            ipc_recv_payload(st->fd, errbuf, plen < sizeof(errbuf) ? plen : sizeof(errbuf) - 1);
            av_log(avctx, AV_LOG_ERROR, "nvenc-glue: helper ERR: %s\n", errbuf);
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: unexpected message type %u\n", type);
        return AVERROR_EXTERNAL;
    }
}

/* Sends INIT to an already-spawned helper and waits for INIT_OK/ERR.
 * Shared by the plain h264_nvenc/hevc_nvenc encoders and the libx264
 * hijack's GPU-then-CPU-fallback routing below. crf < 0 / preset ==
 * NULL / x264opts == NULL mean "not applicable" (only meaningful for
 * codec_id 2). Returns 0 on INIT_OK, negative AVERROR otherwise;
 * *out_errbuf gets the helper's error string on failure (caller-owned
 * buffer) so the caller can decide whether to fall back or give up. */
static int send_ipc_init(AVCodecContext *avctx, int fd, uint32_t codec_id,
                          float crf, const char *preset, const char *x264opts,
                          char *out_errbuf, size_t out_errbuf_len) {
    uint32_t preset_len = preset ? (uint32_t)strlen(preset) : 0;
    uint32_t x264opts_len = x264opts ? (uint32_t)strlen(x264opts) : 0;
    size_t msg_len = sizeof(struct ipc_init_req) + preset_len + x264opts_len;
    uint8_t *msgbuf = av_malloc(msg_len);
    if (!msgbuf) return AVERROR(ENOMEM);

    struct ipc_init_req req = {
        .codec_id = codec_id,
        .width = (uint32_t)avctx->width,
        .height = (uint32_t)avctx->height,
        .pix_fmt = (int32_t)AV_PIX_FMT_YUV420P,
        .bit_rate = avctx->bit_rate,
        .gop_size = (uint32_t)(avctx->gop_size > 0 ? avctx->gop_size : 25),
        .framerate_num = (uint32_t)(avctx->framerate.num > 0 ? avctx->framerate.num : 25),
        .framerate_den = (uint32_t)(avctx->framerate.den > 0 ? avctx->framerate.den : 1),
        .timebase_num = (uint32_t)(avctx->time_base.num > 0 ? avctx->time_base.num : 1),
        .timebase_den = (uint32_t)(avctx->time_base.den > 0 ? avctx->time_base.den : 25),
        .rc_max_rate = avctx->rc_max_rate,
        .rc_buffer_size = avctx->rc_buffer_size,
        .crf = crf,
        .preset_len = preset_len,
        .x264opts_len = x264opts_len,
    };
    memcpy(msgbuf, &req, sizeof(req));
    if (preset_len) memcpy(msgbuf + sizeof(req), preset, preset_len);
    if (x264opts_len) memcpy(msgbuf + sizeof(req) + preset_len, x264opts, x264opts_len);

    int rc = ipc_send_msg(fd, IPC_MSG_INIT, msgbuf, (uint32_t)msg_len);
    av_freep(&msgbuf);
    if (rc < 0) {
        snprintf(out_errbuf, out_errbuf_len, "INIT send failed");
        return AVERROR(EIO);
    }

    uint8_t type; uint32_t plen;
    if (ipc_recv_header(fd, &type, &plen) < 0) {
        snprintf(out_errbuf, out_errbuf_len, "INIT reply read failed");
        return AVERROR(EIO);
    }
    if (type == IPC_MSG_INIT_ERR) {
        char helper_err[200] = {0};
        ipc_recv_payload(fd, helper_err, plen < sizeof(helper_err) ? plen : sizeof(helper_err) - 1);
        snprintf(out_errbuf, out_errbuf_len, "helper INIT_ERR: %s", helper_err);
        return AVERROR_EXTERNAL;
    }
    if (type != IPC_MSG_INIT_OK) {
        snprintf(out_errbuf, out_errbuf_len, "unexpected INIT reply type %u", type);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int nvenc_glue_enc_init(AVCodecContext *avctx) {
    /* priv_data is host-allocated/zeroed now (priv_data_size != 0,
     * priv_class set) so any -crf/-preset option targeting this codec
     * directly has already been applied to ds->crf/ds->preset by the
     * time we get here. x264opts has no nvenc equivalent and is
     * accepted-but-unused, purely so it doesn't get silently dropped
     * by the host's option filtering in a way that would be confusing
     * to debug later. */
    struct nvenc_direct_enc_state *ds = avctx->priv_data;
    ds->base.fd = -1;
    ds->base.helper_pid = -1;

    if (spawn_helper(&ds->base.fd, &ds->base.helper_pid) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: failed to spawn nvenc-helper\n");
        return AVERROR(EIO);
    }

    char errbuf[256];
    uint32_t codec_id = avctx->codec_id == AV_CODEC_ID_HEVC ? 1 : 0;
    int ret = send_ipc_init(avctx, ds->base.fd, codec_id, ds->crf, ds->preset, NULL, errbuf, sizeof(errbuf));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: %s\n", errbuf);
        return ret;
    }
    return 0;
}

static int nvenc_glue_enc_close(AVCodecContext *avctx) {
    /* priv_data itself is host-owned now (priv_data_size != 0) --
     * freed by avcodec_free_context, not by us. Only our own
     * heap-allocated packet queue needs cleanup here. */
    struct nvenc_glue_enc_state *st = avctx->priv_data;
    if (st) {
        reap_helper(st->fd, st->helper_pid);
        while (st->head) {
            struct pending_packet *p = st->head;
            st->head = p->next;
            av_free(p);
        }
    }
    return 0;
}

/* Packs a possibly-strided AVFrame's YUV420P planes into a contiguous
 * buffer matching ipc_frame_hdr's layout (Y, then U, then V, no
 * padding). */
static uint8_t *pack_frame(const AVFrame *frame, size_t *out_len) {
    int w = frame->width, h = frame->height;
    size_t y_size = (size_t)w * h;
    size_t c_size = (size_t)(w / 2) * (h / 2);
    size_t total = sizeof(struct ipc_frame_hdr) + y_size + 2 * c_size;
    uint8_t *buf = av_malloc(total);
    if (!buf) return NULL;

    struct ipc_frame_hdr hdr = { .pts = frame->pts };
    memcpy(buf, &hdr, sizeof(hdr));
    uint8_t *p = buf + sizeof(hdr);
    for (int row = 0; row < h; row++)
        memcpy(p + (size_t)row * w, frame->data[0] + (size_t)row * frame->linesize[0], w);
    p += y_size;
    for (int row = 0; row < h / 2; row++)
        memcpy(p + (size_t)row * (w / 2), frame->data[1] + (size_t)row * frame->linesize[1], w / 2);
    p += c_size;
    for (int row = 0; row < h / 2; row++)
        memcpy(p + (size_t)row * (w / 2), frame->data[2] + (size_t)row * frame->linesize[2], w / 2);

    *out_len = total;
    return buf;
}

/* Legacy FF_CODEC_CB_TYPE_ENCODE callback: the host's own encode.c
 * pulls the frame off its internal queue and hands it to us directly,
 * so we never touch avctx->internal ourselves. */
static int nvenc_glue_encode(AVCodecContext *avctx, AVPacket *avpkt,
                              const AVFrame *frame, int *got_packet_ptr) {
    struct nvenc_glue_enc_state *st = avctx->priv_data;
    *got_packet_ptr = 0;

    if (pop_packet(st, avpkt)) {
        *got_packet_ptr = 1;
        return 0;
    }

    if (frame) {
        size_t buf_len;
        uint8_t *buf = pack_frame(frame, &buf_len);
        if (!buf) return AVERROR(ENOMEM);
        int rc = ipc_send_msg(st->fd, IPC_MSG_FRAME, buf, (uint32_t)buf_len);
        av_freep(&buf);
        if (rc < 0) return AVERROR(EIO);
    } else {
        if (ipc_send_msg(st->fd, IPC_MSG_FLUSH, NULL, 0) < 0)
            return AVERROR(EIO);
    }

    int ret = drain_enc_into_queue(avctx, st);
    if (ret < 0) return ret;

    if (pop_packet(st, avpkt))
        *got_packet_ptr = 1;
    return 0;
}

/* ===================== libx264 hijack (GPU-first, CPU fallback) ===================== */

/* Registers under the exact name "libx264" -- Plex Media Server
 * explicitly writes "-codec:0 libx264" into the Transcoder command
 * line for the vast majority of transcode sessions (its default/
 * fallback CPU encoder), a decision made in its own process before
 * Transcoder even starts. Unlike h264_nvenc/hevc_nvenc (which only
 * get requested if its own internal capability check approves, a
 * check we couldn't get to recognize this plugin -- see
 * docs/history.md), "libx264" gets requested unconditionally whenever
 * it decides not to use hardware encode. Since Transcoder's own codec
 * resolution for a given name is a dumb, local, first-match lookup
 * decoupled from that capability check (the same mechanism that
 * already lets our decoder aliases win), replacing "libx264" itself
 * is the only way to get real hardware encode used for real,
 * server-driven sessions.
 *
 * MUST NOT unconditionally force GPU: there may be good reasons to
 * want CPU (headroom for other GPU work, multiple concurrent
 * sessions exceeding NVENC's session limit, etc.), and every session
 * that would have used real libx264 now flows through this code, so
 * a wrong call here has a much bigger blast radius than the decoder
 * hijack. Policy: always attempt GPU (h264_nvenc) first since it's
 * strictly faster when available; if the helper's own INIT reports
 * failure (GPU busy/unavailable/session-limit), fall back to a
 * genuine CPU x264 encode via the bundled real libx264 (nvenc-helper
 * codec_id 2) with the real crf/preset/x264opts Plex requested, so
 * quality is unaffected either way. */

/* Real AVOption-backed state so FFmpeg's own generic option-dictionary
 * parsing (which runs on avctx->priv_data before .init is called, the
 * same way it would for any other AVOption-bearing codec) populates
 * crf/preset/x264opts for us -- unlike our other encoders, this one
 * needs priv_data_size != 0 and a real .priv_class. `base` embedded
 * first so the shared nvenc_glue_encode()/pop_packet() code (which
 * only touches fd/head/tail) keeps working unmodified via simple
 * struct-address-equals-first-member-address casting. */
struct libx264_hijack_state {
    struct nvenc_glue_enc_state base;
    float crf;
    char *preset;
    char *x264opts;
};

#define LIBX264_HIJACK_OFFSET(field) offsetof(struct libx264_hijack_state, field)
#define VE (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM)
static const AVOption libx264_hijack_options[] = {
    { "crf", "constant rate factor (forwarded to real libx264 on CPU fallback)",
      LIBX264_HIJACK_OFFSET(crf), AV_OPT_TYPE_FLOAT, { .dbl = -1 }, -1, 51, VE },
    { "preset", "x264 preset (forwarded to real libx264 on CPU fallback)",
      LIBX264_HIJACK_OFFSET(preset), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { "x264opts", "raw x264 options (forwarded to real libx264 on CPU fallback)",
      LIBX264_HIJACK_OFFSET(x264opts), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VE },
    { NULL }
};
#undef VE

static const AVClass libx264_hijack_class = {
    .class_name = "libx264_hijack",
    .item_name = av_default_item_name,
    .option = libx264_hijack_options,
    .version = LIBAVUTIL_VERSION_INT,
};

static int libx264_hijack_init(AVCodecContext *avctx) {
    struct libx264_hijack_state *hs = avctx->priv_data;
    hs->base.fd = -1;
    hs->base.helper_pid = -1;

    if (spawn_helper(&hs->base.fd, &hs->base.helper_pid) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: libx264 hijack: failed to spawn helper (GPU attempt)\n");
        return AVERROR(EIO);
    }

    char errbuf[256];
    int ret = send_ipc_init(avctx, hs->base.fd, 0 /* h264_nvenc */, hs->crf, hs->preset, NULL, errbuf, sizeof(errbuf));
    if (ret == 0) {
        av_log(avctx, AV_LOG_INFO, "nvenc-glue: libx264 hijack: using GPU (h264_nvenc)\n");
        return 0;
    }

    av_log(avctx, AV_LOG_WARNING, "nvenc-glue: libx264 hijack: GPU unavailable (%s), falling back to CPU x264\n", errbuf);
    reap_helper(hs->base.fd, hs->base.helper_pid);
    hs->base.fd = -1;
    hs->base.helper_pid = -1;

    if (spawn_helper(&hs->base.fd, &hs->base.helper_pid) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: libx264 hijack: failed to spawn helper (CPU attempt)\n");
        return AVERROR(EIO);
    }
    ret = send_ipc_init(avctx, hs->base.fd, 2 /* libx264 CPU */, hs->crf, hs->preset, hs->x264opts,
                         errbuf, sizeof(errbuf));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: libx264 hijack: CPU fallback also failed: %s\n", errbuf);
        return ret;
    }
    av_log(avctx, AV_LOG_INFO, "nvenc-glue: libx264 hijack: using CPU (real libx264)\n");
    return 0;
}

static int libx264_hijack_close(AVCodecContext *avctx) {
    struct libx264_hijack_state *hs = avctx->priv_data;
    reap_helper(hs->base.fd, hs->base.helper_pid);
    while (hs->base.head) {
        struct pending_packet *p = hs->base.head;
        hs->base.head = p->next;
        av_free(p);
    }
    /* preset/x264opts (AV_OPT_TYPE_STRING allocations) are freed by
     * the host's generic av_opt_free(avctx->priv_data) during
     * avcodec_free_context, same as any other AVOption-bearing codec
     * -- not our responsibility to free them here. */
    return 0;
}

/* ===================== Decode side ===================== */

struct pending_frame {
    struct pending_frame *next;
    int64_t pts;
    uint32_t width, height;
    uint8_t data[];
};

struct nvenc_glue_dec_state {
    int fd;
    pid_t helper_pid;
    struct pending_frame *head, *tail;
};

static void enqueue_frame(struct nvenc_glue_dec_state *st, const struct ipc_dec_frame_hdr *hdr, const uint8_t *data) {
    size_t y_size = (size_t)hdr->width * hdr->height;
    size_t c_size = (size_t)(hdr->width / 2) * (hdr->height / 2);
    size_t data_len = y_size + 2 * c_size;
    struct pending_frame *p = av_malloc(sizeof(*p) + data_len);
    if (!p) return;
    p->next = NULL;
    p->pts = hdr->pts;
    p->width = hdr->width;
    p->height = hdr->height;
    memcpy(p->data, data, data_len);
    if (st->tail) st->tail->next = p; else st->head = p;
    st->tail = p;
}

static int pop_frame(struct nvenc_glue_dec_state *st, AVFrame *frame) {
    struct pending_frame *p = st->head;
    if (!p) return 0;
    st->head = p->next;
    if (!st->head) st->tail = NULL;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = (int)p->width;
    frame->height = (int)p->height;
    if (av_frame_get_buffer(frame, 0) == 0) {
        int w = (int)p->width, h = (int)p->height;
        size_t y_size = (size_t)w * h;
        size_t c_size = (size_t)(w / 2) * (h / 2);
        const uint8_t *src = p->data;
        for (int row = 0; row < h; row++)
            memcpy(frame->data[0] + (size_t)row * frame->linesize[0], src + (size_t)row * w, w);
        src += y_size;
        for (int row = 0; row < h / 2; row++)
            memcpy(frame->data[1] + (size_t)row * frame->linesize[1], src + (size_t)row * (w / 2), w / 2);
        src += c_size;
        for (int row = 0; row < h / 2; row++)
            memcpy(frame->data[2] + (size_t)row * frame->linesize[2], src + (size_t)row * (w / 2), w / 2);
        frame->pts = p->pts;
    }
    av_free(p);
    return 1;
}

static int drain_dec_into_queue(AVCodecContext *avctx, struct nvenc_glue_dec_state *st) {
    while (1) {
        uint8_t type; uint32_t plen;
        if (ipc_recv_header(st->fd, &type, &plen) < 0) {
            av_log(avctx, AV_LOG_ERROR, "nvenc-glue: decode helper connection lost\n");
            return AVERROR(EIO);
        }
        if (type == IPC_MSG_DEC_DONE) {
            ipc_recv_payload(st->fd, NULL, 0);
            return 0;
        }
        if (type == IPC_MSG_DEC_FRAME) {
            uint8_t *fbuf = av_malloc(plen);
            if (!fbuf || ipc_recv_payload(st->fd, fbuf, plen) < 0) {
                av_freep(&fbuf);
                return AVERROR(EIO);
            }
            struct ipc_dec_frame_hdr fhdr;
            memcpy(&fhdr, fbuf, sizeof(fhdr));
            enqueue_frame(st, &fhdr, fbuf + sizeof(fhdr));
            av_freep(&fbuf);
            continue;
        }
        if (type == IPC_MSG_ERR) {
            char errbuf[256] = {0};
            ipc_recv_payload(st->fd, errbuf, plen < sizeof(errbuf) ? plen : sizeof(errbuf) - 1);
            av_log(avctx, AV_LOG_ERROR, "nvenc-glue: decode helper ERR: %s\n", errbuf);
            return AVERROR_EXTERNAL;
        }
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: unexpected decode message type %u\n", type);
        return AVERROR_EXTERNAL;
    }
}

static int nvenc_glue_dec_init(AVCodecContext *avctx) {
    struct nvenc_glue_dec_state *st = av_mallocz(sizeof(*st));
    if (!st) return AVERROR(ENOMEM);
    avctx->priv_data = st;
    st->fd = -1;
    st->helper_pid = -1;

    if (spawn_helper(&st->fd, &st->helper_pid) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: failed to spawn nvenc-helper (decode)\n");
        return AVERROR(EIO);
    }

    /* extradata (SPS/PPS/VPS parameter sets) is out-of-band in
     * MKV/MP4 containers -- without forwarding it, the helper's cuvid
     * decoder never sees the parameter sets and silently produces
     * zero frames on real content (only worked in isolated testing
     * against a bare elementary stream with inline parameters). */
    uint32_t extradata_size = avctx->extradata_size > 0 ? (uint32_t)avctx->extradata_size : 0;
    size_t msg_len = sizeof(struct ipc_dec_init_req) + extradata_size;
    uint8_t *msgbuf = av_malloc(msg_len);
    if (!msgbuf) return AVERROR(ENOMEM);
    struct ipc_dec_init_req req = {
        .codec_id = avctx->codec_id == AV_CODEC_ID_HEVC ? 1 : 0,
        .extradata_size = extradata_size,
    };
    memcpy(msgbuf, &req, sizeof(req));
    if (extradata_size > 0)
        memcpy(msgbuf + sizeof(req), avctx->extradata, extradata_size);
    int rc = ipc_send_msg(st->fd, IPC_MSG_DEC_INIT, msgbuf, (uint32_t)msg_len);
    av_freep(&msgbuf);
    if (rc < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: DEC_INIT send failed\n");
        return AVERROR(EIO);
    }

    uint8_t type; uint32_t plen;
    if (ipc_recv_header(st->fd, &type, &plen) < 0) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: DEC_INIT reply read failed\n");
        return AVERROR(EIO);
    }
    if (type == IPC_MSG_DEC_INIT_ERR) {
        char errbuf[256] = {0};
        ipc_recv_payload(st->fd, errbuf, plen < sizeof(errbuf) ? plen : sizeof(errbuf) - 1);
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: helper DEC_INIT_ERR: %s\n", errbuf);
        return AVERROR_EXTERNAL;
    }
    if (type != IPC_MSG_DEC_INIT_OK) {
        av_log(avctx, AV_LOG_ERROR, "nvenc-glue: unexpected DEC_INIT reply type %u\n", type);
        return AVERROR_EXTERNAL;
    }
    return 0;
}

static int nvenc_glue_dec_close(AVCodecContext *avctx) {
    struct nvenc_glue_dec_state *st = avctx->priv_data;
    if (st) {
        reap_helper(st->fd, st->helper_pid);
        while (st->head) {
            struct pending_frame *p = st->head;
            st->head = p->next;
            av_free(p);
        }
        av_freep(&avctx->priv_data);
    }
    return 0;
}

/* Legacy FF_CODEC_CB_TYPE_DECODE callback: the host's own decode.c
 * pulls the packet off its internal queue and hands it to us directly
 * (decode_simple_internal calls ff_decode_get_packet itself, then
 * codec->cb.decode(avctx, frame, &got_frame, pkt)) -- same reasoning
 * as the encode side, we never touch avctx->internal. avpkt->size == 0
 * signals drain/flush. */
static int nvenc_glue_decode(AVCodecContext *avctx, AVFrame *frame,
                              int *got_frame_ptr, AVPacket *avpkt) {
    struct nvenc_glue_dec_state *st = avctx->priv_data;
    *got_frame_ptr = 0;

    if (pop_frame(st, frame)) {
        *got_frame_ptr = 1;
        return avpkt->size;
    }

    size_t msg_len = sizeof(struct ipc_dec_packet_hdr) + (size_t)(avpkt->size > 0 ? avpkt->size : 0);
    uint8_t *msgbuf = av_malloc(msg_len);
    if (!msgbuf) return AVERROR(ENOMEM);
    struct ipc_dec_packet_hdr hdr = {
        .pts = avpkt->pts,
        .size = avpkt->size > 0 ? (uint32_t)avpkt->size : 0,
    };
    memcpy(msgbuf, &hdr, sizeof(hdr));
    if (avpkt->size > 0)
        memcpy(msgbuf + sizeof(hdr), avpkt->data, avpkt->size);
    int rc = ipc_send_msg(st->fd, IPC_MSG_DEC_PACKET, msgbuf, (uint32_t)msg_len);
    av_freep(&msgbuf);
    if (rc < 0) return AVERROR(EIO);

    int ret = drain_dec_into_queue(avctx, st);
    if (ret < 0) return ret;

    if (pop_frame(st, frame))
        *got_frame_ptr = 1;
    return avpkt->size;
}

/* ===================== Codec table ===================== */

/* Real priv_class + priv_data_size (see nvenc_direct_enc_state above)
 * so a -crf option targeting these directly (which is what happens
 * when Plex's own internal codec resolution opens these instead of
 * going through the "libx264" hijack, even with "-codec:0 libx264" on
 * the command line -- see docs/history.md) actually lands somewhere. */
static const FFCodec ff_h264_nvenc_glue_encoder = {
    .p.name         = "h264_nvenc",
    .p.long_name    = "NVIDIA NVENC H.264 encoder (RPC bridge)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    /* AV_CODEC_CAP_HARDWARE matches real hardware encoders' declared
     * capabilities -- Plex's own hardware-transcode dispatcher gates
     * encoder selection on this flag, not just on the encoder existing
     * and working when invoked directly (this encoder worked correctly
     * under manual invocation well before that selection worked). Not
     * adding AV_CODEC_CAP_ENCODER_FLUSH since we don't implement a
     * .flush callback -- claiming it without the matching
     * implementation risks a mid-stream seek/flush behaving
     * incorrectly. */
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
    .p.pix_fmts     = nvenc_pix_fmts,
    .p.priv_class   = &nvenc_direct_class,
    .priv_data_size = sizeof(struct nvenc_direct_enc_state),
    .init           = nvenc_glue_enc_init,
    .cb_type        = FF_CODEC_CB_TYPE_ENCODE,
    .cb.encode      = nvenc_glue_encode,
    .close          = nvenc_glue_enc_close,
};

static const FFCodec ff_hevc_nvenc_glue_encoder = {
    .p.name         = "hevc_nvenc",
    .p.long_name    = "NVIDIA NVENC HEVC encoder (RPC bridge)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HEVC,
    /* AV_CODEC_CAP_HARDWARE matches real hardware encoders' declared
     * capabilities -- Plex's own hardware-transcode dispatcher gates
     * encoder selection on this flag, not just on the encoder existing
     * and working when invoked directly (this encoder worked correctly
     * under manual invocation well before that selection worked). Not
     * adding AV_CODEC_CAP_ENCODER_FLUSH since we don't implement a
     * .flush callback -- claiming it without the matching
     * implementation risks a mid-stream seek/flush behaving
     * incorrectly. */
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
    .p.pix_fmts     = nvenc_pix_fmts,
    .p.priv_class   = &nvenc_direct_class,
    .priv_data_size = sizeof(struct nvenc_direct_enc_state),
    .init           = nvenc_glue_enc_init,
    .cb_type        = FF_CODEC_CB_TYPE_ENCODE,
    .cb.encode      = nvenc_glue_encode,
    .close          = nvenc_glue_enc_close,
};

/* Hijacks Plex's own default CPU encoder name -- see the file-header
 * note above libx264_hijack_init for why this is the only way to get
 * real hardware encode used for actual server-driven sessions. Uses
 * nvenc_glue_encode (the same shared encode/drain logic as the plain
 * h264_nvenc/hevc_nvenc encoders above) since libx264_hijack_state
 * embeds nvenc_glue_enc_state as its first member. */
static const FFCodec ff_libx264_hijack_encoder = {
    .p.name         = "libx264",
    .p.long_name    = "libx264 H.264 / AVC (RPC bridge, GPU-first with CPU fallback)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .p.capabilities = AV_CODEC_CAP_DELAY,
    .p.pix_fmts     = nvenc_pix_fmts,
    .p.priv_class   = &libx264_hijack_class,
    .priv_data_size = sizeof(struct libx264_hijack_state),
    .init           = libx264_hijack_init,
    .cb_type        = FF_CODEC_CB_TYPE_ENCODE,
    .cb.encode      = nvenc_glue_encode,
    .close          = libx264_hijack_close,
};

/* Registered under the plain codec names ("h264"/"hevc") so Plex's own
 * by-ID/by-name codec selection can find them the same way it finds
 * Plex's own software decoders -- see the file-header note on the
 * resulting name-collision risk and how it was tested. */
static const FFCodec ff_h264_nvdec_glue_decoder = {
    .p.name         = "h264",
    .p.long_name    = "NVIDIA NVDEC H.264 decoder (RPC bridge)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    /* AV_CODEC_CAP_HARDWARE + AV_CODEC_CAP_AVOID_PROBING match real
     * cuvid decoders' declared capabilities exactly. Not AV_CODEC_CAP_DR1
     * -- that signals the decoder participates in the host's
     * get_buffer2 pooling, which we don't implement (we just allocate
     * our own AVFrame buffers directly in pop_frame). */
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .hw_configs     = nvdec_glue_hw_configs,
    .priv_data_size = 0,
    .init           = nvenc_glue_dec_init,
    .cb_type        = FF_CODEC_CB_TYPE_DECODE,
    .cb.decode      = nvenc_glue_decode,
    .close          = nvenc_glue_dec_close,
};

static const FFCodec ff_hevc_nvdec_glue_decoder = {
    .p.name         = "hevc",
    .p.long_name    = "NVIDIA NVDEC HEVC decoder (RPC bridge)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HEVC,
    /* AV_CODEC_CAP_HARDWARE + AV_CODEC_CAP_AVOID_PROBING match real
     * cuvid decoders' declared capabilities exactly. Not AV_CODEC_CAP_DR1
     * -- that signals the decoder participates in the host's
     * get_buffer2 pooling, which we don't implement (we just allocate
     * our own AVFrame buffers directly in pop_frame). */
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .hw_configs     = nvdec_glue_hw_configs,
    .priv_data_size = 0,
    .init           = nvenc_glue_dec_init,
    .cb_type        = FF_CODEC_CB_TYPE_DECODE,
    .cb.decode      = nvenc_glue_decode,
    .close          = nvenc_glue_dec_close,
};

/* Additional aliases, registered purely so Plex's own hardware-
 * transcode capability check finds a hardware decoder to exist at
 * all -- that check looks for a decoder registered under a
 * "<codec>_nvdec"-style name. Confirmed necessary because decode via
 * the plain "h264"/"hevc" names above already worked at actual
 * runtime (real GPU decode confirmed via GPU process/memory listings
 * during playback) while that capability check still reported no
 * hardware decode accelerator found -- two decoupled mechanisms, the
 * check doesn't look at what actually gets used. Plex's real command
 * line always requests decode by the bare codec name
 * ("-codec:0 hevc"), never by this alias, so these exist solely to be
 * *found*, not to be *invoked*. See docs/history.md. */
static const FFCodec ff_h264_nvdec_alias_decoder = {
    .p.name         = "h264_nvdec",
    .p.long_name    = "NVIDIA NVDEC H.264 decoder (RPC bridge, probe alias)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .hw_configs     = nvdec_glue_hw_configs,
    .priv_data_size = 0,
    .init           = nvenc_glue_dec_init,
    .cb_type        = FF_CODEC_CB_TYPE_DECODE,
    .cb.decode      = nvenc_glue_decode,
    .close          = nvenc_glue_dec_close,
};

static const FFCodec ff_hevc_nvdec_alias_decoder = {
    .p.name         = "hevc_nvdec",
    .p.long_name    = "NVIDIA NVDEC HEVC decoder (RPC bridge, probe alias)",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HEVC,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .hw_configs     = nvdec_glue_hw_configs,
    .priv_data_size = 0,
    .init           = nvenc_glue_dec_init,
    .cb_type        = FF_CODEC_CB_TYPE_DECODE,
    .cb.decode      = nvenc_glue_decode,
    .close          = nvenc_glue_dec_close,
};

/* --- av_init_library ABI (see docs/history.md): ctx is a small
 * vtable of host callbacks. --- */
typedef unsigned int (*avcodec_version_fn)(void);
typedef const char *(*av_version_info_fn)(void);
typedef void (*register_fn)(void *codec);

int av_init_library(void **ctx, unsigned int log_level) {
    (void)log_level;
    avcodec_version_fn get_version = (avcodec_version_fn)ctx[4];
    av_version_info_fn get_build_hash = (av_version_info_fn)ctx[3];
    register_fn register_codec = (register_fn)ctx[5];

    if (get_version() != EXPECTED_AVCODEC_VERSION)
        return -1;
    if (strcmp(get_build_hash(), EXPECTED_BUILD_HASH) != 0)
        return -1;

    register_codec((void *)&ff_h264_nvenc_glue_encoder);
    register_codec((void *)&ff_hevc_nvenc_glue_encoder);
    register_codec((void *)&ff_libx264_hijack_encoder);
    register_codec((void *)&ff_h264_nvdec_glue_decoder);
    register_codec((void *)&ff_hevc_nvdec_glue_decoder);
    register_codec((void *)&ff_h264_nvdec_alias_decoder);
    register_codec((void *)&ff_hevc_nvdec_alias_decoder);
    return 0;
}
