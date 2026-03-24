/*
 * test_named_pipe.c - Integration tests for L1 Windows Named Pipe transport.
 *
 * Uses threads for server/client. Prints PASS/FAIL for each test.
 * Returns 0 on all-pass.
 */

#ifdef _WIN32

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR "C:\\Temp\\nipc_test"

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", name);
        g_fail++;
    }
}

static void unique_service(char *buf, size_t len)
{
    LONG n = InterlockedIncrement(&g_service_counter);
    snprintf(buf, len, "test_%ld_%lu", (long)n, (unsigned long)GetCurrentProcessId());
}

static nipc_np_server_config_t default_server_config(void)
{
    return (nipc_np_server_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
    };
}

static nipc_np_client_config_t default_client_config(void)
{
    return (nipc_np_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
    };
}

typedef enum {
    FAKE_ACK_BAD_HEADER = 1,
    FAKE_ACK_WRONG_KIND,
    FAKE_ACK_BAD_STATUS,
    FAKE_ACK_UNSUPPORTED_STATUS,
    FAKE_ACK_BAD_PAYLOAD,
    FAKE_ACK_GOOD_THEN_CLOSE,
    FAKE_ACK_GOOD_THEN_BAD_CHUNK,
    FAKE_ACK_GOOD_THEN_LIMITED_RESPONSE,
    FAKE_ACK_GOOD_THEN_TOO_MANY_ITEMS,
    FAKE_ACK_GOOD_THEN_BAD_BATCH_DIR,
    FAKE_ACK_GOOD_THEN_SHORT_RESPONSE,
    FAKE_ACK_GOOD_THEN_BAD_RESPONSE_HEADER,
    FAKE_ACK_GOOD_THEN_ZERO_MESSAGE,
    FAKE_ACK_CLOSE_BEFORE_ACK,
} fake_ack_mode_t;

typedef enum {
    FAKE_HELLO_BAD_HEADER = 1,
    FAKE_HELLO_WRONG_KIND,
    FAKE_HELLO_BAD_PAYLOAD,
    FAKE_HELLO_CLOSE_BEFORE_HELLO,
    FAKE_HELLO_UNSUPPORTED_PROFILE,
} fake_hello_mode_t;

typedef struct {
    const char *service;
    HANDLE ready_event;
    int result;
    fake_ack_mode_t mode;
    uint32_t packet_size;
} fake_server_thread_ctx_t;

typedef struct {
    const char *service;
    HANDLE ready_event;
    int result;
    fake_hello_mode_t mode;
} fake_client_thread_ctx_t;

static int raw_pipe_write(HANDLE pipe, const void *buf, DWORD len)
{
    DWORD written = 0;
    return WriteFile(pipe, buf, len, &written, NULL) && written == len;
}

static int raw_pipe_read(HANDLE pipe, void *buf, DWORD cap, DWORD *bytes_read)
{
    DWORD n = 0;
    BOOL ok = ReadFile(pipe, buf, cap, &n, NULL);
    if (!ok)
        return 0;
    *bytes_read = n;
    return 1;
}

static HANDLE create_raw_pipe(const char *service, uint32_t buf_size, wchar_t *pipe_name)
{
    if (nipc_np_build_pipe_name(pipe_name, NIPC_NP_MAX_PIPE_NAME,
                                TEST_RUN_DIR, service) < 0)
        return INVALID_HANDLE_VALUE;

    return CreateNamedPipeW(
        pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        buf_size,
        buf_size,
        0,
        NULL);
}

static HANDLE connect_raw_pipe(const wchar_t *pipe_name)
{
    for (int i = 0; i < 100; i++) {
        HANDLE h = CreateFileW(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(h, &mode, NULL, NULL)) {
                CloseHandle(h);
                return INVALID_HANDLE_VALUE;
            }
            return h;
        }
        Sleep(10);
    }
    return INVALID_HANDLE_VALUE;
}

static int connect_named_pipe_server(HANDLE pipe)
{
    if (ConnectNamedPipe(pipe, NULL))
        return 1;
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

static size_t build_control_packet(uint8_t *dst, size_t dst_size,
                                   uint16_t kind, uint16_t code,
                                   uint32_t transport_status,
                                   const void *payload, size_t payload_len)
{
    if (dst_size < NIPC_HEADER_LEN + payload_len)
        return 0;

    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = kind,
        .code = code,
        .flags = 0,
        .item_count = 1,
        .message_id = 0,
        .payload_len = (uint32_t)payload_len,
        .transport_status = transport_status,
    };

    nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    if (payload_len > 0)
        memcpy(dst + NIPC_HEADER_LEN, payload, payload_len);
    return NIPC_HEADER_LEN + payload_len;
}

static size_t build_response_packet(uint8_t *dst, size_t dst_size,
                                    uint16_t code, uint16_t flags,
                                    uint32_t item_count,
                                    uint64_t message_id,
                                    uint32_t transport_status,
                                    const void *payload, size_t payload_len)
{
    if (dst_size < NIPC_HEADER_LEN + payload_len)
        return 0;

    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_RESPONSE,
        .code = code,
        .flags = flags,
        .item_count = item_count,
        .message_id = message_id,
        .payload_len = (uint32_t)payload_len,
        .transport_status = transport_status,
    };

    nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    if (payload_len > 0)
        memcpy(dst + NIPC_HEADER_LEN, payload, payload_len);
    return NIPC_HEADER_LEN + payload_len;
}

static size_t build_valid_hello_payload(uint8_t *dst, size_t dst_size, uint32_t packet_size)
{
    nipc_hello_t hello = {
        .layout_version = 1,
        .flags = 0,
        .supported_profiles = NIPC_PROFILE_BASELINE,
        .preferred_profiles = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items = 16,
        .auth_token = AUTH_TOKEN,
        .packet_size = packet_size,
    };
    nipc_hello_encode(&hello, dst, dst_size);
    return 44;
}

static size_t build_custom_hello_payload(uint8_t *dst, size_t dst_size,
                                         uint32_t packet_size,
                                         uint32_t supported_profiles,
                                         uint32_t preferred_profiles,
                                         uint64_t auth_token)
{
    nipc_hello_t hello = {
        .layout_version = 1,
        .flags = 0,
        .supported_profiles = supported_profiles,
        .preferred_profiles = preferred_profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items = 16,
        .auth_token = auth_token,
        .packet_size = packet_size,
    };
    nipc_hello_encode(&hello, dst, dst_size);
    return 44;
}

static size_t build_valid_hello_ack_payload(uint8_t *dst, size_t dst_size,
                                            uint32_t packet_size, uint64_t session_id)
{
    nipc_hello_ack_t ack = {
        .layout_version = 1,
        .flags = 0,
        .server_supported_profiles = NIPC_PROFILE_BASELINE,
        .intersection_profiles = NIPC_PROFILE_BASELINE,
        .selected_profile = NIPC_PROFILE_BASELINE,
        .agreed_max_request_payload_bytes = 65536,
        .agreed_max_request_batch_items = 16,
        .agreed_max_response_payload_bytes = 65536,
        .agreed_max_response_batch_items = 16,
        .agreed_packet_size = packet_size,
        .session_id = session_id,
    };
    nipc_hello_ack_encode(&ack, dst, dst_size);
    return 48;
}

static DWORD WINAPI fake_ack_server_thread(LPVOID arg)
{
    fake_server_thread_ctx_t *ctx = (fake_server_thread_ctx_t *)arg;
    ctx->result = 0;

    uint32_t packet_size = ctx->packet_size ? ctx->packet_size : NIPC_NP_DEFAULT_PACKET_SIZE;
    wchar_t pipe_name[NIPC_NP_MAX_PIPE_NAME];
    HANDLE pipe = create_raw_pipe(ctx->service, packet_size, pipe_name);
    if (pipe == INVALID_HANDLE_VALUE) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);
    if (!connect_named_pipe_server(pipe)) {
        CloseHandle(pipe);
        return 1;
    }

    uint8_t buf[4096];
    DWORD bytes_read = 0;
    if (!raw_pipe_read(pipe, buf, sizeof(buf), &bytes_read)) {
        CloseHandle(pipe);
        return 1;
    }

    uint8_t payload[64];
    uint8_t msg[4096];
    size_t msg_len = 0;

    if (ctx->mode == FAKE_ACK_CLOSE_BEFORE_ACK) {
        CloseHandle(pipe);
        ctx->result = 1;
        return 0;
    }

    if (ctx->mode == FAKE_ACK_BAD_HEADER) {
        memset(msg, 0, 16);
        msg_len = 16;
    } else if (ctx->mode == FAKE_ACK_WRONG_KIND) {
        size_t payload_len = build_valid_hello_ack_payload(payload, sizeof(payload),
                                                           packet_size, 1);
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_RESPONSE, NIPC_CODE_HELLO_ACK,
                                       NIPC_STATUS_OK, payload, payload_len);
    } else if (ctx->mode == FAKE_ACK_BAD_STATUS) {
        size_t payload_len = build_valid_hello_ack_payload(payload, sizeof(payload),
                                                           packet_size, 1);
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_CONTROL, NIPC_CODE_HELLO_ACK,
                                       NIPC_STATUS_INTERNAL_ERROR, payload, payload_len);
    } else if (ctx->mode == FAKE_ACK_UNSUPPORTED_STATUS) {
        size_t payload_len = build_valid_hello_ack_payload(payload, sizeof(payload),
                                                           packet_size, 1);
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_CONTROL, NIPC_CODE_HELLO_ACK,
                                       NIPC_STATUS_UNSUPPORTED, payload, payload_len);
    } else {
        size_t payload_len = build_valid_hello_ack_payload(payload, sizeof(payload),
                                                           packet_size, 1);
        if (ctx->mode == FAKE_ACK_BAD_PAYLOAD)
            payload_len = 8;
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_CONTROL, NIPC_CODE_HELLO_ACK,
                                       NIPC_STATUS_OK, payload, payload_len);
    }

    if (!raw_pipe_write(pipe, msg, (DWORD)msg_len)) {
        CloseHandle(pipe);
        return 1;
    }

    if (ctx->mode == FAKE_ACK_GOOD_THEN_CLOSE) {
        CloseHandle(pipe);
        ctx->result = 1;
        return 0;
    }

    if (ctx->mode == FAKE_ACK_GOOD_THEN_ZERO_MESSAGE) {
        if (!raw_pipe_write(pipe, "", 0)) {
            CloseHandle(pipe);
            return 1;
        }
        CloseHandle(pipe);
        ctx->result = 1;
        return 0;
    }

    if (ctx->mode == FAKE_ACK_GOOD_THEN_BAD_CHUNK ||
        ctx->mode == FAKE_ACK_GOOD_THEN_LIMITED_RESPONSE ||
        ctx->mode == FAKE_ACK_GOOD_THEN_TOO_MANY_ITEMS ||
        ctx->mode == FAKE_ACK_GOOD_THEN_BAD_BATCH_DIR ||
        ctx->mode == FAKE_ACK_GOOD_THEN_SHORT_RESPONSE ||
        ctx->mode == FAKE_ACK_GOOD_THEN_BAD_RESPONSE_HEADER) {
        if (!raw_pipe_read(pipe, buf, sizeof(buf), &bytes_read)) {
            CloseHandle(pipe);
            return 1;
        }

        nipc_header_t req_hdr;
        if (nipc_header_decode(buf, (size_t)bytes_read, &req_hdr) != NIPC_OK) {
            CloseHandle(pipe);
            return 1;
        }

        if (ctx->mode == FAKE_ACK_GOOD_THEN_BAD_CHUNK) {
            uint8_t payload_bytes[200];
            memset(payload_bytes, 0xAB, sizeof(payload_bytes));

            nipc_header_t resp_hdr = {
                .magic = NIPC_MAGIC_MSG,
                .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_RESPONSE,
                .code = req_hdr.code,
                .flags = 0,
                .item_count = 1,
                .message_id = req_hdr.message_id,
                .payload_len = sizeof(payload_bytes),
                .transport_status = NIPC_STATUS_OK,
            };

            uint8_t first_pkt[128];
            nipc_header_encode(&resp_hdr, first_pkt, NIPC_HEADER_LEN);
            memcpy(first_pkt + NIPC_HEADER_LEN, payload_bytes, 96);
            if (!raw_pipe_write(pipe, first_pkt, NIPC_HEADER_LEN + 96)) {
                CloseHandle(pipe);
                return 1;
            }

            nipc_chunk_header_t chk = {
                .magic = NIPC_MAGIC_CHUNK,
                .version = NIPC_VERSION,
                .flags = 0,
                .message_id = req_hdr.message_id,
                .total_message_len = NIPC_HEADER_LEN + sizeof(payload_bytes),
                .chunk_index = 2, /* intentionally wrong: should be 1 */
                .chunk_count = 3,
                .chunk_payload_len = 96,
            };

            uint8_t chunk_pkt[128];
            nipc_chunk_header_encode(&chk, chunk_pkt, NIPC_HEADER_LEN);
            memcpy(chunk_pkt + NIPC_HEADER_LEN, payload_bytes + 96, 96);
            if (!raw_pipe_write(pipe, chunk_pkt, NIPC_HEADER_LEN + 96)) {
                CloseHandle(pipe);
                return 1;
            }
        } else if (ctx->mode == FAKE_ACK_GOOD_THEN_LIMITED_RESPONSE) {
            uint8_t first_pkt[NIPC_HEADER_LEN];
            nipc_header_t resp_hdr = {
                .magic = NIPC_MAGIC_MSG,
                .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_RESPONSE,
                .code = req_hdr.code,
                .flags = 0,
                .item_count = 1,
                .message_id = req_hdr.message_id,
                .payload_len = 70000,
                .transport_status = NIPC_STATUS_OK,
            };
            nipc_header_encode(&resp_hdr, first_pkt, sizeof(first_pkt));
            size_t first_len = sizeof(first_pkt);
            if (!raw_pipe_write(pipe, first_pkt, (DWORD)first_len)) {
                CloseHandle(pipe);
                return 1;
            }
        } else if (ctx->mode == FAKE_ACK_GOOD_THEN_TOO_MANY_ITEMS) {
            uint8_t first_pkt[NIPC_HEADER_LEN];
            size_t first_len = build_response_packet(
                first_pkt, sizeof(first_pkt),
                req_hdr.code, NIPC_FLAG_BATCH, 17, req_hdr.message_id,
                NIPC_STATUS_OK, NULL, 0);
            if (!raw_pipe_write(pipe, first_pkt, (DWORD)first_len)) {
                CloseHandle(pipe);
                return 1;
            }
        } else if (ctx->mode == FAKE_ACK_GOOD_THEN_BAD_BATCH_DIR) {
            uint8_t bad_payload[8] = {0};
            uint8_t first_pkt[NIPC_HEADER_LEN + sizeof(bad_payload)];
            size_t first_len = build_response_packet(
                first_pkt, sizeof(first_pkt),
                req_hdr.code, NIPC_FLAG_BATCH, 2, req_hdr.message_id,
                NIPC_STATUS_OK, bad_payload, sizeof(bad_payload));
            if (!raw_pipe_write(pipe, first_pkt, (DWORD)first_len)) {
                CloseHandle(pipe);
                return 1;
            }
        } else if (ctx->mode == FAKE_ACK_GOOD_THEN_SHORT_RESPONSE) {
            uint8_t short_pkt[8] = {0};
            if (!raw_pipe_write(pipe, short_pkt, (DWORD)sizeof(short_pkt))) {
                CloseHandle(pipe);
                return 1;
            }
        } else if (ctx->mode == FAKE_ACK_GOOD_THEN_BAD_RESPONSE_HEADER) {
            uint8_t bad_pkt[NIPC_HEADER_LEN] = {0};
            size_t bad_len = build_response_packet(
                bad_pkt, sizeof(bad_pkt),
                req_hdr.code, 0, 1, req_hdr.message_id,
                NIPC_STATUS_OK, NULL, 0);
            if (bad_len < NIPC_HEADER_LEN) {
                CloseHandle(pipe);
                return 1;
            }
            bad_pkt[0] = 0;
            bad_pkt[1] = 0;
            bad_pkt[2] = 0;
            bad_pkt[3] = 0;
            if (!raw_pipe_write(pipe, bad_pkt, (DWORD)bad_len)) {
                CloseHandle(pipe);
                return 1;
            }
        }
    }

    CloseHandle(pipe);
    ctx->result = 1;
    return 0;
}

static DWORD WINAPI fake_hello_client_thread(LPVOID arg)
{
    fake_client_thread_ctx_t *ctx = (fake_client_thread_ctx_t *)arg;
    ctx->result = 0;

    wchar_t pipe_name[NIPC_NP_MAX_PIPE_NAME];
    if (nipc_np_build_pipe_name(pipe_name, NIPC_NP_MAX_PIPE_NAME,
                                TEST_RUN_DIR, ctx->service) < 0) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    HANDLE pipe = connect_raw_pipe(pipe_name);
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;

    uint8_t payload[64];
    uint8_t msg[256];
    size_t msg_len = 0;

    if (ctx->mode == FAKE_HELLO_CLOSE_BEFORE_HELLO) {
        CloseHandle(pipe);
        ctx->result = 1;
        return 0;
    }

    if (ctx->mode == FAKE_HELLO_BAD_HEADER) {
        memset(msg, 0, 16);
        msg_len = 16;
    } else if (ctx->mode == FAKE_HELLO_WRONG_KIND) {
        size_t payload_len = build_valid_hello_payload(payload, sizeof(payload),
                                                       NIPC_NP_DEFAULT_PACKET_SIZE);
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_RESPONSE, NIPC_CODE_HELLO,
                                       NIPC_STATUS_OK, payload, payload_len);
    } else if (ctx->mode == FAKE_HELLO_UNSUPPORTED_PROFILE) {
        size_t payload_len = build_custom_hello_payload(payload, sizeof(payload),
                                                        NIPC_NP_DEFAULT_PACKET_SIZE,
                                                        0, 0, AUTH_TOKEN);
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_CONTROL, NIPC_CODE_HELLO,
                                       NIPC_STATUS_OK, payload, payload_len);
    } else {
        size_t payload_len = build_valid_hello_payload(payload, sizeof(payload),
                                                       NIPC_NP_DEFAULT_PACKET_SIZE);
        payload_len = 8;
        msg_len = build_control_packet(msg, sizeof(msg),
                                       NIPC_KIND_CONTROL, NIPC_CODE_HELLO,
                                       NIPC_STATUS_OK, payload, payload_len);
    }

    ctx->result = raw_pipe_write(pipe, msg, (DWORD)msg_len);
    if (ctx->result)
        Sleep(50);
    CloseHandle(pipe);
    return ctx->result ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Test: FNV-1a hash                                                  */
/* ------------------------------------------------------------------ */

static void test_fnv1a(void)
{
    printf("--- FNV-1a 64-bit hash ---\n");

    uint64_t empty = nipc_fnv1a_64("", 0);
    check("empty hash = offset_basis", empty == NIPC_FNV1A_OFFSET_BASIS);

    uint64_t h1 = nipc_fnv1a_64("/var/run/netdata", 16);
    uint64_t h2 = nipc_fnv1a_64("/var/run/netdata", 16);
    check("deterministic", h1 == h2);

    uint64_t h3 = nipc_fnv1a_64("/tmp/netdata", 12);
    check("different inputs differ", h1 != h3);
}

/* ------------------------------------------------------------------ */
/*  Test: pipe name derivation                                         */
/* ------------------------------------------------------------------ */

static void test_pipe_name(void)
{
    printf("--- Pipe name derivation ---\n");

    wchar_t name[NIPC_NP_MAX_PIPE_NAME];
    int rc = nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                      "/var/run/netdata", "cgroups-snapshot");
    check("build_pipe_name success", rc == 0);

    /* Check prefix */
    const wchar_t *prefix = L"\\\\.\\pipe\\netipc-";
    int prefix_match = 1;
    for (int i = 0; prefix[i]; i++) {
        if (name[i] != prefix[i]) {
            prefix_match = 0;
            break;
        }
    }
    check("pipe name prefix", prefix_match);

    /* deterministic */
    wchar_t name2[NIPC_NP_MAX_PIPE_NAME];
    nipc_np_build_pipe_name(name2, NIPC_NP_MAX_PIPE_NAME,
                             "/var/run/netdata", "cgroups-snapshot");
    check("pipe name deterministic", wcscmp(name, name2) == 0);

    /* invalid service name */
    rc = nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                  "/var/run", "bad/name");
    check("reject invalid service name", rc < 0);

    rc = nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                  "/var/run", "");
    check("reject empty service name", rc < 0);

    rc = nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                  "/var/run", ".");
    check("reject dot service name", rc < 0);
}

/* ------------------------------------------------------------------ */
/*  Test: single client ping-pong                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *run_dir;
    const char *service;
    int         server_ok;
    HANDLE      ready_event; /* signaled when server is ready */
} server_thread_ctx_t;

static DWORD WINAPI server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_np_server_config_t cfg = default_server_config();
    nipc_np_listener_t listener;

    nipc_np_error_t err = nipc_np_listen(ctx->run_dir, ctx->service,
                                          &cfg, &listener);
    if (err != NIPC_NP_OK) {
        fprintf(stderr, "server: listen failed: %d\n", err);
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    if (err != NIPC_NP_OK) {
        fprintf(stderr, "server: accept failed: %d\n", err);
        nipc_np_close_listener(&listener);
        return 1;
    }

    /* Receive one message */
    uint8_t buf[4160];
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;

    err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
    if (err != NIPC_NP_OK) {
        fprintf(stderr, "server: receive failed: %d\n", err);
        nipc_np_close_session(&session);
        nipc_np_close_listener(&listener);
        return 1;
    }

    /* Echo as response */
    nipc_header_t resp = hdr;
    resp.kind = NIPC_KIND_RESPONSE;
    resp.transport_status = NIPC_STATUS_OK;

    err = nipc_np_send(&session, &resp, payload, payload_len);
    if (err != NIPC_NP_OK) {
        fprintf(stderr, "server: send failed: %d\n", err);
    } else {
        ctx->server_ok = 1;
    }

    nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    return 0;
}

static void test_ping_pong(void)
{
    printf("--- Single client ping-pong ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    server_thread_ctx_t ctx = {
        .run_dir     = TEST_RUN_DIR,
        .service     = service,
        .server_ok   = 0,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, server_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    /* Client side */
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;

    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("client connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        /* Build payload */
        uint8_t payload[256];
        for (int i = 0; i < (int)sizeof(payload); i++)
            payload[i] = (uint8_t)(i & 0xFF);

        nipc_header_t hdr = {
            .kind       = NIPC_KIND_REQUEST,
            .code       = NIPC_METHOD_INCREMENT,
            .flags      = 0,
            .item_count = 1,
            .message_id = 42,
        };

        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("client send", err == NIPC_NP_OK);

        /* Receive response */
        uint8_t rbuf[4160];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;

        err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                               &rhdr, &rpayload, &rpayload_len);
        check("client receive", err == NIPC_NP_OK);
        check("response kind", rhdr.kind == NIPC_KIND_RESPONSE);
        check("response message_id", rhdr.message_id == 42);
        check("response payload length", rpayload_len == sizeof(payload));
        check("response payload match",
              rpayload_len == sizeof(payload) &&
              memcmp(rpayload, payload, sizeof(payload)) == 0);

        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("server ok", ctx.server_ok);
}

typedef struct {
    const char *run_dir;
    const char *service;
    HANDLE      ready_event;
    int         accept_ok;
    uint32_t    accepted_selected_profile;
} preferred_profile_thread_ctx_t;

static DWORD WINAPI preferred_profile_server_thread(LPVOID arg)
{
    preferred_profile_thread_ctx_t *ctx = (preferred_profile_thread_ctx_t *)arg;
    nipc_np_server_config_t cfg = default_server_config();
    cfg.preferred_profiles = NIPC_PROFILE_BASELINE;

    nipc_np_listener_t listener;
    nipc_np_error_t err = nipc_np_listen(ctx->run_dir, ctx->service, &cfg, &listener);
    if (err != NIPC_NP_OK) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    if (err == NIPC_NP_OK) {
        ctx->accepted_selected_profile = session.selected_profile;
        ctx->accept_ok = 1;
        nipc_np_close_session(&session);
    }

    nipc_np_close_listener(&listener);
    return (err == NIPC_NP_OK) ? 0 : 1;
}

static void test_preferred_profile_selection(void)
{
    printf("--- Preferred profile selection ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    preferred_profile_thread_ctx_t ctx = {
        .run_dir = TEST_RUN_DIR,
        .service = service,
        .ready_event = ready_event,
        .accept_ok = 0,
        .accepted_selected_profile = 0,
    };

    HANDLE thread = CreateThread(NULL, 0, preferred_profile_server_thread, &ctx, 0, NULL);
    check("preferred-profile server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ready_event);
        return;
    }

    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.preferred_profiles = NIPC_PROFILE_BASELINE;

    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("preferred-profile client connect", err == NIPC_NP_OK);
    if (err == NIPC_NP_OK) {
        check("preferred-profile client selected baseline",
              session.selected_profile == NIPC_PROFILE_BASELINE);
        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("preferred-profile server accepted", ctx.accept_ok);
    check("preferred-profile server selected baseline",
          ctx.accepted_selected_profile == NIPC_PROFILE_BASELINE);
}

/* ------------------------------------------------------------------ */
/*  Test: auth failure                                                 */
/* ------------------------------------------------------------------ */

static DWORD WINAPI server_auth_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_np_server_config_t cfg = default_server_config();
    nipc_np_listener_t listener;

    nipc_np_error_t err = nipc_np_listen(ctx->run_dir, ctx->service,
                                          &cfg, &listener);
    if (err != NIPC_NP_OK) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    /* Expect auth failure */
    ctx->server_ok = (err == NIPC_NP_ERR_AUTH_FAILED);

    if (err == NIPC_NP_OK)
        nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    return 0;
}

static void test_auth_failure(void)
{
    printf("--- Auth failure ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    server_thread_ctx_t ctx = {
        .run_dir     = TEST_RUN_DIR,
        .service     = service,
        .server_ok   = 0,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, server_auth_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    /* Client with wrong auth token */
    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.auth_token = 0xBADBADBADull;
    nipc_np_session_t session;

    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("client gets auth_failed", err == NIPC_NP_ERR_AUTH_FAILED);

    if (err == NIPC_NP_OK)
        nipc_np_close_session(&session);

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("server detects auth_failed", ctx.server_ok);
}

/* ------------------------------------------------------------------ */
/*  Test: chunked request/response                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *run_dir;
    const char *service;
    int         server_ok;
    int         round_trips;
    HANDLE      ready_event;
} chunked_server_ctx_t;

static DWORD WINAPI chunked_server_thread(LPVOID arg)
{
    chunked_server_ctx_t *ctx = (chunked_server_ctx_t *)arg;
    nipc_np_server_config_t cfg = default_server_config();
    cfg.packet_size = 128;
    cfg.max_request_payload_bytes = 65536;
    cfg.max_response_payload_bytes = 65536;

    nipc_np_listener_t listener;
    nipc_np_error_t err = nipc_np_listen(ctx->run_dir, ctx->service,
                                          &cfg, &listener);
    if (err != NIPC_NP_OK) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    if (err != NIPC_NP_OK) {
        nipc_np_close_listener(&listener);
        return 1;
    }

    uint8_t buf[256];
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;
    int round_trips = (ctx->round_trips > 0) ? ctx->round_trips : 1;

    for (int i = 0; i < round_trips; i++) {
        err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
        if (err != NIPC_NP_OK)
            break;

        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        resp.transport_status = NIPC_STATUS_OK;
        err = nipc_np_send(&session, &resp, payload, payload_len);
        if (err != NIPC_NP_OK)
            break;
    }

    ctx->server_ok = (err == NIPC_NP_OK);
    nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    return 0;
}

static void test_chunked_ping_pong(void)
{
    printf("--- Chunked request/response ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    chunked_server_ctx_t ctx = {
        .run_dir = TEST_RUN_DIR,
        .service = service,
        .server_ok = 0,
        .round_trips = 1,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, chunked_server_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("chunked connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        check("chunked negotiated packet_size", session.packet_size == 128);

        size_t big_len = 500;
        uint8_t *big = (uint8_t *)malloc(big_len);
        check("chunked payload alloc", big != NULL);

        if (big) {
            for (size_t i = 0; i < big_len; i++)
                big[i] = (uint8_t)(i & 0xFF);

            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST,
                .code = NIPC_METHOD_INCREMENT,
                .item_count = 1,
                .message_id = 7,
            };

            err = nipc_np_send(&session, &hdr, big, big_len);
            check("send chunked request", err == NIPC_NP_OK);

            uint8_t rbuf[256];
            nipc_header_t rhdr;
            const void *rpayload;
            size_t rpayload_len;

            err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                                   &rhdr, &rpayload, &rpayload_len);
            check("receive chunked response", err == NIPC_NP_OK);
            check("chunked response message_id", err == NIPC_NP_OK && rhdr.message_id == 7);
            check("chunked response payload len", err == NIPC_NP_OK && rpayload_len == big_len);
            check("chunked response payload data",
                  err == NIPC_NP_OK &&
                  rpayload_len == big_len &&
                  memcmp(rpayload, big, big_len) == 0);

            free(big);
        }

        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("chunked server ok", ctx.server_ok);
}

static void test_chunked_receive_reuses_buffer(void)
{
    printf("--- Chunked receive reuses session buffer ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    chunked_server_ctx_t ctx = {
        .run_dir = TEST_RUN_DIR,
        .service = service,
        .server_ok = 0,
        .round_trips = 2,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, chunked_server_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("chunked reuse connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        size_t big_len = 500;
        uint8_t *first = (uint8_t *)malloc(big_len);
        uint8_t *second = (uint8_t *)malloc(big_len);
        check("chunked reuse payload alloc", first != NULL && second != NULL);

        if (first && second) {
            for (size_t i = 0; i < big_len; i++) {
                first[i] = (uint8_t)(i & 0xFF);
                second[i] = (uint8_t)((i + 17) & 0xFF);
            }

            uint8_t rbuf[256];
            nipc_header_t rhdr;
            const void *rpayload;
            size_t rpayload_len;
            void *first_recv_buf = NULL;
            size_t first_recv_cap = 0;

            for (int round = 0; round < 2; round++) {
                uint8_t *payload = (round == 0) ? first : second;
                uint64_t message_id = (uint64_t)(17 + round);
                nipc_header_t hdr = {
                    .kind = NIPC_KIND_REQUEST,
                    .code = NIPC_METHOD_INCREMENT,
                    .item_count = 1,
                    .message_id = message_id,
                };

                err = nipc_np_send(&session, &hdr, payload, big_len);
                check(round == 0 ? "chunked reuse first send" : "chunked reuse second send",
                      err == NIPC_NP_OK);

                if (err != NIPC_NP_OK)
                    break;

                err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                                      &rhdr, &rpayload, &rpayload_len);
                check(round == 0 ? "chunked reuse first receive" : "chunked reuse second receive",
                      err == NIPC_NP_OK);
                check(round == 0 ? "chunked reuse first response id" : "chunked reuse second response id",
                      err == NIPC_NP_OK && rhdr.message_id == message_id);
                check(round == 0 ? "chunked reuse first response len" : "chunked reuse second response len",
                      err == NIPC_NP_OK && rpayload_len == big_len);
                check(round == 0 ? "chunked reuse first response payload" : "chunked reuse second response payload",
                      err == NIPC_NP_OK && rpayload_len == big_len && memcmp(rpayload, payload, big_len) == 0);

                if (round == 0 && err == NIPC_NP_OK) {
                    first_recv_buf = session.recv_buf;
                    first_recv_cap = session.recv_buf_size;
                    check("chunked reuse first receive grows buffer",
                          first_recv_buf != NULL && first_recv_cap >= big_len);
                } else if (round == 1 && err == NIPC_NP_OK) {
                    check("chunked reuse keeps recv buffer pointer",
                          session.recv_buf == first_recv_buf);
                    check("chunked reuse keeps recv buffer capacity",
                          session.recv_buf_size == first_recv_cap);
                }
            }
        }

        free(first);
        free(second);
        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("chunked reuse server ok", ctx.server_ok);
}

/* ------------------------------------------------------------------ */
/*  Test: duplicate / unknown message_id                               */
/* ------------------------------------------------------------------ */

static DWORD WINAPI wrong_id_server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_np_server_config_t cfg = default_server_config();
    nipc_np_listener_t listener;

    nipc_np_error_t err = nipc_np_listen(ctx->run_dir, ctx->service,
                                          &cfg, &listener);
    if (err != NIPC_NP_OK) {
        SetEvent(ctx->ready_event);
        return 1;
    }

    SetEvent(ctx->ready_event);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    if (err != NIPC_NP_OK) {
        nipc_np_close_listener(&listener);
        return 1;
    }

    uint8_t buf[256];
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;
    err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
    if (err == NIPC_NP_OK) {
        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        resp.transport_status = NIPC_STATUS_OK;
        resp.message_id = hdr.message_id + 1000;
        err = nipc_np_send(&session, &resp, payload, payload_len);
    }

    ctx->server_ok = (err == NIPC_NP_OK);
    nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    return 0;
}

static void test_duplicate_message_id(void)
{
    printf("--- Duplicate request message_id ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    server_thread_ctx_t ctx = {
        .run_dir = TEST_RUN_DIR,
        .service = service,
        .server_ok = 0,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, server_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("duplicate connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t payload[] = { 0xAA };
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 100,
        };

        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("first send ok", err == NIPC_NP_OK);

        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("duplicate message_id rejected", err == NIPC_NP_ERR_DUPLICATE_MSG_ID);

        uint8_t rbuf[256];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;
        err = nipc_np_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rpayload, &rpayload_len);
        check("first response still received", err == NIPC_NP_OK);

        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("duplicate server ok", ctx.server_ok);
}

static void test_unknown_response_message_id(void)
{
    printf("--- Unknown response message_id ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    server_thread_ctx_t ctx = {
        .run_dir = TEST_RUN_DIR,
        .service = service,
        .server_ok = 0,
        .ready_event = ready_event,
    };

    HANDLE thread = CreateThread(NULL, 0, wrong_id_server_thread, &ctx, 0, NULL);
    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("unknown-id connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t payload[] = { 0x11 };
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 5,
        };
        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("unknown-id request send", err == NIPC_NP_OK);

        uint8_t rbuf[256];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;
        err = nipc_np_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rpayload, &rpayload_len);
        check("unknown response message_id rejected", err == NIPC_NP_ERR_UNKNOWN_MSG_ID);

        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("unknown-id server ok", ctx.server_ok);
}

static void test_client_handshake_rejections(void)
{
    printf("--- Client handshake protocol rejections ---\n");

    struct {
        const char *name;
        fake_ack_mode_t mode;
        nipc_np_error_t expected;
        nipc_np_error_t alternate_expected;
        uint32_t packet_size;
    } cases[] = {
        { "bad HELLO_ACK header", FAKE_ACK_BAD_HEADER, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
        { "wrong HELLO_ACK kind", FAKE_ACK_WRONG_KIND, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
        { "bad HELLO_ACK status", FAKE_ACK_BAD_STATUS, NIPC_NP_ERR_HANDSHAKE, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
        { "unsupported HELLO_ACK status", FAKE_ACK_UNSUPPORTED_STATUS, NIPC_NP_ERR_NO_PROFILE, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
        { "bad HELLO_ACK payload", FAKE_ACK_BAD_PAYLOAD, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
        { "peer closes before HELLO_ACK", FAKE_ACK_CLOSE_BEFORE_ACK, NIPC_NP_ERR_RECV, NIPC_NP_OK, NIPC_NP_DEFAULT_PACKET_SIZE },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service));

        HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        fake_server_thread_ctx_t ctx = {
            .service = service,
            .ready_event = ready_event,
            .result = 0,
            .mode = cases[i].mode,
            .packet_size = cases[i].packet_size,
        };

        HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
        check("fake ack server thread created", thread != NULL);
        if (!thread) {
            CloseHandle(ready_event);
            continue;
        }

        WaitForSingleObject(ready_event, 5000);
        CloseHandle(ready_event);

        nipc_np_client_config_t ccfg = default_client_config();
        ccfg.packet_size = cases[i].packet_size;
        nipc_np_session_t session;
        nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
        if (err != cases[i].expected && err != cases[i].alternate_expected)
            printf("    note: %s returned %d\n", cases[i].name, (int)err);
        check(cases[i].name,
              err == cases[i].expected || err == cases[i].alternate_expected);
        if (err == NIPC_NP_OK)
            nipc_np_close_session(&session);

        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        check("fake ack server completed", ctx.result == 1);
    }
}

static void test_server_handshake_rejections(void)
{
    printf("--- Server handshake protocol rejections ---\n");

    struct {
        const char *name;
        fake_hello_mode_t mode;
        nipc_np_error_t expected;
        nipc_np_error_t alternate_expected;
    } cases[] = {
        { "bad HELLO header", FAKE_HELLO_BAD_HEADER, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK },
        { "wrong HELLO kind", FAKE_HELLO_WRONG_KIND, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK },
        { "bad HELLO payload", FAKE_HELLO_BAD_PAYLOAD, NIPC_NP_ERR_PROTOCOL, NIPC_NP_OK },
        { "peer closes before HELLO", FAKE_HELLO_CLOSE_BEFORE_HELLO, NIPC_NP_ERR_RECV, NIPC_NP_ERR_ACCEPT },
        { "unsupported HELLO profiles", FAKE_HELLO_UNSUPPORTED_PROFILE, NIPC_NP_ERR_NO_PROFILE, NIPC_NP_OK },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service));

        nipc_np_server_config_t scfg = default_server_config();
        nipc_np_listener_t listener;
        nipc_np_error_t err = nipc_np_listen(TEST_RUN_DIR, service, &scfg, &listener);
        check("server listen for fake hello", err == NIPC_NP_OK);
        if (err != NIPC_NP_OK)
            continue;

        HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        fake_client_thread_ctx_t ctx = {
            .service = service,
            .ready_event = ready_event,
            .result = 0,
            .mode = cases[i].mode,
        };

        HANDLE thread = CreateThread(NULL, 0, fake_hello_client_thread, &ctx, 0, NULL);
        check("fake hello client thread created", thread != NULL);
        if (!thread) {
            CloseHandle(ready_event);
            nipc_np_close_listener(&listener);
            continue;
        }

        WaitForSingleObject(ready_event, 5000);
        CloseHandle(ready_event);

        nipc_np_session_t session;
        err = nipc_np_accept(&listener, 1, &session);
        if (err != cases[i].expected && err != cases[i].alternate_expected)
            printf("    note: %s returned %d\n", cases[i].name, (int)err);
        check(cases[i].name,
              err == cases[i].expected || err == cases[i].alternate_expected);
        if (err == NIPC_NP_OK)
            nipc_np_close_session(&session);

        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        check("fake hello client completed", ctx.result == 1);
        nipc_np_close_listener(&listener);
    }
}

static void test_receive_after_peer_disconnect(void)
{
    printf("--- Receive after peer disconnect ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    fake_server_thread_ctx_t ctx = {
        .service = service,
        .ready_event = ready_event,
        .result = 0,
        .mode = FAKE_ACK_GOOD_THEN_CLOSE,
        .packet_size = NIPC_NP_DEFAULT_PACKET_SIZE,
    };

    HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
    check("disconnect fake server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ready_event);
        return;
    }

    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("disconnect test connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t buf[128];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
        check("receive sees peer disconnect", err == NIPC_NP_ERR_RECV);
        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("disconnect fake server completed", ctx.result == 1);
}

static void test_receive_after_zero_byte_message(void)
{
    printf("--- Receive after zero-byte message ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    fake_server_thread_ctx_t ctx = {
        .service = service,
        .ready_event = ready_event,
        .result = 0,
        .mode = FAKE_ACK_GOOD_THEN_ZERO_MESSAGE,
        .packet_size = NIPC_NP_DEFAULT_PACKET_SIZE,
    };

    HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
    check("zero-message fake server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ready_event);
        return;
    }

    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("zero-message test connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t buf[128];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
        check("receive sees zero-byte message as recv error", err == NIPC_NP_ERR_RECV);
        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("zero-message fake server completed", ctx.result == 1);
}

static void test_chunk_validation_error(void)
{
    printf("--- Chunk validation error ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    fake_server_thread_ctx_t ctx = {
        .service = service,
        .ready_event = ready_event,
        .result = 0,
        .mode = FAKE_ACK_GOOD_THEN_BAD_CHUNK,
        .packet_size = 128,
    };

    HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
    check("chunk fake server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ready_event);
        return;
    }

    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("chunk test connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t payload[1] = { 0x55 };
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 77,
        };

        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("chunk test request send", err == NIPC_NP_OK);

        if (err == NIPC_NP_OK) {
            uint8_t rbuf[256];
            nipc_header_t rhdr;
            const void *rpayload;
            size_t rpayload_len;
            err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                                  &rhdr, &rpayload, &rpayload_len);
            check("chunk mismatch rejected", err == NIPC_NP_ERR_CHUNK);
        }

        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("chunk fake server completed", ctx.result == 1);
}

static void test_response_limit_validation(void)
{
    printf("--- Response limit validation ---\n");

    struct {
        const char *name;
        fake_ack_mode_t mode;
        nipc_np_error_t expected;
    } cases[] = {
        { "response payload limit rejected", FAKE_ACK_GOOD_THEN_LIMITED_RESPONSE, NIPC_NP_ERR_LIMIT_EXCEEDED },
        { "response batch item_count limit rejected", FAKE_ACK_GOOD_THEN_TOO_MANY_ITEMS, NIPC_NP_ERR_LIMIT_EXCEEDED },
        { "response batch directory rejected", FAKE_ACK_GOOD_THEN_BAD_BATCH_DIR, NIPC_NP_ERR_PROTOCOL },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service));

        HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        fake_server_thread_ctx_t ctx = {
            .service = service,
            .ready_event = ready_event,
            .result = 0,
            .mode = cases[i].mode,
            .packet_size = NIPC_NP_DEFAULT_PACKET_SIZE,
        };

        HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
        check("limit fake server thread created", thread != NULL);
        if (!thread) {
            CloseHandle(ready_event);
            continue;
        }

        WaitForSingleObject(ready_event, 5000);
        CloseHandle(ready_event);

        nipc_np_client_config_t ccfg = default_client_config();
        ccfg.max_response_payload_bytes = 65536;
        ccfg.max_response_batch_items = 16;

        nipc_np_session_t session;
        nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
        check("limit test connect", err == NIPC_NP_OK);

        if (err == NIPC_NP_OK) {
            uint8_t payload[1] = { 0x11 };
            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST,
                .code = NIPC_METHOD_INCREMENT,
                .item_count = 1,
                .message_id = 123,
            };

            err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
            check("limit test request send", err == NIPC_NP_OK);

            if (err == NIPC_NP_OK) {
                uint8_t rbuf[256];
                nipc_header_t rhdr;
                const void *rpayload;
                size_t rpayload_len;
                err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                                      &rhdr, &rpayload, &rpayload_len);
                check(cases[i].name, err == cases[i].expected);
            }

            nipc_np_close_session(&session);
        }

        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        check("limit fake server completed", ctx.result == 1);
    }
}

static void test_response_protocol_validation(void)
{
    printf("--- Response protocol validation ---\n");

    struct {
        const char *name;
        fake_ack_mode_t mode;
    } cases[] = {
        { "short response header rejected", FAKE_ACK_GOOD_THEN_SHORT_RESPONSE },
        { "bad decoded response header rejected", FAKE_ACK_GOOD_THEN_BAD_RESPONSE_HEADER },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service));

        HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        fake_server_thread_ctx_t ctx = {
            .service = service,
            .ready_event = ready_event,
            .result = 0,
            .mode = cases[i].mode,
            .packet_size = NIPC_NP_DEFAULT_PACKET_SIZE,
        };

        HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
        check("protocol fake server thread created", thread != NULL);
        if (!thread) {
            CloseHandle(ready_event);
            continue;
        }

        WaitForSingleObject(ready_event, 5000);
        CloseHandle(ready_event);

        nipc_np_client_config_t ccfg = default_client_config();
        nipc_np_session_t session;
        nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
        check("protocol test connect", err == NIPC_NP_OK);

        if (err == NIPC_NP_OK) {
            uint8_t payload[1] = { 0x33 };
            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST,
                .code = NIPC_METHOD_INCREMENT,
                .item_count = 1,
                .message_id = 456,
            };

            err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
            check("protocol test request send", err == NIPC_NP_OK);

            if (err == NIPC_NP_OK) {
                uint8_t rbuf[256];
                nipc_header_t rhdr;
                const void *rpayload;
                size_t rpayload_len;
                err = nipc_np_receive(&session, rbuf, sizeof(rbuf),
                                      &rhdr, &rpayload, &rpayload_len);
                check(cases[i].name, err == NIPC_NP_ERR_PROTOCOL);
            }

            nipc_np_close_session(&session);
        }

        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        check("protocol fake server completed", ctx.result == 1);
    }
}

static void test_zero_chunk_budget_rejected(void)
{
    printf("--- Zero chunk budget rejected ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    HANDLE ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    fake_server_thread_ctx_t ctx = {
        .service = service,
        .ready_event = ready_event,
        .result = 0,
        .mode = FAKE_ACK_GOOD_THEN_CLOSE,
        .packet_size = NIPC_HEADER_LEN,
    };

    HANDLE thread = CreateThread(NULL, 0, fake_ack_server_thread, &ctx, 0, NULL);
    check("zero-budget fake server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ready_event);
        return;
    }

    WaitForSingleObject(ready_event, 5000);
    CloseHandle(ready_event);

    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.packet_size = NIPC_HEADER_LEN;
    nipc_np_session_t session;
    nipc_np_error_t err = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("zero-budget connect", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        uint8_t payload[1] = { 0x22 };
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 321,
        };

        err = nipc_np_send(&session, &hdr, payload, sizeof(payload));
        check("zero chunk budget rejected", err == NIPC_NP_ERR_BAD_PARAM);
        nipc_np_close_session(&session);
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    check("zero-budget fake server completed", ctx.result == 1);
}

/* ------------------------------------------------------------------ */
/*  Test: listener conflicts / bad params / noop close                 */
/* ------------------------------------------------------------------ */

static void test_addr_in_use(void)
{
    printf("--- Address in use ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_np_server_config_t cfg = default_server_config();
    nipc_np_listener_t a;
    nipc_np_listener_t b;

    nipc_np_error_t err = nipc_np_listen(TEST_RUN_DIR, service, &cfg, &a);
    check("first listen ok", err == NIPC_NP_OK);

    if (err == NIPC_NP_OK) {
        err = nipc_np_listen(TEST_RUN_DIR, service, &cfg, &b);
        check("second listen gets addr_in_use", err == NIPC_NP_ERR_ADDR_IN_USE);
        nipc_np_close_listener(&a);
    }
}

static void test_bad_params_and_noop_close(void)
{
    printf("--- Bad params / noop close ---\n");

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_server_config_t scfg = default_server_config();
    nipc_np_session_t session = {0};
    session.pipe = INVALID_HANDLE_VALUE;
    nipc_np_listener_t listener = {0};
    listener.pipe = INVALID_HANDLE_VALUE;
    nipc_header_t hdr = {
        .kind = NIPC_KIND_REQUEST,
        .code = NIPC_METHOD_INCREMENT,
        .item_count = 1,
        .message_id = 1,
    };
    uint8_t recv_buf[128];
    nipc_header_t recv_hdr;
    const void *payload = NULL;
    size_t payload_len = 0;
    char long_service[512];
    memset(long_service, 'x', sizeof(long_service) - 1);
    long_service[sizeof(long_service) - 1] = '\0';

    check("connect bad service param",
          nipc_np_connect(TEST_RUN_DIR, NULL, &ccfg, &session) == NIPC_NP_ERR_BAD_PARAM);
    check("connect bad config param",
          nipc_np_connect(TEST_RUN_DIR, "svc", NULL, &session) == NIPC_NP_ERR_BAD_PARAM);
    check("connect bad out param",
          nipc_np_connect(TEST_RUN_DIR, "svc", &ccfg, NULL) == NIPC_NP_ERR_BAD_PARAM);
    check("listen bad config param",
          nipc_np_listen(TEST_RUN_DIR, "svc", NULL, &listener) == NIPC_NP_ERR_BAD_PARAM);
    check("listen bad out param",
          nipc_np_listen(TEST_RUN_DIR, "svc", &scfg, NULL) == NIPC_NP_ERR_BAD_PARAM);
    check("listen overlong service rejected",
          nipc_np_listen(TEST_RUN_DIR, long_service, &scfg, &listener) == NIPC_NP_ERR_BAD_PARAM);
    check("accept bad listener param",
          nipc_np_accept(NULL, 1, &session) == NIPC_NP_ERR_BAD_PARAM);
    check("accept bad out param",
          nipc_np_accept(&listener, 1, NULL) == NIPC_NP_ERR_BAD_PARAM);
    check("connect overlong service rejected",
          nipc_np_connect(TEST_RUN_DIR, long_service, &ccfg, &session) == NIPC_NP_ERR_BAD_PARAM);
    check("connect missing service rejected",
          nipc_np_connect(TEST_RUN_DIR, "missing-service", &ccfg, &session) == NIPC_NP_ERR_CONNECT);
    check("send null session rejected",
          nipc_np_send(NULL, &hdr, NULL, 0) == NIPC_NP_ERR_BAD_PARAM);
    check("send invalid handle rejected",
          nipc_np_send(&session, &hdr, NULL, 0) == NIPC_NP_ERR_BAD_PARAM);
    check("receive null session rejected",
          nipc_np_receive(NULL, recv_buf, sizeof(recv_buf), &recv_hdr, &payload, &payload_len)
              == NIPC_NP_ERR_BAD_PARAM);
    check("receive invalid handle rejected",
          nipc_np_receive(&session, recv_buf, sizeof(recv_buf), &recv_hdr, &payload, &payload_len)
              == NIPC_NP_ERR_BAD_PARAM);

    nipc_np_close_session(NULL);
    nipc_np_close_listener(NULL);
    nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    check("noop close on null and invalid handles", 1);
}

static void test_accept_on_closed_listener(void)
{
    printf("--- Accept on closed listener ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_np_server_config_t cfg = default_server_config();
    nipc_np_listener_t listener;
    nipc_np_error_t err = nipc_np_listen(TEST_RUN_DIR, service, &cfg, &listener);
    check("listen for closed-listener accept", err == NIPC_NP_OK);
    if (err != NIPC_NP_OK)
        return;

    check("close raw listener handle", CloseHandle(listener.pipe) != 0);

    nipc_np_session_t session;
    err = nipc_np_accept(&listener, 1, &session);
    check("closed listener accept rejected", err == NIPC_NP_ERR_ACCEPT);

    listener.pipe = INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ */
/*  Test: service name validation                                      */
/* ------------------------------------------------------------------ */

static void test_service_validation(void)
{
    printf("--- Service name validation ---\n");

    wchar_t name[NIPC_NP_MAX_PIPE_NAME];

    check("valid name ok",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "test-svc_1.0") == 0);
    check("reject empty",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "") < 0);
    check("reject dot",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", ".") < 0);
    check("reject dotdot",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "..") < 0);
    check("reject slash",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "a/b") < 0);
    check("reject space",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "a b") < 0);
    check("reject backslash",
          nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME,
                                   "/run", "a\\b") < 0);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== Named Pipe Transport Tests ===\n\n");

    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_fnv1a();
    test_pipe_name();
    test_service_validation();
    test_ping_pong();
    test_preferred_profile_selection();
    test_chunked_ping_pong();
    test_chunked_receive_reuses_buffer();
    test_auth_failure();
    test_duplicate_message_id();
    test_unknown_response_message_id();
    test_client_handshake_rejections();
    test_server_handshake_rejections();
    test_receive_after_peer_disconnect();
    test_receive_after_zero_byte_message();
    test_chunk_validation_error();
    test_response_limit_validation();
    test_response_protocol_validation();
    test_zero_chunk_budget_rejected();
    test_addr_in_use();
    test_bad_params_and_noop_close();
    test_accept_on_closed_listener();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else /* !_WIN32 */

#include <stdio.h>
int main(void)
{
    printf("Named Pipe tests skipped (not Windows)\n");
    return 0;
}

#endif /* _WIN32 */
