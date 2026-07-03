/*
 * Minimal length-prefixed framing I/O shared by glue.c and
 * nvenc-helper.c. Kept as static inline functions in a header (not a
 * .c file) so each side compiles it standalone with its own toolchain
 * (glue.c under musl-hostile constraints, nvenc-helper under glibc) --
 * no shared .o, no ABI to keep in sync beyond this header and
 * ipc_protocol.h.
 */
#ifndef NVENC_IPC_IO_H
#define NVENC_IPC_IO_H

#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static inline int ipc_write_full(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static inline int ipc_read_full(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* peer closed */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Sends [u32 len][u8 type][payload]. len = 1 + payload_len. */
static inline int ipc_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len) {
    uint32_t total_len = payload_len + 1;
    uint8_t hdr[5];
    memcpy(hdr, &total_len, 4);
    hdr[4] = type;
    if (ipc_write_full(fd, hdr, 5) < 0) return -1;
    if (payload_len > 0 && ipc_write_full(fd, payload, payload_len) < 0) return -1;
    return 0;
}

/* Reads the length+type header only. Caller then knows how many
 * payload bytes to read (total_len - 1) via ipc_read_full directly,
 * or ipc_recv_payload below for the common "read it all into a
 * caller buffer" case. */
static inline int ipc_recv_header(int fd, uint8_t *type, uint32_t *payload_len) {
    uint8_t hdr[5];
    if (ipc_read_full(fd, hdr, 5) < 0) return -1;
    uint32_t total_len;
    memcpy(&total_len, hdr, 4);
    if (total_len < 1) return -1;
    *type = hdr[4];
    *payload_len = total_len - 1;
    return 0;
}

/* Reads exactly payload_len bytes into buf. Caller must have already
 * called ipc_recv_header and sized buf >= payload_len. */
static inline int ipc_recv_payload(int fd, void *buf, uint32_t payload_len) {
    if (payload_len == 0) return 0;
    return ipc_read_full(fd, buf, payload_len);
}

#endif
