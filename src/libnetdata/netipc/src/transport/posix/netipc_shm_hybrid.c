#define _GNU_SOURCE

#include <netipc/netipc_shm_hybrid.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NETIPC_SHM_REGION_MAGIC 0x4e53484du
#define NETIPC_SHM_REGION_VERSION 1u

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct netipc_shm_region {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t owner_pid;
    uint32_t owner_generation;
    _Atomic uint64_t req_seq;
    _Atomic uint64_t resp_seq;
    sem_t req_sem;
    sem_t resp_sem;
    uint8_t request_frame[NETIPC_FRAME_SIZE];
    uint8_t response_frame[NETIPC_FRAME_SIZE];
};

struct netipc_shm_server {
    int fd;
    char path[PATH_MAX];
    uint32_t spin_tries;
    struct netipc_shm_region *region;
    uint64_t last_request_seq;
    uint64_t last_response_seq;
};

struct netipc_shm_client {
    int fd;
    char path[PATH_MAX];
    uint32_t spin_tries;
    struct netipc_shm_region *region;
    uint64_t next_request_seq;
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

static int compute_deadline(uint32_t timeout_ms, struct timespec *deadline) {
    if (!deadline) {
        errno = EINVAL;
        return -1;
    }

    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return -1;
    }

    uint64_t nsec = (uint64_t)deadline->tv_nsec + (uint64_t)timeout_ms * 1000000ull;
    deadline->tv_sec += (time_t)(nsec / 1000000000ull);
    deadline->tv_nsec = (long)(nsec % 1000000000ull);
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

static int wait_on_semaphore(sem_t *sem, uint32_t timeout_ms) {
    int rc;

    if (timeout_ms == 0u) {
        do {
            rc = sem_wait(sem);
        } while (rc != 0 && errno == EINTR);
        return rc;
    }

    struct timespec deadline;
    if (compute_deadline(timeout_ms, &deadline) != 0) {
        return -1;
    }

    do {
        rc = sem_timedwait(sem, &deadline);
    } while (rc != 0 && errno == EINTR);

    return rc;
}

static inline void spin_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static int wait_for_sequence(_Atomic uint64_t *seq,
                             uint64_t target,
                             sem_t *sem,
                             uint32_t spin_tries,
                             uint32_t timeout_ms) {
    if (!seq || !sem) {
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
            if (wait_on_semaphore(sem, 0u) != 0) {
                if (errno == EINVAL) {
                    struct timespec ts = {0, 50000}; /* semaphore optional peer: poll with tiny sleep */
                    nanosleep(&ts, NULL);
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

        if (wait_on_semaphore(sem, 1u) != 0) {
            if (errno == ETIMEDOUT || errno == EINVAL) {
                if (errno == EINVAL) {
                    struct timespec ts = {0, 50000}; /* semaphore optional peer: poll with tiny sleep */
                    nanosleep(&ts, NULL);
                }
                continue;
            }
            return -1;
        }
    }
}

static int map_region(int fd, struct netipc_shm_region **out_region) {
    if (fd < 0 || !out_region) {
        errno = EINVAL;
        return -1;
    }

    void *mem = mmap(NULL,
                     sizeof(struct netipc_shm_region),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fd,
                     0);
    if (mem == MAP_FAILED) {
        return -1;
    }

    *out_region = (struct netipc_shm_region *)mem;
    return 0;
}

static void unmap_region(struct netipc_shm_region *region) {
    if (region) {
        munmap(region, sizeof(struct netipc_shm_region));
    }
}

static int try_takeover_stale_endpoint(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    bool stale = false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(struct netipc_shm_region)) {
        stale = true;
    }

    struct netipc_shm_region *region = NULL;
    if (!stale && map_region(fd, &region) != 0) {
        stale = true;
    }

    if (!stale && region) {
        if (region->magic != NETIPC_SHM_REGION_MAGIC || region->version != NETIPC_SHM_REGION_VERSION) {
            stale = true;
        } else {
            int owner_state = endpoint_owned_by_live_server(fd, region->owner_pid);
            if (owner_state < 0) {
                int saved = errno;
                unmap_region(region);
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

    if (region) {
        unmap_region(region);
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

    if (build_endpoint_path(config, server->path) != 0) {
        free(server);
        return -1;
    }

    server->spin_tries = effective_spin_tries(config);

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

    if (ftruncate(server->fd, (off_t)sizeof(struct netipc_shm_region)) != 0) {
        int saved = errno;
        close(server->fd);
        unlink(server->path);
        free(server);
        errno = saved;
        return -1;
    }

    if (map_region(server->fd, &server->region) != 0) {
        int saved = errno;
        close(server->fd);
        unlink(server->path);
        free(server);
        errno = saved;
        return -1;
    }

    memset(server->region, 0, sizeof(*server->region));
    server->region->magic = NETIPC_SHM_REGION_MAGIC;
    server->region->version = NETIPC_SHM_REGION_VERSION;
    server->region->owner_pid = (int32_t)getpid();
    server->region->owner_generation = 1u;

    if (sem_init(&server->region->req_sem, 1, 0) != 0 || sem_init(&server->region->resp_sem, 1, 0) != 0) {
        int saved = errno;
        sem_destroy(&server->region->req_sem);
        unmap_region(server->region);
        close(server->fd);
        unlink(server->path);
        free(server);
        errno = saved;
        return -1;
    }

    *out_server = server;
    return 0;
}

int netipc_shm_server_receive_frame(netipc_shm_server_t *server,
                                    uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint32_t timeout_ms) {
    if (!server || !frame || !server->region) {
        errno = EINVAL;
        return -1;
    }

    uint64_t target = server->last_request_seq + 1u;
    if (wait_for_sequence(&server->region->req_seq,
                          target,
                          &server->region->req_sem,
                          server->spin_tries,
                          timeout_ms) != 0) {
        return -1;
    }

    memcpy(frame, server->region->request_frame, NETIPC_FRAME_SIZE);
    server->last_request_seq = atomic_load_explicit(&server->region->req_seq, memory_order_acquire);
    return 0;
}

int netipc_shm_server_send_frame(netipc_shm_server_t *server, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    if (!server || !frame || !server->region) {
        errno = EINVAL;
        return -1;
    }

    if (server->last_request_seq == 0u || server->last_request_seq == server->last_response_seq) {
        errno = EPROTO;
        return -1;
    }

    memcpy(server->region->response_frame, frame, NETIPC_FRAME_SIZE);
    atomic_store_explicit(&server->region->resp_seq, server->last_request_seq, memory_order_release);
    if (sem_post(&server->region->resp_sem) != 0) {
        return -1;
    }

    server->last_response_seq = server->last_request_seq;
    return 0;
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

    if (server->region) {
        server->region->owner_pid = 0;
        sem_destroy(&server->region->req_sem);
        sem_destroy(&server->region->resp_sem);
        unmap_region(server->region);
        server->region = NULL;
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

    if (build_endpoint_path(config, client->path) != 0) {
        free(client);
        return -1;
    }

    client->fd = open(client->path, O_RDWR | O_CLOEXEC);
    if (client->fd < 0) {
        free(client);
        return -1;
    }

    if (map_region(client->fd, &client->region) != 0) {
        int saved = errno;
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    if (client->region->magic != NETIPC_SHM_REGION_MAGIC ||
        client->region->version != NETIPC_SHM_REGION_VERSION) {
        unmap_region(client->region);
        close(client->fd);
        free(client);
        errno = EPROTO;
        return -1;
    }

    int owner_state = endpoint_owned_by_live_server(client->fd, client->region->owner_pid);
    if (owner_state < 0) {
        int saved = errno;
        unmap_region(client->region);
        close(client->fd);
        free(client);
        errno = saved;
        return -1;
    }

    if (owner_state == 0) {
        unmap_region(client->region);
        close(client->fd);
        free(client);
        errno = ECONNREFUSED;
        return -1;
    }

    *out_client = client;
    return 0;
}

int netipc_shm_client_call_frame(netipc_shm_client_t *client,
                                 const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                 uint8_t response_frame[NETIPC_FRAME_SIZE],
                                 uint32_t timeout_ms) {
    if (!client || !request_frame || !response_frame || !client->region) {
        errno = EINVAL;
        return -1;
    }

    uint64_t seq = client->next_request_seq + 1u;
    client->next_request_seq = seq;

    memcpy(client->region->request_frame, request_frame, NETIPC_FRAME_SIZE);
    atomic_store_explicit(&client->region->req_seq, seq, memory_order_release);

    if (sem_post(&client->region->req_sem) != 0) {
        return -1;
    }

    if (wait_for_sequence(&client->region->resp_seq,
                          seq,
                          &client->region->resp_sem,
                          client->spin_tries,
                          timeout_ms) != 0) {
        return -1;
    }

    memcpy(response_frame, client->region->response_frame, NETIPC_FRAME_SIZE);
    return 0;
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

    if (netipc_shm_client_call_frame(client, req_frame, resp_frame, timeout_ms) != 0) {
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

void netipc_shm_client_destroy(netipc_shm_client_t *client) {
    if (!client) {
        return;
    }

    if (client->region) {
        unmap_region(client->region);
        client->region = NULL;
    }

    if (client->fd >= 0) {
        close(client->fd);
    }

    free(client);
}
