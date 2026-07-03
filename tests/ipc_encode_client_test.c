/*
 * Standalone test client mimicking what glue.c will do: create a
 * socketpair, fork+exec nvenc-helper with one end on IPC_HELPER_FD,
 * send INIT + a few FRAMEs + FLUSH + SHUTDOWN, and verify PACKETs come
 * back. This is a throwaway validation harness, not part of the
 * shipped plugin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "ipc_protocol.h"
#include "ipc_io.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/nvenc-helper\n", argv[0]);
        return 1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        close(sv[0]);
        dup2(sv[1], IPC_HELPER_FD);
        close(sv[1]);
        execl(argv[1], argv[1], (char *)NULL);
        perror("execl");
        _exit(127);
    }
    close(sv[1]);
    int fd = sv[0];

    int w = 640, h = 360;
    struct ipc_init_req init = {
        .codec_id = 0,
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .pix_fmt = 0, /* AV_PIX_FMT_YUV420P == 0 in this ffmpeg build */
        .bit_rate = 2000000,
        .gop_size = 25,
        .framerate_num = 25,
        .framerate_den = 1,
        .timebase_num = 1,
        .timebase_den = 25,
    };
    if (ipc_send_msg(fd, IPC_MSG_INIT, &init, sizeof(init)) < 0) {
        fprintf(stderr, "send INIT failed\n"); return 1;
    }

    uint8_t type; uint32_t plen;
    if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv INIT reply failed\n"); return 1; }
    if (type == IPC_MSG_INIT_ERR) {
        char errbuf[256] = {0};
        ipc_recv_payload(fd, errbuf, plen > sizeof(errbuf)-1 ? sizeof(errbuf)-1 : plen);
        fprintf(stderr, "INIT_ERR: %s\n", errbuf);
        return 1;
    }
    if (type != IPC_MSG_INIT_OK) { fprintf(stderr, "unexpected reply type %u\n", type); return 1; }
    fprintf(stderr, "[CLIENT] INIT_OK\n");

    size_t y_size = (size_t)w * h;
    size_t c_size = (size_t)(w/2) * (h/2);
    size_t frame_data_len = y_size + 2 * c_size;
    uint8_t *frame_data = malloc(frame_data_len);
    memset(frame_data, 128, frame_data_len);

    size_t hdr_plus_data = sizeof(struct ipc_frame_hdr) + frame_data_len;
    uint8_t *msgbuf = malloc(hdr_plus_data);

    int total_packets = 0;
    for (int i = 0; i < 3; i++) {
        struct ipc_frame_hdr fh = { .pts = i };
        memcpy(msgbuf, &fh, sizeof(fh));
        memcpy(msgbuf + sizeof(fh), frame_data, frame_data_len);
        if (ipc_send_msg(fd, IPC_MSG_FRAME, msgbuf, (uint32_t)hdr_plus_data) < 0) {
            fprintf(stderr, "send FRAME failed\n"); return 1;
        }
        while (1) {
            if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv failed\n"); return 1; }
            if (type == IPC_MSG_FRAME_DONE) { ipc_recv_payload(fd, NULL, 0); break; }
            if (type == IPC_MSG_PACKET) {
                uint8_t *pbuf = malloc(plen);
                ipc_recv_payload(fd, pbuf, plen);
                struct ipc_packet_hdr ph;
                memcpy(&ph, pbuf, sizeof(ph));
                fprintf(stderr, "[CLIENT] got packet #%d size=%u pts=%lld\n",
                        ++total_packets, ph.size, (long long)ph.pts);
                free(pbuf);
            } else if (type == IPC_MSG_ERR) {
                char errbuf[256] = {0};
                ipc_recv_payload(fd, errbuf, plen > sizeof(errbuf)-1 ? sizeof(errbuf)-1 : plen);
                fprintf(stderr, "[CLIENT] ERR: %s\n", errbuf);
                return 1;
            } else {
                fprintf(stderr, "unexpected type %u\n", type);
                return 1;
            }
        }
    }

    if (ipc_send_msg(fd, IPC_MSG_FLUSH, NULL, 0) < 0) { fprintf(stderr, "send FLUSH failed\n"); return 1; }
    while (1) {
        if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv failed\n"); return 1; }
        if (type == IPC_MSG_FRAME_DONE) { ipc_recv_payload(fd, NULL, 0); break; }
        if (type == IPC_MSG_PACKET) {
            uint8_t *pbuf = malloc(plen);
            ipc_recv_payload(fd, pbuf, plen);
            struct ipc_packet_hdr ph;
            memcpy(&ph, pbuf, sizeof(ph));
            fprintf(stderr, "[CLIENT] flush got packet #%d size=%u pts=%lld\n",
                    ++total_packets, ph.size, (long long)ph.pts);
            free(pbuf);
        } else {
            fprintf(stderr, "unexpected type %u during flush\n", type);
            return 1;
        }
    }

    ipc_send_msg(fd, IPC_MSG_SHUTDOWN, NULL, 0);
    int status;
    waitpid(pid, &status, 0);

    if (total_packets < 1) {
        fprintf(stderr, "FAIL: no packets received\n");
        return 1;
    }
    fprintf(stderr, "SUCCESS: %d packet(s) round-tripped through nvenc-helper over IPC\n", total_packets);
    return 0;
}
