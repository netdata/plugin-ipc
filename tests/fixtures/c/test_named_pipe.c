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

    err = nipc_np_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
    if (err == NIPC_NP_OK) {
        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        resp.transport_status = NIPC_STATUS_OK;
        err = nipc_np_send(&session, &resp, payload, payload_len);
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
    check("accept bad listener param",
          nipc_np_accept(NULL, 1, &session) == NIPC_NP_ERR_BAD_PARAM);
    check("accept bad out param",
          nipc_np_accept(&listener, 1, NULL) == NIPC_NP_ERR_BAD_PARAM);
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
    test_chunked_ping_pong();
    test_auth_failure();
    test_duplicate_message_id();
    test_unknown_response_message_id();
    test_addr_in_use();
    test_bad_params_and_noop_close();

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
