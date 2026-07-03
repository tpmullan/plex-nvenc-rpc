/*
 * Full send_packet/receive_frame round-trip test for h264_cuvid, built
 * under plain glibc. Proves hardware decode works cleanly under glibc,
 * the decode-side counterpart to nvenc_glibc_selftest.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.h264\n", argv[0]);
        return 1;
    }

    const AVCodec *codec = avcodec_find_decoder_by_name("h264_cuvid");
    if (!codec) {
        fprintf(stderr, "h264_cuvid decoder not found\n");
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] found decoder %s\n", codec->name);

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    int ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_open2 failed: %s\n", errbuf);
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] avcodec_open2 OK\n");

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = av_malloc(sz + AV_INPUT_BUFFER_PADDING_SIZE);
    fread(buf, 1, sz, f);
    fclose(f);

    AVPacket *pkt = av_packet_alloc();
    pkt->data = buf;
    pkt->size = (int)sz;

    fprintf(stderr, "[CHECKPOINT] before avcodec_send_packet (%ld bytes)\n", sz);
    ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_send_packet failed: %s\n", errbuf);
        return 1;
    }
    fprintf(stderr, "[CHECKPOINT] avcodec_send_packet OK\n");
    avcodec_send_packet(ctx, NULL); /* flush */

    AVFrame *frame = av_frame_alloc();
    int got = 0;
    while (1) {
        ret = avcodec_receive_frame(ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "avcodec_receive_frame failed: %s\n", errbuf);
            return 1;
        }
        fprintf(stderr, "[CHECKPOINT] got frame %dx%d fmt=%d pts=%lld\n",
                frame->width, frame->height, frame->format, (long long)frame->pts);
        got++;
        av_frame_unref(frame);
    }

    if (got == 0) {
        fprintf(stderr, "FAIL: no frames produced\n");
        return 1;
    }
    fprintf(stderr, "SUCCESS: full glibc NVDEC round-trip produced %d frame(s)\n", got);
    return 0;
}
