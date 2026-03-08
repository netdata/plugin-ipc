#define _GNU_SOURCE

#include <netipc/netipc_uds_seqpacket.h>
#include <netipc/netipc_shm_hybrid.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define NETIPC_NEGOTIATION_MAGIC 0x4e48534bu
#define NETIPC_NEGOTIATION_VERSION 1u
#define NETIPC_NEGOTIATION_FRAME_SIZE 64u

#define NETIPC_NEGOTIATION_HELLO 1u
#define NETIPC_NEGOTIATION_ACK 2u

#define NETIPC_NEGOTIATION_STATUS_OK 0u

#define NETIPC_NEG_OFFSET_MAGIC 0u
#define NETIPC_NEG_OFFSET_VERSION 4u
#define NETIPC_NEG_OFFSET_TYPE 6u
#define NETIPC_NEG_OFFSET_SUPPORTED 8u
#define NETIPC_NEG_OFFSET_PREFERRED 12u
#define NETIPC_NEG_OFFSET_INTERSECTION 16u
#define NETIPC_NEG_OFFSET_SELECTED 20u
#define NETIPC_NEG_OFFSET_AUTH_TOKEN 24u
#define NETIPC_NEG_OFFSET_STATUS 32u
#define NETIPC_NEG_OFFSET_RESERVED0 36u

#define NETIPC_IMPLEMENTED_PROFILES (NETIPC_PROFILE_UDS_SEQPACKET | NETIPC_PROFILE_SHM_HYBRID)

struct netipc_negotiation_message {
    uint16_t type;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t intersection_profiles;
    uint32_t selected_profile;
    uint64_t auth_token;
    uint32_t status;
};

struct netipc_uds_seqpacket_server {
    int listen_fd;
    int conn_fd;
    char path[PATH_MAX];
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
    uint32_t negotiated_profile;
    netipc_shm_server_t *shm_server;
};

struct netipc_uds_seqpacket_client {
    int fd;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
    uint32_t negotiated_profile;
    uint64_t next_request_id;
    netipc_shm_client_t *shm_client;
};

static uint16_t host_to_le16(uint16_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap16(value);
#endif
}

static uint32_t host_to_le32(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

static uint64_t host_to_le64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap64(value);
#endif
}

static uint16_t le16_to_host(uint16_t value) { return host_to_le16(value); }
static uint32_t le32_to_host(uint32_t value) { return host_to_le32(value); }
static uint64_t le64_to_host(uint64_t value) { return host_to_le64(value); }

static void write_u16_le(uint8_t *dst, uint16_t value) {
    uint16_t v = host_to_le16(value);
    memcpy(dst, &v, sizeof(v));
}

static void write_u32_le(uint8_t *dst, uint32_t value) {
    uint32_t v = host_to_le32(value);
    memcpy(dst, &v, sizeof(v));
}

static void write_u64_le(uint8_t *dst, uint64_t value) {
    uint64_t v = host_to_le64(value);
    memcpy(dst, &v, sizeof(v));
}

static uint16_t read_u16_le(const uint8_t *src) {
    uint16_t v;
    memcpy(&v, src, sizeof(v));
    return le16_to_host(v);
}

static uint32_t read_u32_le(const uint8_t *src) {
    uint32_t v;
    memcpy(&v, src, sizeof(v));
    return le32_to_host(v);
}

static uint64_t read_u64_le(const uint8_t *src) {
    uint64_t v;
    memcpy(&v, src, sizeof(v));
    return le64_to_host(v);
}

static bool config_is_valid(const struct netipc_uds_seqpacket_config *config) {
    return config && config->run_dir && config->run_dir[0] != '\0' && config->service_name &&
           config->service_name[0] != '\0';
}

static mode_t effective_mode(const struct netipc_uds_seqpacket_config *config) {
    if (!config || config->file_mode == 0u) {
        return (mode_t)0600;
    }

    return (mode_t)config->file_mode;
}

static uint32_t effective_supported_profiles(const struct netipc_uds_seqpacket_config *config) {
    uint32_t supported = NETIPC_SEQPACKET_DEFAULT_PROFILES;
    if (config && config->supported_profiles != 0u) {
        supported = config->supported_profiles;
    }

    supported &= NETIPC_IMPLEMENTED_PROFILES;
    if (supported == 0u) {
        supported = NETIPC_SEQPACKET_DEFAULT_PROFILES;
    }
    return supported;
}

static uint32_t effective_preferred_profiles(const struct netipc_uds_seqpacket_config *config,
                                             uint32_t supported) {
    uint32_t preferred = supported;
    if (config && config->preferred_profiles != 0u) {
        preferred = config->preferred_profiles;
    }

    preferred &= supported;
    if (preferred == 0u) {
        preferred = supported;
    }
    return preferred;
}

static int build_endpoint_path(const struct netipc_uds_seqpacket_config *config, char out_path[PATH_MAX]) {
    if (!config_is_valid(config) || !out_path) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(out_path, PATH_MAX, "%s/%s.sock", config->run_dir, config->service_name);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static char *dup_nonempty(const char *s) {
    if (!s || s[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    size_t n = strlen(s) + 1u;
    char *out = malloc(n);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

static int monotonic_now_ns(uint64_t *out_now_ns) {
    if (!out_now_ns) {
        errno = EINVAL;
        return -1;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }

    *out_now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

static int sleep_micros(uint32_t usec) {
    struct timespec ts = {
        .tv_sec = (time_t)(usec / 1000000u),
        .tv_nsec = (long)((usec % 1000000u) * 1000u),
    };

    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}

static int create_shm_client_with_retry(const char *run_dir,
                                        const char *service_name,
                                        uint32_t timeout_ms,
                                        netipc_shm_client_t **out_client) {
    if (!run_dir || !service_name || !out_client) {
        errno = EINVAL;
        return -1;
    }
    *out_client = NULL;

    struct netipc_shm_config shm_config = {
        .run_dir = run_dir,
        .service_name = service_name,
        .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
        .file_mode = 0u,
    };

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline) {
        if (monotonic_now_ns(&deadline_ns) != 0) {
            return -1;
        }
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    for (;;) {
        if (netipc_shm_client_create(&shm_config, out_client) == 0) {
            return 0;
        }

        int saved = errno;
        if (saved != ENOENT && saved != ECONNREFUSED && saved != EPROTO) {
            errno = saved;
            return -1;
        }

        if (has_deadline) {
            uint64_t now_ns = 0;
            if (monotonic_now_ns(&now_ns) != 0) {
                return -1;
            }
            if (now_ns >= deadline_ns) {
                errno = ETIMEDOUT;
                return -1;
            }
        }

        if (sleep_micros(500u) != 0) {
            return -1;
        }
    }
}

static int wait_fd_event(int fd, short events, bool has_deadline, uint64_t deadline_ns) {
    struct pollfd pfd = {
        .fd = fd,
        .events = events,
        .revents = 0,
    };

    for (;;) {
        int timeout_ms = -1;
        if (has_deadline) {
            uint64_t now_ns = 0;
            if (monotonic_now_ns(&now_ns) != 0) {
                return -1;
            }

            if (now_ns >= deadline_ns) {
                errno = ETIMEDOUT;
                return -1;
            }

            uint64_t remain_ns = deadline_ns - now_ns;
            timeout_ms = (int)((remain_ns + 999999ull) / 1000000ull);
            if (timeout_ms < 0) {
                timeout_ms = 0;
            }
        }

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = ECONNRESET;
                return -1;
            }

            if ((pfd.revents & events) != 0) {
                return 0;
            }
            continue;
        }

        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static int send_exact(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    if (!buf || len == 0u) {
        errno = EINVAL;
        return -1;
    }

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline && monotonic_now_ns(&deadline_ns) != 0) {
        return -1;
    }
    if (has_deadline) {
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    size_t sent = 0u;
    while (sent < len) {
        ssize_t rc = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }

        if (rc == 0) {
            errno = ECONNRESET;
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd_event(fd, POLLOUT, has_deadline, deadline_ns) != 0) {
                return -1;
            }
            continue;
        }

        return -1;
    }

    return 0;
}

static int recv_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms) {
    if (!buf || len == 0u) {
        errno = EINVAL;
        return -1;
    }

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline && monotonic_now_ns(&deadline_ns) != 0) {
        return -1;
    }
    if (has_deadline) {
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    size_t received = 0u;
    while (received < len) {
        ssize_t rc = recv(fd, buf + received, len - received, 0);
        if (rc > 0) {
            received += (size_t)rc;
            continue;
        }

        if (rc == 0) {
            errno = ECONNRESET;
            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd_event(fd, POLLIN, has_deadline, deadline_ns) != 0) {
                return -1;
            }
            continue;
        }

        return -1;
    }

    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return -1;
    }

    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_socket_options(int fd) {
#ifndef SO_NOSIGPIPE
    (void)fd;
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
        return -1;
    }
#endif

    return 0;
}

static int create_seqpacket_socket(void) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

#if SOCK_CLOEXEC == 0
    if (set_cloexec(fd) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#endif

    if (set_nonblocking(fd) != 0 || set_socket_options(fd) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

static int make_sockaddr(const char *path, struct sockaddr_un *out_addr, socklen_t *out_len) {
    if (!path || !out_addr || !out_len) {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(path);
    if (path_len >= sizeof(out_addr->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sun_family = AF_UNIX;
    memcpy(out_addr->sun_path, path, path_len + 1u);
    *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
    return 0;
}

static int connect_with_timeout(int fd, const char *path, uint32_t timeout_ms) {
    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (make_sockaddr(path, &addr, &addr_len) != 0) {
        return -1;
    }

    int rc = connect(fd, (struct sockaddr *)&addr, addr_len);
    if (rc == 0) {
        return 0;
    }

    if (errno != EINPROGRESS) {
        return -1;
    }

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline && monotonic_now_ns(&deadline_ns) != 0) {
        return -1;
    }
    if (has_deadline) {
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    if (wait_fd_event(fd, POLLOUT, has_deadline, deadline_ns) != 0) {
        return -1;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
        return -1;
    }

    if (so_error != 0) {
        errno = so_error;
        return -1;
    }

    return 0;
}

static int try_takeover_stale_socket(const char *path) {
    int fd = create_seqpacket_socket();
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (make_sockaddr(path, &addr, &addr_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    int rc = connect(fd, (struct sockaddr *)&addr, addr_len);
    if (rc == 0) {
        close(fd);
        errno = EADDRINUSE;
        return 0;
    }

    int saved = errno;
    close(fd);

    if (saved != ECONNREFUSED && saved != ENOENT && saved != ENOTSOCK && saved != ECONNRESET) {
        errno = saved;
        return -1;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }

    return 1;
}

static uint32_t select_profile(uint32_t mask) {
    if ((mask & NETIPC_PROFILE_UDS_SEQPACKET) != 0u) {
        return NETIPC_PROFILE_UDS_SEQPACKET;
    }
    if ((mask & NETIPC_PROFILE_SHM_HYBRID) != 0u) {
        return NETIPC_PROFILE_SHM_HYBRID;
    }
    if ((mask & NETIPC_PROFILE_SHM_FUTEX) != 0u) {
        return NETIPC_PROFILE_SHM_FUTEX;
    }
    return 0u;
}

static void encode_negotiation_frame(uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE],
                                     const struct netipc_negotiation_message *msg) {
    memset(frame, 0, NETIPC_NEGOTIATION_FRAME_SIZE);
    write_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC, NETIPC_NEGOTIATION_MAGIC);
    write_u16_le(frame + NETIPC_NEG_OFFSET_VERSION, NETIPC_NEGOTIATION_VERSION);
    write_u16_le(frame + NETIPC_NEG_OFFSET_TYPE, msg->type);
    write_u32_le(frame + NETIPC_NEG_OFFSET_SUPPORTED, msg->supported_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_PREFERRED, msg->preferred_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_INTERSECTION, msg->intersection_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_SELECTED, msg->selected_profile);
    write_u64_le(frame + NETIPC_NEG_OFFSET_AUTH_TOKEN, msg->auth_token);
    write_u32_le(frame + NETIPC_NEG_OFFSET_STATUS, msg->status);
    write_u32_le(frame + NETIPC_NEG_OFFSET_RESERVED0, 0u);
}

static int decode_negotiation_frame(const uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE],
                                    uint16_t expected_type,
                                    struct netipc_negotiation_message *out_msg) {
    if (!frame || !out_msg) {
        errno = EINVAL;
        return -1;
    }

    uint32_t magic = read_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC);
    uint16_t version = read_u16_le(frame + NETIPC_NEG_OFFSET_VERSION);
    uint16_t type = read_u16_le(frame + NETIPC_NEG_OFFSET_TYPE);
    if (magic != NETIPC_NEGOTIATION_MAGIC || version != NETIPC_NEGOTIATION_VERSION || type != expected_type) {
        errno = EPROTO;
        return -1;
    }

    out_msg->type = type;
    out_msg->supported_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_SUPPORTED);
    out_msg->preferred_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_PREFERRED);
    out_msg->intersection_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_INTERSECTION);
    out_msg->selected_profile = read_u32_le(frame + NETIPC_NEG_OFFSET_SELECTED);
    out_msg->auth_token = read_u64_le(frame + NETIPC_NEG_OFFSET_AUTH_TOKEN);
    out_msg->status = read_u32_le(frame + NETIPC_NEG_OFFSET_STATUS);
    return 0;
}

static int write_negotiation(int fd, const struct netipc_negotiation_message *msg, uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation_frame(frame, msg);
    return send_exact(fd, frame, sizeof(frame), timeout_ms);
}

static int read_negotiation(int fd,
                            uint16_t expected_type,
                            struct netipc_negotiation_message *out_msg,
                            uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    if (recv_exact(fd, frame, sizeof(frame), timeout_ms) != 0) {
        return -1;
    }

    return decode_negotiation_frame(frame, expected_type, out_msg);
}

static int perform_server_handshake(const netipc_uds_seqpacket_server_t *server,
                                    int fd,
                                    uint32_t timeout_ms,
                                    uint32_t *out_selected_profile) {
    if (!server || fd < 0 || !out_selected_profile) {
        errno = EINVAL;
        return -1;
    }

    struct netipc_negotiation_message hello;
    if (read_negotiation(fd, NETIPC_NEGOTIATION_HELLO, &hello, timeout_ms) != 0) {
        return -1;
    }

    struct netipc_negotiation_message ack = {
        .type = NETIPC_NEGOTIATION_ACK,
        .supported_profiles = server->supported_profiles,
        .preferred_profiles = server->preferred_profiles,
        .intersection_profiles = hello.supported_profiles & server->supported_profiles,
        .selected_profile = 0u,
        .auth_token = 0u,
        .status = NETIPC_NEGOTIATION_STATUS_OK,
    };

    if (server->auth_token != 0u && hello.auth_token != server->auth_token) {
        ack.status = (uint32_t)EACCES;
    } else {
        uint32_t candidates = ack.intersection_profiles & server->preferred_profiles;
        if (candidates == 0u) {
            candidates = ack.intersection_profiles;
        }

        ack.selected_profile = select_profile(candidates);
        if (ack.selected_profile == 0u) {
            ack.status = (uint32_t)ENOTSUP;
        }
    }

    if (write_negotiation(fd, &ack, timeout_ms) != 0) {
        return -1;
    }

    if (ack.status != NETIPC_NEGOTIATION_STATUS_OK) {
        errno = (int)ack.status;
        return -1;
    }

    *out_selected_profile = ack.selected_profile;
    return 0;
}

static int perform_client_handshake(netipc_uds_seqpacket_client_t *client, uint32_t timeout_ms) {
    if (!client || client->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct netipc_negotiation_message hello = {
        .type = NETIPC_NEGOTIATION_HELLO,
        .supported_profiles = client->supported_profiles,
        .preferred_profiles = client->preferred_profiles,
        .intersection_profiles = 0u,
        .selected_profile = 0u,
        .auth_token = client->auth_token,
        .status = NETIPC_NEGOTIATION_STATUS_OK,
    };

    if (write_negotiation(client->fd, &hello, timeout_ms) != 0) {
        return -1;
    }

    struct netipc_negotiation_message ack;
    if (read_negotiation(client->fd, NETIPC_NEGOTIATION_ACK, &ack, timeout_ms) != 0) {
        return -1;
    }

    if (ack.status != NETIPC_NEGOTIATION_STATUS_OK) {
        errno = (int)ack.status;
        return -1;
    }

    if ((ack.intersection_profiles & client->supported_profiles) == 0u || ack.selected_profile == 0u ||
        (ack.selected_profile & client->supported_profiles) == 0u) {
        errno = EPROTO;
        return -1;
    }

    client->negotiated_profile = ack.selected_profile;
    return 0;
}

int netipc_uds_seqpacket_server_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_server_t **out_server) {
    if (!config || !out_server) {
        errno = EINVAL;
        return -1;
    }
    *out_server = NULL;

    netipc_uds_seqpacket_server_t *server = calloc(1, sizeof(*server));
    if (!server) {
        return -1;
    }
    server->listen_fd = -1;
    server->conn_fd = -1;
    server->supported_profiles = effective_supported_profiles(config);
    server->preferred_profiles = effective_preferred_profiles(config, server->supported_profiles);
    server->auth_token = config->auth_token;

    server->run_dir = dup_nonempty(config->run_dir);
    server->service_name = dup_nonempty(config->service_name);
    if (!server->run_dir || !server->service_name) {
        int saved = errno;
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    if (build_endpoint_path(config, server->path) != 0) {
        int saved = errno;
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    server->listen_fd = create_seqpacket_socket();
    if (server->listen_fd < 0) {
        free(server->run_dir);
        free(server->service_name);
        free(server);
        return -1;
    }

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (make_sockaddr(server->path, &addr, &addr_len) != 0) {
        int saved = errno;
        close(server->listen_fd);
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    bool bound = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (bind(server->listen_fd, (struct sockaddr *)&addr, addr_len) == 0) {
            bound = true;
            break;
        }

        if (errno != EADDRINUSE) {
            int saved = errno;
            close(server->listen_fd);
            free(server->run_dir);
            free(server->service_name);
            free(server);
            errno = saved;
            return -1;
        }

        int takeover = try_takeover_stale_socket(server->path);
        if (takeover <= 0) {
            int saved = errno;
            close(server->listen_fd);
            free(server->run_dir);
            free(server->service_name);
            free(server);
            errno = (takeover == 0) ? EADDRINUSE : saved;
            return -1;
        }
    }

    if (!bound) {
        int saved = (errno != 0) ? errno : EADDRINUSE;
        close(server->listen_fd);
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    if (chmod(server->path, effective_mode(config)) != 0) {
        int saved = errno;
        close(server->listen_fd);
        unlink(server->path);
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    if (listen(server->listen_fd, 16) != 0) {
        int saved = errno;
        close(server->listen_fd);
        unlink(server->path);
        free(server->run_dir);
        free(server->service_name);
        free(server);
        errno = saved;
        return -1;
    }

    *out_server = server;
    return 0;
}

int netipc_uds_seqpacket_server_accept(netipc_uds_seqpacket_server_t *server, uint32_t timeout_ms) {
    if (!server || server->listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (server->conn_fd >= 0) {
        errno = EISCONN;
        return -1;
    }

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline && monotonic_now_ns(&deadline_ns) != 0) {
        return -1;
    }
    if (has_deadline) {
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    if (wait_fd_event(server->listen_fd, POLLIN, has_deadline, deadline_ns) != 0) {
        return -1;
    }

    int fd = accept(server->listen_fd, NULL, NULL);
    if (fd < 0) {
        return -1;
    }

    if (set_nonblocking(fd) != 0 || set_cloexec(fd) != 0 || set_socket_options(fd) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    uint32_t negotiated_profile = 0;
    if (perform_server_handshake(server, fd, timeout_ms, &negotiated_profile) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (server->shm_server) {
            netipc_shm_server_destroy(server->shm_server);
            server->shm_server = NULL;
        }

        struct netipc_shm_config shm_config = {
            .run_dir = server->run_dir,
            .service_name = server->service_name,
            .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
            .file_mode = 0u,
        };

        if (netipc_shm_server_create(&shm_config, &server->shm_server) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    } else if (server->shm_server) {
        netipc_shm_server_destroy(server->shm_server);
        server->shm_server = NULL;
    }

    server->conn_fd = fd;
    server->negotiated_profile = negotiated_profile;
    return 0;
}

int netipc_uds_seqpacket_server_receive_frame(netipc_uds_seqpacket_server_t *server,
                                              uint8_t frame[NETIPC_FRAME_SIZE],
                                              uint32_t timeout_ms) {
    if (!server || server->conn_fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (server->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!server->shm_server) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_server_receive_frame(server->shm_server, frame, timeout_ms);
    }

    return recv_exact(server->conn_fd, frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_server_send_frame(netipc_uds_seqpacket_server_t *server,
                                           const uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!server || server->conn_fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (server->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        (void)timeout_ms;
        if (!server->shm_server) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_server_send_frame(server->shm_server, frame);
    }

    return send_exact(server->conn_fd, frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_server_receive_increment(netipc_uds_seqpacket_server_t *server,
                                                  uint64_t *request_id,
                                                  struct netipc_increment_request *request,
                                                  uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_uds_seqpacket_server_receive_frame(server, frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_request(frame, request_id, request);
}

int netipc_uds_seqpacket_server_send_increment(netipc_uds_seqpacket_server_t *server,
                                               uint64_t request_id,
                                               const struct netipc_increment_response *response,
                                               uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_encode_increment_response(frame, request_id, response) != 0) {
        return -1;
    }

    return netipc_uds_seqpacket_server_send_frame(server, frame, timeout_ms);
}

uint32_t netipc_uds_seqpacket_server_negotiated_profile(const netipc_uds_seqpacket_server_t *server) {
    if (!server) {
        return 0u;
    }

    return server->negotiated_profile;
}

void netipc_uds_seqpacket_server_destroy(netipc_uds_seqpacket_server_t *server) {
    if (!server) {
        return;
    }

    if (server->conn_fd >= 0) {
        close(server->conn_fd);
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }

    if (server->path[0] != '\0') {
        unlink(server->path);
    }

    if (server->shm_server) {
        netipc_shm_server_destroy(server->shm_server);
        server->shm_server = NULL;
    }

    free(server->run_dir);
    free(server->service_name);
    free(server);
}

int netipc_uds_seqpacket_client_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_client_t **out_client,
                                       uint32_t timeout_ms) {
    if (!config || !out_client) {
        errno = EINVAL;
        return -1;
    }
    *out_client = NULL;

    netipc_uds_seqpacket_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return -1;
    }
    client->fd = -1;
    client->supported_profiles = effective_supported_profiles(config);
    client->preferred_profiles = effective_preferred_profiles(config, client->supported_profiles);
    client->auth_token = config->auth_token;

    client->run_dir = dup_nonempty(config->run_dir);
    client->service_name = dup_nonempty(config->service_name);
    if (!client->run_dir || !client->service_name) {
        int saved = errno;
        free(client->run_dir);
        free(client->service_name);
        free(client);
        errno = saved;
        return -1;
    }

    char path[PATH_MAX];
    if (build_endpoint_path(config, path) != 0) {
        int saved = errno;
        free(client->run_dir);
        free(client->service_name);
        free(client);
        errno = saved;
        return -1;
    }

    client->fd = create_seqpacket_socket();
    if (client->fd < 0) {
        free(client->run_dir);
        free(client->service_name);
        free(client);
        return -1;
    }

    if (connect_with_timeout(client->fd, path, timeout_ms) != 0) {
        int saved = errno;
        close(client->fd);
        free(client->run_dir);
        free(client->service_name);
        free(client);
        errno = saved;
        return -1;
    }

    if (perform_client_handshake(client, timeout_ms) != 0) {
        int saved = errno;
        close(client->fd);
        free(client->run_dir);
        free(client->service_name);
        free(client);
        errno = saved;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (create_shm_client_with_retry(client->run_dir, client->service_name, timeout_ms, &client->shm_client) !=
            0) {
            int saved = errno;
            close(client->fd);
            free(client->run_dir);
            free(client->service_name);
            free(client);
            errno = saved;
            return -1;
        }
    }

    *out_client = client;
    return 0;
}

int netipc_uds_seqpacket_client_call_frame(netipc_uds_seqpacket_client_t *client,
                                           const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                           uint8_t response_frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !request_frame || !response_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!client->shm_client) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_client_call_frame(client->shm_client, request_frame, response_frame, timeout_ms);
    }

    if (send_exact(client->fd, request_frame, NETIPC_FRAME_SIZE, timeout_ms) != 0) {
        return -1;
    }

    if (recv_exact(client->fd, response_frame, NETIPC_FRAME_SIZE, timeout_ms) != 0) {
        return -1;
    }

    return 0;
}

int netipc_uds_seqpacket_client_call_increment(netipc_uds_seqpacket_client_t *client,
                                               const struct netipc_increment_request *request,
                                               struct netipc_increment_response *response,
                                               uint32_t timeout_ms) {
    if (!client || !request || !response) {
        errno = EINVAL;
        return -1;
    }

    uint8_t req_frame[NETIPC_FRAME_SIZE];
    uint8_t resp_frame[NETIPC_FRAME_SIZE];
    uint64_t request_id = ++client->next_request_id;

    if (netipc_encode_increment_request(req_frame, request_id, request) != 0) {
        return -1;
    }

    if (netipc_uds_seqpacket_client_call_frame(client, req_frame, resp_frame, timeout_ms) != 0) {
        return -1;
    }

    uint64_t response_id = 0;
    if (netipc_decode_increment_response(resp_frame, &response_id, response) != 0) {
        return -1;
    }

    if (response_id != request_id) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

uint32_t netipc_uds_seqpacket_client_negotiated_profile(const netipc_uds_seqpacket_client_t *client) {
    if (!client) {
        return 0u;
    }
    return client->negotiated_profile;
}

void netipc_uds_seqpacket_client_destroy(netipc_uds_seqpacket_client_t *client) {
    if (!client) {
        return;
    }

    if (client->fd >= 0) {
        close(client->fd);
    }

    if (client->shm_client) {
        netipc_shm_client_destroy(client->shm_client);
        client->shm_client = NULL;
    }

    free(client->run_dir);
    free(client->service_name);
    free(client);
}
