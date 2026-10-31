/* Test-only linker wrappers that make chosen socket calls fail once with
 * EINTR, as a signal arriving before any data moved does. */

#include "uds_eintr_wrap.h"

#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

static unsigned g_send_eintr;
static unsigned g_sendmsg_eintr;
static unsigned g_recv_eintr;

void uds_eintr_wrap_set(unsigned send_count, unsigned sendmsg_count,
                        unsigned recv_count)
{
    __atomic_store_n(&g_send_eintr, send_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sendmsg_eintr, sendmsg_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_recv_eintr, recv_count, __ATOMIC_RELEASE);
}

unsigned uds_eintr_wrap_pending(void)
{
    return __atomic_load_n(&g_send_eintr, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_sendmsg_eintr, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_recv_eintr, __ATOMIC_ACQUIRE);
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

ssize_t __wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    if (take(&g_sendmsg_eintr))
        return -1;
    return __real_sendmsg(fd, msg, flags);
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
    if (take(&g_recv_eintr))
        return -1;
    return __real_recv(fd, buf, len, flags);
}

#if defined(__GLIBC__) && defined(__USE_TIME_BITS64)
/* With 64-bit time_t on 32-bit glibc, sys/socket.h redirects sendmsg to
 * __sendmsg64, so the library references that symbol instead. */
ssize_t __real___sendmsg64(int fd, const struct msghdr *msg, int flags);

ssize_t __wrap___sendmsg64(int fd, const struct msghdr *msg, int flags)
{
    if (take(&g_sendmsg_eintr))
        return -1;
    return __real___sendmsg64(fd, msg, flags);
}
#endif
