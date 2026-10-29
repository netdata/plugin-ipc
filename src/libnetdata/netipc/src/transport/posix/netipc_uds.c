/*
 * netipc_uds.c - shared low-level helpers for the POSIX UDS transport.
 */

#include "netipc_uds_internal.h"

#include <limits.h>
#include <string.h>
#include <sys/socket.h>

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
    /* Linux rejects a datagram larger than SO_SNDBUF minus 32 bytes of
     * accounting overhead with EMSGSIZE; very large buffers also hit
     * ENOBUFS near 1 MiB, hence the fixed cap. */
    uint32_t limit = NIPC_UDS_PACKET_SIZE_FALLBACK;
    int val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &len) == 0 &&
        val > NIPC_UDS_SNDBUF_OVERHEAD)
        limit = (uint32_t)val - NIPC_UDS_SNDBUF_OVERHEAD;

    if (limit > NIPC_UDS_PACKET_SIZE_MAX)
        limit = NIPC_UDS_PACKET_SIZE_MAX;
    if (configured != 0 && configured < limit)
        return configured;
    return limit;
}

nipc_uds_error_t nipc_uds_raw_send(int fd, const void *data, size_t len)
{
    ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
    if (n < 0 || (size_t)n != len)
        return NIPC_UDS_ERR_SEND;
    return NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_raw_send_iov(int fd, const void *hdr, size_t hdr_len,
                                       const void *payload, size_t payload_len)
{
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
    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0 || (size_t)n != total)
        return NIPC_UDS_ERR_SEND;

    return NIPC_UDS_OK;
}

ssize_t nipc_uds_raw_recv(int fd, void *buf, size_t buf_len)
{
    return recv(fd, buf, buf_len, 0);
}
