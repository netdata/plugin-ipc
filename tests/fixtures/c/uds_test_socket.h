/* Socket type of the UDS transport on this platform, for tests that open
 * raw sockets: macOS has no AF_UNIX SOCK_SEQPACKET and carries the same
 * packets over a stream (docs/level1-posix-uds.md). */
#ifndef UDS_TEST_SOCKET_H
#define UDS_TEST_SOCKET_H

#include <sys/socket.h>

#if defined(__APPLE__)
#define TEST_UDS_SOCK_TYPE SOCK_STREAM
#define TEST_UDS_STREAM 1
#else
#define TEST_UDS_SOCK_TYPE SOCK_SEQPACKET
#define TEST_UDS_STREAM 0
#endif

#endif /* UDS_TEST_SOCKET_H */
