/*
 * Full send_frame/receive_packet round-trip test for h264_nvenc, built
 * and run under plain glibc (no musl, no compat shims). Proves the
 * subprocess/RPC bridge architecture's core assumption: real NVENC
 * encoding works cleanly outside Plex's musl process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

int main(void) {
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        fprintf(stderr, "h264_nvenc encoder not found\n");
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] found encoder %s\n", codec->name);

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->width = 640;
    ctx->height = 360;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = (AVRational){1, 25};
    ctx->framerate = (AVRational){25, 1};
    ctx->bit_rate = 2000000;
    ctx->gop_size = 25;

    fprintf(stderr, "[CHECKPOINT] before avcodec_open2\n");
    int ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_open2 failed: %s\n", errbuf);
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] avcodec_open2 OK\n");

    AVFrame *frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "av_frame_get_buffer failed\n");
        return 1;
    }

    /* Fill a trivial gray frame. */
    memset(frame->data[0], 128, frame->linesize[0] * ctx->height);
    memset(frame->data[1], 128, frame->linesize[1] * ctx->height / 2);
    memset(frame->data[2], 128, frame->linesize[2] * ctx->height / 2);
    frame->pts = 0;

    fprintf(stderr, "[CHECKPOINT] before avcodec_send_frame\n");
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_send_frame failed: %s\n", errbuf);
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] avcodec_send_frame OK\n");

    AVPacket *pkt = av_packet_alloc();
    ret = avcodec_send_frame(ctx, NULL); /* flush */
    (void)ret;

    int got = 0;
    while (1) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "avcodec_receive_packet failed: %s\n", errbuf);
            return 1;
        }
        fprintf(stderr, "[CHECKPOINT] got packet size=%d pts=%lld\n",
                pkt->size, (long long)pkt->pts);
        got++;
        av_packet_unref(pkt);
    }

    if (got == 0) {
        fprintf(stderr, "FAIL: no packets produced\n");
        return 1;
    }

    fprintf(stderr, "SUCCESS: full glibc NVENC round-trip produced %d packet(s)\n", got);
    return 0;
}
