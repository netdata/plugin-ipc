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
    err = nipc_np_accept(&listener, &session);
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
    err = nipc_np_accept(&listener, &session);
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
    test_auth_failure();

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
