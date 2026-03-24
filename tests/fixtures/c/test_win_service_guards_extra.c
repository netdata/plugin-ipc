/*
 * test_win_service_guards_extra.c - Extra coverage-only Windows service tests.
 *
 * Keep these deterministic service-only cases in a separate executable from
 * test_win_service_guards.exe. The smaller executables are more stable under
 * gcov-instrumented Windows runs than one large all-in-one guard binary.
 */

#ifdef _WIN32

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;
static HANDLE g_block_entered = NULL;
static HANDLE g_block_release = NULL;

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "C:\\Temp\\nipc_win_service_guards"
#define RESPONSE_BUF_SIZE 65536

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", name);
        g_fail++;
    }
    fflush(stdout);
}

static void unique_service(char *buf, size_t len, const char *prefix)
{
    LONG n = InterlockedIncrement(&g_service_counter);
    snprintf(buf, len, "%s_%ld_%lu",
             prefix, (long)n, (unsigned long)GetCurrentProcessId());
}

static nipc_np_server_config_t default_server_config(void)
{
    return (nipc_np_server_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 16,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = 0,
    };
}

static nipc_np_client_config_t default_client_config(void)
{
    return (nipc_np_client_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 16,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = 0,
    };
}

static bool on_increment_blocking(void *user, uint64_t request, uint64_t *response)
{
    (void)user;

    if (g_block_entered)
        SetEvent(g_block_entered);

    if (g_block_release)
        WaitForSingleObject(g_block_release, 10000);

    *response = request + 1;
    return true;
}

static nipc_cgroups_handlers_t blocking_increment_handlers = {
    .on_increment = on_increment_blocking,
    .on_string_reverse = NULL,
    .on_cgroups_snapshot = NULL,
    .snapshot_max_items = 0,
    .user = NULL,
};

typedef struct {
    char service[64];
    int worker_count;
    nipc_np_server_config_t config;
    nipc_cgroups_handlers_t handlers;
    HANDLE ready_event;
    volatile LONG init_ok;
    nipc_managed_server_t server;
} server_thread_ctx_t;

static DWORD WINAPI managed_server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_error_t err = nipc_server_init_typed(&ctx->server, TEST_RUN_DIR,
                                              ctx->service, &ctx->config,
                                              ctx->worker_count, &ctx->handlers);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed for %s: %d\n", ctx->service, err);
        InterlockedExchange(&ctx->init_ok, 0);
        SetEvent(ctx->ready_event);
        return 1;
    }

    InterlockedExchange(&ctx->init_ok, 1);
    SetEvent(ctx->ready_event);
    nipc_server_run(&ctx->server);
    return 0;
}

static HANDLE start_server_named(server_thread_ctx_t *ctx,
                                 const char *service,
                                 int worker_count,
                                 const nipc_np_server_config_t *config,
                                 const nipc_cgroups_handlers_t *handlers)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = worker_count;
    ctx->config = *config;
    ctx->handlers = *handlers;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->init_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, managed_server_thread, ctx, 0, NULL);
    check("extra guard server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("extra guard server ready event", wr == WAIT_OBJECT_0);
    check("extra guard server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
    CloseHandle(ctx->ready_event);
    ctx->ready_event = NULL;

    if (wr != WAIT_OBJECT_0 ||
        InterlockedCompareExchange(&ctx->init_ok, 0, 0) != 1) {
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
        return NULL;
    }

    return thread;
}

static void stop_server_drain(server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    check("extra guard server thread exited",
          WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("extra guard server drain completed",
          nipc_server_drain(&ctx->server, 10000));
}

static bool refresh_until_ready(nipc_client_ctx_t *client, int max_tries, DWORD sleep_ms)
{
    for (int i = 0; i < max_tries; i++) {
        nipc_client_refresh(client);
        if (nipc_client_ready(client))
            return true;
        Sleep(sleep_ms);
    }

    return false;
}

static bool wait_for_server_sessions(nipc_managed_server_t *server,
                                     int target_count,
                                     int max_tries,
                                     DWORD sleep_ms)
{
    for (int i = 0; i < max_tries; i++) {
        int count;

        EnterCriticalSection(&server->sessions_lock);
        count = server->session_count;
        LeaveCriticalSection(&server->sessions_lock);

        if (count >= target_count)
            return true;

        Sleep(sleep_ms);
    }

    return false;
}

typedef struct {
    char service[64];
    HANDLE done_event;
    nipc_error_t err;
    uint64_t value_out;
} blocking_client_ctx_t;

static DWORD WINAPI blocking_client_thread(LPVOID arg)
{
    blocking_client_ctx_t *ctx = (blocking_client_ctx_t *)arg;
    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();

    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);
    if (refresh_until_ready(&client, 100, 10))
        ctx->err = nipc_client_call_increment(&client, 41, &ctx->value_out);
    else
        ctx->err = NIPC_ERR_NOT_READY;

    nipc_client_close(&client);
    SetEvent(ctx->done_event);
    return 0;
}

typedef struct {
    nipc_managed_server_t *server;
    HANDLE done_event;
} destroy_thread_ctx_t;

static DWORD WINAPI destroy_thread_proc(LPVOID arg)
{
    destroy_thread_ctx_t *ctx = (destroy_thread_ctx_t *)arg;
    nipc_server_destroy(ctx->server);
    SetEvent(ctx->done_event);
    return 0;
}

static void close_block_events(void)
{
    if (g_block_entered)
        CloseHandle(g_block_entered);
    if (g_block_release)
        CloseHandle(g_block_release);
    g_block_entered = NULL;
    g_block_release = NULL;
}

static void test_worker_limit_rejects_extra_client(void)
{
    printf("--- Worker limit rejects extra client ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_worker_limit");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 1, &scfg, &blocking_increment_handlers);
    if (!server_thread)
        return;

    g_block_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_block_release = CreateEvent(NULL, TRUE, FALSE, NULL);

    blocking_client_ctx_t client_ctx = {
        .done_event = CreateEvent(NULL, TRUE, FALSE, NULL),
        .err = NIPC_ERR_NOT_READY,
        .value_out = 0,
    };
    strncpy(client_ctx.service, service, sizeof(client_ctx.service) - 1);

    HANDLE client_thread = CreateThread(NULL, 0, blocking_client_thread, &client_ctx, 0, NULL);
    check("worker-limit client thread created", client_thread != NULL);
    check("worker-limit handler entered",
          client_thread != NULL && WaitForSingleObject(g_block_entered, 10000) == WAIT_OBJECT_0);
    check("worker-limit server tracks active session",
          wait_for_server_sessions(&sctx.server, 1, 100, 10));

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t second = { .pipe = INVALID_HANDLE_VALUE };
    check("worker-limit second client connect ok",
          nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &second) == NIPC_NP_OK);
    if (second.pipe != INVALID_HANDLE_VALUE) {
        uint8_t req_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
        uint8_t recv_buf[128];
        nipc_header_t req = {0};
        nipc_header_t resp_hdr;
        const void *payload = NULL;
        size_t payload_len = 0;
        size_t req_len = nipc_increment_encode(99, req_buf, sizeof(req_buf));
        bool rejected = false;

        req.kind = NIPC_KIND_REQUEST;
        req.code = NIPC_METHOD_INCREMENT;
        req.item_count = 1;
        req.message_id = 201;
        req.transport_status = NIPC_STATUS_OK;

        if (req_len == 0 ||
            nipc_np_send(&second, &req, req_buf, req_len) != NIPC_NP_OK) {
            rejected = true;
        } else if (nipc_np_receive(&second, recv_buf, sizeof(recv_buf),
                                   &resp_hdr, &payload, &payload_len) != NIPC_NP_OK) {
            rejected = true;
        }

        check("worker-limit extra client is rejected", rejected);
        nipc_np_close_session(&second);
    }

    SetEvent(g_block_release);
    check("worker-limit first client finished",
          client_thread != NULL && WaitForSingleObject(client_ctx.done_event, 10000) == WAIT_OBJECT_0);
    check("worker-limit first client still succeeds",
          client_ctx.err == NIPC_OK && client_ctx.value_out == 42);

    if (client_thread)
        CloseHandle(client_thread);
    CloseHandle(client_ctx.done_event);
    close_block_events();

    stop_server_drain(&sctx, server_thread);
}

static void test_server_destroy_joins_active_session(void)
{
    printf("--- Server destroy joins active session ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_destroy_join");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 1, &scfg, &blocking_increment_handlers);
    if (!server_thread)
        return;

    g_block_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_block_release = CreateEvent(NULL, TRUE, FALSE, NULL);

    blocking_client_ctx_t client_ctx = {
        .done_event = CreateEvent(NULL, TRUE, FALSE, NULL),
        .err = NIPC_ERR_NOT_READY,
        .value_out = 0,
    };
    strncpy(client_ctx.service, service, sizeof(client_ctx.service) - 1);

    HANDLE client_thread = CreateThread(NULL, 0, blocking_client_thread, &client_ctx, 0, NULL);
    check("destroy-join client thread created", client_thread != NULL);
    check("destroy-join handler entered",
          client_thread != NULL && WaitForSingleObject(g_block_entered, 10000) == WAIT_OBJECT_0);
    check("destroy-join server tracks active session",
          wait_for_server_sessions(&sctx.server, 1, 100, 10));

    nipc_server_stop(&sctx.server);
    check("destroy-join server thread exits after stop",
          WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);

    destroy_thread_ctx_t destroy_ctx = {
        .server = &sctx.server,
        .done_event = CreateEvent(NULL, TRUE, FALSE, NULL),
    };
    HANDLE destroy_thread = CreateThread(NULL, 0, destroy_thread_proc, &destroy_ctx, 0, NULL);
    check("destroy-join destroy thread created", destroy_thread != NULL);

    Sleep(50);
    SetEvent(g_block_release);

    check("destroy-join destroy finished",
          destroy_thread != NULL && WaitForSingleObject(destroy_ctx.done_event, 10000) == WAIT_OBJECT_0);
    check("destroy-join client finished",
          client_thread != NULL && WaitForSingleObject(client_ctx.done_event, 10000) == WAIT_OBJECT_0);
    check("destroy-join client still succeeds",
          client_ctx.err == NIPC_OK && client_ctx.value_out == 42);

    if (client_thread)
        CloseHandle(client_thread);
    if (destroy_thread)
        CloseHandle(destroy_thread);
    CloseHandle(client_ctx.done_event);
    CloseHandle(destroy_ctx.done_event);
    close_block_events();
    CloseHandle(server_thread);
}

static void test_named_pipe_send_failure_recovers(void)
{
    printf("--- Named Pipe send failure after peer disconnect ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_send_fail");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 1, &scfg, &blocking_increment_handlers);
    if (!server_thread)
        return;

    g_block_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_block_release = CreateEvent(NULL, TRUE, FALSE, NULL);

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t raw = { .pipe = INVALID_HANDLE_VALUE };
    check("send-failure raw client connect ok",
          nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &raw) == NIPC_NP_OK);

    if (raw.pipe != INVALID_HANDLE_VALUE) {
        uint8_t req_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
        nipc_header_t req = {0};
        size_t req_len = nipc_increment_encode(41, req_buf, sizeof(req_buf));

        req.kind = NIPC_KIND_REQUEST;
        req.code = NIPC_METHOD_INCREMENT;
        req.item_count = 1;
        req.message_id = 301;
        req.transport_status = NIPC_STATUS_OK;

        check("send-failure raw request send ok",
              req_len > 0 && nipc_np_send(&raw, &req, req_buf, req_len) == NIPC_NP_OK);
        check("send-failure handler entered",
              WaitForSingleObject(g_block_entered, 10000) == WAIT_OBJECT_0);

        nipc_np_close_session(&raw);
        SetEvent(g_block_release);
    }

    Sleep(100);

    {
        nipc_client_ctx_t client;
        uint64_t value_out = 0;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("send-failure replacement client ready",
              refresh_until_ready(&client, 100, 10));
        if (nipc_client_ready(&client)) {
            nipc_error_t err = nipc_client_call_increment(&client, 5, &value_out);
            check("send-failure replacement increment succeeds",
                  err == NIPC_OK && value_out == 6);
        }
        nipc_client_close(&client);
    }

    close_block_events();
    stop_server_drain(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Guard Extra Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_worker_limit_rejects_extra_client();
    test_server_destroy_joins_active_session();
    test_named_pipe_send_failure_recovers();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows service guard extra tests skipped (not Windows)\n");
    return 0;
}

#endif
