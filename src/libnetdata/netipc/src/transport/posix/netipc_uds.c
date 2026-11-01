/*
 * netipc_uds.c - shared low-level helpers for the POSIX UDS transport.
 */

#include "netipc_uds_internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

bool nipc_uds_header_payload_len(size_t payload_len, size_t *msg_len_out)
{
    if (payload_len > SIZE_MAX - NIPC_HEADER_LEN)
        return false;

    size_t msg_len = NIPC_HEADER_LEN + payload_len;
    if (msg_len > UINT32_MAX)
        return false;

    *msg_len_out = msg_len;
    return true;
}

uint32_t nipc_uds_packet_size(int fd, uint32_t configured)
{
#if NIPC_UDS_STREAM
    /* A stream has no per-message size cap; the fixed cap keeps the first
     * packet's buffer bounded and the chunking code shared. */
    (void)fd;
    uint32_t limit = NIPC_UDS_PACKET_SIZE_MAX;
#else
    /* Linux rejects a datagram larger than SO_SNDBUF minus 32 bytes of
     * accounting overhead with EMSGSIZE; very large buffers also hit
     * ENOBUFS near 1 MiB, hence the fixed cap. */
    uint32_t limit = NIPC_UDS_PACKET_SIZE_FALLBACK;
    int val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &len) == 0 &&
        val > NIPC_UDS_SNDBUF_OVERHEAD)
        limit = (uint32_t)val - NIPC_UDS_SNDBUF_OVERHEAD;
#endif

    if (limit > NIPC_UDS_PACKET_SIZE_MAX)
        limit = NIPC_UDS_PACKET_SIZE_MAX;
    if (configured != 0 && configured < limit)
        return configured;
    return limit;
}

/* SOCK_SEQPACKET sends and receives whole messages or nothing, so a call a
 * signal interrupted (EINTR) moved no data and is retried. */
nipc_uds_error_t nipc_uds_raw_send(int fd, const void *data, size_t len)
{
#if NIPC_UDS_STREAM
    return nipc_uds_stream_send_iov(fd, data, len, NULL, 0);
#else
    ssize_t n;
    do {
        n = send(fd, data, len, NIPC_UDS_SEND_FLAGS);
    } while (n < 0 && errno == EINTR);
    if (n < 0 || (size_t)n != len)
        return NIPC_UDS_ERR_SEND;
    return NIPC_UDS_OK;
#endif
}

nipc_uds_error_t nipc_uds_raw_send_iov(int fd, const void *hdr, size_t hdr_len,
                                       const void *payload, size_t payload_len)
{
#if NIPC_UDS_STREAM
    return nipc_uds_stream_send_iov(fd, hdr, hdr_len, payload, payload_len);
#else
    struct iovec iov[2];
    struct msghdr msg;
    int iovcnt = 0;

    memset(&msg, 0, sizeof(msg));

    iov[0].iov_base = (void *)hdr;
    iov[0].iov_len = hdr_len;
    iovcnt = 1;

    if (payload && payload_len > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = payload_len;
        iovcnt = 2;
    }

    msg.msg_iov = iov;
    msg.msg_iovlen = iovcnt;

    size_t total = hdr_len + payload_len;
    ssize_t n;
    do {
        n = sendmsg(fd, &msg, NIPC_UDS_SEND_FLAGS);
    } while (n < 0 && errno == EINTR);
    if (n < 0 || (size_t)n != total)
        return NIPC_UDS_ERR_SEND;

    return NIPC_UDS_OK;
#endif
}

ssize_t nipc_uds_raw_recv(int fd, void *buf, size_t buf_len)
{
    ssize_t n;
    do {
        n = recv(fd, buf, buf_len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Stream-socket packet I/O                                           */
/* ------------------------------------------------------------------ */

nipc_uds_error_t nipc_uds_stream_read_exact(int fd, void *dst, size_t len,
                                           const nipc_uds_wait_t *wait,
                                           bool mid_packet)
{
    uint8_t *p = dst;
    size_t got = 0;
    while (got < len) {
        if (wait) {
            nipc_uds_error_t werr = nipc_uds_wait_for_readable(fd, wait);
            if (werr != NIPC_UDS_OK)
                return werr;
        }
        ssize_t n = nipc_uds_raw_recv(fd, p + got, len - got);
        if (n == 0)
            return (mid_packet || got > 0) ? NIPC_UDS_ERR_PROTOCOL
                                           : NIPC_UDS_ERR_RECV;
        if (n < 0)
            return NIPC_UDS_ERR_RECV;
        got += (size_t)n;
    }
    return NIPC_UDS_OK;
}

size_t nipc_uds_stream_body_len(const void *hdr, size_t buf_len,
                                uint32_t packet_size)
{
    /* The header fields are read raw; the receive path validates them. */
    size_t declared = 0;
    uint32_t magic;
    memcpy(&magic, hdr, sizeof(magic));
    if (magic == NIPC_MAGIC_MSG) {
        nipc_header_t h;
        memcpy(&h, hdr, NIPC_HEADER_LEN);
        size_t cap = packet_size > NIPC_HEADER_LEN
            ? (size_t)packet_size - NIPC_HEADER_LEN
            : buf_len - NIPC_HEADER_LEN;
        declared = h.payload_len < cap ? h.payload_len : cap;
    } else if (magic == NIPC_MAGIC_CHUNK) {
        nipc_chunk_header_t chk;
        memcpy(&chk, hdr, NIPC_HEADER_LEN);
        declared = chk.chunk_payload_len;
    }

    size_t room = buf_len - NIPC_HEADER_LEN;
    return declared < room ? declared : room;
}

nipc_uds_error_t nipc_uds_stream_recv_packet(int fd, void *buf, size_t buf_len,
                                             uint32_t packet_size,
                                             const nipc_uds_wait_t *wait,
                                             size_t *len_out)
{
    *len_out = 0;
    if (buf_len < NIPC_HEADER_LEN)
        return NIPC_UDS_ERR_BAD_PARAM;

    uint8_t *p = buf;
    nipc_uds_error_t err = nipc_uds_stream_read_exact(fd, p, NIPC_HEADER_LEN,
                                                      wait, false);
    if (err != NIPC_UDS_OK)
        return err;

    size_t body = nipc_uds_stream_body_len(p, buf_len, packet_size);
    err = nipc_uds_stream_read_exact(fd, p + NIPC_HEADER_LEN, body, wait, true);
    if (err != NIPC_UDS_OK)
        return err;

    *len_out = NIPC_HEADER_LEN + body;
    return NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_stream_send_iov(int fd, const void *hdr,
                                          size_t hdr_len, const void *payload,
                                          size_t payload_len)
{
    struct iovec iov[2];
    int iovcnt = 1;
    iov[0].iov_base = (void *)hdr;
    iov[0].iov_len = hdr_len;
    if (payload && payload_len > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = payload_len;
        iovcnt = 2;
    }

    struct iovec *cur = iov;
    while (iovcnt > 0) {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = cur;
        msg.msg_iovlen = iovcnt;

        /* A stream write may move part of the data; EINTR means none moved. */
        ssize_t n = sendmsg(fd, &msg, NIPC_UDS_SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return NIPC_UDS_ERR_SEND;
        }
        if (n == 0)
            return NIPC_UDS_ERR_SEND;

        size_t left = (size_t)n;
        while (iovcnt > 0 && left >= cur->iov_len) {
            left -= cur->iov_len;
            cur++;
            iovcnt--;
        }
        if (iovcnt > 0) {
            cur->iov_base = (uint8_t *)cur->iov_base + left;
            cur->iov_len -= left;
        }
    }
    return NIPC_UDS_OK;
}
