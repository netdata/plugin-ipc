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
#include "test_win_raw_client_helpers.h"

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

static nipc_server_config_t default_typed_server_config(void)
{
    return (nipc_server_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 16,
        .auth_token                 = AUTH_TOKEN,
    };
}

static nipc_client_config_t default_typed_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 16,
        .auth_token                 = AUTH_TOKEN,
    };
}

static bool on_increment(void *user, uint64_t request, uint64_t *response)
{
    (void)user;
    *response = request + 1;
    return true;
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

static bool on_string_reverse(void *user,
                              const char *request_str, uint32_t request_str_len,
                              char *response_str, uint32_t response_capacity,
                              uint32_t *response_str_len)
{
    (void)user;

    if (request_str_len > response_capacity)
        return false;

    for (uint32_t i = 0; i < request_str_len; i++)
        response_str[i] = request_str[request_str_len - 1 - i];

    *response_str_len = request_str_len;
    return true;
}

static nipc_error_t dispatch_increment_request(const nipc_header_t *request_hdr,
                                               const uint8_t *request_payload,
                                               size_t request_len,
                                               uint8_t *response_buf,
                                               size_t response_buf_size,
                                               size_t *response_len_out,
                                               nipc_increment_handler_fn handler)
{
    if (request_hdr->code != NIPC_METHOD_INCREMENT)
        return NIPC_ERR_BAD_LAYOUT;

    if (!handler)
        return NIPC_ERR_HANDLER_FAILED;

    if (!nipc_dispatch_increment(request_payload, request_len,
                                 response_buf, response_buf_size,
                                 response_len_out, handler, NULL))
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

static nipc_error_t dispatch_string_reverse_request(const nipc_header_t *request_hdr,
                                                    const uint8_t *request_payload,
                                                    size_t request_len,
                                                    uint8_t *response_buf,
                                                    size_t response_buf_size,
                                                    size_t *response_len_out,
                                                    nipc_string_reverse_handler_fn handler)
{
    if (request_hdr->code != NIPC_METHOD_STRING_REVERSE)
        return NIPC_ERR_BAD_LAYOUT;

    if (!handler)
        return NIPC_ERR_HANDLER_FAILED;

    if (!nipc_dispatch_string_reverse(request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out, handler, NULL))
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

static nipc_error_t raw_noop_handler(void *user,
                                     const nipc_header_t *request_hdr,
                                     const uint8_t *request_payload,
                                     size_t request_len,
                                     uint8_t *response_buf,
                                     size_t response_buf_size,
                                     size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;
    (void)request_payload;
    (void)request_len;
    (void)response_buf;
    (void)response_buf_size;
    *response_len_out = 0;
    return NIPC_OK;
}

static nipc_error_t raw_increment_handler(void *user,
                                          const nipc_header_t *request_hdr,
                                          const uint8_t *request_payload,
                                          size_t request_len,
                                          uint8_t *response_buf,
                                          size_t response_buf_size,
                                          size_t *response_len_out)
{
    (void)user;
    return dispatch_increment_request(request_hdr, request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out, on_increment);
}

static nipc_error_t raw_blocking_increment_handler(void *user,
                                                   const nipc_header_t *request_hdr,
                                                   const uint8_t *request_payload,
                                                   size_t request_len,
                                                   uint8_t *response_buf,
                                                   size_t response_buf_size,
                                                   size_t *response_len_out)
{
    (void)user;
    return dispatch_increment_request(request_hdr, request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out, on_increment_blocking);
}

static nipc_error_t raw_missing_increment_handler(void *user,
                                                  const nipc_header_t *request_hdr,
                                                  const uint8_t *request_payload,
                                                  size_t request_len,
                                                  uint8_t *response_buf,
                                                  size_t response_buf_size,
                                                  size_t *response_len_out)
{
    (void)user;
    return dispatch_increment_request(request_hdr, request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out, NULL);
}

static nipc_error_t raw_string_reverse_handler(void *user,
                                               const nipc_header_t *request_hdr,
                                               const uint8_t *request_payload,
                                               size_t request_len,
                                               uint8_t *response_buf,
                                               size_t response_buf_size,
                                               size_t *response_len_out)
{
    (void)user;
    return dispatch_string_reverse_request(request_hdr, request_payload, request_len,
                                           response_buf, response_buf_size,
                                           response_len_out, on_string_reverse);
}

static nipc_error_t raw_missing_string_handler(void *user,
                                               const nipc_header_t *request_hdr,
                                               const uint8_t *request_payload,
                                               size_t request_len,
                                               uint8_t *response_buf,
                                               size_t response_buf_size,
                                               size_t *response_len_out)
{
    (void)user;
    return dispatch_string_reverse_request(request_hdr, request_payload, request_len,
                                           response_buf, response_buf_size,
                                           response_len_out, NULL);
}

static bool on_snapshot_one(void *user,
                            const nipc_cgroups_req_t *request,
                            nipc_cgroups_builder_t *builder)
{
    (void)user;

    if (request->layout_version != 1 || request->flags != 0)
        return false;

    return nipc_cgroups_builder_add(builder,
                                    1001,
                                    0,
                                    1,
                                    "docker-abc123",
                                    (uint32_t)strlen("docker-abc123"),
                                    "/sys/fs/cgroup/docker/abc123",
                                    (uint32_t)strlen("/sys/fs/cgroup/docker/abc123"))
           == NIPC_OK;
}

static bool on_snapshot_collision(void *user,
                                  const nipc_cgroups_req_t *request,
                                  nipc_cgroups_builder_t *builder)
{
    (void)user;

    static const char *names[] = {
        "aa", "aq", "a1", "bp", "b0", "xz", "yy", "y9"
    };
    static const char *paths[] = {
        "/cg/aa", "/cg/aq", "/cg/a1", "/cg/bp",
        "/cg/b0", "/cg/xz", "/cg/yy", "/cg/y9"
    };

    if (request->layout_version != 1 || request->flags != 0)
        return false;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        nipc_error_t err = nipc_cgroups_builder_add(
            builder,
            42,
            0,
            1,
            names[i],
            (uint32_t)strlen(names[i]),
            paths[i],
            (uint32_t)strlen(paths[i]));
        if (err != NIPC_OK)
            return false;
    }

    return true;
}

static nipc_cgroups_service_handler_t snapshot_one_service_handler = {
    .handle = on_snapshot_one,
    .snapshot_max_items = 1,
    .user = NULL,
};

static nipc_cgroups_service_handler_t missing_snapshot_service_handler = {
    .handle = NULL,
    .snapshot_max_items = 0,
    .user = NULL,
};

static nipc_cgroups_service_handler_t collision_snapshot_service_handler = {
    .handle = on_snapshot_collision,
    .snapshot_max_items = 8,
    .user = NULL,
};

typedef struct {
    char service[64];
    int worker_count;
    nipc_server_config_t typed_config;
    nipc_np_server_config_t raw_config;
    bool use_typed;
    nipc_cgroups_service_handler_t service_handler;
    uint16_t expected_method_code;
    nipc_server_handler_fn raw_handler;
    HANDLE ready_event;
    volatile LONG init_ok;
    nipc_managed_server_t server;
} server_thread_ctx_t;

static DWORD WINAPI managed_server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_error_t err;

    if (ctx->use_typed) {
        err = nipc_server_init_typed(&ctx->server, TEST_RUN_DIR,
                                     ctx->service, &ctx->typed_config,
                                     ctx->worker_count, &ctx->service_handler);
    } else {
        err = nipc_server_init(&ctx->server, TEST_RUN_DIR,
                               ctx->service, &ctx->raw_config,
                               ctx->worker_count, ctx->expected_method_code,
                               ctx->raw_handler, NULL);
    }
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
                                 const nipc_server_config_t *config,
                                 const nipc_cgroups_service_handler_t *service_handler)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = worker_count;
    ctx->typed_config = *config;
    ctx->use_typed = true;
    ctx->service_handler = *service_handler;
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

static HANDLE start_raw_server_named(server_thread_ctx_t *ctx,
                                     const char *service,
                                     int worker_count,
                                     const nipc_np_server_config_t *config,
                                     uint16_t expected_method_code,
                                     nipc_server_handler_fn raw_handler)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = worker_count;
    ctx->raw_config = *config;
    ctx->use_typed = false;
    ctx->expected_method_code = expected_method_code;
    ctx->raw_handler = raw_handler;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->init_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, managed_server_thread, ctx, 0, NULL);
    check("extra guard raw server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("extra guard raw server ready event", wr == WAIT_OBJECT_0);
    check("extra guard raw server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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
    nipc_client_config_t ccfg = default_typed_client_config();

    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);
    if (refresh_until_ready(&client, 100, 10))
        ctx->err = test_win_client_call_increment_raw(&client, 41, &ctx->value_out);
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

typedef struct {
    nipc_managed_server_t *server;
    HANDLE done_event;
    BOOL drained;
} drain_thread_ctx_t;

static DWORD WINAPI destroy_thread_proc(LPVOID arg)
{
    destroy_thread_ctx_t *ctx = (destroy_thread_ctx_t *)arg;
    nipc_server_destroy(ctx->server);
    SetEvent(ctx->done_event);
    return 0;
}

static DWORD WINAPI drain_thread_proc(LPVOID arg)
{
    drain_thread_ctx_t *ctx = (drain_thread_ctx_t *)arg;
    ctx->drained = nipc_server_drain(ctx->server, 1) ? TRUE : FALSE;
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
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 1, &scfg,
        NIPC_METHOD_INCREMENT, raw_blocking_increment_handler);
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
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 1, &scfg,
        NIPC_METHOD_INCREMENT, raw_blocking_increment_handler);
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
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 1, &scfg,
        NIPC_METHOD_INCREMENT, raw_blocking_increment_handler);
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
        nipc_client_config_t typed_ccfg = default_typed_client_config();
        uint64_t value_out = 0;
        nipc_client_init(&client, TEST_RUN_DIR, service, &typed_ccfg);
        check("send-failure replacement client ready",
              refresh_until_ready(&client, 100, 10));
        if (nipc_client_ready(&client)) {
            nipc_error_t err = test_win_client_call_increment_raw(&client, 5, &value_out);
            check("send-failure replacement increment succeeds",
                  err == NIPC_OK && value_out == 6);
        }
        nipc_client_close(&client);
    }

    close_block_events();
    stop_server_drain(&sctx, server_thread);
}

static void test_string_dispatch_missing_handlers_and_unknown_method(void)
{
    printf("--- Dispatch edge paths ---\n");

    {
        char service[64];
        nipc_np_server_config_t scfg = default_server_config();
        server_thread_ctx_t sctx;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_np_session_t session = { .pipe = INVALID_HANDLE_VALUE };

        unique_service(service, sizeof(service), "svc_unknown_method");
        HANDLE server_thread = start_raw_server_named(
            &sctx, service, 4, &scfg,
            NIPC_METHOD_INCREMENT, raw_increment_handler);
        if (!server_thread)
            return;

        check("raw unknown-method connect ok",
              nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session) == NIPC_NP_OK);
        if (session.pipe != INVALID_HANDLE_VALUE) {
            uint8_t recv_buf[1024];
            nipc_header_t req = {0};
            nipc_header_t resp_hdr;
            const void *payload = NULL;
            size_t payload_len = 0;

            req.kind = NIPC_KIND_REQUEST;
            req.code = 0x7ffe;
            req.item_count = 1;
            req.message_id = 77;
            req.transport_status = NIPC_STATUS_OK;

            check("raw unknown-method send ok",
                  nipc_np_send(&session, &req, NULL, 0) == NIPC_NP_OK);
            check("raw unknown-method response recv ok",
                  nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                                  &resp_hdr, &payload, &payload_len) == NIPC_NP_OK);
            check("unknown method maps to unsupported response",
                  resp_hdr.kind == NIPC_KIND_RESPONSE &&
                  resp_hdr.code == req.code &&
                  resp_hdr.message_id == req.message_id &&
                  resp_hdr.transport_status == NIPC_STATUS_UNSUPPORTED &&
                  payload_len == 0);
            nipc_np_close_session(&session);
        }
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        nipc_np_server_config_t scfg = default_server_config();
        server_thread_ctx_t sctx;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_np_session_t session = { .pipe = INVALID_HANDLE_VALUE };

        unique_service(service, sizeof(service), "svc_missing_inc");
        HANDLE server_thread = start_raw_server_named(
            &sctx, service, 4, &scfg,
            NIPC_METHOD_INCREMENT, raw_missing_increment_handler);
        if (!server_thread)
            return;

        check("missing-increment raw connect ok",
              nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session) == NIPC_NP_OK);
        if (session.pipe != INVALID_HANDLE_VALUE) {
            uint8_t req_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
            uint8_t recv_buf[1024];
            nipc_header_t req = {0};
            nipc_header_t resp_hdr;
            const void *payload = NULL;
            size_t payload_len = 0;
            size_t req_len = nipc_increment_encode(41, req_buf, sizeof(req_buf));

            req.kind = NIPC_KIND_REQUEST;
            req.code = NIPC_METHOD_INCREMENT;
            req.item_count = 1;
            req.message_id = 91;
            req.transport_status = NIPC_STATUS_OK;

            check("missing-increment raw send ok",
                  req_len > 0 && nipc_np_send(&session, &req, req_buf, req_len) == NIPC_NP_OK);
            check("missing-increment raw recv ok",
                  nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                                  &resp_hdr, &payload, &payload_len) == NIPC_NP_OK);
            check("missing increment handler returns internal-error response",
                  resp_hdr.kind == NIPC_KIND_RESPONSE &&
                  resp_hdr.code == req.code &&
                  resp_hdr.message_id == req.message_id &&
                  resp_hdr.transport_status == NIPC_STATUS_INTERNAL_ERROR &&
                  payload_len == 0);
            nipc_np_close_session(&session);
        }
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        nipc_server_config_t scfg = default_typed_server_config();
        server_thread_ctx_t sctx;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_np_session_t session = { .pipe = INVALID_HANDLE_VALUE };

        unique_service(service, sizeof(service), "svc_missing_snap");
        HANDLE server_thread = start_server_named(
            &sctx, service, 4, &scfg, &missing_snapshot_service_handler);
        if (!server_thread)
            return;

        check("missing-snapshot raw connect ok",
              nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session) == NIPC_NP_OK);
        if (session.pipe != INVALID_HANDLE_VALUE) {
            uint8_t req_buf[8];
            uint8_t recv_buf[1024];
            nipc_header_t req = {0};
            nipc_header_t resp_hdr;
            const void *payload = NULL;
            size_t payload_len = 0;
            nipc_cgroups_req_t snap_req = { .layout_version = 1, .flags = 0 };
            size_t req_len = nipc_cgroups_req_encode(&snap_req, req_buf, sizeof(req_buf));

            req.kind = NIPC_KIND_REQUEST;
            req.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
            req.item_count = 1;
            req.message_id = 92;
            req.transport_status = NIPC_STATUS_OK;

            check("missing-snapshot raw send ok",
                  req_len > 0 && nipc_np_send(&session, &req, req_buf, req_len) == NIPC_NP_OK);
            check("missing-snapshot raw recv ok",
                  nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                                  &resp_hdr, &payload, &payload_len) == NIPC_NP_OK);
            check("missing snapshot handler returns internal-error response",
                  resp_hdr.kind == NIPC_KIND_RESPONSE &&
                  resp_hdr.code == req.code &&
                  resp_hdr.message_id == req.message_id &&
                  resp_hdr.transport_status == NIPC_STATUS_INTERNAL_ERROR &&
                  payload_len == 0);
            nipc_np_close_session(&session);
        }
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        nipc_np_server_config_t scfg = default_server_config();
        server_thread_ctx_t sctx;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_np_session_t session = { .pipe = INVALID_HANDLE_VALUE };

        unique_service(service, sizeof(service), "svc_missing_str");
        HANDLE server_thread = start_raw_server_named(
            &sctx, service, 4, &scfg,
            NIPC_METHOD_STRING_REVERSE, raw_missing_string_handler);
        if (!server_thread)
            return;

        check("missing-string raw connect ok",
              nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session) == NIPC_NP_OK);
        if (session.pipe != INVALID_HANDLE_VALUE) {
            uint8_t req_buf[128];
            uint8_t recv_buf[1024];
            nipc_header_t req = {0};
            nipc_header_t resp_hdr;
            const void *payload = NULL;
            size_t payload_len = 0;
            size_t req_len = nipc_string_reverse_encode("abc", 3, req_buf, sizeof(req_buf));

            req.kind = NIPC_KIND_REQUEST;
            req.code = NIPC_METHOD_STRING_REVERSE;
            req.item_count = 1;
            req.message_id = 93;
            req.transport_status = NIPC_STATUS_OK;

            check("missing-string raw send ok",
                  req_len > 0 && nipc_np_send(&session, &req, req_buf, req_len) == NIPC_NP_OK);
            check("missing-string raw recv ok",
                  nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                                  &resp_hdr, &payload, &payload_len) == NIPC_NP_OK);
            check("missing string handler returns internal-error response",
                  resp_hdr.kind == NIPC_KIND_RESPONSE &&
                  resp_hdr.code == req.code &&
                  resp_hdr.message_id == req.message_id &&
                  resp_hdr.transport_status == NIPC_STATUS_INTERNAL_ERROR &&
                  payload_len == 0);
            nipc_np_close_session(&session);
        }
        stop_server_drain(&sctx, server_thread);
    }
}

static void test_server_init_truncation_and_typed_error_propagation(void)
{
    printf("--- Server init truncation and typed error propagation ---\n");

    char long_run_dir[512];
    char long_service[192];
    nipc_managed_server_t server;
    nipc_np_server_config_t raw_scfg = default_server_config();
    nipc_server_config_t typed_scfg = default_typed_server_config();

    memset(long_run_dir, 'r', sizeof(long_run_dir) - 1);
    long_run_dir[0] = 'C';
    long_run_dir[1] = ':';
    long_run_dir[2] = '\\';
    long_run_dir[3] = 'T';
    long_run_dir[4] = 'e';
    long_run_dir[5] = 'm';
    long_run_dir[6] = 'p';
    long_run_dir[7] = '\\';
    long_run_dir[sizeof(long_run_dir) - 1] = '\0';

    memset(long_service, 's', sizeof(long_service) - 1);
    long_service[sizeof(long_service) - 1] = '\0';

    {
        nipc_error_t err = nipc_server_init(&server, long_run_dir, long_service,
                                            &raw_scfg, 1, NIPC_METHOD_INCREMENT,
                                            raw_noop_handler, NULL);
        check("raw init with long names succeeds", err == NIPC_OK);
        if (err == NIPC_OK) {
            check("server run_dir truncated to fit",
                  strlen(server.run_dir) == sizeof(server.run_dir) - 1);
            check("server service_name truncated to fit",
                  strlen(server.service_name) == sizeof(server.service_name) - 1);
            nipc_server_destroy(&server);
        }
    }

    check("typed init propagates raw invalid-service failure",
          nipc_server_init_typed(&server, TEST_RUN_DIR, "svc/typed_bad_name",
                                 &typed_scfg, 1, &snapshot_one_service_handler)
          == NIPC_ERR_BAD_LAYOUT);
}

static void test_cache_collision_lookup_and_rehash(void)
{
    printf("--- Cache collision lookup / rehash ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_collide");

    server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 4, &scfg, &collision_snapshot_service_handler);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_typed_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("collision refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("collision cache item_count == 8", cache.item_count == 8);
    check("collision cache bucket_count == 16", cache.bucket_count == 16);
    check("collision lookup reaches probed entry",
          nipc_cgroups_cache_lookup(&cache, 42, "y9") != NULL);

    nipc_cgroups_cache_close(&cache);
    stop_server_drain(&sctx, server_thread);
}

static void test_drain_timeout_on_blocked_handler(void)
{
    printf("--- Drain timeout on blocked handler ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_drain_block");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_server_config();
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 4, &scfg,
        NIPC_METHOD_INCREMENT, raw_blocking_increment_handler);
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
    check("blocking client thread created", client_thread != NULL);
    check("blocking handler entered",
          client_thread != NULL && WaitForSingleObject(g_block_entered, 10000) == WAIT_OBJECT_0);

    drain_thread_ctx_t drain_ctx = {
        .server = &sctx.server,
        .done_event = CreateEvent(NULL, TRUE, FALSE, NULL),
        .drained = TRUE,
    };
    HANDLE drain_thread = CreateThread(NULL, 0, drain_thread_proc, &drain_ctx, 0, NULL);
    check("drain thread created", drain_thread != NULL);

    Sleep(50);
    SetEvent(g_block_release);

    check("drain thread finished",
          drain_thread != NULL && WaitForSingleObject(drain_ctx.done_event, 10000) == WAIT_OBJECT_0);
    check("drain reports timeout", drain_ctx.drained == FALSE);
    check("blocking client finished",
          client_thread != NULL && WaitForSingleObject(client_ctx.done_event, 10000) == WAIT_OBJECT_0);
    check("blocking increment eventually succeeds",
          client_ctx.err == NIPC_OK && client_ctx.value_out == 42);
    check("server thread exits after timed-out drain",
          WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);

    if (client_thread)
        CloseHandle(client_thread);
    if (drain_thread)
        CloseHandle(drain_thread);
    CloseHandle(client_ctx.done_event);
    CloseHandle(drain_ctx.done_event);
    close_block_events();
    CloseHandle(server_thread);
}

int main(void)
{
    printf("=== Windows Service Guard Extra Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_string_dispatch_missing_handlers_and_unknown_method();
    test_server_init_truncation_and_typed_error_propagation();
    test_cache_collision_lookup_and_rehash();
    test_drain_timeout_on_blocked_handler();
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
