/* Test-only linker wrappers that make chosen socket calls fail once with
 * EINTR, as a signal arriving before any data moved does. */

#include "uds_eintr_wrap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

static unsigned g_send_eintr;
static unsigned g_sendmsg_eintr;
static unsigned g_recv_eintr;
static unsigned g_short_recv;
static unsigned g_short_sendmsg;

void uds_eintr_wrap_set(unsigned send_count, unsigned sendmsg_count,
                        unsigned recv_count)
{
    __atomic_store_n(&g_send_eintr, send_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sendmsg_eintr, sendmsg_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_recv_eintr, recv_count, __ATOMIC_RELEASE);
}

void uds_short_io_set(unsigned recv_count, unsigned sendmsg_count)
{
    __atomic_store_n(&g_short_recv, recv_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_short_sendmsg, sendmsg_count, __ATOMIC_RELEASE);
}

unsigned uds_eintr_wrap_pending(void)
{
    return __atomic_load_n(&g_send_eintr, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_sendmsg_eintr, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_recv_eintr, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_short_recv, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_short_sendmsg, __ATOMIC_ACQUIRE);
}

/* Consume one pending injection for a call site, if any. */
static bool take_one(unsigned *count)
{
    unsigned left = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    while (left > 0) {
        if (__atomic_compare_exchange_n(count, &left, left - 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

/* Consume one pending EINTR for a call site, if any. */
static bool take(unsigned *count)
{
    unsigned left = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    while (left > 0) {
        if (__atomic_compare_exchange_n(count, &left, left - 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            errno = EINTR;
            return true;
        }
    }
    return false;
}

ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t __real_recv(int fd, void *buf, size_t len, int flags);

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
    if (take(&g_send_eintr))
        return -1;
    return __real_send(fd, buf, len, flags);
}

/* The first byte of msg's data as a one-byte message, if it has any. */
static bool first_byte_only(const struct msghdr *msg, struct msghdr *one,
                            struct iovec *iov)
{
    for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++) {
        if (msg->msg_iov[i].iov_len == 0)
            continue;
        *one = *msg;
        iov->iov_base = msg->msg_iov[i].iov_base;
        iov->iov_len = 1;
        one->msg_iov = iov;
        one->msg_iovlen = 1;
        return true;
    }
    return false;
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    if (take(&g_sendmsg_eintr))
        return -1;
    struct msghdr one;
    struct iovec iov;
    if (first_byte_only(msg, &one, &iov) && take_one(&g_short_sendmsg))
        return __real_sendmsg(fd, &one, flags);
    return __real_sendmsg(fd, msg, flags);
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
    if (take(&g_recv_eintr))
        return -1;
    if (len > 1 && take_one(&g_short_recv))
        len = 1;
    return __real_recv(fd, buf, len, flags);
}

static unsigned g_accept_eintr;

void uds_eintr_wrap_set_accept(unsigned accept_count)
{
    __atomic_store_n(&g_accept_eintr, accept_count, __ATOMIC_RELEASE);
}

unsigned uds_eintr_wrap_accept_pending(void)
{
    return __atomic_load_n(&g_accept_eintr, __ATOMIC_ACQUIRE);
}

int __real_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);

int __wrap_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    if (take(&g_accept_eintr))
        return -1;
    return __real_accept4(fd, addr, len, flags);
}

static const char *g_bind_probe_path;
static int g_bind_probe_mode = -1;

void uds_bind_probe_arm(const char *path)
{
    g_bind_probe_mode = -1;
    __atomic_store_n(&g_bind_probe_path, path, __ATOMIC_RELEASE);
}

int uds_bind_probe_mode(void)
{
    return g_bind_probe_mode;
}

int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    const char *path = __atomic_exchange_n(&g_bind_probe_path, NULL,
                                           __ATOMIC_ACQ_REL);
    if (path) {
        int saved_errno = errno;
        int pfd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        struct stat st;
        if (pfd >= 0 && fstat(pfd, &st) == 0)
            g_bind_probe_mode = (int)(st.st_mode & 0777);
        if (pfd >= 0)
            close(pfd);
        unlink(path);
        errno = saved_errno;
    }
    return __real_bind(fd, addr, len);
}

#if defined(__GLIBC__) && defined(__USE_TIME_BITS64)
/* With 64-bit time_t on 32-bit glibc, sys/socket.h redirects sendmsg to
 * __sendmsg64, so the library references that symbol instead. */
ssize_t __real___sendmsg64(int fd, const struct msghdr *msg, int flags);

ssize_t __wrap___sendmsg64(int fd, const struct msghdr *msg, int flags)
{
    if (take(&g_sendmsg_eintr))
        return -1;
    struct msghdr one;
    struct iovec iov;
    if (first_byte_only(msg, &one, &iov) && take_one(&g_short_sendmsg))
        return __real___sendmsg64(fd, &one, flags);
    return __real___sendmsg64(fd, msg, flags);
}
#endif
