/* Run-directory scans and stats must survive 64-bit inode numbers and
 * directory offsets; 32-bit glibc builds without LFS fail them with
 * EOVERFLOW. Only file-local types are affected (no off_t/stat in headers). */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

/* accept4() is a GNU/BSD extension on glibc and musl. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "netipc_uds_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <time.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

/* Longest single connect wait, so an abort ends a connect promptly. */
#define UDS_CONNECT_SLICE_MS 100u

static pthread_mutex_t bind_umask_lock = PTHREAD_MUTEX_INITIALIZER;

/* Sockets must not leak into children a consumer forks and execs: an
 * inherited session socket keeps the peer from ever seeing EOF, which is
 * the dead-peer signal. Platforms without SOCK_CLOEXEC set the flag right
 * after creation. */
static int seqpacket_socket_cloexec(void)
{
#ifdef SOCK_CLOEXEC
    return socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
#else
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd >= 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

static int accept_cloexec(int listen_fd)
{
#if defined(SOCK_CLOEXEC) && (defined(__linux__) || defined(__FreeBSD__))
    return accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
#else
    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

static int bind_owner_only_socket(int fd, const struct sockaddr_un *addr)
{
    int rc = pthread_mutex_lock(&bind_umask_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    /* Bind path sockets as 0600 without a post-bind permission window. */
    // Flawfinder: ignore
    mode_t old_mask = umask(S_IXUSR | S_IRWXG | S_IRWXO); // nosemgrep // NOSONAR
    int bind_rc = bind(fd, (const struct sockaddr *)addr, sizeof(*addr));
    int saved_errno = errno;
    // Flawfinder: ignore
    umask(old_mask); // nosemgrep // NOSONAR

    rc = pthread_mutex_unlock(&bind_umask_lock);
    if (bind_rc < 0) {
        errno = saved_errno;
        return -1;
    }
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int copy_cstr_checked(char *dst, size_t dst_size, const char *src)
{
    if (!dst || !src || dst_size == 0)
        return -1;

    size_t len = 0;
    while (len < dst_size && src[len] != '\0')
        len++;
    if (len == dst_size)
        return -1;

    memcpy(dst, src, len + 1);
    return 0;
}

static int fill_sockaddr_path(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    return copy_cstr_checked(addr->sun_path, sizeof(addr->sun_path), path);
}

static int validate_service_name(const char *name)
{
    if (!name || name[0] == '\0')
        return -1;

    if (name[0] == '.' && (name[1] == '\0' ||
                           (name[1] == '.' && name[2] == '\0')))
        return -1;

    for (const char *p = name; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return -1;
    }
    return 0;
}

int nipc_uds_build_socket_name(char *dst, size_t dst_len,
                               const char *service_name)
{
    if (validate_service_name(service_name) < 0)
        return -2;

    int n = snprintf(dst, dst_len, "%s.sock", service_name);
    if (n < 0 || (size_t)n >= dst_len)
        return -1;
    return 0;
}

int nipc_uds_build_socket_path(char *dst, size_t dst_len,
                               const char *run_dir,
                               const char *service_name)
{
    char socket_name[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int name_rc = nipc_uds_build_socket_name(socket_name, sizeof(socket_name),
                                             service_name);
    if (name_rc < 0)
        return name_rc;

    int n = snprintf(dst, dst_len, "%s/%s", run_dir, socket_name);
    if (n < 0 || (size_t)n >= dst_len)
        return -1;
    return 0;
}

static int unlink_stale_socket_path(const char *run_dir,
                                    const char *socket_name)
{
    DIR *dir = opendir(run_dir);
    if (!dir)
        return -1;

    int dir_fd = dirfd(dir);
    if (dir_fd < 0) {
        closedir(dir);
        return -1;
    }

    /* Whatever sits at the endpoint name is not a live server (the connect
     * probe already failed): a dead server's socket or a foreign file.
     * Reclaim the name; directories are junk too. */
    int ret = -1;
    if (unlinkat(dir_fd, socket_name, 0) == 0 || errno == ENOENT)
        ret = 0;
    else if (errno == EISDIR && unlinkat(dir_fd, socket_name, AT_REMOVEDIR) == 0)
        ret = 0;

    closedir(dir);
    return ret;
}

int nipc_uds_check_and_recover_stale(const char *run_dir,
                                     const char *socket_name,
                                     const char *path)
{
    int probe = seqpacket_socket_cloexec();
    if (probe < 0) {
        /* Cannot probe liveness (fd exhaustion) — keep the endpoint and
         * report it in use rather than risk deleting a live socket. */
        return 1;
    }

    struct sockaddr_un addr;
    if (fill_sockaddr_path(&addr, path) != 0) {
        close(probe);
        return -1;
    }

    int ret;
    if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(probe);
        ret = 1;
    } else {
        int saved_errno = errno;
        close(probe);
        if (saved_errno == ENOENT) {
            ret = -1;
        } else {
            ret = (unlink_stale_socket_path(run_dir, socket_name) == 0) ? 0 : 1;
        }
    }
    return ret;
}

static void (*g_listen_hook)(void *user);
static void *g_listen_hook_user;

void nipc_uds_set_listen_hook_for_tests(void (*hook)(void *user), void *user)
{
    g_listen_hook_user = user;
    g_listen_hook = hook;
}

/* Take the endpoint lock {run_dir}/{service_name}.lock. A server's socket
 * file exists from bind() until listen(), and connects in that window are
 * refused just as for a dead server's leftover file, so a connect probe
 * alone cannot tell a starting server from a stale one. The lock can: its
 * holder owns the endpoint, and the kernel drops it when that process
 * dies. The file stays on disk; removing it would let a starter that
 * already opened it lock an orphaned inode.
 * Returns 0 with *fd_out locked, 1 if another listener holds the lock,
 * -1 on error. */
static int acquire_endpoint_lock(const char *run_dir, const char *service_name,
                                 int *fd_out)
{
    char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 8];
    int n = snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", run_dir,
                     service_name);
    if (n < 0 || (size_t)n >= sizeof(lock_path))
        return -1;

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;

    int rc;
    do {
        rc = flock(fd, LOCK_EX | LOCK_NB);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        int saved_errno = errno;
        close(fd);
        return (saved_errno == EWOULDBLOCK) ? 1 : -1;
    }
    *fd_out = fd;
    return 0;
}

nipc_uds_error_t nipc_uds_listen(const char *run_dir,
                                  const char *service_name,
                                  const nipc_uds_server_config_t *config,
                                  nipc_uds_listener_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int path_rc = nipc_uds_build_socket_path(path, sizeof(path), run_dir,
                                             service_name);
    if (path_rc == -2)
        return NIPC_UDS_ERR_BAD_PARAM;
    if (path_rc < 0)
        return NIPC_UDS_ERR_PATH_TOO_LONG;

    char socket_name[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int name_rc = nipc_uds_build_socket_name(socket_name, sizeof(socket_name),
                                             service_name);
    if (name_rc == -2)
        return NIPC_UDS_ERR_BAD_PARAM;
    if (name_rc < 0)
        return NIPC_UDS_ERR_PATH_TOO_LONG;

    int lock_fd = -1;
    int lock_rc = acquire_endpoint_lock(run_dir, service_name, &lock_fd);
    if (lock_rc == 1)
        return NIPC_UDS_ERR_ADDR_IN_USE;
    if (lock_rc < 0)
        return NIPC_UDS_ERR_SOCKET;

    /* The probe still guards against a live server that takes no lock (an
     * older library version). */
    int stale = nipc_uds_check_and_recover_stale(run_dir, socket_name, path);
    if (stale == 1) {
        close(lock_fd);
        return NIPC_UDS_ERR_ADDR_IN_USE;
    }

    int fd = seqpacket_socket_cloexec();
    if (fd < 0) {
        close(lock_fd);
        return NIPC_UDS_ERR_SOCKET;
    }

    struct sockaddr_un addr;
    if (fill_sockaddr_path(&addr, path) != 0) {
        close(fd);
        close(lock_fd);
        return NIPC_UDS_ERR_PATH_TOO_LONG;
    }

    if (bind_owner_only_socket(fd, &addr) < 0) {
        close(fd);
        close(lock_fd);
        return NIPC_UDS_ERR_SOCKET;
    }

    if (g_listen_hook)
        g_listen_hook(g_listen_hook_user);

    int backlog = config->backlog > 0 ? config->backlog : UDS_DEFAULT_BACKLOG;
    if (listen(fd, backlog) < 0) {
        close(fd);
        unlink(path);
        close(lock_fd);
        return NIPC_UDS_ERR_SOCKET;
    }

    out->fd = fd;
    out->config = *config;
    if (copy_cstr_checked(out->path, sizeof(out->path), path) != 0) {
        close(fd);
        unlink(path);
        close(lock_fd);
        memset(out, 0, sizeof(*out));
        out->fd = -1;
        return NIPC_UDS_ERR_PATH_TOO_LONG;
    }
    out->lock_fd = lock_fd;
    out->has_lock = true;

    return NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_accept_pending(nipc_uds_listener_t *listener,
                                          int *fd_out)
{
    if (!listener || !fd_out)
        return NIPC_UDS_ERR_BAD_PARAM;

    *fd_out = accept_cloexec(listener->fd);
    return *fd_out < 0 ? NIPC_UDS_ERR_ACCEPT : NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_accept_handshake(int fd,
                                            const nipc_uds_server_config_t *config,
                                            uint64_t session_id,
                                            uint32_t timeout_ms,
                                            nipc_uds_session_t *out)
{
    if (!out || !config || fd < 0) {
        if (fd >= 0)
            close(fd);
        return NIPC_UDS_ERR_BAD_PARAM;
    }

    memset(out, 0, sizeof(*out));
    out->fd = -1;

    nipc_uds_error_t err = nipc_uds_wait_readable(fd, timeout_ms, -1);
    if (err == NIPC_UDS_OK)
        err = nipc_uds_server_handshake(fd, config, session_id, out);
    if (err != NIPC_UDS_OK) {
        close(fd);
        out->fd = -1;
        return err;
    }

    return NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_accept(nipc_uds_listener_t *listener,
                                  uint64_t session_id,
                                  nipc_uds_session_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int client_fd;
    nipc_uds_error_t err = nipc_uds_accept_pending(listener, &client_fd);
    if (err != NIPC_UDS_OK)
        return err;

    return nipc_uds_accept_handshake(client_fd, &listener->config, session_id,
                                     NIPC_SERVER_HANDSHAKE_TIMEOUT_MS, out);
}

static uint64_t connect_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool connect_abort_raised(int abort_fd)
{
    if (abort_fd < 0)
        return false;
    struct pollfd pfd = { .fd = abort_fd, .events = POLLIN, .revents = 0 };
    return poll(&pfd, 1, 0) > 0;
}

/* Remaining time until deadline_ms (0 = no deadline) as a timeout for
 * the next wait; false once the deadline passed. */
static bool connect_remaining_ms(uint64_t deadline_ms, uint32_t *remaining)
{
    *remaining = 0;
    if (deadline_ms == 0)
        return true;
    uint64_t now = connect_monotonic_ms();
    if (now >= deadline_ms)
        return false;
    uint64_t left = deadline_ms - now;
    *remaining = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
    return true;
}

/* A blocking AF_UNIX connect waits while the listener's backlog is full.
 * On Linux SO_SNDTIMEO bounds that wait and the connect fails with EAGAIN
 * when it expires; waiting in slices lets an abort end it. Other systems
 * refuse a full backlog at once. */
static nipc_uds_error_t connect_bounded(int fd, const struct sockaddr_un *addr,
                                        uint64_t deadline_ms, int abort_fd)
{
    nipc_uds_error_t result = NIPC_UDS_OK;
    for (;;) {
        if (connect_abort_raised(abort_fd)) {
            result = NIPC_UDS_ERR_ABORTED;
            break;
        }
        uint32_t slice = UDS_CONNECT_SLICE_MS;
        uint32_t remaining;
        if (!connect_remaining_ms(deadline_ms, &remaining)) {
            result = NIPC_UDS_ERR_TIMEOUT;
            break;
        }
        if (deadline_ms != 0 && remaining < slice)
            slice = remaining;

        struct timeval tv = {
            .tv_sec = (time_t)(slice / 1000u),
            .tv_usec = (suseconds_t)((slice % 1000u) * 1000u),
        };
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0)
            break;
        if (errno == EISCONN)
            break;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
            errno != EALREADY) {
            result = NIPC_UDS_ERR_CONNECT;
            break;
        }
    }

    /* Session sends block without a timeout. */
    struct timeval none = { 0, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
    return result;
}

nipc_uds_error_t nipc_uds_connect_timeout(const char *run_dir,
                                           const char *service_name,
                                           const nipc_uds_client_config_t *config,
                                           uint32_t timeout_ms,
                                           int abort_fd,
                                           nipc_uds_session_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    uint64_t deadline_ms = timeout_ms ? connect_monotonic_ms() + timeout_ms : 0;

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int path_rc = nipc_uds_build_socket_path(path, sizeof(path), run_dir,
                                             service_name);
    if (path_rc == -2)
        return NIPC_UDS_ERR_BAD_PARAM;
    if (path_rc < 0)
        return NIPC_UDS_ERR_PATH_TOO_LONG;

    int fd = seqpacket_socket_cloexec();
    if (fd < 0)
        return NIPC_UDS_ERR_SOCKET;

    struct sockaddr_un addr;
    if (fill_sockaddr_path(&addr, path) != 0) {
        close(fd);
        return NIPC_UDS_ERR_PATH_TOO_LONG;
    }

    nipc_uds_error_t err = connect_bounded(fd, &addr, deadline_ms, abort_fd);
    if (err != NIPC_UDS_OK) {
        close(fd);
        return err;
    }

    uint32_t remaining;
    if (!connect_remaining_ms(deadline_ms, &remaining)) {
        close(fd);
        return NIPC_UDS_ERR_TIMEOUT;
    }

    err = nipc_uds_client_handshake(fd, config, remaining, abort_fd, out);
    if (err != NIPC_UDS_OK) {
        close(fd);
        out->fd = -1;
        return err;
    }

    return NIPC_UDS_OK;
}

nipc_uds_error_t nipc_uds_connect(const char *run_dir,
                                   const char *service_name,
                                   const nipc_uds_client_config_t *config,
                                   nipc_uds_session_t *out)
{
    return nipc_uds_connect_timeout(run_dir, service_name, config, 0, -1, out);
}

void nipc_uds_close_session(nipc_uds_session_t *session)
{
    if (!session)
        return;

    if (session->fd >= 0) {
        close(session->fd);
        session->fd = -1;
    }

    free(session->recv_buf);
    session->recv_buf = NULL;
    session->recv_buf_size = 0;

    free(session->inflight_ids);
    session->inflight_ids = NULL;
    session->inflight_count = 0;
    session->inflight_capacity = 0;
}

void nipc_uds_close_listener(nipc_uds_listener_t *listener)
{
    if (!listener)
        return;

    if (listener->fd >= 0) {
        close(listener->fd);
        listener->fd = -1;
    }

    if (listener->path[0]) {
        unlink(listener->path);
        listener->path[0] = '\0';
    }

    /* Release the endpoint only after its socket file is gone. */
    if (listener->has_lock) {
        close(listener->lock_fd);
        listener->has_lock = false;
        listener->lock_fd = -1;
    }
}
