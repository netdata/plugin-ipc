/*
 * test_win_service.c - Windows integration tests for the L2 service layer.
 *
 * Focus:
 *   - client lifecycle and status
 *   - typed increment / string_reverse / cgroups snapshot calls
 *   - retry after a broken client-side session
 *   - auth/profile error mapping
 *   - cache refresh, preserve, and reconnect behavior
 *   - raw protocol violation termination on a single session
 *
 * Returns 0 on all-pass.
 */

#ifdef _WIN32

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "C:\\Temp\\nipc_win_service"
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

static bool on_increment(void *user, uint64_t request, uint64_t *response)
{
    (void)user;
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

static bool on_snapshot(void *user,
                        const nipc_cgroups_req_t *request,
                        nipc_cgroups_builder_t *builder)
{
    (void)user;

    if (request->layout_version != 1 || request->flags != 0)
        return false;

    static const struct {
        uint32_t hash;
        uint32_t enabled;
        const char *name;
        const char *path;
    } items[] = {
        { 1001, 1, "docker-abc123", "/sys/fs/cgroup/docker/abc123" },
        { 2002, 1, "k8s-pod-xyz",   "/sys/fs/cgroup/kubepods/xyz" },
        { 3003, 0, "systemd-user",  "/sys/fs/cgroup/user.slice/user-1000" },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        nipc_error_t err = nipc_cgroups_builder_add(
            builder,
            items[i].hash,
            0,
            items[i].enabled,
            items[i].name,
            (uint32_t)strlen(items[i].name),
            items[i].path,
            (uint32_t)strlen(items[i].path));
        if (err != NIPC_OK)
            return false;
    }

    return true;
}

static bool on_snapshot_fail(void *user,
                             const nipc_cgroups_req_t *request,
                             nipc_cgroups_builder_t *builder)
{
    (void)user;
    (void)request;
    (void)builder;
    return false;
}

static bool on_snapshot_empty(void *user,
                              const nipc_cgroups_req_t *request,
                              nipc_cgroups_builder_t *builder)
{
    (void)user;
    if (request->layout_version != 1 || request->flags != 0)
        return false;
    return true;
}

static nipc_cgroups_handlers_t full_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = on_string_reverse,
    .on_cgroups_snapshot = on_snapshot,
    .snapshot_max_items = 3,
    .user = NULL,
};

static nipc_cgroups_handlers_t failing_snapshot_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = on_string_reverse,
    .on_cgroups_snapshot = on_snapshot_fail,
    .snapshot_max_items = 3,
    .user = NULL,
};

static nipc_cgroups_handlers_t empty_snapshot_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = on_string_reverse,
    .on_cgroups_snapshot = on_snapshot_empty,
    .snapshot_max_items = 1,
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
    check("server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("server ready event", wr == WAIT_OBJECT_0);
    check("server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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

static HANDLE start_default_server_named(server_thread_ctx_t *ctx,
                                         const char *service,
                                         int worker_count,
                                         const nipc_cgroups_handlers_t *handlers)
{
    nipc_np_server_config_t config = default_server_config();
    return start_server_named(ctx, service, worker_count, &config, handlers);
}

static void stop_server(server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    check("server acceptor exited", WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("server drained cleanly", nipc_server_drain(&ctx->server, 10000));
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

static void test_client_lifecycle(void)
{
    printf("--- Client lifecycle ---\n");

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    char service[64];
    unique_service(service, sizeof(service), "svc_lifecycle");

    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("initial state DISCONNECTED", client.state == NIPC_CLIENT_DISCONNECTED);
    check("initial ready false", !nipc_client_ready(&client));

    check("refresh without server changes state", nipc_client_refresh(&client));
    check("state becomes NOT_FOUND", client.state == NIPC_CLIENT_NOT_FOUND);

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread) {
        nipc_client_close(&client);
        return;
    }

    check("refresh with server changes state", nipc_client_refresh(&client));
    check("state becomes READY", client.state == NIPC_CLIENT_READY);
    check("ready cached true", nipc_client_ready(&client));

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_client_lifecycle_ready(void)
{
    printf("--- Client lifecycle ready/close ---\n");

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    char service[64];
    unique_service(service, sizeof(service), "svc_ready");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client reaches READY", refresh_until_ready(&client, 100, 10));
    check("ready state cached", nipc_client_ready(&client));

    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("connect_count == 1", status.connect_count == 1);
    check("reconnect_count == 0", status.reconnect_count == 0);

    nipc_client_close(&client);
    check("close resets state", client.state == NIPC_CLIENT_DISCONNECTED);

    stop_server(&sctx, server_thread);
}

static void test_multi_method_calls(void)
{
    printf("--- Increment / string_reverse / snapshot / batch ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_methods");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client ready", refresh_until_ready(&client, 100, 10));

    if (nipc_client_ready(&client)) {
        uint64_t value = 0;
        nipc_error_t err = nipc_client_call_increment(&client, 41, &value);
        check("increment(41) ok", err == NIPC_OK);
        check("increment(41) == 42", err == NIPC_OK && value == 42);

        nipc_string_reverse_view_t str_view;
        err = nipc_client_call_string_reverse(&client, "hello", 5, &str_view);
        check("string_reverse ok", err == NIPC_OK);
        check("string_reverse == olleh",
              err == NIPC_OK && str_view.str_len == 5 &&
              memcmp(str_view.str, "olleh", 5) == 0);

        nipc_cgroups_resp_view_t cg_view;
        err = nipc_client_call_cgroups_snapshot(&client, &cg_view);
        check("snapshot ok", err == NIPC_OK);
        check("snapshot item_count == 3", err == NIPC_OK && cg_view.item_count == 3);
        check("snapshot generation == 0", err == NIPC_OK && cg_view.generation == 0);

        uint64_t in[4] = { 1, 41, 99, 1000 };
        uint64_t out[4] = { 0 };
        err = nipc_client_call_increment_batch(&client, in, 4, out);
        check("increment batch ok", err == NIPC_OK);
        check("increment batch values",
              err == NIPC_OK &&
              out[0] == 2 && out[1] == 42 && out[2] == 100 && out[3] == 1001);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("call_count == 4", status.call_count == 4);
        check("error_count == 0", status.error_count == 0);
    }

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_retry_on_broken_session(void)
{
    printf("--- Retry after broken session ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_retry");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client ready", refresh_until_ready(&client, 100, 10));

    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("first snapshot ok", err == NIPC_OK);

        if (client.session_valid) {
            nipc_np_close_session(&client.session);
            client.session_valid = false;
        }

        err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("retry snapshot ok", err == NIPC_OK);
        check("retry snapshot item_count == 3", err == NIPC_OK && view.item_count == 3);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("reconnect_count >= 1", status.reconnect_count >= 1);
    }

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_handler_failure(void)
{
    printf("--- Handler failure ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hfail");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &failing_snapshot_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client ready", refresh_until_ready(&client, 100, 10));

    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("snapshot fails when handler fails", err != NIPC_OK);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("error_count >= 1", status.error_count >= 1);
    }

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_client_auth_failure(void)
{
    printf("--- Client auth failure mapping ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_auth");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.auth_token = 0x1111111111111111ull;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    check("state is AUTH_FAILED", client.state == NIPC_CLIENT_AUTH_FAILED);

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_client_incompatible(void)
{
    printf("--- Client incompatible profile mapping ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_incompat");

    nipc_np_server_config_t scfg = default_server_config();
    scfg.supported_profiles = NIPC_PROFILE_SHM_HYBRID;

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_BASELINE;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    check("state is INCOMPATIBLE", client.state == NIPC_CLIENT_INCOMPATIBLE);

    nipc_client_close(&client);
    stop_server(&sctx, server_thread);
}

static void test_status_reporting(void)
{
    printf("--- Status reporting ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_status");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client ready", refresh_until_ready(&client, 100, 10));

    nipc_client_status_t s0;
    nipc_client_status(&client, &s0);
    check("initial connect_count == 1", s0.connect_count == 1);
    check("initial call_count == 0", s0.call_count == 0);
    check("initial error_count == 0", s0.error_count == 0);

    if (nipc_client_ready(&client)) {
        for (int i = 0; i < 3; i++) {
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("snapshot call ok", err == NIPC_OK);
        }
    }

    nipc_client_status_t s1;
    nipc_client_status(&client, &s1);
    check("call_count == 3", s1.call_count == 3);
    check("error_count still 0", s1.error_count == 0);

    nipc_client_close(&client);
    {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("call on disconnected client fails", err == NIPC_ERR_NOT_READY);
    }

    nipc_client_status_t s2;
    nipc_client_status(&client, &s2);
    check("error_count incremented", s2.error_count == 1);

    stop_server(&sctx, server_thread);
}

static void test_cache_refresh_preserves(void)
{
    printf("--- Cache refresh failure preserves data ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_pres");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("first refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("cache ready", nipc_cgroups_cache_ready(&cache));
    check("cached item present",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    nipc_client_close(&cache.client);
    stop_server(&sctx, server_thread);

    check("refresh without server fails", !nipc_cgroups_cache_refresh(&cache));
    check("cache stays ready", nipc_cgroups_cache_ready(&cache));
    check("old cached item preserved",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("success_count still 1", status.refresh_success_count == 1);
    check("failure_count >= 1", status.refresh_failure_count >= 1);

    nipc_cgroups_cache_close(&cache);
}

static void test_cache_reconnect_rebuilds(void)
{
    printf("--- Cache reconnect rebuilds ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_reconn");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("first refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("item_count == 3", cache.item_count == 3);

    if (cache.client.session_valid) {
        nipc_np_close_session(&cache.client.session);
        cache.client.session_valid = false;
    }

    check("refresh after reconnect ok", nipc_cgroups_cache_refresh(&cache));
    check("item_count still == 3", cache.item_count == 3);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("refresh_success_count == 2", status.refresh_success_count == 2);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, server_thread);
}

static void test_cache_empty_snapshot(void)
{
    printf("--- Cache empty snapshot ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_empty");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &empty_snapshot_handlers);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_np_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("empty refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("cache ready after empty refresh", nipc_cgroups_cache_ready(&cache));
    check("item_count == 0", cache.item_count == 0);
    check("lookup miss on empty cache",
          nipc_cgroups_cache_lookup(&cache, 123, "missing") == NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("status item_count == 0", status.item_count == 0);
    check("status success_count == 1", status.refresh_success_count == 1);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, server_thread);
}

static void test_non_request_terminates_session(void)
{
    printf("--- Non-request message terminates only that session ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_nonreq");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_np_client_config_t ccfg = default_client_config();
    nipc_np_session_t session;
    nipc_np_error_t uerr = nipc_np_connect(TEST_RUN_DIR, service, &ccfg, &session);
    check("raw connect ok", uerr == NIPC_NP_OK);

    if (uerr == NIPC_NP_OK) {
        nipc_header_t bad = {0};
        bad.kind = NIPC_KIND_RESPONSE;
        bad.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        bad.item_count = 0;
        bad.message_id = 1;
        bad.transport_status = NIPC_STATUS_OK;
        check("send non-request ok", nipc_np_send(&session, &bad, NULL, 0) == NIPC_NP_OK);

        Sleep(200);

        nipc_header_t good = {0};
        good.kind = NIPC_KIND_REQUEST;
        good.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        good.item_count = 1;
        good.message_id = 2;
        good.transport_status = NIPC_STATUS_OK;

        uint8_t req_buf[4];
        nipc_cgroups_req_t req = { .layout_version = 1, .flags = 0 };
        nipc_cgroups_req_encode(&req, req_buf, sizeof(req_buf));
        (void)nipc_np_send(&session, &good, req_buf, sizeof(req_buf));

        uint8_t recv_buf[4096];
        nipc_header_t resp_hdr;
        const void *payload = NULL;
        size_t payload_len = 0;
        check("recv after non-request fails",
              nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                              &resp_hdr, &payload, &payload_len) != NIPC_NP_OK);

        nipc_np_close_session(&session);
    }

    nipc_client_ctx_t verify_client;
    nipc_client_init(&verify_client, TEST_RUN_DIR, service, &ccfg);
    check("verify client ready", refresh_until_ready(&verify_client, 100, 10));
    if (nipc_client_ready(&verify_client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&verify_client, &view);
        check("normal call succeeds after bad session", err == NIPC_OK);
        check("response item_count == 3", err == NIPC_OK && view.item_count == 3);
    }
    nipc_client_close(&verify_client);

    stop_server(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_client_lifecycle();
    test_client_lifecycle_ready();
    test_multi_method_calls();
    test_retry_on_broken_session();
    test_handler_failure();
    test_client_auth_failure();
    test_client_incompatible();
    test_status_reporting();
    test_cache_refresh_preserves();
    test_cache_reconnect_rebuilds();
    test_cache_empty_snapshot();
    test_non_request_terminates_session();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows service tests skipped (not Windows)\n");
    return 0;
}

#endif
