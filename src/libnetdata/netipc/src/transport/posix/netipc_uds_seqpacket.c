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
#define NETIPC_NEGOTIATION_PAYLOAD_OFFSET 8u
#define NETIPC_NEGOTIATION_STATUS_OFFSET 48u

#define NETIPC_NEGOTIATION_HELLO 1u
#define NETIPC_NEGOTIATION_ACK 2u

#define NETIPC_NEGOTIATION_STATUS_OK 0u
#define NETIPC_NEGOTIATION_DEFAULT_BATCH_ITEMS 1u

#define NETIPC_NEG_OFFSET_MAGIC 0u
#define NETIPC_NEG_OFFSET_VERSION 4u
#define NETIPC_NEG_OFFSET_TYPE 6u

#if defined(__linux__)
#define NETIPC_IMPLEMENTED_PROFILES (NETIPC_PROFILE_UDS_SEQPACKET | NETIPC_PROFILE_SHM_HYBRID)
#else
#define NETIPC_IMPLEMENTED_PROFILES (NETIPC_PROFILE_UDS_SEQPACKET)
#endif

struct netipc_uds_seqpacket_listener {
    int listen_fd;
    char path[PATH_MAX];
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
};

struct netipc_uds_seqpacket_session {
    int conn_fd;
    char *run_dir;
    char *service_name;
    uint32_t negotiated_profile;
    uint32_t packet_size;
    size_t max_request_message_len;
    size_t max_response_message_len;
    netipc_shm_server_t *shm_server;
};

struct netipc_uds_seqpacket_server {
    netipc_uds_seqpacket_listener_t *listener;
    netipc_uds_seqpacket_session_t *session;
};

struct netipc_uds_seqpacket_client {
    int fd;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
    uint32_t negotiated_profile;
    uint32_t packet_size;
    size_t max_request_message_len;
    size_t max_response_message_len;
    uint64_t next_request_id;
    netipc_shm_client_t *shm_client;
};

struct handshake_result {
    uint32_t selected_profile;
    uint32_t packet_size;
    size_t max_request_message_len;
    size_t max_response_message_len;
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

static uint32_t negotiate_limit_u32(uint32_t offered, uint32_t local_limit) {
    if (offered == 0u || local_limit == 0u) {
        return 0u;
    }
    return offered < local_limit ? offered : local_limit;
}

static uint32_t effective_payload_limit(uint32_t value) {
    return value != 0u ? value : NETIPC_MAX_PAYLOAD_DEFAULT;
}

static uint32_t effective_batch_limit(uint32_t value) {
    return value != 0u ? value : NETIPC_NEGOTIATION_DEFAULT_BATCH_ITEMS;
}

static int compute_max_message_len(uint32_t max_payload_bytes,
                                   uint32_t max_batch_items,
                                   size_t *out_total_size) {
    if (!out_total_size) {
        errno = EINVAL;
        return -1;
    }

    size_t total = netipc_msg_max_batch_total_size(max_payload_bytes, max_batch_items);
    if (total == 0u) {
        if (errno == 0) {
            errno = EOVERFLOW;
        }
        return -1;
    }

    *out_total_size = total;
    return 0;
}

static int compute_seqpacket_packet_size(int fd,
                                         size_t max_request_message_len,
                                         size_t max_response_message_len,
                                         uint32_t *out_packet_size) {
    if (fd < 0 || !out_packet_size) {
        errno = EINVAL;
        return -1;
    }

    int sndbuf = 0;
    socklen_t optlen = (socklen_t)sizeof(sndbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen) != 0 || sndbuf <= 32) {
        return -1;
    }

    size_t logical_limit = max_request_message_len;
    if (max_response_message_len > logical_limit) {
        logical_limit = max_response_message_len;
    }

    size_t packet_size = (size_t)sndbuf - 32u;
    if (logical_limit != 0u && packet_size > logical_limit) {
        packet_size = logical_limit;
    }
    if (packet_size <= NETIPC_CHUNK_HEADER_LEN || packet_size > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    *out_packet_size = (uint32_t)packet_size;
    return 0;
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
                                        size_t max_request_message_len,
                                        size_t max_response_message_len,
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
        .max_request_message_bytes = (uint32_t)max_request_message_len,
        .max_response_message_bytes = (uint32_t)max_response_message_len,
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

static int send_seqpacket_packet(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
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

    for (;;) {
        ssize_t rc = send(fd, buf, len, MSG_NOSIGNAL);
        if (rc > 0) {
            if ((size_t)rc != len) {
                errno = EPROTO;
                return -1;
            }
            return 0;
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
}

static int send_seqpacket_iov_packet(int fd,
                                     const struct iovec *iov,
                                     size_t iov_count,
                                     size_t expected_len,
                                     uint32_t timeout_ms) {
    if (!iov || iov_count == 0u || expected_len == 0u) {
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

    for (;;) {
        struct msghdr msg = {
            .msg_name = NULL,
            .msg_namelen = 0u,
            .msg_iov = (struct iovec *)iov,
            .msg_iovlen = iov_count,
            .msg_control = NULL,
            .msg_controllen = 0u,
            .msg_flags = 0,
        };

        ssize_t rc = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (rc > 0) {
            if ((size_t)rc != expected_len) {
                errno = EPROTO;
                return -1;
            }
            return 0;
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
}

static int recv_seqpacket_packet(int fd,
                                 uint8_t *buf,
                                 size_t capacity,
                                 size_t *out_message_len,
                                 uint32_t timeout_ms) {
    if (!buf || !out_message_len || capacity == 0u) {
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

    for (;;) {
        struct iovec iov = {
            .iov_base = buf,
            .iov_len = capacity,
        };
        struct msghdr msg = {
            .msg_name = NULL,
            .msg_namelen = 0u,
            .msg_iov = &iov,
            .msg_iovlen = 1u,
            .msg_control = NULL,
            .msg_controllen = 0u,
            .msg_flags = 0,
        };

        ssize_t rc = recvmsg(fd, &msg, 0);
        if (rc > 0) {
            if ((msg.msg_flags & MSG_TRUNC) != 0) {
                errno = EMSGSIZE;
                return -1;
            }

            *out_message_len = (size_t)rc;
            return 0;
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
}

static int recv_seqpacket_scatter_packet(int fd,
                                         uint8_t *header,
                                         size_t header_len,
                                         uint8_t *payload,
                                         size_t payload_capacity,
                                         size_t *out_message_len,
                                         uint32_t timeout_ms) {
    if (!header || header_len == 0u || !payload || !out_message_len || payload_capacity == 0u) {
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

    for (;;) {
        struct iovec iov[2] = {
            {.iov_base = header, .iov_len = header_len},
            {.iov_base = payload, .iov_len = payload_capacity},
        };
        struct msghdr msg = {
            .msg_name = NULL,
            .msg_namelen = 0u,
            .msg_iov = iov,
            .msg_iovlen = 2u,
            .msg_control = NULL,
            .msg_controllen = 0u,
            .msg_flags = 0,
        };

        ssize_t rc = recvmsg(fd, &msg, 0);
        if (rc > 0) {
            if ((msg.msg_flags & MSG_TRUNC) != 0) {
                errno = EMSGSIZE;
                return -1;
            }

            *out_message_len = (size_t)rc;
            return 0;
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
}

static int validate_message_len_for_send(const uint8_t *message,
                                         size_t message_len,
                                         size_t max_message_len) {
    if (!message || message_len == 0u) {
        errno = EINVAL;
        return -1;
    }

    if (message_len > max_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    struct netipc_msg_header header;
    if (netipc_decode_msg_header(message, message_len, &header) != 0) {
        return -1;
    }

    size_t total = netipc_msg_total_size(&header);
    if (total != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int validate_received_message(uint8_t *message,
                                     size_t message_len,
                                     size_t max_message_len) {
    if (!message || message_len == 0u) {
        errno = EINVAL;
        return -1;
    }

    if (message_len > max_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    struct netipc_msg_header header;
    if (netipc_decode_msg_header(message, message_len, &header) != 0) {
        return -1;
    }

    size_t total = netipc_msg_total_size(&header);
    if (total != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int compute_chunk_payload_budget(uint32_t packet_size, size_t *out_budget) {
    if (!out_budget || packet_size <= NETIPC_CHUNK_HEADER_LEN) {
        errno = EINVAL;
        return -1;
    }

    *out_budget = (size_t)packet_size - NETIPC_CHUNK_HEADER_LEN;
    return 0;
}

static int send_chunked_message(int fd,
                                const uint8_t *message,
                                size_t message_len,
                                uint32_t packet_size,
                                uint32_t timeout_ms) {
    size_t chunk_payload_budget = 0u;
    if (compute_chunk_payload_budget(packet_size, &chunk_payload_budget) != 0) {
        return -1;
    }

    struct netipc_msg_header logical_header;
    if (netipc_decode_msg_header(message, message_len, &logical_header) != 0) {
        return -1;
    }

    uint32_t chunk_count = (uint32_t)((message_len + chunk_payload_budget - 1u) / chunk_payload_budget);
    uint8_t chunk_header_buf[NETIPC_CHUNK_HEADER_LEN];

    for (uint32_t chunk_index = 0u, offset = 0u; chunk_index < chunk_count; ++chunk_index) {
        size_t remaining = message_len - (size_t)offset;
        size_t chunk_payload_len = remaining < chunk_payload_budget ? remaining : chunk_payload_budget;
        struct netipc_chunk_header chunk_header = {
            .magic = NETIPC_CHUNK_MAGIC,
            .version = NETIPC_CHUNK_VERSION,
            .flags = 0u,
            .message_id = logical_header.message_id,
            .total_message_len = (uint32_t)message_len,
            .chunk_index = chunk_index,
            .chunk_count = chunk_count,
            .chunk_payload_len = (uint32_t)chunk_payload_len,
        };

        if (netipc_encode_chunk_header(chunk_header_buf, sizeof(chunk_header_buf), &chunk_header) != 0) {
            return -1;
        }

        struct iovec iov[2] = {
            {.iov_base = chunk_header_buf, .iov_len = sizeof(chunk_header_buf)},
            {.iov_base = (void *)(message + offset), .iov_len = chunk_payload_len},
        };
        if (send_seqpacket_iov_packet(fd, iov, 2u, sizeof(chunk_header_buf) + chunk_payload_len, timeout_ms) != 0) {
            return -1;
        }

        offset += (uint32_t)chunk_payload_len;
    }

    return 0;
}

static int recv_transport_message(int fd,
                                  uint8_t *message,
                                  size_t message_capacity,
                                  size_t *out_message_len,
                                  size_t max_message_len,
                                  uint32_t packet_size,
                                  uint32_t timeout_ms) {
    if (!message || !out_message_len || message_capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    size_t first_capacity = message_capacity;
    if (packet_size != 0u && first_capacity > packet_size) {
        first_capacity = packet_size;
    }

    size_t first_packet_len = 0u;
    if (recv_seqpacket_packet(fd, message, first_capacity, &first_packet_len, timeout_ms) != 0) {
        return -1;
    }

    struct netipc_msg_header logical_header;
    if (first_packet_len >= NETIPC_MSG_HEADER_LEN &&
        netipc_decode_msg_header(message, first_packet_len, &logical_header) == 0) {
        size_t total = netipc_msg_total_size(&logical_header);
        if (total == first_packet_len) {
            if (validate_received_message(message, first_packet_len, max_message_len) != 0) {
                return -1;
            }
            *out_message_len = first_packet_len;
            return 0;
        }
    }

    struct netipc_chunk_header chunk_header;
    if (netipc_decode_chunk_header(message, first_packet_len, &chunk_header) != 0) {
        return -1;
    }

    if ((size_t)chunk_header.total_message_len > message_capacity ||
        (size_t)chunk_header.total_message_len > max_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    if (chunk_header.chunk_index != 0u || chunk_header.chunk_count < 2u) {
        errno = EPROTO;
        return -1;
    }

    size_t first_payload_len = first_packet_len - NETIPC_CHUNK_HEADER_LEN;
    if (first_payload_len != (size_t)chunk_header.chunk_payload_len) {
        errno = EPROTO;
        return -1;
    }

    memmove(message, message + NETIPC_CHUNK_HEADER_LEN, first_payload_len);
    size_t offset = first_payload_len;

    for (uint32_t expected_index = 1u; expected_index < chunk_header.chunk_count; ++expected_index) {
        uint8_t continuation_header_buf[NETIPC_CHUNK_HEADER_LEN];
        size_t remaining = (size_t)chunk_header.total_message_len - offset;
        size_t packet_payload_cap = remaining;
        if (packet_size > NETIPC_CHUNK_HEADER_LEN &&
            packet_payload_cap > (size_t)packet_size - NETIPC_CHUNK_HEADER_LEN) {
            packet_payload_cap = (size_t)packet_size - NETIPC_CHUNK_HEADER_LEN;
        }

        size_t packet_len = 0u;
        if (recv_seqpacket_scatter_packet(fd,
                                          continuation_header_buf,
                                          sizeof(continuation_header_buf),
                                          message + offset,
                                          packet_payload_cap,
                                          &packet_len,
                                          timeout_ms) != 0) {
            return -1;
        }

        struct netipc_chunk_header continuation_header;
        if (packet_len < NETIPC_CHUNK_HEADER_LEN ||
            netipc_decode_chunk_header(continuation_header_buf,
                                       sizeof(continuation_header_buf),
                                       &continuation_header) != 0) {
            errno = EPROTO;
            return -1;
        }

        size_t payload_len = packet_len - NETIPC_CHUNK_HEADER_LEN;
        if (continuation_header.message_id != chunk_header.message_id ||
            continuation_header.total_message_len != chunk_header.total_message_len ||
            continuation_header.chunk_count != chunk_header.chunk_count ||
            continuation_header.chunk_index != expected_index ||
            payload_len != (size_t)continuation_header.chunk_payload_len) {
            errno = EPROTO;
            return -1;
        }

        offset += payload_len;
    }

    if (offset != (size_t)chunk_header.total_message_len) {
        errno = EPROTO;
        return -1;
    }

    if (validate_received_message(message, offset, max_message_len) != 0) {
        return -1;
    }

    *out_message_len = offset;
    return 0;
}

static int send_transport_message(int fd,
                                  const uint8_t *message,
                                  size_t message_len,
                                  size_t max_message_len,
                                  uint32_t packet_size,
                                  uint32_t timeout_ms) {
    if (validate_message_len_for_send(message, message_len, max_message_len) != 0) {
        return -1;
    }

    if (packet_size != 0u && message_len > (size_t)packet_size) {
        return send_chunked_message(fd, message, message_len, packet_size, timeout_ms);
    }

    return send_seqpacket_packet(fd, message, message_len, timeout_ms);
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

static void encode_negotiation_header(uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE], uint16_t type) {
    memset(frame, 0, NETIPC_NEGOTIATION_FRAME_SIZE);
    write_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC, NETIPC_NEGOTIATION_MAGIC);
    write_u16_le(frame + NETIPC_NEG_OFFSET_VERSION, NETIPC_NEGOTIATION_VERSION);
    write_u16_le(frame + NETIPC_NEG_OFFSET_TYPE, type);
}

static int validate_negotiation_header(const uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE], uint16_t expected_type) {
    if (!frame) {
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

    return 0;
}

static int write_hello_negotiation(int fd, const struct netipc_hello *hello, uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation_header(frame, NETIPC_NEGOTIATION_HELLO);
    if (netipc_encode_hello_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                    NETIPC_CONTROL_HELLO_PAYLOAD_LEN,
                                    hello) != 0) {
        return -1;
    }
    return send_exact(fd, frame, sizeof(frame), timeout_ms);
}

static int write_ack_negotiation(int fd,
                                 const struct netipc_hello_ack *hello_ack,
                                 uint32_t status,
                                 uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation_header(frame, NETIPC_NEGOTIATION_ACK);
    if (netipc_encode_hello_ack_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                        NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN,
                                        hello_ack) != 0) {
        return -1;
    }
    write_u32_le(frame + NETIPC_NEGOTIATION_STATUS_OFFSET, status);
    return send_exact(fd, frame, sizeof(frame), timeout_ms);
}

static int read_hello_negotiation(int fd, struct netipc_hello *hello, uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    if (!hello) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(fd, frame, sizeof(frame), timeout_ms) != 0) {
        return -1;
    }
    if (validate_negotiation_header(frame, NETIPC_NEGOTIATION_HELLO) != 0) {
        return -1;
    }
    return netipc_decode_hello_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                       NETIPC_CONTROL_HELLO_PAYLOAD_LEN,
                                       hello);
}

static int read_ack_negotiation(int fd,
                                struct netipc_hello_ack *hello_ack,
                                uint32_t *out_status,
                                uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    if (!hello_ack || !out_status) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(fd, frame, sizeof(frame), timeout_ms) != 0) {
        return -1;
    }
    if (validate_negotiation_header(frame, NETIPC_NEGOTIATION_ACK) != 0) {
        return -1;
    }
    if (netipc_decode_hello_ack_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                        NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN,
                                        hello_ack) != 0) {
        return -1;
    }
    *out_status = read_u32_le(frame + NETIPC_NEGOTIATION_STATUS_OFFSET);
    return 0;
}

static int perform_server_handshake(const netipc_uds_seqpacket_listener_t *listener,
                                    int fd,
                                    uint32_t timeout_ms,
                                    struct handshake_result *out_result) {
    if (!listener || fd < 0 || !out_result) {
        errno = EINVAL;
        return -1;
    }

    struct netipc_hello hello;
    if (read_hello_negotiation(fd, &hello, timeout_ms) != 0) {
        return -1;
    }

    size_t local_max_request_message_len = 0u;
    size_t local_max_response_message_len = 0u;
    if (compute_max_message_len(listener->max_request_payload_bytes,
                                listener->max_request_batch_items,
                                &local_max_request_message_len) != 0 ||
        compute_max_message_len(listener->max_response_payload_bytes,
                                listener->max_response_batch_items,
                                &local_max_response_message_len) != 0) {
        return -1;
    }

    uint32_t local_packet_size = 0u;
    if (compute_seqpacket_packet_size(fd,
                                      local_max_request_message_len,
                                      local_max_response_message_len,
                                      &local_packet_size) != 0) {
        return -1;
    }

    struct netipc_hello_ack ack = {
        .layout_version = hello.layout_version,
        .flags = 0u,
        .server_supported_profiles = listener->supported_profiles,
        .intersection_profiles = hello.supported_profiles & listener->supported_profiles,
        .selected_profile = 0u,
        .agreed_max_request_payload_bytes =
            negotiate_limit_u32(hello.max_request_payload_bytes, listener->max_request_payload_bytes),
        .agreed_max_request_batch_items =
            negotiate_limit_u32(hello.max_request_batch_items, listener->max_request_batch_items),
        .agreed_max_response_payload_bytes =
            negotiate_limit_u32(hello.max_response_payload_bytes, listener->max_response_payload_bytes),
        .agreed_max_response_batch_items =
            negotiate_limit_u32(hello.max_response_batch_items, listener->max_response_batch_items),
        .agreed_packet_size = negotiate_limit_u32(hello.packet_size, local_packet_size),
    };
    uint32_t status = NETIPC_NEGOTIATION_STATUS_OK;

    struct handshake_result result = {
        .selected_profile = 0u,
        .packet_size = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };

    if (listener->auth_token != 0u && hello.auth_token != listener->auth_token) {
        status = (uint32_t)EACCES;
    } else {
        uint32_t candidates = ack.intersection_profiles & listener->preferred_profiles;
        if (candidates == 0u) {
            candidates = ack.intersection_profiles;
        }

        ack.selected_profile = select_profile(candidates);
        if (ack.selected_profile == 0u) {
            status = (uint32_t)ENOTSUP;
        } else if (ack.agreed_max_request_payload_bytes == 0u || ack.agreed_max_request_batch_items == 0u ||
                   ack.agreed_max_response_payload_bytes == 0u || ack.agreed_max_response_batch_items == 0u ||
                   ack.agreed_packet_size == 0u) {
            status = (uint32_t)EPROTO;
        } else if (compute_max_message_len(ack.agreed_max_request_payload_bytes,
                                           ack.agreed_max_request_batch_items,
                                           &result.max_request_message_len) != 0 ||
                   compute_max_message_len(ack.agreed_max_response_payload_bytes,
                                           ack.agreed_max_response_batch_items,
                                           &result.max_response_message_len) != 0) {
            status = (uint32_t)((errno != 0) ? errno : EOVERFLOW);
        } else if ((size_t)ack.agreed_packet_size >
                   (result.max_request_message_len > result.max_response_message_len
                        ? result.max_request_message_len
                        : result.max_response_message_len)) {
            status = (uint32_t)EPROTO;
        } else {
            result.selected_profile = ack.selected_profile;
            result.packet_size = ack.agreed_packet_size;
        }
    }

    if (write_ack_negotiation(fd, &ack, status, timeout_ms) != 0) {
        return -1;
    }

    if (status != NETIPC_NEGOTIATION_STATUS_OK) {
        errno = (int)status;
        return -1;
    }

    *out_result = result;
    return 0;
}

static int perform_client_handshake(netipc_uds_seqpacket_client_t *client,
                                    uint32_t timeout_ms,
                                    struct handshake_result *out_result) {
    if (!client || client->fd < 0 || !out_result) {
        errno = EINVAL;
        return -1;
    }

    uint32_t local_packet_size = 0u;
    if (compute_seqpacket_packet_size(client->fd,
                                      client->max_request_message_len,
                                      client->max_response_message_len,
                                      &local_packet_size) != 0) {
        return -1;
    }

    struct netipc_hello hello = {
        .layout_version = NETIPC_MSG_VERSION,
        .flags = 0u,
        .supported_profiles = client->supported_profiles,
        .preferred_profiles = client->preferred_profiles,
        .max_request_payload_bytes = client->max_request_payload_bytes,
        .max_request_batch_items = client->max_request_batch_items,
        .max_response_payload_bytes = client->max_response_payload_bytes,
        .max_response_batch_items = client->max_response_batch_items,
        .auth_token = client->auth_token,
        .packet_size = local_packet_size,
    };

    if (write_hello_negotiation(client->fd, &hello, timeout_ms) != 0) {
        return -1;
    }

    struct netipc_hello_ack ack;
    uint32_t status = NETIPC_NEGOTIATION_STATUS_OK;
    if (read_ack_negotiation(client->fd, &ack, &status, timeout_ms) != 0) {
        return -1;
    }

    if (status != NETIPC_NEGOTIATION_STATUS_OK) {
        errno = (int)status;
        return -1;
    }

    struct handshake_result result = {
        .selected_profile = ack.selected_profile,
        .packet_size = ack.agreed_packet_size,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };

    if ((ack.intersection_profiles & client->supported_profiles) == 0u || ack.selected_profile == 0u ||
        (ack.selected_profile & client->supported_profiles) == 0u ||
        ack.agreed_max_request_payload_bytes == 0u || ack.agreed_max_request_batch_items == 0u ||
        ack.agreed_max_response_payload_bytes == 0u || ack.agreed_max_response_batch_items == 0u ||
        ack.agreed_packet_size == 0u || ack.agreed_packet_size > local_packet_size ||
        compute_max_message_len(ack.agreed_max_request_payload_bytes,
                                ack.agreed_max_request_batch_items,
                                &result.max_request_message_len) != 0 ||
        compute_max_message_len(ack.agreed_max_response_payload_bytes,
                                ack.agreed_max_response_batch_items,
                                &result.max_response_message_len) != 0 ||
        (size_t)ack.agreed_packet_size >
            (result.max_request_message_len > result.max_response_message_len
                 ? result.max_request_message_len
                 : result.max_response_message_len)) {
        errno = EPROTO;
        return -1;
    }

    *out_result = result;
    return 0;
}

int netipc_uds_seqpacket_listener_create(const struct netipc_uds_seqpacket_config *config,
                                         netipc_uds_seqpacket_listener_t **out_listener) {
    if (!config || !out_listener) {
        errno = EINVAL;
        return -1;
    }
    *out_listener = NULL;

    netipc_uds_seqpacket_listener_t *listener = calloc(1, sizeof(*listener));
    if (!listener) {
        return -1;
    }
    listener->listen_fd = -1;
    listener->supported_profiles = effective_supported_profiles(config);
    listener->preferred_profiles = effective_preferred_profiles(config, listener->supported_profiles);
    listener->max_request_payload_bytes = effective_payload_limit(config->max_request_payload_bytes);
    listener->max_request_batch_items = effective_batch_limit(config->max_request_batch_items);
    listener->max_response_payload_bytes = effective_payload_limit(config->max_response_payload_bytes);
    listener->max_response_batch_items = effective_batch_limit(config->max_response_batch_items);
    listener->auth_token = config->auth_token;

    listener->run_dir = dup_nonempty(config->run_dir);
    listener->service_name = dup_nonempty(config->service_name);
    if (!listener->run_dir || !listener->service_name) {
        int saved = errno;
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    if (build_endpoint_path(config, listener->path) != 0) {
        int saved = errno;
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    listener->listen_fd = create_seqpacket_socket();
    if (listener->listen_fd < 0) {
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        return -1;
    }

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (make_sockaddr(listener->path, &addr, &addr_len) != 0) {
        int saved = errno;
        close(listener->listen_fd);
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    bool bound = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (bind(listener->listen_fd, (struct sockaddr *)&addr, addr_len) == 0) {
            bound = true;
            break;
        }

        if (errno != EADDRINUSE) {
            int saved = errno;
            close(listener->listen_fd);
            free(listener->run_dir);
            free(listener->service_name);
            free(listener);
            errno = saved;
            return -1;
        }

        int takeover = try_takeover_stale_socket(listener->path);
        if (takeover <= 0) {
            int saved = errno;
            close(listener->listen_fd);
            free(listener->run_dir);
            free(listener->service_name);
            free(listener);
            errno = (takeover == 0) ? EADDRINUSE : saved;
            return -1;
        }
    }

    if (!bound) {
        int saved = (errno != 0) ? errno : EADDRINUSE;
        close(listener->listen_fd);
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    if (chmod(listener->path, effective_mode(config)) != 0) {
        int saved = errno;
        close(listener->listen_fd);
        unlink(listener->path);
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    if (listen(listener->listen_fd, 16) != 0) {
        int saved = errno;
        close(listener->listen_fd);
        unlink(listener->path);
        free(listener->run_dir);
        free(listener->service_name);
        free(listener);
        errno = saved;
        return -1;
    }

    *out_listener = listener;
    return 0;
}

int netipc_uds_seqpacket_listener_accept(netipc_uds_seqpacket_listener_t *listener,
                                         netipc_uds_seqpacket_session_t **out_session,
                                         uint32_t timeout_ms) {
    if (!listener || listener->listen_fd < 0 || !out_session) {
        errno = EINVAL;
        return -1;
    }
    *out_session = NULL;

    bool has_deadline = timeout_ms > 0u;
    uint64_t deadline_ns = 0;
    if (has_deadline && monotonic_now_ns(&deadline_ns) != 0) {
        return -1;
    }
    if (has_deadline) {
        deadline_ns += (uint64_t)timeout_ms * 1000000ull;
    }

    if (wait_fd_event(listener->listen_fd, POLLIN, has_deadline, deadline_ns) != 0) {
        return -1;
    }

    int fd = accept(listener->listen_fd, NULL, NULL);
    if (fd < 0) {
        return -1;
    }

    if (set_nonblocking(fd) != 0 || set_cloexec(fd) != 0 || set_socket_options(fd) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct handshake_result negotiated = {
        .selected_profile = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };
    if (perform_server_handshake(listener, fd, timeout_ms, &negotiated) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    netipc_uds_seqpacket_session_t *session = calloc(1, sizeof(*session));
    if (!session) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    session->conn_fd = fd;
    session->run_dir = dup_nonempty(listener->run_dir);
    session->service_name = dup_nonempty(listener->service_name);
    if (!session->run_dir || !session->service_name) {
        int saved = errno;
        free(session->run_dir);
        free(session->service_name);
        close(fd);
        free(session);
        errno = saved;
        return -1;
    }

    session->negotiated_profile = negotiated.selected_profile;
    session->packet_size = negotiated.packet_size;
    session->max_request_message_len = negotiated.max_request_message_len;
    session->max_response_message_len = negotiated.max_response_message_len;
    session->shm_server = NULL;

    if (negotiated.selected_profile == NETIPC_PROFILE_SHM_HYBRID) {
        struct netipc_shm_config shm_config = {
            .run_dir = session->run_dir,
            .service_name = session->service_name,
            .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
            .file_mode = 0u,
            .max_request_message_bytes = (uint32_t)negotiated.max_request_message_len,
            .max_response_message_bytes = (uint32_t)negotiated.max_response_message_len,
        };

        if (netipc_shm_server_create(&shm_config, &session->shm_server) != 0) {
            int saved = errno;
            free(session->run_dir);
            free(session->service_name);
            close(fd);
            free(session);
            errno = saved;
            return -1;
        }
    }

    *out_session = session;
    return 0;
}

int netipc_uds_seqpacket_listener_fd(const netipc_uds_seqpacket_listener_t *listener) {
    if (!listener) {
        errno = EINVAL;
        return -1;
    }
    return listener->listen_fd;
}

void netipc_uds_seqpacket_listener_destroy(netipc_uds_seqpacket_listener_t *listener) {
    if (!listener) {
        return;
    }

    if (listener->listen_fd >= 0) {
        close(listener->listen_fd);
    }
    if (listener->path[0] != '\0') {
        unlink(listener->path);
    }
    free(listener->run_dir);
    free(listener->service_name);
    free(listener);
}

int netipc_uds_seqpacket_session_receive_frame(netipc_uds_seqpacket_session_t *session,
                                               uint8_t frame[NETIPC_FRAME_SIZE],
                                               uint32_t timeout_ms) {
    if (!session || session->conn_fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (session->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!session->shm_server) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_server_receive_frame(session->shm_server, frame, timeout_ms);
    }

    return recv_exact(session->conn_fd, frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_session_send_frame(netipc_uds_seqpacket_session_t *session,
                                            const uint8_t frame[NETIPC_FRAME_SIZE],
                                            uint32_t timeout_ms) {
    if (!session || session->conn_fd < 0 || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (session->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        (void)timeout_ms;
        if (!session->shm_server) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_server_send_frame(session->shm_server, frame);
    }

    return send_exact(session->conn_fd, frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_session_receive_message(netipc_uds_seqpacket_session_t *session,
                                                 uint8_t *message,
                                                 size_t message_capacity,
                                                 size_t *out_message_len,
                                                 uint32_t timeout_ms) {
    if (!session || session->conn_fd < 0 || !message || !out_message_len) {
        errno = EINVAL;
        return -1;
    }

    if (session->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!session->shm_server) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_server_receive_message(session->shm_server,
                                                 message,
                                                 message_capacity,
                                                 out_message_len,
                                                 timeout_ms);
    }

    if (session->max_request_message_len == 0u || message_capacity < session->max_request_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    size_t message_len = 0u;
    if (recv_transport_message(session->conn_fd,
                               message,
                               message_capacity,
                               &message_len,
                               session->max_request_message_len,
                               session->packet_size,
                               timeout_ms) != 0) {
        return -1;
    }

    *out_message_len = message_len;
    return 0;
}

int netipc_uds_seqpacket_session_send_message(netipc_uds_seqpacket_session_t *session,
                                              const uint8_t *message,
                                              size_t message_len,
                                              uint32_t timeout_ms) {
    if (!session || session->conn_fd < 0 || !message) {
        errno = EINVAL;
        return -1;
    }

    if (session->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!session->shm_server) {
            errno = EPROTO;
            return -1;
        }
        (void)timeout_ms;
        return netipc_shm_server_send_message(session->shm_server, message, message_len);
    }

    if (session->max_response_message_len == 0u) {
        errno = EPROTO;
        return -1;
    }
    if (validate_message_len_for_send(message, message_len, session->max_response_message_len) != 0) {
        return -1;
    }

    return send_transport_message(session->conn_fd,
                                  message,
                                  message_len,
                                  session->max_response_message_len,
                                  session->packet_size,
                                  timeout_ms);
}

int netipc_uds_seqpacket_session_receive_increment(netipc_uds_seqpacket_session_t *session,
                                                   uint64_t *request_id,
                                                   struct netipc_increment_request *request,
                                                   uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_uds_seqpacket_session_receive_frame(session, frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_request(frame, request_id, request);
}

int netipc_uds_seqpacket_session_send_increment(netipc_uds_seqpacket_session_t *session,
                                                uint64_t request_id,
                                                const struct netipc_increment_response *response,
                                                uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_encode_increment_response(frame, request_id, response) != 0) {
        return -1;
    }

    return netipc_uds_seqpacket_session_send_frame(session, frame, timeout_ms);
}

uint32_t netipc_uds_seqpacket_session_negotiated_profile(const netipc_uds_seqpacket_session_t *session) {
    if (!session) {
        return 0u;
    }

    return session->negotiated_profile;
}

int netipc_uds_seqpacket_session_fd(const netipc_uds_seqpacket_session_t *session) {
    if (!session) {
        errno = EINVAL;
        return -1;
    }
    return session->conn_fd;
}

void netipc_uds_seqpacket_session_destroy(netipc_uds_seqpacket_session_t *session) {
    if (!session) {
        return;
    }

    if (session->conn_fd >= 0) {
        close(session->conn_fd);
    }
    if (session->shm_server) {
        netipc_shm_server_destroy(session->shm_server);
        session->shm_server = NULL;
    }
    free(session->run_dir);
    free(session->service_name);
    free(session);
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
    server->listener = NULL;
    server->session = NULL;

    if (netipc_uds_seqpacket_listener_create(config, &server->listener) != 0) {
        int saved = errno;
        free(server);
        errno = saved;
        return -1;
    }

    *out_server = server;
    return 0;
}

int netipc_uds_seqpacket_server_accept(netipc_uds_seqpacket_server_t *server, uint32_t timeout_ms) {
    if (!server || !server->listener) {
        errno = EINVAL;
        return -1;
    }
    if (server->session) {
        errno = EISCONN;
        return -1;
    }

    return netipc_uds_seqpacket_listener_accept(server->listener, &server->session, timeout_ms);
}

int netipc_uds_seqpacket_server_receive_frame(netipc_uds_seqpacket_server_t *server,
                                              uint8_t frame[NETIPC_FRAME_SIZE],
                                              uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_receive_frame(server->session, frame, timeout_ms);
}

int netipc_uds_seqpacket_server_send_frame(netipc_uds_seqpacket_server_t *server,
                                           const uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_send_frame(server->session, frame, timeout_ms);
}

int netipc_uds_seqpacket_server_receive_message(netipc_uds_seqpacket_server_t *server,
                                                uint8_t *message,
                                                size_t message_capacity,
                                                size_t *out_message_len,
                                                uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_receive_message(server->session,
                                                        message,
                                                        message_capacity,
                                                        out_message_len,
                                                        timeout_ms);
}

int netipc_uds_seqpacket_server_send_message(netipc_uds_seqpacket_server_t *server,
                                             const uint8_t *message,
                                             size_t message_len,
                                             uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_send_message(server->session, message, message_len, timeout_ms);
}

int netipc_uds_seqpacket_server_receive_increment(netipc_uds_seqpacket_server_t *server,
                                                  uint64_t *request_id,
                                                  struct netipc_increment_request *request,
                                                  uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_receive_increment(server->session, request_id, request, timeout_ms);
}

int netipc_uds_seqpacket_server_send_increment(netipc_uds_seqpacket_server_t *server,
                                               uint64_t request_id,
                                               const struct netipc_increment_response *response,
                                               uint32_t timeout_ms) {
    if (!server || !server->session) {
        errno = EINVAL;
        return -1;
    }
    return netipc_uds_seqpacket_session_send_increment(server->session, request_id, response, timeout_ms);
}

uint32_t netipc_uds_seqpacket_server_negotiated_profile(const netipc_uds_seqpacket_server_t *server) {
    if (!server || !server->session) {
        return 0u;
    }

    return netipc_uds_seqpacket_session_negotiated_profile(server->session);
}

void netipc_uds_seqpacket_server_destroy(netipc_uds_seqpacket_server_t *server) {
    if (!server) {
        return;
    }

    if (server->session) {
        netipc_uds_seqpacket_session_destroy(server->session);
        server->session = NULL;
    }
    if (server->listener) {
        netipc_uds_seqpacket_listener_destroy(server->listener);
        server->listener = NULL;
    }
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
    client->max_request_payload_bytes = effective_payload_limit(config->max_request_payload_bytes);
    client->max_request_batch_items = effective_batch_limit(config->max_request_batch_items);
    client->max_response_payload_bytes = effective_payload_limit(config->max_response_payload_bytes);
    client->max_response_batch_items = effective_batch_limit(config->max_response_batch_items);
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

    struct handshake_result negotiated = {
        .selected_profile = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };
    if (perform_client_handshake(client, timeout_ms, &negotiated) != 0) {
        int saved = errno;
        close(client->fd);
        free(client->run_dir);
        free(client->service_name);
        free(client);
        errno = saved;
        return -1;
    }

    client->negotiated_profile = negotiated.selected_profile;
    client->packet_size = negotiated.packet_size;
    client->max_request_message_len = negotiated.max_request_message_len;
    client->max_response_message_len = negotiated.max_response_message_len;

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (create_shm_client_with_retry(client->run_dir,
                                         client->service_name,
                                         client->max_request_message_len,
                                         client->max_response_message_len,
                                         timeout_ms,
                                         &client->shm_client) != 0) {
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

    if (netipc_uds_seqpacket_client_send_frame(client, request_frame, timeout_ms) != 0) {
        return -1;
    }
    return netipc_uds_seqpacket_client_receive_frame(client, response_frame, timeout_ms);
}

int netipc_uds_seqpacket_client_receive_message(netipc_uds_seqpacket_client_t *client,
                                                uint8_t *response_message,
                                                size_t response_capacity,
                                                size_t *out_response_len,
                                                uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !response_message || !out_response_len) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!client->shm_client) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_client_receive_message(client->shm_client,
                                                 response_message,
                                                 response_capacity,
                                                 out_response_len,
                                                 timeout_ms);
    }

    if (client->max_response_message_len == 0u) {
        errno = EPROTO;
        return -1;
    }
    if (response_capacity < client->max_response_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    return recv_transport_message(client->fd,
                                  response_message,
                                  response_capacity,
                                  out_response_len,
                                  client->max_response_message_len,
                                  client->packet_size,
                                  timeout_ms);
}

int netipc_uds_seqpacket_client_send_message(netipc_uds_seqpacket_client_t *client,
                                             const uint8_t *request_message,
                                             size_t request_message_len,
                                             uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !request_message) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!client->shm_client) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_client_send_message(client->shm_client,
                                              request_message,
                                              request_message_len,
                                              timeout_ms);
    }

    if (client->max_request_message_len == 0u) {
        errno = EPROTO;
        return -1;
    }

    return send_transport_message(client->fd,
                                  request_message,
                                  request_message_len,
                                  client->max_request_message_len,
                                  client->packet_size,
                                  timeout_ms);
}

int netipc_uds_seqpacket_client_receive_frame(netipc_uds_seqpacket_client_t *client,
                                              uint8_t response_frame[NETIPC_FRAME_SIZE],
                                              uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !response_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!client->shm_client) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_client_receive_frame(client->shm_client, response_frame, timeout_ms);
    }

    return recv_exact(client->fd, response_frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_client_send_frame(netipc_uds_seqpacket_client_t *client,
                                           const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !request_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        if (!client->shm_client) {
            errno = EPROTO;
            return -1;
        }
        return netipc_shm_client_send_frame(client->shm_client, request_frame, timeout_ms);
    }

    return send_exact(client->fd, request_frame, NETIPC_FRAME_SIZE, timeout_ms);
}

int netipc_uds_seqpacket_client_receive_increment(netipc_uds_seqpacket_client_t *client,
                                                  uint64_t *request_id,
                                                  struct netipc_increment_response *response,
                                                  uint32_t timeout_ms) {
    uint8_t response_frame[NETIPC_FRAME_SIZE];

    if (!client || !request_id || !response) {
        errno = EINVAL;
        return -1;
    }

    if (netipc_uds_seqpacket_client_receive_frame(client, response_frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_response(response_frame, request_id, response);
}

int netipc_uds_seqpacket_client_send_increment(netipc_uds_seqpacket_client_t *client,
                                               uint64_t request_id,
                                               const struct netipc_increment_request *request,
                                               uint32_t timeout_ms) {
    uint8_t request_frame[NETIPC_FRAME_SIZE];

    if (!client || !request) {
        errno = EINVAL;
        return -1;
    }

    if (netipc_encode_increment_request(request_frame, request_id, request) != 0) {
        return -1;
    }

    return netipc_uds_seqpacket_client_send_frame(client, request_frame, timeout_ms);
}

int netipc_uds_seqpacket_client_call_message(netipc_uds_seqpacket_client_t *client,
                                             const uint8_t *request_message,
                                             size_t request_message_len,
                                             uint8_t *response_message,
                                             size_t response_capacity,
                                             size_t *out_response_len,
                                             uint32_t timeout_ms) {
    if (!client || client->fd < 0 || !request_message || !response_message || !out_response_len) {
        errno = EINVAL;
        return -1;
    }

    if (netipc_uds_seqpacket_client_send_message(client, request_message, request_message_len, timeout_ms) != 0) {
        return -1;
    }
    return netipc_uds_seqpacket_client_receive_message(client,
                                                       response_message,
                                                       response_capacity,
                                                       out_response_len,
                                                       timeout_ms);
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
