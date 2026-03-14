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
#define NETIPC_NEGOTIATION_PAYLOAD_OFFSET 8u
#define NETIPC_NEGOTIATION_STATUS_OFFSET 48u

#define NETIPC_NEGOTIATION_HELLO 1u
#define NETIPC_NEGOTIATION_ACK 2u

#define NETIPC_NEGOTIATION_STATUS_OK 0u
#define NETIPC_NEGOTIATION_DEFAULT_BATCH_ITEMS 1u

#define NETIPC_NEG_OFFSET_MAGIC 0u
#define NETIPC_NEG_OFFSET_VERSION 4u
#define NETIPC_NEG_OFFSET_TYPE 6u

#define NETIPC_IMPLEMENTED_PROFILES \
    (NETIPC_PROFILE_NAMED_PIPE | NETIPC_PROFILE_SHM_HYBRID | NETIPC_PROFILE_SHM_BUSYWAIT | NETIPC_PROFILE_SHM_WAITADDR)
#define NETIPC_PIPE_NAME_CAPACITY 256u
#define NETIPC_SERVICE_NAME_CAPACITY 96u

struct netipc_named_pipe_server {
    HANDLE pipe;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
    uint32_t shm_spin_tries;
    uint32_t negotiated_profile;
    uint32_t packet_size;
    size_t max_request_message_len;
    size_t max_response_message_len;
    netipc_win_shm_server_t *shm_server;
    bool connected;
};

struct netipc_named_pipe_client {
    HANDLE pipe;
    char *run_dir;
    char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
    uint32_t shm_spin_tries;
    uint32_t negotiated_profile;
    uint32_t packet_size;
    size_t max_request_message_len;
    size_t max_response_message_len;
    netipc_win_shm_client_t *shm_client;
    uint64_t next_request_id;
};

struct handshake_result {
    uint32_t selected_profile;
    uint32_t packet_size;
    uint32_t agreed_max_request_payload_bytes;
    uint32_t agreed_max_request_batch_items;
    uint32_t agreed_max_response_payload_bytes;
    uint32_t agreed_max_response_batch_items;
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

static int compute_named_pipe_packet_size(size_t max_request_message_len,
                                          size_t max_response_message_len,
                                          uint32_t *out_packet_size) {
    if (!out_packet_size) {
        errno = EINVAL;
        return -1;
    }

    size_t logical_limit =
        max_request_message_len > max_response_message_len ? max_request_message_len : max_response_message_len;
    if (logical_limit <= NETIPC_CHUNK_HEADER_LEN || logical_limit > (size_t)UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    *out_packet_size = (uint32_t)logical_limit;
    return 0;
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

static bool is_shm_profile(uint32_t profile) {
    return profile == NETIPC_PROFILE_SHM_HYBRID ||
           profile == NETIPC_PROFILE_SHM_BUSYWAIT ||
           profile == NETIPC_PROFILE_SHM_WAITADDR;
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

static int wait_for_pipe_message(HANDLE pipe, uint32_t timeout_ms, DWORD *out_message_len) {
    if (!out_message_len) {
        errno = EINVAL;
        return -1;
    }

    ULONGLONG deadline_ms = timeout_ms == 0u ? 0ull : now_ms() + (ULONGLONG)timeout_ms;
    for (;;) {
        DWORD bytes_available = 0u;
        DWORD bytes_left_this_message = 0u;
        if (!PeekNamedPipe(pipe, NULL, 0u, NULL, &bytes_available, &bytes_left_this_message)) {
            return set_errno_from_win32(GetLastError());
        }

        if (bytes_left_this_message != 0u || bytes_available != 0u) {
            *out_message_len = (bytes_left_this_message != 0u) ? bytes_left_this_message : bytes_available;
            return 0;
        }
        if (deadline_expired(deadline_ms)) {
            errno = ETIMEDOUT;
            return -1;
        }
        sleep_millis(1u);
    }
}

static int drain_current_pipe_message(HANDLE pipe) {
    uint8_t scratch[256];

    for (;;) {
        DWORD bytes_read = 0u;
        if (ReadFile(pipe, scratch, (DWORD)sizeof(scratch), &bytes_read, NULL)) {
            return 0;
        }

        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA) {
            continue;
        }
        return set_errno_from_win32(error);
    }
}

static int read_pipe_message(HANDLE pipe,
                             uint8_t *message,
                             size_t message_capacity,
                             size_t *out_message_len,
                             uint32_t timeout_ms) {
    if (!message || !out_message_len || message_capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    DWORD message_len = 0u;
    if (wait_for_pipe_message(pipe, timeout_ms, &message_len) != 0) {
        return -1;
    }

    if ((size_t)message_len > message_capacity) {
        (void)drain_current_pipe_message(pipe);
        errno = EMSGSIZE;
        return -1;
    }

    DWORD bytes_read = 0u;
    if (!ReadFile(pipe, message, message_len, &bytes_read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA) {
            (void)drain_current_pipe_message(pipe);
            errno = EMSGSIZE;
            return -1;
        }
        return set_errno_from_win32(error);
    }

    if (bytes_read != message_len) {
        errno = EPROTO;
        return -1;
    }

    *out_message_len = (size_t)bytes_read;
    return 0;
}

static int write_pipe_message(HANDLE pipe, const uint8_t *message, size_t message_len) {
    if (!message || message_len == 0u || message_len > (size_t)UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    DWORD bytes_written = 0u;
    if (!WriteFile(pipe, message, (DWORD)message_len, &bytes_written, NULL)) {
        return set_errno_from_win32(GetLastError());
    }

    if (bytes_written != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int read_pipe_frame(HANDLE pipe,
                           uint8_t frame[NETIPC_FRAME_SIZE],
                           uint32_t timeout_ms) {
    size_t frame_len = 0u;

    if (read_pipe_message(pipe, frame, NETIPC_FRAME_SIZE, &frame_len, timeout_ms) != 0) {
        return -1;
    }

    if (frame_len != NETIPC_FRAME_SIZE) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int write_pipe_frame(HANDLE pipe, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    return write_pipe_message(pipe, frame, NETIPC_FRAME_SIZE);
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

static int validate_received_message(const uint8_t *message,
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

static int send_chunked_message(HANDLE pipe,
                                const uint8_t *message,
                                size_t message_len,
                                uint32_t packet_size) {
    if (!pipe || !message || message_len == 0u) {
        errno = EINVAL;
        return -1;
    }

    size_t chunk_payload_budget = 0u;
    if (compute_chunk_payload_budget(packet_size, &chunk_payload_budget) != 0) {
        return -1;
    }

    struct netipc_msg_header header;
    if (netipc_decode_msg_header(message, message_len, &header) != 0) {
        return -1;
    }

    uint32_t chunk_count = (uint32_t)((message_len + chunk_payload_budget - 1u) / chunk_payload_budget);
    uint8_t *packet = (uint8_t *)malloc(packet_size);
    if (!packet) {
        errno = ENOMEM;
        return -1;
    }

    size_t offset = 0u;
    int rc = 0;
    for (uint32_t chunk_index = 0u; chunk_index < chunk_count; ++chunk_index) {
        size_t remaining = message_len - offset;
        size_t chunk_payload_len =
            remaining < chunk_payload_budget ? remaining : chunk_payload_budget;
        struct netipc_chunk_header chunk = {
            .magic = NETIPC_CHUNK_MAGIC,
            .version = NETIPC_CHUNK_VERSION,
            .flags = 0u,
            .message_id = header.message_id,
            .total_message_len = (uint32_t)message_len,
            .chunk_index = chunk_index,
            .chunk_count = chunk_count,
            .chunk_payload_len = (uint32_t)chunk_payload_len,
        };
        if (netipc_encode_chunk_header(packet, NETIPC_CHUNK_HEADER_LEN, &chunk) != 0) {
            rc = -1;
            break;
        }
        memcpy(packet + NETIPC_CHUNK_HEADER_LEN, message + offset, chunk_payload_len);
        if (write_pipe_message(pipe, packet, NETIPC_CHUNK_HEADER_LEN + chunk_payload_len) != 0) {
            rc = -1;
            break;
        }
        offset += chunk_payload_len;
    }

    free(packet);
    return rc;
}

static int recv_transport_message(HANDLE pipe,
                                  uint8_t *message,
                                  size_t message_capacity,
                                  size_t max_message_len,
                                  uint32_t packet_size,
                                  size_t *out_message_len,
                                  uint32_t timeout_ms) {
    if (!pipe || !message || !out_message_len || message_capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    size_t packet_capacity = packet_size != 0u ? (size_t)packet_size : max_message_len;
    if (packet_capacity == 0u) {
        errno = EINVAL;
        return -1;
    }

    uint8_t *packet = (uint8_t *)malloc(packet_capacity);
    if (!packet) {
        errno = ENOMEM;
        return -1;
    }

    size_t packet_len = 0u;
    int rc = -1;

    if (read_pipe_message(pipe, packet, packet_capacity, &packet_len, timeout_ms) != 0) {
        goto cleanup;
    }

    if (packet_len >= NETIPC_MSG_HEADER_LEN) {
        struct netipc_msg_header header;
        if (netipc_decode_msg_header(packet, packet_len, &header) == 0) {
            size_t total = netipc_msg_total_size(&header);
            if (total == packet_len) {
                if (packet_len > message_capacity) {
                    errno = EMSGSIZE;
                    goto cleanup;
                }
                memcpy(message, packet, packet_len);
                if (validate_received_message(message, packet_len, max_message_len) != 0) {
                    goto cleanup;
                }
                *out_message_len = packet_len;
                rc = 0;
                goto cleanup;
            }
        }
    }

    if (packet_len < NETIPC_CHUNK_HEADER_LEN) {
        errno = EPROTO;
        goto cleanup;
    }

    struct netipc_chunk_header first_chunk;
    if (netipc_decode_chunk_header(packet, NETIPC_CHUNK_HEADER_LEN, &first_chunk) != 0) {
        goto cleanup;
    }
    if (first_chunk.chunk_index != 0u || first_chunk.chunk_count < 2u) {
        errno = EPROTO;
        goto cleanup;
    }
    if ((size_t)first_chunk.total_message_len > message_capacity ||
        (size_t)first_chunk.total_message_len > max_message_len) {
        errno = EMSGSIZE;
        goto cleanup;
    }

    size_t payload_len = packet_len - NETIPC_CHUNK_HEADER_LEN;
    if (payload_len != (size_t)first_chunk.chunk_payload_len) {
        errno = EPROTO;
        goto cleanup;
    }

    memcpy(message, packet + NETIPC_CHUNK_HEADER_LEN, payload_len);
    size_t offset = payload_len;

    for (uint32_t expected_index = 1u; expected_index < first_chunk.chunk_count; ++expected_index) {
        if (read_pipe_message(pipe, packet, packet_capacity, &packet_len, timeout_ms) != 0) {
            goto cleanup;
        }
        if (packet_len < NETIPC_CHUNK_HEADER_LEN) {
            errno = EPROTO;
            goto cleanup;
        }

        struct netipc_chunk_header chunk;
        if (netipc_decode_chunk_header(packet, NETIPC_CHUNK_HEADER_LEN, &chunk) != 0) {
            goto cleanup;
        }

        payload_len = packet_len - NETIPC_CHUNK_HEADER_LEN;
        if (chunk.message_id != first_chunk.message_id ||
            chunk.total_message_len != first_chunk.total_message_len ||
            chunk.chunk_count != first_chunk.chunk_count ||
            chunk.chunk_index != expected_index ||
            payload_len != (size_t)chunk.chunk_payload_len ||
            offset + payload_len > (size_t)first_chunk.total_message_len) {
            errno = EPROTO;
            goto cleanup;
        }

        memcpy(message + offset, packet + NETIPC_CHUNK_HEADER_LEN, payload_len);
        offset += payload_len;
    }

    if (offset != (size_t)first_chunk.total_message_len) {
        errno = EPROTO;
        goto cleanup;
    }
    if (validate_received_message(message, offset, max_message_len) != 0) {
        goto cleanup;
    }

    *out_message_len = offset;
    rc = 0;

cleanup:
    free(packet);
    return rc;
}

static int send_transport_message(HANDLE pipe,
                                  const uint8_t *message,
                                  size_t message_len,
                                  size_t max_message_len,
                                  uint32_t packet_size) {
    if (validate_message_len_for_send(message, message_len, max_message_len) != 0) {
        return -1;
    }

    if (packet_size != 0u && message_len > (size_t)packet_size) {
        return send_chunked_message(pipe, message, message_len, packet_size);
    }

    return write_pipe_message(pipe, message, message_len);
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

static void encode_negotiation_header(uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE], uint16_t type) {
    memset(frame, 0, NETIPC_NEGOTIATION_FRAME_SIZE);

    write_u32_le(frame + NETIPC_NEG_OFFSET_MAGIC, NETIPC_NEGOTIATION_MAGIC);
    write_u16_le(frame + NETIPC_NEG_OFFSET_VERSION, NETIPC_NEGOTIATION_VERSION);
    write_u16_le(frame + NETIPC_NEG_OFFSET_TYPE, type);
}

static int validate_negotiation_header(const uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE],
                                       uint16_t expected_type) {
    if (!frame) {
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
    return 0;
}

static int write_hello_negotiation(HANDLE pipe, const struct netipc_hello *hello) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation_header(frame, NETIPC_NEGOTIATION_HELLO);
    if (netipc_encode_hello_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                    NETIPC_CONTROL_HELLO_PAYLOAD_LEN,
                                    hello) != 0) {
        return -1;
    }
    return write_pipe_message(pipe, frame, sizeof(frame));
}

static int write_ack_negotiation(HANDLE pipe,
                                 const struct netipc_hello_ack *hello_ack,
                                 uint32_t status) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    encode_negotiation_header(frame, NETIPC_NEGOTIATION_ACK);
    if (netipc_encode_hello_ack_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                        NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN,
                                        hello_ack) != 0) {
        return -1;
    }
    write_u32_le(frame + NETIPC_NEGOTIATION_STATUS_OFFSET, status);
    return write_pipe_message(pipe, frame, sizeof(frame));
}

static int read_hello_negotiation(HANDLE pipe, struct netipc_hello *hello, uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    if (!hello) {
        errno = EINVAL;
        return -1;
    }
    size_t frame_len = 0u;
    if (read_pipe_message(pipe, frame, sizeof(frame), &frame_len, timeout_ms) != 0) {
        return -1;
    }
    if (frame_len != sizeof(frame)) {
        errno = EPROTO;
        return -1;
    }
    if (validate_negotiation_header(frame, NETIPC_NEGOTIATION_HELLO) != 0) {
        return -1;
    }
    return netipc_decode_hello_payload(frame + NETIPC_NEGOTIATION_PAYLOAD_OFFSET,
                                       NETIPC_CONTROL_HELLO_PAYLOAD_LEN,
                                       hello);
}

static int read_ack_negotiation(HANDLE pipe,
                                struct netipc_hello_ack *hello_ack,
                                uint32_t *out_status,
                                uint32_t timeout_ms) {
    uint8_t frame[NETIPC_NEGOTIATION_FRAME_SIZE];
    if (!hello_ack || !out_status) {
        errno = EINVAL;
        return -1;
    }
    size_t frame_len = 0u;
    if (read_pipe_message(pipe, frame, sizeof(frame), &frame_len, timeout_ms) != 0) {
        return -1;
    }
    if (frame_len != sizeof(frame)) {
        errno = EPROTO;
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

static uint32_t select_profile(uint32_t candidates) {
    if ((candidates & NETIPC_PROFILE_SHM_WAITADDR) != 0u) {
        return NETIPC_PROFILE_SHM_WAITADDR;
    }
    if ((candidates & NETIPC_PROFILE_SHM_BUSYWAIT) != 0u) {
        return NETIPC_PROFILE_SHM_BUSYWAIT;
    }
    if ((candidates & NETIPC_PROFILE_SHM_HYBRID) != 0u) {
        return NETIPC_PROFILE_SHM_HYBRID;
    }
    if ((candidates & NETIPC_PROFILE_NAMED_PIPE) != 0u) {
        return NETIPC_PROFILE_NAMED_PIPE;
    }
    return 0u;
}

static int perform_server_handshake(const netipc_named_pipe_server_t *server,
                                    HANDLE pipe,
                                    uint32_t timeout_ms,
                                    struct handshake_result *out_result) {
    if (!server || !out_result) {
        errno = EINVAL;
        return -1;
    }

    struct netipc_hello hello;
    if (read_hello_negotiation(pipe, &hello, timeout_ms) != 0) {
        return -1;
    }

    size_t local_max_request_message_len = 0u;
    size_t local_max_response_message_len = 0u;
    uint32_t local_packet_size = 0u;
    if (compute_max_message_len(server->max_request_payload_bytes,
                                server->max_request_batch_items,
                                &local_max_request_message_len) != 0 ||
        compute_max_message_len(server->max_response_payload_bytes,
                                server->max_response_batch_items,
                                &local_max_response_message_len) != 0 ||
        compute_named_pipe_packet_size(local_max_request_message_len,
                                       local_max_response_message_len,
                                       &local_packet_size) != 0) {
        return -1;
    }

    struct netipc_hello_ack ack = {
        .layout_version = hello.layout_version,
        .flags = 0u,
        .server_supported_profiles = server->supported_profiles,
        .intersection_profiles = hello.supported_profiles & server->supported_profiles,
        .selected_profile = 0u,
        .agreed_max_request_payload_bytes =
            negotiate_limit_u32(hello.max_request_payload_bytes, server->max_request_payload_bytes),
        .agreed_max_request_batch_items =
            negotiate_limit_u32(hello.max_request_batch_items, server->max_request_batch_items),
        .agreed_max_response_payload_bytes =
            negotiate_limit_u32(hello.max_response_payload_bytes, server->max_response_payload_bytes),
        .agreed_max_response_batch_items =
            negotiate_limit_u32(hello.max_response_batch_items, server->max_response_batch_items),
        .agreed_packet_size = negotiate_limit_u32(hello.packet_size, local_packet_size),
    };
    uint32_t status = NETIPC_NEGOTIATION_STATUS_OK;

    struct handshake_result result = {
        .selected_profile = 0u,
        .agreed_max_request_payload_bytes = 0u,
        .agreed_max_request_batch_items = 0u,
        .agreed_max_response_payload_bytes = 0u,
        .agreed_max_response_batch_items = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };

    if (server->auth_token != 0u && hello.auth_token != server->auth_token) {
        status = ERROR_ACCESS_DENIED;
    } else {
        uint32_t candidates = ack.intersection_profiles & server->preferred_profiles;
        if (candidates == 0u) {
            candidates = ack.intersection_profiles;
        }
        ack.selected_profile = select_profile(candidates);
        if (ack.selected_profile == 0u) {
            status = ERROR_NOT_SUPPORTED;
        } else if (ack.agreed_max_request_payload_bytes == 0u ||
                   ack.agreed_max_request_batch_items == 0u ||
                   ack.agreed_max_response_payload_bytes == 0u ||
                   ack.agreed_max_response_batch_items == 0u ||
                   ack.agreed_packet_size == 0u) {
            status = ERROR_INVALID_PARAMETER;
        } else if (compute_max_message_len(ack.agreed_max_request_payload_bytes,
                                           ack.agreed_max_request_batch_items,
                                           &result.max_request_message_len) != 0 ||
                   compute_max_message_len(ack.agreed_max_response_payload_bytes,
                                           ack.agreed_max_response_batch_items,
                                           &result.max_response_message_len) != 0) {
            status = ERROR_INVALID_PARAMETER;
        } else if ((size_t)ack.agreed_packet_size >
                   (result.max_request_message_len > result.max_response_message_len
                        ? result.max_request_message_len
                        : result.max_response_message_len)) {
            status = ERROR_INVALID_PARAMETER;
        } else {
            result.selected_profile = ack.selected_profile;
            result.packet_size = ack.agreed_packet_size;
            result.agreed_max_request_payload_bytes = ack.agreed_max_request_payload_bytes;
            result.agreed_max_request_batch_items = ack.agreed_max_request_batch_items;
            result.agreed_max_response_payload_bytes = ack.agreed_max_response_payload_bytes;
            result.agreed_max_response_batch_items = ack.agreed_max_response_batch_items;
        }
    }

    if (write_ack_negotiation(pipe, &ack, status) != 0) {
        return -1;
    }

    if (status != NETIPC_NEGOTIATION_STATUS_OK) {
        return set_errno_from_win32(status);
    }

    *out_result = result;
    return 0;
}

static int perform_client_handshake(const netipc_named_pipe_client_t *client,
                                    HANDLE pipe,
                                    uint32_t timeout_ms,
                                    struct handshake_result *out_result) {
    if (!client || !out_result) {
        errno = EINVAL;
        return -1;
    }

    size_t local_max_request_message_len = 0u;
    size_t local_max_response_message_len = 0u;
    uint32_t local_packet_size = 0u;
    if (compute_max_message_len(client->max_request_payload_bytes,
                                client->max_request_batch_items,
                                &local_max_request_message_len) != 0 ||
        compute_max_message_len(client->max_response_payload_bytes,
                                client->max_response_batch_items,
                                &local_max_response_message_len) != 0 ||
        compute_named_pipe_packet_size(local_max_request_message_len,
                                       local_max_response_message_len,
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

    if (write_hello_negotiation(pipe, &hello) != 0) {
        return -1;
    }

    struct netipc_hello_ack ack;
    uint32_t status = NETIPC_NEGOTIATION_STATUS_OK;
    if (read_ack_negotiation(pipe, &ack, &status, timeout_ms) != 0) {
        return -1;
    }
    if (status != NETIPC_NEGOTIATION_STATUS_OK) {
        return set_errno_from_win32(status);
    }
    struct handshake_result result = {
        .selected_profile = ack.selected_profile,
        .agreed_max_request_payload_bytes = ack.agreed_max_request_payload_bytes,
        .agreed_max_request_batch_items = ack.agreed_max_request_batch_items,
        .agreed_max_response_payload_bytes = ack.agreed_max_response_payload_bytes,
        .agreed_max_response_batch_items = ack.agreed_max_response_batch_items,
        .packet_size = ack.agreed_packet_size,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };

    if ((ack.intersection_profiles & client->supported_profiles) == 0u ||
        ack.selected_profile == 0u ||
        (ack.selected_profile & ack.intersection_profiles) == 0u ||
        (ack.selected_profile & client->supported_profiles) == 0u ||
        ack.agreed_max_request_payload_bytes == 0u ||
        ack.agreed_max_request_batch_items == 0u ||
        ack.agreed_max_response_payload_bytes == 0u ||
        ack.agreed_max_response_batch_items == 0u ||
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
    server->max_request_payload_bytes = effective_payload_limit(config->max_request_payload_bytes);
    server->max_request_batch_items = effective_batch_limit(config->max_request_batch_items);
    server->max_response_payload_bytes = effective_payload_limit(config->max_response_payload_bytes);
    server->max_response_batch_items = effective_batch_limit(config->max_response_batch_items);
    server->auth_token = config->auth_token;
    server->shm_spin_tries = config->shm_spin_tries;
    size_t max_request_message_len =
        netipc_msg_max_batch_total_size(server->max_request_payload_bytes, server->max_request_batch_items);
    size_t max_response_message_len =
        netipc_msg_max_batch_total_size(server->max_response_payload_bytes, server->max_response_batch_items);
    size_t max_default_message_len =
        max_request_message_len > max_response_message_len ? max_request_message_len : max_response_message_len;
    if (max_request_message_len == 0u || max_response_message_len == 0u ||
        max_default_message_len > (size_t)UINT32_MAX) {
        int saved = (errno != 0) ? errno : EOVERFLOW;
        free(server->service_name);
        free(server->run_dir);
        free(server);
        errno = saved;
        return -1;
    }

    server->pipe = CreateNamedPipeW(pipe_name,
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                    1u,
                                    (DWORD)max_default_message_len,
                                    (DWORD)max_default_message_len,
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

    struct handshake_result negotiated = {
        .selected_profile = 0u,
        .packet_size = 0u,
        .agreed_max_request_payload_bytes = 0u,
        .agreed_max_request_batch_items = 0u,
        .agreed_max_response_payload_bytes = 0u,
        .agreed_max_response_batch_items = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };
    if (perform_server_handshake(server,
                                 server->pipe,
                                 timeout_ms,
                                 &negotiated) != 0) {
        DisconnectNamedPipe(server->pipe);
        server->connected = false;
        return -1;
    }

    if (is_shm_profile(negotiated.selected_profile)) {
        struct netipc_named_pipe_config config = {
            .run_dir = server->run_dir,
            .service_name = server->service_name,
            .supported_profiles = server->supported_profiles,
            .preferred_profiles = server->preferred_profiles,
            .max_request_payload_bytes = negotiated.agreed_max_request_payload_bytes,
            .max_request_batch_items = negotiated.agreed_max_request_batch_items,
            .max_response_payload_bytes = negotiated.agreed_max_response_payload_bytes,
            .max_response_batch_items = negotiated.agreed_max_response_batch_items,
            .auth_token = server->auth_token,
            .shm_spin_tries = server->shm_spin_tries,
        };
        if (netipc_win_shm_server_create(&config,
                                         negotiated.selected_profile,
                                         &server->shm_server) != 0) {
            DisconnectNamedPipe(server->pipe);
            server->connected = false;
            server->negotiated_profile = 0u;
            return -1;
        }
    }

    server->negotiated_profile = negotiated.selected_profile;
    server->packet_size = negotiated.packet_size;
    server->max_request_message_len = negotiated.max_request_message_len;
    server->max_response_message_len = negotiated.max_response_message_len;
    server->connected = true;
    return 0;
}

int netipc_named_pipe_server_receive_message(netipc_named_pipe_server_t *server,
                                             uint8_t *message,
                                             size_t message_capacity,
                                             size_t *out_message_len,
                                             uint32_t timeout_ms) {
    if (!server || !message || !out_message_len || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(server->negotiated_profile)) {
        return netipc_win_shm_server_receive_message(server->shm_server,
                                                     message,
                                                     message_capacity,
                                                     out_message_len,
                                                     timeout_ms);
    }

    if (server->max_request_message_len == 0u || message_capacity < server->max_request_message_len) {
        errno = EMSGSIZE;
        return -1;
    }

    return recv_transport_message(server->pipe,
                                  message,
                                  message_capacity,
                                  server->max_request_message_len,
                                  server->packet_size,
                                  out_message_len,
                                  timeout_ms);
}

int netipc_named_pipe_server_send_message(netipc_named_pipe_server_t *server,
                                          const uint8_t *message,
                                          size_t message_len,
                                          uint32_t timeout_ms) {
    (void)timeout_ms;

    if (!server || !message || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(server->negotiated_profile)) {
        return netipc_win_shm_server_send_message(server->shm_server,
                                                  message,
                                                  message_len,
                                                  timeout_ms);
    }

    if (server->max_response_message_len == 0u) {
        errno = EPROTO;
        return -1;
    }
    return send_transport_message(server->pipe,
                                  message,
                                  message_len,
                                  server->max_response_message_len,
                                  server->packet_size);
}

int netipc_named_pipe_server_receive_frame(netipc_named_pipe_server_t *server,
                                           uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms) {
    if (!server || !frame || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(server->negotiated_profile)) {
        return netipc_win_shm_server_receive_frame(server->shm_server, frame, timeout_ms);
    }

    return read_pipe_frame(server->pipe, frame, timeout_ms);
}

int netipc_named_pipe_server_send_frame(netipc_named_pipe_server_t *server,
                                        const uint8_t frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms) {
    (void)timeout_ms;

    if (!server || !frame || server->pipe == INVALID_HANDLE_VALUE || !server->connected) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(server->negotiated_profile)) {
        return netipc_win_shm_server_send_frame(server->shm_server, frame, timeout_ms);
    }

    return write_pipe_frame(server->pipe, frame);
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
    client->max_request_payload_bytes = effective_payload_limit(config->max_request_payload_bytes);
    client->max_request_batch_items = effective_batch_limit(config->max_request_batch_items);
    client->max_response_payload_bytes = effective_payload_limit(config->max_response_payload_bytes);
    client->max_response_batch_items = effective_batch_limit(config->max_response_batch_items);
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

    struct handshake_result negotiated = {
        .selected_profile = 0u,
        .packet_size = 0u,
        .agreed_max_request_payload_bytes = 0u,
        .agreed_max_request_batch_items = 0u,
        .agreed_max_response_payload_bytes = 0u,
        .agreed_max_response_batch_items = 0u,
        .max_request_message_len = 0u,
        .max_response_message_len = 0u,
    };
    if (perform_client_handshake(client,
                                 client->pipe,
                                 timeout_ms,
                                 &negotiated) != 0) {
        CloseHandle(client->pipe);
        free(client->service_name);
        free(client->run_dir);
        free(client);
        return -1;
    }

    client->negotiated_profile = negotiated.selected_profile;
    client->packet_size = negotiated.packet_size;
    client->max_request_message_len = negotiated.max_request_message_len;
    client->max_response_message_len = negotiated.max_response_message_len;

    if (is_shm_profile(client->negotiated_profile)) {
        struct netipc_named_pipe_config shm_config = {
            .run_dir = client->run_dir,
            .service_name = client->service_name,
            .supported_profiles = client->supported_profiles,
            .preferred_profiles = client->preferred_profiles,
            .max_request_payload_bytes = negotiated.agreed_max_request_payload_bytes,
            .max_request_batch_items = negotiated.agreed_max_request_batch_items,
            .max_response_payload_bytes = negotiated.agreed_max_response_payload_bytes,
            .max_response_batch_items = negotiated.agreed_max_response_batch_items,
            .auth_token = client->auth_token,
            .shm_spin_tries = client->shm_spin_tries,
        };
        if (netipc_win_shm_client_create(&shm_config,
                                         client->negotiated_profile,
                                         &client->shm_client,
                                         timeout_ms) != 0) {
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

int netipc_named_pipe_client_call_message(netipc_named_pipe_client_t *client,
                                          const uint8_t *request_message,
                                          size_t request_message_len,
                                          uint8_t *response_message,
                                          size_t response_capacity,
                                          size_t *out_response_len,
                                          uint32_t timeout_ms) {
    if (!client || !request_message || !response_message || !out_response_len ||
        client->pipe == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(client->negotiated_profile)) {
        return netipc_win_shm_client_call_message(client->shm_client,
                                                  request_message,
                                                  request_message_len,
                                                  response_message,
                                                  response_capacity,
                                                  out_response_len,
                                                  timeout_ms);
    }

    if (client->max_request_message_len == 0u || client->max_response_message_len == 0u) {
        errno = EPROTO;
        return -1;
    }
    if (response_capacity < client->max_response_message_len) {
        errno = EMSGSIZE;
        return -1;
    }
    if (send_transport_message(client->pipe,
                               request_message,
                               request_message_len,
                               client->max_request_message_len,
                               client->packet_size) != 0) {
        return -1;
    }

    return recv_transport_message(client->pipe,
                                  response_message,
                                  response_capacity,
                                  client->max_response_message_len,
                                  client->packet_size,
                                  out_response_len,
                                  timeout_ms);
}

int netipc_named_pipe_client_call_frame(netipc_named_pipe_client_t *client,
                                        const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                        uint8_t response_frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms) {
    if (!client || !request_frame || !response_frame || client->pipe == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }

    if (is_shm_profile(client->negotiated_profile)) {
        return netipc_win_shm_client_call_frame(client->shm_client,
                                                request_frame,
                                                response_frame,
                                                timeout_ms);
    }

    if (write_pipe_frame(client->pipe, request_frame) != 0) {
        return -1;
    }
    return read_pipe_frame(client->pipe, response_frame, timeout_ms);
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
