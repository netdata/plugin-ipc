#include "netipc_shm_hybrid_win.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define NETIPC_WIN_SHM_REGION_MAGIC 0x4e535748u
#define NETIPC_WIN_SHM_REGION_VERSION 2u
#define NETIPC_WIN_SHM_NAME_CAPACITY 256u
#define NETIPC_WIN_SHM_SERVICE_CAPACITY 96u
#define NETIPC_BUSYWAIT_DEADLINE_POLL_MASK 1023u
#define NETIPC_CACHELINE_SIZE 64u

struct netipc_win_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t profile;
    uint32_t spin_tries;
    uint8_t reserved[NETIPC_CACHELINE_SIZE - 16u];
};

struct netipc_win_shm_request_slot {
    volatile LONG64 seq;
    volatile LONG client_closed;
    volatile LONG server_waiting;
    uint8_t reserved[NETIPC_CACHELINE_SIZE - 16u];
    uint8_t frame[NETIPC_FRAME_SIZE];
};

struct netipc_win_shm_response_slot {
    volatile LONG64 seq;
    volatile LONG server_closed;
    volatile LONG client_waiting;
    uint8_t reserved[NETIPC_CACHELINE_SIZE - 16u];
    uint8_t frame[NETIPC_FRAME_SIZE];
};

struct netipc_win_shm_region {
    struct netipc_win_shm_header header;
    struct netipc_win_shm_request_slot request;
    struct netipc_win_shm_response_slot response;
};

struct netipc_win_shm_server {
    HANDLE mapping;
    HANDLE request_event;
    HANDLE response_event;
    struct netipc_win_shm_region *region;
    LONG64 last_request_seq;
    LONG64 active_request_seq;
    uint32_t profile;
    uint32_t spin_tries;
};

struct netipc_win_shm_client {
    HANDLE mapping;
    HANDLE request_event;
    HANDLE response_event;
    struct netipc_win_shm_region *region;
    LONG64 next_request_seq;
    uint32_t profile;
    uint32_t spin_tries;
};

static int set_errno_from_win32(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_FILE_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_INVALID_HANDLE:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER:
            errno = EINVAL;
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            errno = ENOMEM;
            break;
        case ERROR_NOT_SUPPORTED:
            errno = ENOTSUP;
            break;
        case ERROR_PIPE_BUSY:
        case ERROR_SEM_TIMEOUT:
        case WAIT_TIMEOUT:
            errno = ETIMEDOUT;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}

static bool profile_is_valid(uint32_t profile) {
    return profile == NETIPC_PROFILE_SHM_HYBRID ||
           profile == NETIPC_PROFILE_SHM_BUSYWAIT ||
           profile == NETIPC_PROFILE_SHM_WAITADDR;
}

static bool profile_uses_events(uint32_t profile) {
    return profile == NETIPC_PROFILE_SHM_HYBRID;
}

static bool profile_uses_waitaddr(uint32_t profile) {
    (void)profile;
    return false; /* WaitOnAddress is intra-process only; disabled for cross-process IPC */
}

static uint64_t fnv1a64_update(uint64_t hash, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes) {
        return hash;
    }

    for (size_t i = 0u; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static uint64_t endpoint_hash(const struct netipc_named_pipe_config *config) {
    uint64_t hash = 14695981039346656037ull;

    if (!config) {
        return hash;
    }

    hash = fnv1a64_update(hash, config->run_dir, strlen(config->run_dir));
    hash = fnv1a64_update(hash, "\n", 1u);
    hash = fnv1a64_update(hash, config->service_name, strlen(config->service_name));
    hash = fnv1a64_update(hash, "\n", 1u);
    hash = fnv1a64_update(hash, &config->auth_token, sizeof(config->auth_token));
    return hash;
}

static void sanitize_service_name(const char *service_name, char out[NETIPC_WIN_SHM_SERVICE_CAPACITY]) {
    size_t j = 0u;
    if (!out) {
        return;
    }

    if (!service_name) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0u; service_name[i] != '\0' && j + 1u < NETIPC_WIN_SHM_SERVICE_CAPACITY; ++i) {
        unsigned char ch = (unsigned char)service_name[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.') {
            out[j++] = (char)ch;
        } else {
            out[j++] = '_';
        }
    }

    if (j == 0u) {
        memcpy(out, "service", 8u);
        return;
    }

    out[j] = '\0';
}

static int build_kernel_object_name(const struct netipc_named_pipe_config *config,
                                    uint32_t profile,
                                    const char *suffix,
                                    wchar_t out_name[NETIPC_WIN_SHM_NAME_CAPACITY]) {
    if (!config || !config->run_dir || config->run_dir[0] == '\0' ||
        !config->service_name || config->service_name[0] == '\0' ||
        !profile_is_valid(profile) || !suffix || !out_name) {
        errno = EINVAL;
        return -1;
    }

    char sanitized_service[NETIPC_WIN_SHM_SERVICE_CAPACITY];
    sanitize_service_name(config->service_name, sanitized_service);

    char object_name[NETIPC_WIN_SHM_NAME_CAPACITY];
    int written = snprintf(object_name,
                           sizeof(object_name),
                           "Local\\netipc-%016llx-%s-p%u-%s",
                           (unsigned long long)endpoint_hash(config),
                           sanitized_service,
                           profile,
                           suffix);
    if (written < 0 || (size_t)written >= sizeof(object_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (int i = 0; i <= written; ++i) {
        out_name[i] = (wchar_t)(unsigned char)object_name[i];
    }
    return 0;
}

static uint32_t effective_spin_tries(const struct netipc_named_pipe_config *config) {
    if (config && config->shm_spin_tries != 0u) {
        return config->shm_spin_tries;
    }
    return NETIPC_SHM_HYBRID_DEFAULT_SPIN_TRIES;
}

#if defined(__GNUC__) || defined(__clang__)
static LONG64 load_seq_acquire(volatile LONG64 *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static LONG load_flag_acquire(volatile LONG *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_seq_release(volatile LONG64 *value, LONG64 next_value) {
    __atomic_store_n(value, next_value, __ATOMIC_RELEASE);
}

static void store_flag_release(volatile LONG *value, LONG next_value) {
    __atomic_store_n(value, next_value, __ATOMIC_RELEASE);
}
#else
static LONG64 load_seq_acquire(volatile LONG64 *value) {
    LONG64 current = *value;
    MemoryBarrier();
    return current;
}

static LONG load_flag_acquire(volatile LONG *value) {
    LONG current = *value;
    MemoryBarrier();
    return current;
}

static void store_seq_release(volatile LONG64 *value, LONG64 next_value) {
    MemoryBarrier();
    *value = next_value;
}

static void store_flag_release(volatile LONG *value, LONG next_value) {
    MemoryBarrier();
    *value = next_value;
}
#endif

/* Full memory fence (SeqCst / MFENCE on x86) to prevent store-load
 * reordering.  Needed between a store of a WAITING flag and the
 * subsequent load of a SEQ counter (and vice-versa) to avoid the
 * classic Dekker / Peterson race on x86 where Release/Acquire
 * ordering alone cannot prevent a store from being reordered past
 * a subsequent load to a different address. */
static void seq_cst_fence(void) {
#if defined(__GNUC__) || defined(__clang__)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#else
    MemoryBarrier();
#endif
}

static ULONGLONG now_ms(void) {
    return GetTickCount64();
}

static DWORD wait_timeout_ms(ULONGLONG deadline_ms) {
    if (deadline_ms == 0ull) {
        return INFINITE;
    }

    ULONGLONG now = now_ms();
    if (now >= deadline_ms) {
        return 0u;
    }

    ULONGLONG remaining = deadline_ms - now;
    if (remaining > (ULONGLONG)MAXDWORD) {
        return MAXDWORD;
    }
    return (DWORD)remaining;
}

static bool region_is_ready(const struct netipc_win_shm_region *region, uint32_t profile) {
    return region &&
           region->header.magic == NETIPC_WIN_SHM_REGION_MAGIC &&
           region->header.version == NETIPC_WIN_SHM_REGION_VERSION &&
           region->header.profile == profile;
}

static int wait_for_region_ready(const struct netipc_win_shm_region *region,
                                 uint32_t profile,
                                 uint32_t timeout_ms) {
    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;

    for (;;) {
        if (region_is_ready(region, profile)) {
            return 0;
        }

        if (deadline_ms != 0ull && now_ms() >= deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }

        Sleep(1u);
    }
}

static int wait_for_request(volatile LONG64 *request_seq,
                            LONG64 last_seen,
                            volatile LONG *client_closed,
                            volatile LONG *server_waiting,
                            HANDLE request_event,
                            bool use_events,
                            bool use_waitaddr,
                            uint32_t spin_tries,
                            uint32_t timeout_ms,
                            LONG64 *out_request_seq) {
    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    uint32_t spins = spin_tries;
    uint32_t deadline_poll_counter = 0u;

    for (;;) {
        LONG64 current = load_seq_acquire(request_seq);
        if (current != last_seen) {
            *out_request_seq = current;
            return 0;
        }

        if (load_flag_acquire(client_closed) != 0) {
            errno = EPIPE;
            return -1;
        }

        YieldProcessor();

        if (use_waitaddr) {
            if (spins != 0u) {
                --spins;
                continue;
            }

            store_flag_release(server_waiting, 1);
            seq_cst_fence();

            current = load_seq_acquire(request_seq);
            if (current != last_seen) {
                store_flag_release(server_waiting, 0);
                *out_request_seq = current;
                return 0;
            }

            DWORD wait_ms = wait_timeout_ms(deadline_ms);
            if (wait_ms == 0u && deadline_ms != 0ull) {
                store_flag_release(server_waiting, 0);
                errno = ETIMEDOUT;
                return -1;
            }

            LONG64 compare = last_seen;
            WaitOnAddress((volatile void *)request_seq, &compare, sizeof(LONG64), wait_ms);
            store_flag_release(server_waiting, 0);
            spins = spin_tries;
            continue;
        }

        if (!use_events) {
            if (deadline_ms != 0ull &&
                ((++deadline_poll_counter & NETIPC_BUSYWAIT_DEADLINE_POLL_MASK) == 0u) &&
                now_ms() >= deadline_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            continue;
        }

        if (spins != 0u) {
            --spins;
            continue;
        }

        /* Mark that we are about to block on the kernel event so the
         * client can skip the expensive SetEvent call when it is not needed. */
        store_flag_release(server_waiting, 1);
        seq_cst_fence();

        /* Re-check after setting the flag to avoid lost wakeups. */
        current = load_seq_acquire(request_seq);
        if (current != last_seen) {
            store_flag_release(server_waiting, 0);
            *out_request_seq = current;
            return 0;
        }

        DWORD wait_ms = wait_timeout_ms(deadline_ms);
        if (wait_ms == 0u && deadline_ms != 0ull) {
            store_flag_release(server_waiting, 0);
            errno = ETIMEDOUT;
            return -1;
        }

        DWORD rc = WaitForSingleObject(request_event, wait_ms);
        store_flag_release(server_waiting, 0);
        if (rc == WAIT_OBJECT_0) {
            spins = spin_tries;
            continue;
        }
        if (rc == WAIT_TIMEOUT) {
            errno = ETIMEDOUT;
            return -1;
        }

        return set_errno_from_win32(GetLastError());
    }
}

static int wait_for_response(volatile LONG64 *response_seq,
                             LONG64 desired_seq,
                             volatile LONG *server_closed,
                             volatile LONG *client_waiting,
                             HANDLE response_event,
                             bool use_events,
                             bool use_waitaddr,
                             uint32_t spin_tries,
                             uint32_t timeout_ms) {
    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    uint32_t spins = spin_tries;
    uint32_t deadline_poll_counter = 0u;

    for (;;) {
        LONG64 current = load_seq_acquire(response_seq);
        if (current >= desired_seq) {
            return 0;
        }

        if (load_flag_acquire(server_closed) != 0) {
            errno = EPIPE;
            return -1;
        }

        YieldProcessor();

        if (use_waitaddr) {
            if (spins != 0u) {
                --spins;
                continue;
            }

            store_flag_release(client_waiting, 1);
            seq_cst_fence();

            current = load_seq_acquire(response_seq);
            if (current >= desired_seq) {
                store_flag_release(client_waiting, 0);
                return 0;
            }

            DWORD wait_ms = wait_timeout_ms(deadline_ms);
            if (wait_ms == 0u && deadline_ms != 0ull) {
                store_flag_release(client_waiting, 0);
                errno = ETIMEDOUT;
                return -1;
            }

            LONG64 compare = desired_seq - 1;
            WaitOnAddress((volatile void *)response_seq, &compare, sizeof(LONG64), wait_ms);
            store_flag_release(client_waiting, 0);
            spins = spin_tries;
            continue;
        }

        if (!use_events) {
            if (deadline_ms != 0ull &&
                ((++deadline_poll_counter & NETIPC_BUSYWAIT_DEADLINE_POLL_MASK) == 0u) &&
                now_ms() >= deadline_ms) {
                errno = ETIMEDOUT;
                return -1;
            }
            continue;
        }

        if (spins != 0u) {
            --spins;
            continue;
        }

        /* Mark that we are about to block on the kernel event so the
         * server can skip the expensive SetEvent call when it is not needed. */
        store_flag_release(client_waiting, 1);
        seq_cst_fence();

        /* Re-check after setting the flag to avoid lost wakeups. */
        current = load_seq_acquire(response_seq);
        if (current >= desired_seq) {
            store_flag_release(client_waiting, 0);
            return 0;
        }

        DWORD wait_ms = wait_timeout_ms(deadline_ms);
        if (wait_ms == 0u && deadline_ms != 0ull) {
            store_flag_release(client_waiting, 0);
            errno = ETIMEDOUT;
            return -1;
        }

        DWORD rc = WaitForSingleObject(response_event, wait_ms);
        store_flag_release(client_waiting, 0);
        if (rc == WAIT_OBJECT_0) {
            spins = spin_tries;
            continue;
        }
        if (rc == WAIT_TIMEOUT) {
            errno = ETIMEDOUT;
            return -1;
        }

        return set_errno_from_win32(GetLastError());
    }
}

static void close_handle_if_valid(HANDLE *handle) {
    if (!handle || *handle == NULL || *handle == INVALID_HANDLE_VALUE) {
        return;
    }

    CloseHandle(*handle);
    *handle = INVALID_HANDLE_VALUE;
}

static int create_auto_reset_event(const wchar_t *name, HANDLE *out_handle) {
    HANDLE handle = CreateEventW(NULL, FALSE, FALSE, name);
    if (!handle) {
        return set_errno_from_win32(GetLastError());
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        errno = EEXIST;
        return -1;
    }

    *out_handle = handle;
    return 0;
}

int netipc_win_shm_server_create(const struct netipc_named_pipe_config *config,
                                 uint32_t profile,
                                 netipc_win_shm_server_t **out_server) {
    if (!config || !out_server || !profile_is_valid(profile)) {
        errno = EINVAL;
        return -1;
    }

    *out_server = NULL;

    netipc_win_shm_server_t *server = calloc(1u, sizeof(*server));
    if (!server) {
        errno = ENOMEM;
        return -1;
    }

    server->mapping = INVALID_HANDLE_VALUE;
    server->request_event = INVALID_HANDLE_VALUE;
    server->response_event = INVALID_HANDLE_VALUE;
    server->profile = profile;
    server->spin_tries = effective_spin_tries(config);

    wchar_t mapping_name[NETIPC_WIN_SHM_NAME_CAPACITY];
    wchar_t request_event_name[NETIPC_WIN_SHM_NAME_CAPACITY];
    wchar_t response_event_name[NETIPC_WIN_SHM_NAME_CAPACITY];

    if (build_kernel_object_name(config, profile, "shm", mapping_name) != 0) {
        free(server);
        return -1;
    }
    if (profile_uses_events(profile) &&
        (build_kernel_object_name(config, profile, "req", request_event_name) != 0 ||
         build_kernel_object_name(config, profile, "resp", response_event_name) != 0)) {
        free(server);
        return -1;
    }

    server->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                         NULL,
                                         PAGE_READWRITE,
                                         0u,
                                         (DWORD)sizeof(*server->region),
                                         mapping_name);
    if (!server->mapping) {
        free(server);
        return set_errno_from_win32(GetLastError());
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        close_handle_if_valid(&server->mapping);
        free(server);
        errno = EEXIST;
        return -1;
    }

    server->region = (struct netipc_win_shm_region *)MapViewOfFile(server->mapping,
                                                                   FILE_MAP_ALL_ACCESS,
                                                                   0u,
                                                                   0u,
                                                                   sizeof(*server->region));
    if (!server->region) {
        close_handle_if_valid(&server->mapping);
        free(server);
        return set_errno_from_win32(GetLastError());
    }

    memset(server->region, 0, sizeof(*server->region));
    server->region->header.magic = NETIPC_WIN_SHM_REGION_MAGIC;
    server->region->header.version = NETIPC_WIN_SHM_REGION_VERSION;
    server->region->header.profile = profile;
    server->region->header.spin_tries = server->spin_tries;

    if (profile_uses_events(profile) &&
        (create_auto_reset_event(request_event_name, &server->request_event) != 0 ||
         create_auto_reset_event(response_event_name, &server->response_event) != 0)) {
        if (server->region) {
            UnmapViewOfFile(server->region);
        }
        close_handle_if_valid(&server->response_event);
        close_handle_if_valid(&server->request_event);
        close_handle_if_valid(&server->mapping);
        free(server);
        return -1;
    }

    *out_server = server;
    return 0;
}

int netipc_win_shm_server_receive_frame(netipc_win_shm_server_t *server,
                                        uint8_t frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms) {
    if (!server || !frame || !server->region) {
        errno = EINVAL;
        return -1;
    }

    LONG64 request_seq = 0;
    if (wait_for_request(&server->region->request.seq,
                         server->last_request_seq,
                         &server->region->request.client_closed,
                         &server->region->request.server_waiting,
                         server->request_event,
                         profile_uses_events(server->profile),
                         profile_uses_waitaddr(server->profile),
                         server->spin_tries,
                         timeout_ms,
                         &request_seq) != 0) {
        return -1;
    }

    memcpy(frame, server->region->request.frame, NETIPC_FRAME_SIZE);
    server->active_request_seq = request_seq;
    server->last_request_seq = request_seq;
    return 0;
}

int netipc_win_shm_server_send_frame(netipc_win_shm_server_t *server,
                                     const uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint32_t timeout_ms) {
    (void)timeout_ms;

    if (!server || !frame || !server->region || server->active_request_seq == 0) {
        errno = EINVAL;
        return -1;
    }

    memcpy(server->region->response.frame, frame, NETIPC_FRAME_SIZE);
    store_seq_release(&server->region->response.seq, server->active_request_seq);
    seq_cst_fence();

    if (profile_uses_waitaddr(server->profile)) {
        if (load_flag_acquire(&server->region->response.client_waiting)) {
            WakeByAddressSingle((void *)&server->region->response.seq);
        }
    } else if (profile_uses_events(server->profile)) {
        /* Only signal the event if the client is actually blocked waiting on it.
         * This avoids the expensive SetEvent kernel call (~4μs under Hyper-V)
         * when the client is still in its spin loop and will see the seq change. */
        if (load_flag_acquire(&server->region->response.client_waiting)) {
            if (!SetEvent(server->response_event)) {
                return set_errno_from_win32(GetLastError());
            }
        }
    }

    server->active_request_seq = 0;
    return 0;
}

void netipc_win_shm_server_destroy(netipc_win_shm_server_t *server) {
    if (!server) {
        return;
    }

    if (server->region) {
        store_flag_release(&server->region->response.server_closed, 1);
    }
    if (profile_uses_events(server->profile) &&
        server->request_event != INVALID_HANDLE_VALUE) {
        SetEvent(server->request_event);
    }
    if (profile_uses_events(server->profile) &&
        server->response_event != INVALID_HANDLE_VALUE) {
        SetEvent(server->response_event);
    }
    if (server->region) {
        UnmapViewOfFile(server->region);
    }

    close_handle_if_valid(&server->response_event);
    close_handle_if_valid(&server->request_event);
    close_handle_if_valid(&server->mapping);
    free(server);
}

int netipc_win_shm_client_create(const struct netipc_named_pipe_config *config,
                                 uint32_t profile,
                                 netipc_win_shm_client_t **out_client,
                                 uint32_t timeout_ms) {
    if (!config || !out_client || !profile_is_valid(profile)) {
        errno = EINVAL;
        return -1;
    }

    *out_client = NULL;

    netipc_win_shm_client_t *client = calloc(1u, sizeof(*client));
    if (!client) {
        errno = ENOMEM;
        return -1;
    }

    client->mapping = INVALID_HANDLE_VALUE;
    client->request_event = INVALID_HANDLE_VALUE;
    client->response_event = INVALID_HANDLE_VALUE;
    client->profile = profile;
    client->spin_tries = effective_spin_tries(config);

    wchar_t mapping_name[NETIPC_WIN_SHM_NAME_CAPACITY];
    wchar_t request_event_name[NETIPC_WIN_SHM_NAME_CAPACITY];
    wchar_t response_event_name[NETIPC_WIN_SHM_NAME_CAPACITY];

    if (build_kernel_object_name(config, profile, "shm", mapping_name) != 0) {
        free(client);
        return -1;
    }
    if (profile_uses_events(profile) &&
        (build_kernel_object_name(config, profile, "req", request_event_name) != 0 ||
         build_kernel_object_name(config, profile, "resp", response_event_name) != 0)) {
        free(client);
        return -1;
    }

    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    DWORD last_error = ERROR_FILE_NOT_FOUND;
    for (;;) {
        client->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
        if (client->mapping != NULL) {
            if (!profile_uses_events(profile)) {
                break;
            }

            client->request_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE,
                                               FALSE,
                                               request_event_name);
            if (client->request_event == NULL) {
                last_error = GetLastError();
            } else {
                client->response_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                    FALSE,
                                                    response_event_name);
                if (client->response_event == NULL) {
                    last_error = GetLastError();
                }
            }
            if (client->request_event != NULL && client->response_event != NULL) {
                break;
            }

            close_handle_if_valid(&client->response_event);
            close_handle_if_valid(&client->request_event);
            close_handle_if_valid(&client->mapping);
        } else {
            last_error = GetLastError();
        }

        if (deadline_ms != 0ull && now_ms() >= deadline_ms) {
            free(client);
            errno = (last_error == ERROR_FILE_NOT_FOUND) ? ETIMEDOUT : ENOENT;
            return -1;
        }
        Sleep(1u);
    }

    client->region = (struct netipc_win_shm_region *)MapViewOfFile(client->mapping,
                                                                   FILE_MAP_ALL_ACCESS,
                                                                   0u,
                                                                   0u,
                                                                   sizeof(*client->region));
    if (!client->region) {
        close_handle_if_valid(&client->response_event);
        close_handle_if_valid(&client->request_event);
        close_handle_if_valid(&client->mapping);
        free(client);
        return set_errno_from_win32(GetLastError());
    }

    if (wait_for_region_ready(client->region, profile, timeout_ms) != 0) {
        UnmapViewOfFile(client->region);
        close_handle_if_valid(&client->response_event);
        close_handle_if_valid(&client->request_event);
        close_handle_if_valid(&client->mapping);
        free(client);
        return -1;
    }

    if (client->region->header.spin_tries != 0u) {
        client->spin_tries = client->region->header.spin_tries;
    }

    client->next_request_seq = load_seq_acquire(&client->region->request.seq);

    *out_client = client;
    return 0;
}

int netipc_win_shm_client_call_frame(netipc_win_shm_client_t *client,
                                     const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                     uint8_t response_frame[NETIPC_FRAME_SIZE],
                                     uint32_t timeout_ms) {
    if (!client || !request_frame || !response_frame || !client->region) {
        errno = EINVAL;
        return -1;
    }

    LONG64 request_seq = client->next_request_seq + 1;
    client->next_request_seq = request_seq;

    memcpy(client->region->request.frame, request_frame, NETIPC_FRAME_SIZE);
    store_seq_release(&client->region->request.seq, request_seq);
    seq_cst_fence();

    if (profile_uses_waitaddr(client->profile)) {
        if (load_flag_acquire(&client->region->request.server_waiting)) {
            WakeByAddressSingle((void *)&client->region->request.seq);
        }
    } else if (profile_uses_events(client->profile)) {
        /* Only signal the event if the server is actually blocked waiting on it. */
        if (load_flag_acquire(&client->region->request.server_waiting)) {
            if (!SetEvent(client->request_event)) {
                return set_errno_from_win32(GetLastError());
            }
        }
    }

    if (wait_for_response(&client->region->response.seq,
                          request_seq,
                          &client->region->response.server_closed,
                          &client->region->response.client_waiting,
                          client->response_event,
                          profile_uses_events(client->profile),
                          profile_uses_waitaddr(client->profile),
                          client->spin_tries,
                          timeout_ms) != 0) {
        return -1;
    }

    memcpy(response_frame, client->region->response.frame, NETIPC_FRAME_SIZE);
    return 0;
}

void netipc_win_shm_client_destroy(netipc_win_shm_client_t *client) {
    if (!client) {
        return;
    }

    if (client->region) {
        store_flag_release(&client->region->request.client_closed, 1);
    }
    if (profile_uses_events(client->profile) &&
        client->request_event != INVALID_HANDLE_VALUE) {
        SetEvent(client->request_event);
    }
    if (profile_uses_events(client->profile) &&
        client->response_event != INVALID_HANDLE_VALUE) {
        SetEvent(client->response_event);
    }
    if (client->region) {
        UnmapViewOfFile(client->region);
    }

    close_handle_if_valid(&client->response_event);
    close_handle_if_valid(&client->request_event);
    close_handle_if_valid(&client->mapping);
    free(client);
}
