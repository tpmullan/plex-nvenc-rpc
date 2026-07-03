/*
 * Standalone test client for the decode side of nvenc-helper: fork+exec
 * the helper, send DEC_INIT + one real H.264 elementary-stream packet +
 * a flush, verify DEC_FRAMEs come back. Throwaway validation harness.
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
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/nvenc-helper file.h264\n", argv[0]);
        return 1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }

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

    struct ipc_dec_init_req init = { .codec_id = 0 };
    if (ipc_send_msg(fd, IPC_MSG_DEC_INIT, &init, sizeof(init)) < 0) {
        fprintf(stderr, "send DEC_INIT failed\n"); return 1;
    }
    uint8_t type; uint32_t plen;
    if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv DEC_INIT reply failed\n"); return 1; }
    if (type == IPC_MSG_DEC_INIT_ERR) {
        char errbuf[256] = {0};
        ipc_recv_payload(fd, errbuf, plen > sizeof(errbuf)-1 ? sizeof(errbuf)-1 : plen);
        fprintf(stderr, "DEC_INIT_ERR: %s\n", errbuf);
        return 1;
    }
    if (type != IPC_MSG_DEC_INIT_OK) { fprintf(stderr, "unexpected reply type %u\n", type); return 1; }
    fprintf(stderr, "[CLIENT] DEC_INIT_OK\n");

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *filebuf = malloc(sz);
    if (fread(filebuf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    size_t msg_len = sizeof(struct ipc_dec_packet_hdr) + (size_t)sz;
    uint8_t *msgbuf = malloc(msg_len);
    struct ipc_dec_packet_hdr ph = { .pts = 0, .size = (uint32_t)sz };
    memcpy(msgbuf, &ph, sizeof(ph));
    memcpy(msgbuf + sizeof(ph), filebuf, sz);

    if (ipc_send_msg(fd, IPC_MSG_DEC_PACKET, msgbuf, (uint32_t)msg_len) < 0) {
        fprintf(stderr, "send DEC_PACKET failed\n"); return 1;
    }

    int total_frames = 0;
    while (1) {
        if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv failed\n"); return 1; }
        if (type == IPC_MSG_DEC_DONE) { ipc_recv_payload(fd, NULL, 0); break; }
        if (type == IPC_MSG_DEC_FRAME) {
            uint8_t *fbuf = malloc(plen);
            ipc_recv_payload(fd, fbuf, plen);
            struct ipc_dec_frame_hdr fh;
            memcpy(&fh, fbuf, sizeof(fh));
            fprintf(stderr, "[CLIENT] got frame #%d %ux%u pts=%lld\n",
                    ++total_frames, fh.width, fh.height, (long long)fh.pts);
            free(fbuf);
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

    /* Flush: zero-size DEC_PACKET */
    struct ipc_dec_packet_hdr flush_hdr = { .pts = 0, .size = 0 };
    ipc_send_msg(fd, IPC_MSG_DEC_PACKET, &flush_hdr, sizeof(flush_hdr));
    while (1) {
        if (ipc_recv_header(fd, &type, &plen) < 0) { fprintf(stderr, "recv failed\n"); return 1; }
        if (type == IPC_MSG_DEC_DONE) { ipc_recv_payload(fd, NULL, 0); break; }
        if (type == IPC_MSG_DEC_FRAME) {
            uint8_t *fbuf = malloc(plen);
            ipc_recv_payload(fd, fbuf, plen);
            struct ipc_dec_frame_hdr fh;
            memcpy(&fh, fbuf, sizeof(fh));
            fprintf(stderr, "[CLIENT] flush got frame #%d %ux%u pts=%lld\n",
                    ++total_frames, fh.width, fh.height, (long long)fh.pts);
            free(fbuf);
        } else {
            fprintf(stderr, "unexpected type %u during flush\n", type);
            return 1;
        }
    }

    ipc_send_msg(fd, IPC_MSG_SHUTDOWN, NULL, 0);
    int status;
    waitpid(pid, &status, 0);

    if (total_frames < 1) { fprintf(stderr, "FAIL: no frames received\n"); return 1; }
    fprintf(stderr, "SUCCESS: %d frame(s) round-tripped through nvenc-helper decode path\n", total_frames);
    return 0;
}
