#include <netipc/netipc_named_pipe.h>
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

#define NETIPC_IMPLEMENTED_PROFILES (NETIPC_PROFILE_NAMED_PIPE | NETIPC_PROFILE_SHM_HYBRID)
#define NETIPC_PIPE_NAME_CAPACITY 256u
#define NETIPC_SERVICE_NAME_CAPACITY 96u

struct netipc_negotiation_message {
    uint16_t type;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t intersection_profiles;
    uint32_t selected_profile;
    uint64_t auth_token;
    uint32_t status;
};

struct netipc_named_pipe_server {
    HANDLE pipe;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
    uint32_t shm_spin_tries;
    uint32_t negotiated_profile;
    netipc_win_shm_server_t *shm_server;
    bool connected;
};

struct netipc_named_pipe_client {
    HANDLE pipe;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
    uint32_t shm_spin_tries;
    uint32_t negotiated_profile;
    netipc_win_shm_client_t *shm_client;
    uint64_t next_request_id;
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

static int set_errno_from_win32(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        case ERROR_BAD_PIPE:
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
        case ERROR_PIPE_NOT_CONNECTED:
            errno = EPIPE;
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

static bool config_is_valid(const struct netipc_named_pipe_config *config) {
    return config && config->run_dir && config->run_dir[0] != '\0' && config->service_name &&
           config->service_name[0] != '\0';
}

static char *dup_string(const char *s) {
    size_t len = 0u;
    char *copy = NULL;

    if (!s) {
        errno = EINVAL;
        return NULL;
    }

    len = strlen(s) + 1u;
    copy = (char *)malloc(len);
    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, s, len);
    return copy;
}

static uint32_t effective_supported_profiles(const struct netipc_named_pipe_config *config) {
    uint32_t supported = NETIPC_NAMED_PIPE_DEFAULT_PROFILES;
    if (config && config->supported_profiles != 0u) {
        supported = config->supported_profiles;
    }

    supported &= NETIPC_IMPLEMENTED_PROFILES;
    if (supported == 0u) {
        supported = NETIPC_NAMED_PIPE_DEFAULT_PROFILES;
    }
    return supported;
}

static uint32_t effective_preferred_profiles(const struct netipc_named_pipe_config *config,
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

static uint64_t fnv1a64(const char *s) {
    uint64_t hash = 14695981039346656037ull;
    if (!s) {
        return hash;
    }

    for (; *s != '\0'; ++s) {
        hash ^= (uint8_t)(*s);
        hash *= 1099511628211ull;
    }

    return hash;
}

static void sanitize_service_name(const char *service_name, char out[NETIPC_SERVICE_NAME_CAPACITY]) {
    size_t j = 0u;
    if (!out) {
        return;
    }

    if (!service_name) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0u; service_name[i] != '\0' && j + 1u < NETIPC_SERVICE_NAME_CAPACITY; ++i) {
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

static int build_pipe_name(const struct netipc_named_pipe_config *config,
                           wchar_t out_name[NETIPC_PIPE_NAME_CAPACITY]) {
    if (!config_is_valid(config) || !out_name) {
        errno = EINVAL;
        return -1;
    }

    char sanitized_service[NETIPC_SERVICE_NAME_CAPACITY];
    sanitize_service_name(config->service_name, sanitized_service);

    char pipe_name[NETIPC_PIPE_NAME_CAPACITY];
    int written = snprintf(pipe_name,
                           sizeof(pipe_name),
                           "\\\\.\\pipe\\netipc-%016llx-%s",
                           (unsigned long long)fnv1a64(config->run_dir),
                           sanitized_service);
    if (written < 0 || (size_t)written >= sizeof(pipe_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (int i = 0; i <= written; ++i) {
        out_name[i] = (wchar_t)(unsigned char)pipe_name[i];
    }
    return 0;
}

static ULONGLONG now_ms(void) { return GetTickCount64(); }

static bool deadline_expired(ULONGLONG deadline_ms) { return deadline_ms != 0ull && now_ms() >= deadline_ms; }

static void sleep_millis(DWORD sleep_ms) { Sleep(sleep_ms); }

static int set_pipe_wait_mode(HANDLE pipe, DWORD wait_mode) {
    DWORD mode = PIPE_READMODE_MESSAGE | wait_mode;
    if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
        return set_errno_from_win32(GetLastError());
    }
    return 0;
}

static int read_pipe_message(HANDLE pipe, uint8_t frame[NETIPC_FRAME_SIZE], uint32_t timeout_ms) {
    if (timeout_ms != 0u) {
        ULONGLONG deadline_ms = now_ms() + (ULONGLONG)timeout_ms;
        for (;;) {
            DWORD bytes_available = 0u;
            if (!PeekNamedPipe(pipe, NULL, 0u, NULL, &bytes_available, NULL)) {
                return set_errno_from_win32(GetLastError());
            }

            if (bytes_available != 0u) {
                break;
            }
            if (deadline_expired(deadline_ms)) {
                errno = ETIMEDOUT;
                return -1;
            }
            sleep_millis(1u);
        }
    }

    DWORD bytes_read = 0u;
    if (!ReadFile(pipe, frame, NETIPC_FRAME_SIZE, &bytes_read, NULL)) {
        return set_errno_from_win32(GetLastError());
    }

    if (bytes_read != NETIPC_FRAME_SIZE) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int write_pipe_message(HANDLE pipe, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    DWORD bytes_written = 0u;
    if (!WriteFile(pipe, frame, NETIPC_FRAME_SIZE, &bytes_written, NULL)) {
        return set_errno_from_win32(GetLastError());
    }

    if (bytes_written != NETIPC_FRAME_SIZE) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static void disconnect_pipe(HANDLE pipe, bool connected) {
    if (pipe == INVALID_HANDLE_VALUE || !connected) {
        return;
    }

    /*
     * Flush the final server message before disconnecting so one-shot clients
     * do not observe a broken pipe instead of the queued response.
     */
    if (!FlushFileBuffers(pipe)) {
        DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA &&
            error != ERROR_PIPE_NOT_CONNECTED) {
            (void)error;
        }
    }

    DisconnectNamedPipe(pipe);
}

static void encode_negotiation(uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE],
                               const struct netipc_negotiation_message *message) {
    memset(frame, 0, NETIPC_NEGOTIATION_FRAME_SIZE);

    write_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC, NETIPC_NEGOTIATION_MAGIC);
    write_u16_le(frame + NETIPC_NEG_OFFSET_VERSION, NETIPC_NEGOTIATION_VERSION);
    write_u16_le(frame + NETIPC_NEG_OFFSET_TYPE, message->type);
    write_u32_le(frame + NETIPC_NEG_OFFSET_SUPPORTED, message->supported_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_PREFERRED, message->preferred_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_INTERSECTION, message->intersection_profiles);
    write_u32_le(frame + NETIPC_NEG_OFFSET_SELECTED, message->selected_profile);
    write_u64_le(frame + NETIPC_NEG_OFFSET_AUTH_TOKEN, message->auth_token);
    write_u32_le(frame + NETIPC_NEG_OFFSET_STATUS, message->status);
}

static int decode_negotiation(const uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE],
                              uint16_t expected_type,
                              struct netipc_negotiation_message *out_message) {
    if (!frame || !out_message) {
        errno = EINVAL;
        return -1;
    }

    uint32_t magic = read_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC);
    uint16_t version = read_u16_le(frame + NETIPC_NEG_OFFSET_VERSION);
    uint16_t type = read_u16_le(frame + NETIPC_NEG_OFFSET_TYPE);

    if (magic != NETIPC_NEGOTIATION_MAGIC || version != NETIPC_NEGOTIATION_VERSION ||
        type != expected_type) {
        errno = EPROTO;
        return -1;
    }

    out_message->type = type;
    out_message->supported_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_SUPPORTED);
    out_message->preferred_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_PREFERRED);
    out_message->intersection_profiles = read_u32_le(frame + NETIPC_NEG_OFFSET_INTERSECTION);
    out_message->selected_profile = read_u32_le(frame + NETIPC_NEG_OFFSET_SELECTED);
    out_message->auth_token = read_u64_le(frame + NETIPC_NEG_OFFSET_AUTH_TOKEN);
    out_message->status = read_u32_le(frame + NETIPC_NEG_OFFSET_STATUS);
    return 0;
}

static uint32_t select_profile(uint32_t candidates) {
    if ((candidates & NETIPC_PROFILE_SHM_HYBRID) != 0u) {
        return NETIPC_PROFILE_SHM_HYBRID;
    }
    if ((candidates & NETIPC_PROFILE_NAMED_PIPE) != 0u) {
        return NETIPC_PROFILE_NAMED_PIPE;
    }
    return 0u;
}

static int perform_server_handshake(HANDLE pipe,
                                    uint32_t supported_profiles,
                                    uint32_t preferred_profiles,
                                    uint64_t auth_token,
                                    uint32_t timeout_ms,
                                    uint32_t *out_profile) {
    if (!out_profile) {
        errno = EINVAL;
        return -1;
    }

    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    struct netipc_negotiation_message hello;

    if (read_pipe_message(pipe, frame, timeout_ms) != 0) {
        return -1;
    }
    if (decode_negotiation(frame, NETIPC_NEGOTIATION_HELLO, &hello) != 0) {
        return -1;
    }

    struct netipc_negotiation_message ack = {
        .type = NETIPC_NEGOTIATION_ACK,
        .supported_profiles = supported_profiles,
        .preferred_profiles = preferred_profiles,
        .intersection_profiles = hello.supported_profiles & supported_profiles,
        .selected_profile = 0u,
        .auth_token = 0u,
        .status = NETIPC_NEGOTIATION_STATUS_OK,
    };

    if (auth_token != 0u && hello.auth_token != auth_token) {
        ack.status = ERROR_ACCESS_DENIED;
    } else {
        uint32_t candidates = ack.intersection_profiles & preferred_profiles;
        if (candidates == 0u) {
            candidates = ack.intersection_profiles;
        }
        ack.selected_profile = select_profile(candidates);
        if (ack.selected_profile == 0u) {
            ack.status = ERROR_NOT_SUPPORTED;
        }
    }

    encode_negotiation(frame, &ack);
    if (write_pipe_message(pipe, frame) != 0) {
        return -1;
    }

    if (ack.status != NETIPC_NEGOTIATION_STATUS_OK) {
        return set_errno_from_win32(ack.status);
    }

    *out_profile = ack.selected_profile;
    return 0;
}

static int perform_client_handshake(HANDLE pipe,
                                    uint32_t supported_profiles,
                                    uint32_t preferred_profiles,
                                    uint64_t auth_token,
                                    uint32_t timeout_ms,
                                    uint32_t *out_profile) {
    if (!out_profile) {
        errno = EINVAL;
        return -1;
    }

    struct netipc_negotiation_message hello = {
        .type = NETIPC_NEGOTIATION_HELLO,
        .supported_profiles = supported_profiles,
        .preferred_profiles = preferred_profiles,
        .intersection_profiles = 0u,
        .selected_profile = 0u,
        .auth_token = auth_token,
        .status = NETIPC_NEGOTIATION_STATUS_OK,
    };
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation(frame, &hello);

    if (write_pipe_message(pipe, frame) != 0) {
        return -1;
    }
    if (read_pipe_message(pipe, frame, timeout_ms) != 0) {
        return -1;
    }

    struct netipc_negotiation_message ack;
    if (decode_negotiation(frame, NETIPC_NEGOTIATION_ACK, &ack) != 0) {
        return -1;
    }
    if (ack.status != NETIPC_NEGOTIATION_STATUS_OK) {
        return set_errno_from_win32(ack.status);
    }
    if ((ack.intersection_profiles & supported_profiles) == 0u ||
        ack.selected_profile == 0u ||
        (ack.selected_profile & ack.intersection_profiles) == 0u ||
        (ack.selected_profile & supported_profiles) == 0u) {
        errno = EPROTO;
        return -1;
    }

    *out_profile = ack.selected_profile;
    return 0;
}

int netipc_named_pipe_server_create(const struct netipc_named_pipe_config *config,
                                    netipc_named_pipe_server_t **out_server) {
    if (!config || !out_server) {
        errno = EINVAL;
        return -1;
    }

    *out_server = NULL;

    netipc_named_pipe_server_t *server = calloc(1u, sizeof(*server));
    if (!server) {
        errno = ENOMEM;
        return -1;
    }

    server->pipe = INVALID_HANDLE_VALUE;
    server->run_dir = dup_string(config->run_dir);
    server->service_name = dup_string(config->service_name);
    if (!server->run_dir || !server->service_name) {
        free(server->service_name);
        free(server->run_dir);
        free(server);
        return -1;
    }

    wchar_t pipe_name[NETIPC_PIPE_NAME_CAPACITY];
    if (build_pipe_name(config, pipe_name) != 0) {
        free(server->service_name);
        free(server->run_dir);
        free(server);
        return -1;
    }

    server->supported_profiles = effective_supported_profiles(config);
    server->preferred_profiles = effective_preferred_profiles(config, server->supported_profiles);
    server->auth_token = config->auth_token;
    server->shm_spin_tries = config->shm_spin_tries;
    server->pipe = CreateNamedPipeW(pipe_name,
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                    1u,
                                    NETIPC_FRAME_SIZE * 4u,
                                    NETIPC_FRAME_SIZE * 4u,
                                    0u,
                                    NULL);
    if (server->pipe == INVALID_HANDLE_VALUE) {
        free(server->service_name);
        free(server->run_dir);
        free(server);
        return set_errno_from_win32(GetLastError());
    }

    *out_server = server;
    return 0;
}

int netipc_named_pipe_server_accept(netipc_named_pipe_server_t *server, uint32_t timeout_ms) {
    if (!server || server->pipe == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }

    if (set_pipe_wait_mode(server->pipe, PIPE_NOWAIT) != 0) {
        return -1;
    }

    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    for (;;) {
        BOOL connected = ConnectNamedPipe(server->pipe, NULL);
        if (connected) {
            break;
        }

        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            break;
        }
        if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) {
            set_pipe_wait_mode(server->pipe, PIPE_WAIT);
            return set_errno_from_win32(error);
        }
        if (deadline_expired(deadline_ms)) {
            set_pipe_wait_mode(server->pipe, PIPE_WAIT);
            errno = ETIMEDOUT;
            return -1;
        }
        sleep_millis(1u);
    }

    if (set_pipe_wait_mode(server->pipe, PIPE_WAIT) != 0) {
        return -1;
    }

    if (perform_server_handshake(server->pipe,
                                 server->supported_profiles,
                                 server->preferred_profiles,
                                 server->auth_token,
                                 timeout_ms,
                                 &server->negotiated_profile) != 0) {
        DisconnectNamedPipe(server->pipe);
        server->connected = false;
        return -1;
    }

    if (server->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        struct netipc_named_pipe_config config = {
            .run_dir = server->run_dir,
            .service_name = server->service_name,
            .supported_profiles = server->supported_profiles,
            .preferred_profiles = server->preferred_profiles,
            .auth_token = server->auth_token,
            .shm_spin_tries = server->shm_spin_tries,
        };
        if (netipc_win_shm_server_create(&config, &server->shm_server) != 0) {
            DisconnectNamedPipe(server->pipe);
            server->connected = false;
            server->negotiated_profile = 0u;
            return -1;
        }
    }

    server->connected = true;
    return 0;
}

int netipc_named_pipe_server_receive_frame(netipc_named_pipe_server_t *server,
                                           uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!server || !frame || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (server->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        return netipc_win_shm_server_receive_frame(server->shm_server, frame, timeout_ms);
    }

    return read_pipe_message(server->pipe, frame, timeout_ms);
}

int netipc_named_pipe_server_send_frame(netipc_named_pipe_server_t *server,
                                        const uint8_t frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms) {
    if (!server || !frame || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (server->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        return netipc_win_shm_server_send_frame(server->shm_server, frame, timeout_ms);
    }

    (void)timeout_ms;
    return write_pipe_message(server->pipe, frame);
}

int netipc_named_pipe_server_receive_increment(netipc_named_pipe_server_t *server,
                                               uint64_t *request_id,
                                               struct netipc_increment_request *request,
                                               uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_named_pipe_server_receive_frame(server, frame, timeout_ms) != 0) {
        return -1;
    }

    return netipc_decode_increment_request(frame, request_id, request);
}

int netipc_named_pipe_server_send_increment(netipc_named_pipe_server_t *server,
                                            uint64_t request_id,
                                            const struct netipc_increment_response *response,
                                            uint32_t timeout_ms) {
    uint8_t frame[NETIPC_FRAME_SIZE];
    if (netipc_encode_increment_response(frame, request_id, response) != 0) {
        return -1;
    }

    return netipc_named_pipe_server_send_frame(server, frame, timeout_ms);
}

uint32_t netipc_named_pipe_server_negotiated_profile(const netipc_named_pipe_server_t *server) {
    return server ? server->negotiated_profile : 0u;
}

void netipc_named_pipe_server_destroy(netipc_named_pipe_server_t *server) {
    if (!server) {
        return;
    }

    if (server->shm_server) {
        netipc_win_shm_server_destroy(server->shm_server);
    }
    if (server->pipe != INVALID_HANDLE_VALUE) {
        disconnect_pipe(server->pipe, server->connected);
        CloseHandle(server->pipe);
    }

    free(server->service_name);
    free(server->run_dir);
    free(server);
}

int netipc_named_pipe_client_create(const struct netipc_named_pipe_config *config,
                                    netipc_named_pipe_client_t **out_client,
                                    uint32_t timeout_ms) {
    if (!config || !out_client) {
        errno = EINVAL;
        return -1;
    }

    *out_client = NULL;

    netipc_named_pipe_client_t *client = calloc(1u, sizeof(*client));
    if (!client) {
        errno = ENOMEM;
        return -1;
    }

    client->pipe = INVALID_HANDLE_VALUE;
    client->run_dir = dup_string(config->run_dir);
    client->service_name = dup_string(config->service_name);
    if (!client->run_dir || !client->service_name) {
        free(client->service_name);
        free(client->run_dir);
        free(client);
        return -1;
    }

    wchar_t pipe_name[NETIPC_PIPE_NAME_CAPACITY];
    if (build_pipe_name(config, pipe_name) != 0) {
        free(client->service_name);
        free(client->run_dir);
        free(client);
        return -1;
    }

    client->supported_profiles = effective_supported_profiles(config);
    client->preferred_profiles = effective_preferred_profiles(config, client->supported_profiles);
    client->auth_token = config->auth_token;
    client->shm_spin_tries = config->shm_spin_tries;
    client->next_request_id = 1u;

    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    for (;;) {
        client->pipe = CreateFileW(pipe_name,
                                   GENERIC_READ | GENERIC_WRITE,
                                   0u,
                                   NULL,
                                   OPEN_EXISTING,
                                   0u,
                                   NULL);
        if (client->pipe != INVALID_HANDLE_VALUE) {
            break;
        }

        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            free(client->service_name);
            free(client->run_dir);
            free(client);
            return set_errno_from_win32(error);
        }
        if (deadline_expired(deadline_ms)) {
            free(client->service_name);
            free(client->run_dir);
            free(client);
            errno = ETIMEDOUT;
            return -1;
        }
        if (error == ERROR_PIPE_BUSY) {
            DWORD wait_ms = timeout_ms == 0u ? NMPWAIT_WAIT_FOREVER : 50u;
            WaitNamedPipeW(pipe_name, wait_ms);
        } else {
            sleep_millis(1u);
        }
    }

    if (set_pipe_wait_mode(client->pipe, PIPE_WAIT) != 0) {
        CloseHandle(client->pipe);
        free(client->service_name);
        free(client->run_dir);
        free(client);
        return -1;
    }

    if (perform_client_handshake(client->pipe,
                                 client->supported_profiles,
                                 client->preferred_profiles,
                                 client->auth_token,
                                 timeout_ms,
                                 &client->negotiated_profile) != 0) {
        CloseHandle(client->pipe);
        free(client->service_name);
        free(client->run_dir);
        free(client);
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        struct netipc_named_pipe_config shm_config = {
            .run_dir = client->run_dir,
            .service_name = client->service_name,
            .supported_profiles = client->supported_profiles,
            .preferred_profiles = client->preferred_profiles,
            .auth_token = client->auth_token,
            .shm_spin_tries = client->shm_spin_tries,
        };
        if (netipc_win_shm_client_create(&shm_config, &client->shm_client, timeout_ms) != 0) {
            CloseHandle(client->pipe);
            free(client->service_name);
            free(client->run_dir);
            free(client);
            return -1;
        }
    }

    *out_client = client;
    return 0;
}

int netipc_named_pipe_client_call_frame(netipc_named_pipe_client_t *client,
                                        const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                        uint8_t response_frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms) {
    if (!client || !request_frame || !response_frame || client->pipe == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }

    if (client->negotiated_profile == NETIPC_PROFILE_SHM_HYBRID) {
        return netipc_win_shm_client_call_frame(client->shm_client,
                                                request_frame,
                                                response_frame,
                                                timeout_ms);
    }

    if (write_pipe_message(client->pipe, request_frame) != 0) {
        return -1;
    }

    return read_pipe_message(client->pipe, response_frame, timeout_ms);
}

int netipc_named_pipe_client_call_increment(netipc_named_pipe_client_t *client,
                                            const struct netipc_increment_request *request,
                                            struct netipc_increment_response *response,
                                            uint32_t timeout_ms) {
    if (!client || !request || !response) {
        errno = EINVAL;
        return -1;
    }

    uint8_t request_frame[NETIPC_FRAME_SIZE];
    uint8_t response_frame[NETIPC_FRAME_SIZE];
    uint64_t request_id = client->next_request_id++;

    if (netipc_encode_increment_request(request_frame, request_id, request) != 0) {
        return -1;
    }
    if (netipc_named_pipe_client_call_frame(client, request_frame, response_frame, timeout_ms) != 0) {
        return -1;
    }

    uint64_t response_request_id = 0u;
    if (netipc_decode_increment_response(response_frame, &response_request_id, response) != 0) {
        return -1;
    }
    if (response_request_id != request_id) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

uint32_t netipc_named_pipe_client_negotiated_profile(const netipc_named_pipe_client_t *client) {
    return client ? client->negotiated_profile : 0u;
}

void netipc_named_pipe_client_destroy(netipc_named_pipe_client_t *client) {
    if (!client) {
        return;
    }

    if (client->shm_client) {
        netipc_win_shm_client_destroy(client->shm_client);
    }
    if (client->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(client->pipe);
    }

    free(client->service_name);
    free(client->run_dir);
    free(client);
}
