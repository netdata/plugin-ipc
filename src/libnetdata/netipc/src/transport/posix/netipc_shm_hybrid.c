#define _GNU_SOURCE

#include <netipc/netipc_shm_hybrid.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#endif

#define NETIPC_SHM_REGION_MAGIC 0x4e53484du
#define NETIPC_SHM_REGION_VERSION 3u
#define NETIPC_SHM_REGION_ALIGNMENT 64u

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct netipc_shm_region_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    int32_t owner_pid;
    uint32_t owner_generation;
    uint32_t request_offset;
    uint32_t request_capacity;
    uint32_t response_offset;
    uint32_t response_capacity;
    _Atomic uint64_t req_seq;
    _Atomic uint64_t resp_seq;
    _Atomic uint32_t req_len;
    _Atomic uint32_t resp_len;
    _Atomic uint32_t req_signal;
    _Atomic uint32_t resp_signal;
};

_Static_assert(sizeof(struct netipc_shm_region_header) == 64u, "unexpected SHM region header size");

struct netipc_shm_server {
    int fd;
    char path[PATH_MAX];
    uint32_t spin_tries;
    void *mapping;
    size_t mapping_len;
    struct netipc_shm_region_header *header;
    uint64_t last_request_seq;
    uint64_t last_response_seq;
    size_t max_request_message_len;
    size_t max_response_message_len;
};

struct netipc_shm_client {
    int fd;
    char path[PATH_MAX];
    uint32_t spin_tries;
    void *mapping;
    size_t mapping_len;
    struct netipc_shm_region_header *header;
    uint64_t next_request_seq;
    uint64_t pending_request_seq;
    size_t max_request_message_len;
    size_t max_response_message_len;
};

static int lock_endpoint_fd(int fd) {
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    return fcntl(fd, F_SETLK, &lock);
}

static void unlock_endpoint_fd(int fd) {
    struct flock lock = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };

    (void)fcntl(fd, F_SETLK, &lock);
}

static int endpoint_owned_by_live_server(int fd, int32_t owner_pid) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (owner_pid == (int32_t)getpid()) {
        return 1;
    }

    if (lock_endpoint_fd(fd) == 0) {
        unlock_endpoint_fd(fd);
        return 0;
    }

    if (errno == EACCES || errno == EAGAIN) {
        return 1;
    }

    return -1;
}

static bool config_is_valid(const struct netipc_shm_config *config) {
    return config && config->run_dir && config->service_name && config->run_dir[0] != '\0' &&
           config->service_name[0] != '\0';
}

static uint32_t effective_spin_tries(const struct netipc_shm_config *config) {
    if (!config || config->spin_tries == 0u) {
        return NETIPC_SHM_DEFAULT_SPIN_TRIES;
    }
    return config->spin_tries;
}

static mode_t effective_mode(const struct netipc_shm_config *config) {
    if (!config || config->file_mode == 0u) {
        return (mode_t)0600;
    }
    return (mode_t)config->file_mode;
}

static size_t effective_request_message_len(const struct netipc_shm_config *config) {
    return (config && config->max_request_message_bytes != 0u) ? (size_t)config->max_request_message_bytes
                                                               : (size_t)NETIPC_FRAME_SIZE;
}

static size_t effective_response_message_len(const struct netipc_shm_config *config) {
    return (config && config->max_response_message_bytes != 0u) ? (size_t)config->max_response_message_bytes
                                                                : (size_t)NETIPC_FRAME_SIZE;
}

static int build_endpoint_path(const struct netipc_shm_config *config, char out_path[PATH_MAX]) {
    if (!config_is_valid(config) || !out_path) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(out_path, PATH_MAX, "%s/%s.ipcshm", config->run_dir, config->service_name);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
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

static inline void spin_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static int signal_wait(_Atomic uint32_t *signal, uint32_t expected, const struct timespec *timeout) {
#if defined(__linux__)
    int rc;

    do {
        rc = (int)syscall(SYS_futex, (uint32_t *)(void *)signal, FUTEX_WAIT, expected, timeout, NULL, 0);
    } while (rc != 0 && errno == EINTR);

    return rc;
#else
    (void)signal;
    (void)expected;
    if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (sleep_micros(50u) != 0) {
        return -1;
    }
    errno = EAGAIN;
    return -1;
#endif
}

static void signal_wake(_Atomic uint32_t *signal) {
    atomic_fetch_add_explicit(signal, 1u, memory_order_release);
#if defined(__linux__)
    (void)syscall(SYS_futex, (uint32_t *)(void *)signal, FUTEX_WAKE, 1, NULL, NULL, 0);
#endif
}

static int wait_for_sequence(_Atomic uint64_t *seq,
                             uint64_t target,
                             _Atomic uint32_t *signal,
                             uint32_t spin_tries,
                             uint32_t timeout_ms) {
    if (!seq || !signal) {
        errno = EINVAL;
        return -1;
    }

    uint64_t deadline_ns = 0;
    if (timeout_ms > 0u) {
        uint64_t now_ns = 0;
        if (monotonic_now_ns(&now_ns) != 0) {
            return -1;
        }
        deadline_ns = now_ns + (uint64_t)timeout_ms * 1000000ull;
    }

    for (;;) {
        uint64_t current = atomic_load_explicit(seq, memory_order_acquire);
        if (current >= target) {
            return 0;
        }

        for (uint32_t i = 0; i < spin_tries; ++i) {
            spin_pause();
            current = atomic_load_explicit(seq, memory_order_acquire);
            if (current >= target) {
                return 0;
            }
        }

        if (timeout_ms == 0u) {
            uint32_t expected = atomic_load_explicit(signal, memory_order_acquire);
            current = atomic_load_explicit(seq, memory_order_acquire);
            if (current >= target) {
                return 0;
            }
            if (signal_wait(signal, expected, NULL) != 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                return -1;
            }
            continue;
        }

        uint64_t now_ns = 0;
        if (monotonic_now_ns(&now_ns) != 0) {
            return -1;
        }
        if (now_ns >= deadline_ns) {
            errno = ETIMEDOUT;
            return -1;
        }

        uint64_t remain_ns = deadline_ns - now_ns;
        struct timespec wait_for = {
            .tv_sec = (time_t)(remain_ns / 1000000000ull),
            .tv_nsec = (long)(remain_ns % 1000000000ull),
        };
        uint32_t expected = atomic_load_explicit(signal, memory_order_acquire);
        current = atomic_load_explicit(seq, memory_order_acquire);
        if (current >= target) {
            return 0;
        }
        if (signal_wait(signal, expected, &wait_for) != 0) {
            if (errno == ETIMEDOUT || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
    }
}

static size_t align_up_size(size_t value, size_t alignment) {
    size_t remainder = value % alignment;
    return remainder == 0u ? value : value + (alignment - remainder);
}

static int compute_region_layout(size_t request_capacity,
                                 size_t response_capacity,
                                 uint32_t *out_request_offset,
                                 uint32_t *out_response_offset,
                                 size_t *out_mapping_len) {
    if (!out_request_offset || !out_response_offset || !out_mapping_len || request_capacity == 0u ||
        response_capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    size_t header_len = sizeof(struct netipc_shm_region_header);
    size_t request_offset = align_up_size(header_len, NETIPC_SHM_REGION_ALIGNMENT);
    size_t response_offset = align_up_size(request_offset + request_capacity, NETIPC_SHM_REGION_ALIGNMENT);
    size_t mapping_len = response_offset + response_capacity;

    if (request_offset > UINT32_MAX || response_offset > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    *out_request_offset = (uint32_t)request_offset;
    *out_response_offset = (uint32_t)response_offset;
    *out_mapping_len = mapping_len;
    return 0;
}

static uint8_t *request_area(const struct netipc_shm_region_header *header) {
    return ((uint8_t *)(void *)header) + header->request_offset;
}

static uint8_t *response_area(const struct netipc_shm_region_header *header) {
    return ((uint8_t *)(void *)header) + header->response_offset;
}

static int validate_region_header(const struct netipc_shm_region_header *header, size_t mapping_len) {
    if (!header) {
        errno = EINVAL;
        return -1;
    }

    if (header->magic != NETIPC_SHM_REGION_MAGIC || header->version != NETIPC_SHM_REGION_VERSION ||
        header->header_len != sizeof(struct netipc_shm_region_header) || header->request_capacity == 0u ||
        header->response_capacity == 0u) {
        errno = EPROTO;
        return -1;
    }

    if (header->request_offset < header->header_len) {
        errno = EPROTO;
        return -1;
    }

    if (header->response_offset < header->request_offset + header->request_capacity) {
        errno = EPROTO;
        return -1;
    }

    size_t required_len = (size_t)header->response_offset + (size_t)header->response_capacity;
    if (required_len > mapping_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int map_region(int fd,
                      size_t mapping_len,
                      void **out_mapping,
                      struct netipc_shm_region_header **out_header) {
    if (fd < 0 || mapping_len == 0u || !out_mapping || !out_header) {
        errno = EINVAL;
        return -1;
    }

    void *mem = mmap(NULL, mapping_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        return -1;
    }

    *out_mapping = mem;
    *out_header = (struct netipc_shm_region_header *)mem;
    return 0;
}

static void unmap_region(void *mapping, size_t mapping_len) {
    if (mapping && mapping_len != 0u) {
        munmap(mapping, mapping_len);
    }
}

static int validate_message_len_for_send(const uint8_t *message, size_t message_len, size_t max_message_len) {
    struct netipc_msg_header header;

    if (!message || message_len < NETIPC_MSG_HEADER_LEN || message_len > max_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    if (netipc_decode_msg_header(message, message_len, &header) != 0) {
        return -1;
    }

    if (netipc_msg_total_size(&header) != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int validate_received_message(const uint8_t *message, size_t message_len, size_t max_message_len) {
    struct netipc_msg_header header;

    if (!message || message_len < NETIPC_MSG_HEADER_LEN || message_len > max_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    if (netipc_decode_msg_header(message, message_len, &header) != 0) {
        return -1;
    }

    if (netipc_msg_total_size(&header) != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int try_takeover_stale_endpoint(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    bool stale = false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(struct netipc_shm_region_header)) {
        stale = true;
    }

    void *mapping = NULL;
    struct netipc_shm_region_header *header = NULL;
    if (!stale && map_region(fd, (size_t)st.st_size, &mapping, &header) != 0) {
        stale = true;
    }

    if (!stale && header) {
        if (validate_region_header(header, (size_t)st.st_size) != 0) {
            stale = true;
        } else {
            int owner_state = endpoint_owned_by_live_server(fd, header->owner_pid);
            if (owner_state < 0) {
                int saved = errno;
                unmap_region(mapping, sizeof(struct netipc_shm_region_header));
                close(fd);
                errno = saved;
                return -1;
            }

            if (owner_state == 0) {
                stale = true;
            } else {
                errno = EADDRINUSE;
            }
        }
    }

    if (mapping) {
        unmap_region(mapping, (size_t)st.st_size);
    }
    close(fd);

    if (!stale) {
        return 0;
    }

    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }

    return 1;
}

static int receive_request_bytes(netipc_shm_server_t *server,
                                 uint8_t *message,
                                 size_t message_capacity,
                                 size_t *out_message_len,
                                 uint32_t timeout_ms) {
    if (!server || !message || !out_message_len || !server->header) {
        errno = EINVAL;
        return -1;
    }

    uint64_t target = server->last_request_seq + 1u;
    if (wait_for_sequence(&server->header->req_seq,
                          target,
                          &server->header->req_signal,
                          server->spin_tries,
                          timeout_ms) != 0) {
        return -1;
    }

    uint64_t published_seq = atomic_load_explicit(&server->header->req_seq, memory_order_acquire);
    uint32_t published_len = atomic_load_explicit(&server->header->req_len, memory_order_acquire);
    if ((size_t)published_len > server->max_request_message_len ||
        published_len > server->header->request_capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (message_capacity < (size_t)published_len) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(message, request_area(server->header), (size_t)published_len);
    server->last_request_seq = published_seq;
    *out_message_len = (size_t)published_len;
    return 0;
}

static int send_response_bytes(netipc_shm_server_t *server, const uint8_t *message, size_t message_len) {
    if (!server || !message || !server->header) {
        errno = EINVAL;
        return -1;
    }

    if (server->last_request_seq == 0u || server->last_request_seq == server->last_response_seq) {
        errno = EPROTO;
        return -1;
    }

    if (message_len > server->max_response_message_len || message_len > server->header->response_capacity ||
        message_len > UINT32_MAX) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(response_area(server->header), message, message_len);
    atomic_store_explicit(&server->header->resp_len, (uint32_t)message_len, memory_order_relaxed);
    atomic_store_explicit(&server->header->resp_seq, server->last_request_seq, memory_order_release);
    signal_wake(&server->header->resp_signal);

    server->last_response_seq = server->last_request_seq;
    return 0;
}

static int client_send_request_bytes(netipc_shm_client_t *client,
                                     const uint8_t *request_message,
                                     size_t request_message_len) {
    if (!client || !request_message || !client->header) {
        errno = EINVAL;
        return -1;
    }

    if (client->pending_request_seq != 0u) {
        errno = EBUSY;
        return -1;
    }

    if (request_message_len > client->max_request_message_len ||
        request_message_len > client->header->request_capacity ||
        request_message_len > UINT32_MAX) {
        errno = EMSGSIZE;
        return -1;
    }

    uint64_t seq = client->next_request_seq + 1u;
    client->next_request_seq = seq;

    memcpy(request_area(client->header), request_message, request_message_len);
    atomic_store_explicit(&client->header->req_len, (uint32_t)request_message_len, memory_order_relaxed);
    atomic_store_explicit(&client->header->req_seq, seq, memory_order_release);
    signal_wake(&client->header->req_signal);
    client->pending_request_seq = seq;

    return 0;
}

static int client_receive_response_bytes(netipc_shm_client_t *client,
                                         uint8_t *response_message,
                                         size_t response_capacity,
                                         size_t *out_response_len,
                                         uint32_t timeout_ms) {
    if (!client || !response_message || !out_response_len || !client->header) {
        errno = EINVAL;
        return -1;
    }

    if (client->pending_request_seq == 0u) {
        errno = EPROTO;
        return -1;
    }

    if (wait_for_sequence(&client->header->resp_seq,
                          client->pending_request_seq,
                          &client->header->resp_signal,
                          client->spin_tries,
                          timeout_ms) != 0) {
        return -1;
    }

    uint32_t response_len = atomic_load_explicit(&client->header->resp_len, memory_order_acquire);
    if ((size_t)response_len > client->max_response_message_len ||
        response_len > client->header->response_capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (response_capacity < (size_t)response_len) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(response_message, response_area(client->header), (size_t)response_len);
    *out_response_len = (size_t)response_len;
    client->pending_request_seq = 0u;
    return 0;
}

int netipc_shm_server_create(const struct netipc_shm_config *config, netipc_shm_server_t **out_server) {
    if (!config || !out_server) {
        errno = EINVAL;
        return -1;
    }

    *out_server = NULL;

    netipc_shm_server_t *server = calloc(1, sizeof(*server));
    if (!server) {
        return -1;
    }

    server->fd = -1;
    server->spin_tries = effective_spin_tries(config);
    server->max_request_message_len = effective_request_message_len(config);
    server->max_response_message_len = effective_response_message_len(config);

    if (build_endpoint_path(config, server->path) != 0) {
        free(server);
        return -1;
    }

    uint32_t request_offset = 0u;
    uint32_t response_offset = 0u;
    if (compute_region_layout(server->max_request_message_len,
                              server->max_response_message_len,
                              &request_offset,
                              &response_offset,
                              &server->mapping_len) != 0) {
        free(server);
        return -1;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        server->fd = open(server->path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, effective_mode(config));
        if (server->fd >= 0) {
            if (lock_endpoint_fd(server->fd) != 0) {
                int saved = errno;
                close(server->fd);
                free(server);
                errno = saved;
                return -1;
            }
            break;
        }

        if (errno != EEXIST) {
            free(server);
            return -1;
        }

        int takeover = try_takeover_stale_endpoint(server->path);
        if (takeover < 0) {
            free(server);
            return -1;
        }
        if (takeover == 0) {
            free(server);
            return -1;
        }
    }

    if (server->fd < 0) {
        free(server);
        errno = EEXIST;
        return -1;
    }

    if (ftruncate(server->fd, (off_t)server->mapping_len) != 0) {
        int saved = errno;
        close(server->fd);
        unlink(server->path);
        free(server);
        errno = saved;
        return -1;
    }

    if (map_region(server->fd, server->mapping_len, &server->mapping, &server->header) != 0) {
        int saved = errno;
        close(server->fd);
        unlink(server->path);
        free(server);
        errno = saved;
        return -1;
    }

    memset(server->mapping, 0, server->mapping_len);
    server->header->magic = NETIPC_SHM_REGION_MAGIC;
    server->header->version = NETIPC_SHM_REGION_VERSION;
    server->header->header_len = (uint16_t)sizeof(struct netipc_shm_region_header);
    server->header->owner_pid = (int32_t)getpid();
    server->header->owner_generation = 1u;
    server->header->request_offset = request_offset;
    server->header->request_capacity = (uint32_t)server->max_request_message_len;
    server->header->response_offset = response_offset;
    server->header->response_capacity = (uint32_t)server->max_response_message_len;

    *out_server = server;
    return 0;
}

int netipc_shm_server_receive_message(netipc_shm_server_t *server,
                                      uint8_t *message,
                                      size_t message_capacity,
                                      size_t *out_message_len,
                                      uint32_t timeout_ms) {
    if (!server || !message || !out_message_len) {
        errno = EINVAL;
        return -1;
    }

    if (message_capacity < server->max_request_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    size_t message_len = 0u;
    if (receive_request_bytes(server, message, message_capacity, &message_len, timeout_ms) != 0) {
        return -1;
    }
    if (validate_received_message(message, message_len, server->max_request_message_len) != 0) {
        return -1;
    }

    *out_message_len = message_len;
    return 0;
}

int netipc_shm_server_send_message(netipc_shm_server_t *server, const uint8_t *message, size_t message_len) {
    if (!server || !message) {
        errno = EINVAL;
        return -1;
    }

    if (validate_message_len_for_send(message, message_len, server->max_response_message_len) != 0) {
        return -1;
    }

    return send_response_bytes(server, message, message_len);
}

int netipc_shm_server_receive_frame(netipc_shm_server_t *server,
                                    uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint32_t timeout_ms) {
    size_t frame_len = 0u;

    if (!server || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (receive_request_bytes(server, frame, NETIPC_FRAME_SIZE, &frame_len, timeout_ms) != 0) {
        return -1;
    }
    if (frame_len != NETIPC_FRAME_SIZE) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int netipc_shm_server_send_frame(netipc_shm_server_t *server, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    if (!server || !frame) {
        errno = EINVAL;
        return -1;
    }

    if (server->max_response_message_len < NETIPC_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    return send_response_bytes(server, frame, NETIPC_FRAME_SIZE);
}

int netipc_shm_server_receive_increment(netipc_shm_server_t *server,
                                        uint64_t *request_id,
                                        struct netipc_increment_request *request,
                                        uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_shm_server_receive_frame(server, frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_request(frame, request_id, request);
}

int netipc_shm_server_send_increment(netipc_shm_server_t *server,
                                     uint64_t request_id,
                                     const struct netipc_increment_response *response) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_encode_increment_response(frame, request_id, response) != 0) {
        return -1;
    }

    return netipc_shm_server_send_frame(server, frame);
}

void netipc_shm_server_destroy(netipc_shm_server_t *server) {
    if (!server) {
        return;
    }

    if (server->header) {
        server->header->owner_pid = 0;
    }

    if (server->mapping) {
        unmap_region(server->mapping, server->mapping_len);
        server->mapping = NULL;
        server->header = NULL;
    }

    if (server->fd >= 0) {
        close(server->fd);
    }

    if (server->path[0] != '\0') {
        unlink(server->path);
    }

    free(server);
}

int netipc_shm_client_create(const struct netipc_shm_config *config, netipc_shm_client_t **out_client) {
    if (!config || !out_client) {
        errno = EINVAL;
        return -1;
    }

    *out_client = NULL;

    netipc_shm_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return -1;
    }

    client->fd = -1;
    client->spin_tries = effective_spin_tries(config);
    client->max_request_message_len = effective_request_message_len(config);
    client->max_response_message_len = effective_response_message_len(config);

    if (build_endpoint_path(config, client->path) != 0) {
        free(client);
        return -1;
    }

    client->fd = open(client->path, O_RDWR | O_CLOEXEC);
    if (client->fd < 0) {
        free(client);
        return -1;
    }

    struct stat st;
    if (fstat(client->fd, &st) != 0) {
        int saved = errno;
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    /* A freshly created endpoint can be visible before the server finishes
     * sizing and populating the shared region header. Treat that as
     * protocol-not-ready so the caller can retry instead of leaking a stale
     * errno from earlier socket activity.
     */
    if (st.st_size < (off_t)sizeof(struct netipc_shm_region_header)) {
        int saved = EPROTO;
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }
    client->mapping_len = (size_t)st.st_size;

    if (map_region(client->fd, client->mapping_len, &client->mapping, &client->header) != 0) {
        int saved = errno;
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    if (validate_region_header(client->header, client->mapping_len) != 0) {
        int saved = errno;
        unmap_region(client->mapping, client->mapping_len);
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    if (client->header->request_capacity < client->max_request_message_len ||
        client->header->response_capacity < client->max_response_message_len) {
        int saved = EMSGSIZE;
        unmap_region(client->mapping, client->mapping_len);
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    int owner_state = endpoint_owned_by_live_server(client->fd, client->header->owner_pid);
    if (owner_state < 0) {
        int saved = errno;
        unmap_region(client->mapping, client->mapping_len);
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }
    if (owner_state == 0) {
        unmap_region(client->mapping, client->mapping_len);
        close(client->fd);
        free(client);
        errno = ECONNREFUSED;
        return -1;
    }

    *out_client = client;
    return 0;
}

int netipc_shm_client_call_message(netipc_shm_client_t *client,
                                   const uint8_t *request_message,
                                   size_t request_message_len,
                                   uint8_t *response_message,
                                   size_t response_capacity,
                                   size_t *out_response_len,
                                   uint32_t timeout_ms) {
    if (!client || !request_message || !response_message || !out_response_len) {
        errno = EINVAL;
        return -1;
    }

    if (response_capacity < client->max_response_message_len) {
        errno = EMSGSIZE;
        return -1;
    }
    if (validate_message_len_for_send(request_message, request_message_len, client->max_request_message_len) != 0) {
        return -1;
    }
    if (netipc_shm_client_send_message(client, request_message, request_message_len, timeout_ms) != 0) {
        return -1;
    }
    if (netipc_shm_client_receive_message(client,
                                          response_message,
                                          response_capacity,
                                          out_response_len,
                                          timeout_ms) != 0) {
        return -1;
    }
    if (validate_received_message(response_message, *out_response_len, client->max_response_message_len) != 0) {
        return -1;
    }

    return 0;
}

int netipc_shm_client_call_frame(netipc_shm_client_t *client,
                                 const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                 uint8_t response_frame[NETIPC_FRAME_SIZE],
                                 uint32_t timeout_ms) {
    if (!client || !request_frame || !response_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->max_request_message_len < NETIPC_FRAME_SIZE ||
        client->max_response_message_len < NETIPC_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    if (netipc_shm_client_send_frame(client, request_frame, timeout_ms) != 0) {
        return -1;
    }
    return netipc_shm_client_receive_frame(client, response_frame, timeout_ms);
}

int netipc_shm_client_call_increment(netipc_shm_client_t *client,
                                     const struct netipc_increment_request *request,
                                     struct netipc_increment_response *response,
                                     uint32_t timeout_ms) {
    if (!client || !request || !response) {
        errno = EINVAL;
        return -1;
    }

    uint8_t req_frame[NETIPC_FRAME_SIZE];
    uint8_t resp_frame[NETIPC_FRAME_SIZE];
    uint64_t request_id = client->next_request_seq + 1u;

    if (netipc_encode_increment_request(req_frame, request_id, request) != 0) {
        return -1;
    }

    if (netipc_shm_client_send_frame(client, req_frame, timeout_ms) != 0) {
        return -1;
    }

    if (netipc_shm_client_receive_frame(client, resp_frame, timeout_ms) != 0) {
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

int netipc_shm_client_receive_message(netipc_shm_client_t *client,
                                      uint8_t *response_message,
                                      size_t response_capacity,
                                      size_t *out_response_len,
                                      uint32_t timeout_ms) {
    if (!client || !response_message || !out_response_len) {
        errno = EINVAL;
        return -1;
    }

    if (response_capacity < client->max_response_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    if (client_receive_response_bytes(client,
                                      response_message,
                                      response_capacity,
                                      out_response_len,
                                      timeout_ms) != 0) {
        return -1;
    }

    return validate_received_message(response_message, *out_response_len, client->max_response_message_len);
}

int netipc_shm_client_send_message(netipc_shm_client_t *client,
                                   const uint8_t *request_message,
                                   size_t request_message_len,
                                   uint32_t timeout_ms) {
    (void)timeout_ms;

    if (!client || !request_message) {
        errno = EINVAL;
        return -1;
    }

    if (validate_message_len_for_send(request_message, request_message_len, client->max_request_message_len) != 0) {
        return -1;
    }

    return client_send_request_bytes(client, request_message, request_message_len);
}

int netipc_shm_client_receive_frame(netipc_shm_client_t *client,
                                    uint8_t response_frame[NETIPC_FRAME_SIZE],
                                    uint32_t timeout_ms) {
    size_t response_len = 0u;

    if (!client || !response_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->max_response_message_len < NETIPC_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    if (client_receive_response_bytes(client, response_frame, NETIPC_FRAME_SIZE, &response_len, timeout_ms) != 0) {
        return -1;
    }
    if (response_len != NETIPC_FRAME_SIZE) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int netipc_shm_client_send_frame(netipc_shm_client_t *client,
                                 const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                 uint32_t timeout_ms) {
    (void)timeout_ms;

    if (!client || !request_frame) {
        errno = EINVAL;
        return -1;
    }

    if (client->max_request_message_len < NETIPC_FRAME_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    return client_send_request_bytes(client, request_frame, NETIPC_FRAME_SIZE);
}

int netipc_shm_client_receive_increment(netipc_shm_client_t *client,
                                        uint64_t *request_id,
                                        struct netipc_increment_response *response,
                                        uint32_t timeout_ms) {
    uint8_t response_frame[NETIPC_FRAME_SIZE];

    if (!client || !request_id || !response) {
        errno = EINVAL;
        return -1;
    }

    if (netipc_shm_client_receive_frame(client, response_frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_response(response_frame, request_id, response);
}

int netipc_shm_client_send_increment(netipc_shm_client_t *client,
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

    return netipc_shm_client_send_frame(client, request_frame, timeout_ms);
}

void netipc_shm_client_destroy(netipc_shm_client_t *client) {
    if (!client) {
        return;
    }

    if (client->mapping) {
        unmap_region(client->mapping, client->mapping_len);
        client->mapping = NULL;
        client->header = NULL;
    }

    if (client->fd >= 0) {
        close(client->fd);
    }

    free(client);
}
