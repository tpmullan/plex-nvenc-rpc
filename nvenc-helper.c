/*
 * nvenc-helper: a plain-glibc process that does all real CUDA/NVENC
 * work for the Plex NVENC plugin. Spawned by glue.c (which runs inside
 * Plex Transcoder's musl process) via fork()+exec(), talking back over
 * a socketpair() fd inherited on IPC_HELPER_FD. See
 * docs/plans/2026-07-02-plex-nvenc-transcoder.md, "Decision 2026-07-02:
 * pivot to subprocess/RPC bridge architecture" for why this exists.
 *
 * This binary has zero dependency on Plex's build -- it doesn't link
 * against anything Plex ships, doesn't touch Plex's plugin ABI, and
 * only speaks ipc_protocol.h. It should not need to change across
 * Plex version updates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "ipc_protocol.h"
#include "ipc_io.h"

static AVCodecContext *g_ctx = NULL;
static AVFrame *g_frame = NULL;
static AVPacket *g_pkt = NULL;

/* Separate context for the decode direction -- a single helper process
 * handles both, since a transcode session needs both a decoder and an
 * encoder and spawning two processes per session would double the
 * fork/exec overhead for no benefit (decode and encode never run
 * concurrently against the same GPU context in a way that requires
 * isolation here). */
static AVCodecContext *g_dec_ctx = NULL;
static AVFrame *g_dec_frame = NULL;
static AVPacket *g_dec_pkt = NULL;

static void die_send_err(int fd, uint8_t type, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "nvenc-helper: %s\n", buf);
    ipc_send_msg(fd, type, buf, (uint32_t)strlen(buf) + 1);
}

/* x264 preset names (what Plex actually sends) have no naming overlap
 * with nvenc's own preset scale ("p1".."p7", fastest..slowest/best
 * quality, in this ffnvcodec/SDK generation) -- map by intent (speed
 * vs. quality tradeoff position) rather than by name. Returns NULL for
 * anything unrecognized so the caller leaves nvenc's own default in
 * place instead of passing through a nonsense value. */
static const char *x264_preset_to_nvenc(const char *x264_preset) {
    static const struct { const char *x264; const char *nvenc; } map[] = {
        { "ultrafast", "p1" },
        { "superfast", "p1" },
        { "veryfast",  "p2" },
        { "faster",    "p3" },
        { "fast",      "p3" },
        { "medium",    "p4" },
        { "slow",      "p5" },
        { "slower",    "p6" },
        { "veryslow",  "p7" },
        { "placebo",   "p7" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(x264_preset, map[i].x264) == 0)
            return map[i].nvenc;
    }
    return NULL;
}

static int handle_init(int fd, const uint8_t *payload, uint32_t payload_len) {
    if (payload_len < sizeof(struct ipc_init_req)) {
        die_send_err(fd, IPC_MSG_INIT_ERR, "INIT payload too short");
        return -1;
    }
    struct ipc_init_req req;
    memcpy(&req, payload, sizeof(req));
    const uint8_t *trailer = payload + sizeof(req);
    uint32_t trailer_avail = payload_len - (uint32_t)sizeof(req);
    if (trailer_avail < req.preset_len + req.x264opts_len) {
        die_send_err(fd, IPC_MSG_INIT_ERR, "INIT trailer shorter than declared lengths");
        return -1;
    }
    char preset_buf[128] = {0};
    char x264opts_buf[512] = {0};
    if (req.preset_len > 0) {
        uint32_t n = req.preset_len < sizeof(preset_buf) - 1 ? req.preset_len : sizeof(preset_buf) - 1;
        memcpy(preset_buf, trailer, n);
    }
    if (req.x264opts_len > 0) {
        uint32_t n = req.x264opts_len < sizeof(x264opts_buf) - 1 ? req.x264opts_len : sizeof(x264opts_buf) - 1;
        memcpy(x264opts_buf, trailer + req.preset_len, n);
    }

    const char *codec_name = req.codec_id == 2 ? "libx264"
                            : req.codec_id == 1 ? "hevc_nvenc"
                            : "h264_nvenc";
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        die_send_err(fd, IPC_MSG_INIT_ERR, "encoder %s not found", codec_name);
        return -1;
    }

    g_ctx = avcodec_alloc_context3(codec);
    if (!g_ctx) {
        die_send_err(fd, IPC_MSG_INIT_ERR, "avcodec_alloc_context3 failed");
        return -1;
    }
    g_ctx->width = (int)req.width;
    g_ctx->height = (int)req.height;
    g_ctx->pix_fmt = (enum AVPixelFormat)req.pix_fmt;
    g_ctx->bit_rate = req.bit_rate;
    g_ctx->gop_size = (int)req.gop_size;
    g_ctx->time_base = (AVRational){ (int)req.timebase_num, (int)req.timebase_den };
    g_ctx->framerate = (AVRational){ (int)req.framerate_num, (int)req.framerate_den };
    if (req.rc_max_rate > 0) g_ctx->rc_max_rate = req.rc_max_rate;
    if (req.rc_buffer_size > 0) g_ctx->rc_buffer_size = (int)req.rc_buffer_size;
    /* Must match the host's own AV_CODEC_FLAG_GLOBAL_HEADER so this
     * real encoder actually produces extradata (SPS/PPS/VPS) for
     * IPC_MSG_INIT_OK to hand back -- see ipc_protocol.h. Without this,
     * a global-header session gets no parameter sets from the real
     * encoder at all (it inlines them per-keyframe instead), so there
     * would be nothing correct to forward even after fixing the
     * IPC/glue.c side. */
    if (req.global_header)
        g_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* codec_id 2 (CPU x264 fallback): apply the real quality settings
     * Plex actually requested, using the bundled real libx264 encoder
     * -- full option fidelity since it's genuinely libx264, not an
     * approximation. */
    if (req.codec_id == 2) {
        if (req.crf >= 0)
            av_opt_set_double(g_ctx->priv_data, "crf", req.crf, 0);
        if (preset_buf[0])
            av_opt_set(g_ctx->priv_data, "preset", preset_buf, 0);
        if (x264opts_buf[0])
            av_opt_set(g_ctx->priv_data, "x264opts", x264opts_buf, 0);
    }

    /* codec_id 0/1 (GPU nvenc): Plex requests quality via crf, not an
     * explicit bit_rate -- when crf-based (no -b:v), avctx->bit_rate is
     * 0, and forwarding that straight to nvenc as its target bitrate
     * produces a severely bitrate-starved, low-quality encode at the
     * *correct* resolution (looks like resolution is wrong, but it's
     * actually a bitrate collapse). nvenc has its own constant-quality
     * mode (rc=vbr, cq=<0-51, same scale as x264 crf>) -- use that
     * instead so a crf-based request actually gets quality-based
     * encoding on the GPU too. rc_max_rate/rc_buffer_size (already set
     * above) still apply as caps in vbr mode. */
    if (req.codec_id != 2 && req.crf >= 0) {
        av_opt_set(g_ctx->priv_data, "rc", "vbr", 0);
        av_opt_set_double(g_ctx->priv_data, "cq", req.crf, 0);
    }
    /* nvenc's own preset scale (p1..p7, fastest..slowest/best) has no
     * naming overlap with x264's (ultrafast..placebo) -- translate so
     * Plex's speed/quality tradeoff request still means something on
     * the GPU path instead of being silently dropped. */
    if (req.codec_id != 2 && preset_buf[0]) {
        const char *nvenc_preset = x264_preset_to_nvenc(preset_buf);
        if (nvenc_preset)
            av_opt_set(g_ctx->priv_data, "preset", nvenc_preset, 0);
    }

    int ret = avcodec_open2(g_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        die_send_err(fd, IPC_MSG_INIT_ERR, "avcodec_open2 failed: %s", errbuf);
        avcodec_free_context(&g_ctx);
        return -1;
    }

    g_frame = av_frame_alloc();
    g_frame->format = g_ctx->pix_fmt;
    g_frame->width = g_ctx->width;
    g_frame->height = g_ctx->height;
    if (av_frame_get_buffer(g_frame, 0) < 0) {
        die_send_err(fd, IPC_MSG_INIT_ERR, "av_frame_get_buffer failed");
        avcodec_free_context(&g_ctx);
        return -1;
    }

    g_pkt = av_packet_alloc();

    /* Hand the real encoder's own extradata (SPS/PPS/VPS, populated by
     * avcodec_open2() above when global_header was requested) back to
     * glue.c so it can set it on the HOST AVCodecContext -- see
     * ipc_init_ok in ipc_protocol.h for why this is required. */
    uint32_t extradata_size = g_ctx->extradata_size > 0 ? (uint32_t)g_ctx->extradata_size : 0;
    size_t ok_len = sizeof(struct ipc_init_ok) + extradata_size;
    uint8_t *ok_buf = malloc(ok_len);
    struct ipc_init_ok ok = { .extradata_size = extradata_size };
    memcpy(ok_buf, &ok, sizeof(ok));
    if (extradata_size)
        memcpy(ok_buf + sizeof(ok), g_ctx->extradata, extradata_size);
    ipc_send_msg(fd, IPC_MSG_INIT_OK, ok_buf, (uint32_t)ok_len);
    free(ok_buf);
    fprintf(stderr, "nvenc-helper: initialized %s %ux%u (extradata %u bytes)\n",
            codec_name, req.width, req.height, extradata_size);
    return 0;
}

static int drain_packets(int fd) {
    while (1) {
        int ret = avcodec_receive_packet(g_ctx, g_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            die_send_err(fd, IPC_MSG_ERR, "avcodec_receive_packet failed: %s", errbuf);
            return -1;
        }
        struct ipc_packet_hdr hdr = {
            .pts = g_pkt->pts,
            .dts = g_pkt->dts,
            .size = (uint32_t)g_pkt->size,
            .flags = (uint32_t)g_pkt->flags,
        };
        size_t total = sizeof(hdr) + (size_t)g_pkt->size;
        uint8_t *buf = malloc(total);
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), g_pkt->data, (size_t)g_pkt->size);
        int rc = ipc_send_msg(fd, IPC_MSG_PACKET, buf, (uint32_t)total);
        free(buf);
        av_packet_unref(g_pkt);
        if (rc < 0) return -1;
    }
    return ipc_send_msg(fd, IPC_MSG_FRAME_DONE, NULL, 0);
}

static int handle_frame(int fd, const uint8_t *payload, uint32_t payload_len) {
    if (!g_ctx) {
        die_send_err(fd, IPC_MSG_ERR, "FRAME before INIT");
        return -1;
    }
    if (payload_len < sizeof(struct ipc_frame_hdr)) {
        die_send_err(fd, IPC_MSG_ERR, "FRAME payload too short");
        return -1;
    }
    struct ipc_frame_hdr hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    const uint8_t *data = payload + sizeof(hdr);
    uint32_t data_len = payload_len - (uint32_t)sizeof(hdr);

    int w = g_ctx->width, h = g_ctx->height;
    size_t y_size = (size_t)w * h;
    size_t c_size = (size_t)(w / 2) * (h / 2);
    if (data_len < y_size + 2 * c_size) {
        die_send_err(fd, IPC_MSG_ERR, "FRAME data too short for %dx%d", w, h);
        return -1;
    }

    if (av_frame_make_writable(g_frame) < 0) {
        die_send_err(fd, IPC_MSG_ERR, "av_frame_make_writable failed");
        return -1;
    }
    /* Unpack contiguous Y/U/V planes into the frame's (possibly
     * strided) buffers. */
    for (int row = 0; row < h; row++)
        memcpy(g_frame->data[0] + row * g_frame->linesize[0], data + row * w, w);
    const uint8_t *u = data + y_size;
    const uint8_t *v = u + c_size;
    for (int row = 0; row < h / 2; row++) {
        memcpy(g_frame->data[1] + row * g_frame->linesize[1], u + row * (w / 2), w / 2);
        memcpy(g_frame->data[2] + row * g_frame->linesize[2], v + row * (w / 2), w / 2);
    }
    g_frame->pts = hdr.pts;

    int ret = avcodec_send_frame(g_ctx, g_frame);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        die_send_err(fd, IPC_MSG_ERR, "avcodec_send_frame failed: %s", errbuf);
        return -1;
    }
    return drain_packets(fd);
}

static int handle_flush(int fd) {
    if (!g_ctx) {
        die_send_err(fd, IPC_MSG_ERR, "FLUSH before INIT");
        return -1;
    }
    avcodec_send_frame(g_ctx, NULL);
    return drain_packets(fd);
}

static int handle_dec_init(int fd, const uint8_t *payload, uint32_t payload_len) {
    if (payload_len < sizeof(struct ipc_dec_init_req)) {
        die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "DEC_INIT payload too short");
        return -1;
    }
    struct ipc_dec_init_req req;
    memcpy(&req, payload, sizeof(req));
    const uint8_t *extradata = payload + sizeof(req);
    uint32_t extradata_avail = payload_len - (uint32_t)sizeof(req);
    if (extradata_avail < req.extradata_size) {
        die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "DEC_INIT extradata shorter than declared size");
        return -1;
    }

    const char *codec_name = req.codec_id == 1 ? "hevc_cuvid" : "h264_cuvid";
    const AVCodec *codec = avcodec_find_decoder_by_name(codec_name);
    if (!codec) {
        die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "decoder %s not found", codec_name);
        return -1;
    }

    g_dec_ctx = avcodec_alloc_context3(codec);
    if (!g_dec_ctx) {
        die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "avcodec_alloc_context3 failed");
        return -1;
    }
    /* extradata carries the container's out-of-band SPS/PPS/VPS
     * (avcC/hvcC record) -- without it cuvid never sees the parameter
     * sets and silently decodes zero frames from any real
     * container-demuxed stream. Must be allocated with
     * AV_INPUT_BUFFER_PADDING_SIZE extra bytes per AVCodecContext
     * .extradata's documented contract. avcodec_free_context() owns
     * and frees this allocation once assigned. */
    if (req.extradata_size > 0) {
        g_dec_ctx->extradata = av_malloc(req.extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!g_dec_ctx->extradata) {
            die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "av_malloc for extradata failed");
            avcodec_free_context(&g_dec_ctx);
            return -1;
        }
        memcpy(g_dec_ctx->extradata, extradata, req.extradata_size);
        memset(g_dec_ctx->extradata + req.extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        g_dec_ctx->extradata_size = (int)req.extradata_size;
    }

    /* cuvid derives width/height/profile from the bitstream itself. */
    int ret = avcodec_open2(g_dec_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        die_send_err(fd, IPC_MSG_DEC_INIT_ERR, "avcodec_open2 failed: %s", errbuf);
        avcodec_free_context(&g_dec_ctx);
        return -1;
    }

    g_dec_frame = av_frame_alloc();
    g_dec_pkt = av_packet_alloc();

    ipc_send_msg(fd, IPC_MSG_DEC_INIT_OK, NULL, 0);
    fprintf(stderr, "nvenc-helper: decoder initialized %s\n", codec_name);
    return 0;
}

/* Converts cuvid's NV12 output (interleaved UV) to planar I420 and
 * sends it as an IPC_MSG_DEC_FRAME. */
/* cuvid outputs NV12 for 8-bit sources and P010LE for 10-bit sources
 * (very common for real-world HEVC content). We downsample P010LE's
 * 10-bit samples to 8-bit (high byte of each little-endian 16-bit
 * sample) rather than carrying full 10-bit through the wire format --
 * full bit-depth fidelity isn't the goal here, working hardware
 * decode for arbitrary real content is. Found via live testing against
 * real MKV content, 2026-07-02 (synthetic 8-bit test streams never
 * exercised this path). */
static int send_dec_frame(int fd, const AVFrame *frame) {
    int is_p010 = frame->format == AV_PIX_FMT_P010LE;
    if (frame->format != AV_PIX_FMT_NV12 && !is_p010) {
        die_send_err(fd, IPC_MSG_ERR, "unexpected decoded pix_fmt %d (expected NV12 or P010LE)", frame->format);
        return -1;
    }
    int w = frame->width, h = frame->height;
    size_t y_size = (size_t)w * h;
    size_t c_size = (size_t)(w / 2) * (h / 2);
    size_t total = sizeof(struct ipc_dec_frame_hdr) + y_size + 2 * c_size;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    struct ipc_dec_frame_hdr hdr = {
        .pts = frame->pts,
        .width = (uint32_t)w,
        .height = (uint32_t)h,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    uint8_t *p = buf + sizeof(hdr);
    if (is_p010) {
        for (int row = 0; row < h; row++) {
            const uint8_t *src = frame->data[0] + (size_t)row * frame->linesize[0];
            uint8_t *dst = p + (size_t)row * w;
            for (int col = 0; col < w; col++)
                dst[col] = src[2 * col + 1]; /* high byte of each 16-bit LE sample */
        }
    } else {
        for (int row = 0; row < h; row++)
            memcpy(p + (size_t)row * w, frame->data[0] + (size_t)row * frame->linesize[0], w);
    }
    p += y_size;
    uint8_t *u_plane = p;
    uint8_t *v_plane = p + c_size;
    if (is_p010) {
        for (int row = 0; row < h / 2; row++) {
            const uint8_t *uv = frame->data[1] + (size_t)row * frame->linesize[1];
            for (int col = 0; col < w / 2; col++) {
                u_plane[(size_t)row * (w / 2) + col] = uv[4 * col + 1];
                v_plane[(size_t)row * (w / 2) + col] = uv[4 * col + 3];
            }
        }
    } else {
        for (int row = 0; row < h / 2; row++) {
            const uint8_t *uv = frame->data[1] + (size_t)row * frame->linesize[1];
            for (int col = 0; col < w / 2; col++) {
                u_plane[(size_t)row * (w / 2) + col] = uv[2 * col];
                v_plane[(size_t)row * (w / 2) + col] = uv[2 * col + 1];
            }
        }
    }

    int rc = ipc_send_msg(fd, IPC_MSG_DEC_FRAME, buf, (uint32_t)total);
    free(buf);
    return rc;
}

static int drain_frames(int fd) {
    while (1) {
        int ret = avcodec_receive_frame(g_dec_ctx, g_dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            die_send_err(fd, IPC_MSG_ERR, "avcodec_receive_frame failed: %s", errbuf);
            return -1;
        }
        int rc = send_dec_frame(fd, g_dec_frame);
        av_frame_unref(g_dec_frame);
        if (rc < 0) return -1;
    }
    return ipc_send_msg(fd, IPC_MSG_DEC_DONE, NULL, 0);
}

static int handle_dec_packet(int fd, const uint8_t *payload, uint32_t payload_len) {
    if (!g_dec_ctx) {
        die_send_err(fd, IPC_MSG_ERR, "DEC_PACKET before DEC_INIT");
        return -1;
    }
    if (payload_len < sizeof(struct ipc_dec_packet_hdr)) {
        die_send_err(fd, IPC_MSG_ERR, "DEC_PACKET payload too short");
        return -1;
    }
    struct ipc_dec_packet_hdr hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    const uint8_t *data = payload + sizeof(hdr);
    uint32_t data_len = payload_len - (uint32_t)sizeof(hdr);

    int ret;
    if (hdr.size == 0 && data_len == 0) {
        ret = avcodec_send_packet(g_dec_ctx, NULL); /* flush */
        /* The host calls us repeatedly during drain until we report no
         * frame; a decoder that's already fully drained legitimately
         * returns AVERROR_EOF on a repeat flush call -- that's not an
         * error, just "nothing more to give", so fall through to
         * drain_frames() (which will immediately hit EAGAIN/EOF on
         * receive_frame and send an empty DEC_DONE). */
        if (ret == AVERROR_EOF)
            ret = 0;
    } else {
        if (data_len < hdr.size) {
            die_send_err(fd, IPC_MSG_ERR, "DEC_PACKET data shorter than declared size");
            return -1;
        }
        av_packet_unref(g_dec_pkt);
        if (av_new_packet(g_dec_pkt, (int)hdr.size) < 0) {
            die_send_err(fd, IPC_MSG_ERR, "av_new_packet failed");
            return -1;
        }
        memcpy(g_dec_pkt->data, data, hdr.size);
        g_dec_pkt->pts = hdr.pts;
        ret = avcodec_send_packet(g_dec_ctx, g_dec_pkt);
    }
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        die_send_err(fd, IPC_MSG_ERR, "avcodec_send_packet failed: %s", errbuf);
        return -1;
    }
    return drain_frames(fd);
}

int main(void) {
    int fd = IPC_HELPER_FD;
    uint8_t buf[1 << 20]; /* 1MB scratch, sized for one 720p-ish YUV420P frame; grown below if needed */
    uint8_t *big_buf = NULL;
    size_t big_buf_cap = 0;

    while (1) {
        uint8_t type;
        uint32_t payload_len;
        if (ipc_recv_header(fd, &type, &payload_len) < 0) {
            fprintf(stderr, "nvenc-helper: peer closed, exiting\n");
            break;
        }

        uint8_t *payload = buf;
        if (payload_len > sizeof(buf)) {
            if (payload_len > big_buf_cap) {
                free(big_buf);
                big_buf = malloc(payload_len);
                big_buf_cap = payload_len;
            }
            payload = big_buf;
        }
        if (payload_len > 0 && ipc_recv_payload(fd, payload, payload_len) < 0) {
            fprintf(stderr, "nvenc-helper: short read on payload, exiting\n");
            break;
        }

        int rc = 0;
        switch (type) {
        case IPC_MSG_INIT:
            rc = handle_init(fd, payload, payload_len);
            break;
        case IPC_MSG_FRAME:
            rc = handle_frame(fd, payload, payload_len);
            break;
        case IPC_MSG_FLUSH:
            rc = handle_flush(fd);
            break;
        case IPC_MSG_DEC_INIT:
            rc = handle_dec_init(fd, payload, payload_len);
            break;
        case IPC_MSG_DEC_PACKET:
            rc = handle_dec_packet(fd, payload, payload_len);
            break;
        case IPC_MSG_SHUTDOWN:
            fprintf(stderr, "nvenc-helper: shutdown requested\n");
            goto done;
        default:
            die_send_err(fd, IPC_MSG_ERR, "unknown message type %u", type);
            rc = -1;
            break;
        }
        if (rc < 0) {
            /* Non-fatal protocol errors (e.g. a single bad frame) still
             * leave the connection usable; only exit on transport
             * failure, which ipc_recv_header/ipc_recv_payload already
             * handle by breaking the loop above. */
        }
    }

done:
    free(big_buf);
    if (g_pkt) av_packet_free(&g_pkt);
    if (g_frame) av_frame_free(&g_frame);
    if (g_ctx) avcodec_free_context(&g_ctx);
    if (g_dec_pkt) av_packet_free(&g_dec_pkt);
    if (g_dec_frame) av_frame_free(&g_dec_frame);
    if (g_dec_ctx) avcodec_free_context(&g_dec_ctx);
    return 0;
}
