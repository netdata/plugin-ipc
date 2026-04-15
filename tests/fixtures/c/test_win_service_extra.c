/*
 * test_win_service_extra.c - Focused Windows L2 coverage tests.
 *
 * Keeps retry/cache/init coverage separate from the main Windows service test
 * so the default suite stays stable while these slower paths remain covered.
 */

#if defined(_WIN32) || defined(__MSYS__)

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"
#include "netipc/netipc_win_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;
static const char *g_test_filter = NULL;

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "C:\\Temp\\nipc_win_service_extra"
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

static int should_run_test(const char *name)
{
    if (!g_test_filter || !*g_test_filter)
        return 1;

    return strstr(name, g_test_filter) != NULL;
}

#define RUN_TEST(fn)                                                         \
    do {                                                                     \
        if (should_run_test(#fn))                                            \
            fn();                                                            \
    } while (0)

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

static nipc_server_config_t default_typed_server_config(void)
{
    return (nipc_server_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
    };
}

static nipc_client_config_t default_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .preferred_profiles         = 0,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
    };
}

static nipc_server_config_t default_typed_hybrid_server_config(void)
{
    nipc_server_config_t cfg = default_typed_server_config();
    cfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    cfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    return cfg;
}

static nipc_client_config_t default_typed_hybrid_client_config(void)
{
    nipc_client_config_t cfg = default_client_config();
    cfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    cfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    return cfg;
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

static bool on_snapshot_empty(void *user,
                              const nipc_cgroups_req_t *request,
                              nipc_cgroups_builder_t *builder)
{
    (void)user;
    (void)builder;
    return request->layout_version == 1 && request->flags == 0;
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

static nipc_cgroups_service_handler_t full_service_handler = {
    .handle = on_snapshot,
    .snapshot_max_items = 3,
    .user = NULL,
};

static nipc_cgroups_service_handler_t empty_snapshot_service_handler = {
    .handle = on_snapshot_empty,
    .snapshot_max_items = 1,
    .user = NULL,
};

static nipc_cgroups_service_handler_t failing_snapshot_service_handler = {
    .handle = on_snapshot_fail,
    .snapshot_max_items = 3,
    .user = NULL,
};

typedef struct {
    char service[64];
    int worker_count;
    nipc_server_config_t config;
    nipc_cgroups_service_handler_t service_handler;
    HANDLE ready_event;
    volatile LONG init_ok;
    nipc_managed_server_t server;
} server_thread_ctx_t;

static DWORD WINAPI managed_server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_error_t err = nipc_server_init_typed(&ctx->server, TEST_RUN_DIR,
                                              ctx->service, &ctx->config,
                                              ctx->worker_count, &ctx->service_handler);
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
    ctx->config = *config;
    ctx->service_handler = *service_handler;
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
                                         const nipc_cgroups_service_handler_t *service_handler)
{
    nipc_server_config_t config = default_typed_server_config();
    return start_server_named(ctx, service, worker_count, &config, service_handler);
}

static void stop_server_drain(server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    check("server thread exited", WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("server drain completed", nipc_server_drain(&ctx->server, 10000));
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

static bool refresh_until_state(nipc_client_ctx_t *client,
                                nipc_client_state_t target_state,
                                int max_tries,
                                DWORD sleep_ms)
{
    for (int i = 0; i < max_tries; i++) {
        nipc_client_refresh(client);
        if (client->state == target_state)
            return true;
        Sleep(sleep_ms);
    }

    return client->state == target_state;
}

static void clear_test_faults(void)
{
    nipc_win_service_test_fault_clear();
    nipc_win_shm_test_fault_clear();
}

static void test_client_init_defaults_and_truncation(void)
{
    printf("--- Client init defaults / truncation ---\n");

    char run_dir[512];
    char service_name[256];
    memset(run_dir, 'r', sizeof(run_dir) - 1);
    memset(service_name, 's', sizeof(service_name) - 1);
    run_dir[sizeof(run_dir) - 1] = '\0';
    service_name[sizeof(service_name) - 1] = '\0';

    nipc_client_config_t ccfg = {0};
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service_name, &ccfg);

    check("request payload default max", client.transport_config.max_request_payload_bytes == 16u);
    check("response payload default max", client.transport_config.max_response_payload_bytes == 65536u);
    check("response buffer allocated lazily", client.response_buf_size == 0u);
    check("send buffer allocated lazily", client.send_buf_size == 0u);
    check("run_dir truncated", strlen(client.run_dir) == sizeof(client.run_dir) - 1);
    check("service_name truncated", strlen(client.service_name) == sizeof(client.service_name) - 1);
    check("run_dir NUL-terminated", client.run_dir[sizeof(client.run_dir) - 1] == '\0');
    check("service_name NUL-terminated", client.service_name[sizeof(client.service_name) - 1] == '\0');

    nipc_client_close(&client);
}

static void test_client_init_null_config_defaults(void)
{
    printf("--- Client init NULL config defaults ---\n");

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, "svc_client_null_cfg", NULL);

    check("NULL config request payload default max",
          client.transport_config.max_request_payload_bytes == 16u);
    check("NULL config response payload default max",
          client.transport_config.max_response_payload_bytes == 65536u);
    check("NULL config supported_profiles default",
          client.transport_config.supported_profiles == 0u);
    check("NULL config preferred_profiles default",
          client.transport_config.preferred_profiles == 0u);

    nipc_client_close(&client);
}

static void test_server_init_argument_validation(void)
{
    printf("--- Server init argument validation ---\n");

    nipc_managed_server_t server;
    nipc_np_server_config_t raw_scfg = default_server_config();
    nipc_server_config_t typed_scfg = default_typed_server_config();

    check("raw init null run_dir",
          nipc_server_init(&server, NULL, "svc_raw_null_run",
                           &raw_scfg, 1, NIPC_METHOD_INCREMENT,
                           raw_noop_handler, NULL)
              == NIPC_ERR_BAD_LAYOUT);

    check("raw init null service_name",
          nipc_server_init(&server, TEST_RUN_DIR, NULL,
                           &raw_scfg, 1, NIPC_METHOD_INCREMENT,
                           raw_noop_handler, NULL)
              == NIPC_ERR_BAD_LAYOUT);

    check("raw init null handler",
          nipc_server_init(&server, TEST_RUN_DIR, "svc_raw_null_handler",
                           &raw_scfg, 1, NIPC_METHOD_INCREMENT,
                           NULL, NULL)
              == NIPC_ERR_BAD_LAYOUT);

    check("typed init null service handler",
          nipc_server_init_typed(&server, TEST_RUN_DIR, "svc_typed_null_service_handler",
                                 &typed_scfg, 1, NULL)
              == NIPC_ERR_BAD_LAYOUT);

    check("raw init invalid service name",
          nipc_server_init(&server, TEST_RUN_DIR, "svc/raw_bad_name",
                           &raw_scfg, 1, NIPC_METHOD_INCREMENT,
                           raw_noop_handler, NULL)
              == NIPC_ERR_BAD_LAYOUT);
}

static void test_server_init_worker_count_clamp(void)
{
    printf("--- Server init worker_count clamp ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_worker_clamp");

    nipc_managed_server_t server;
    nipc_server_config_t scfg = default_typed_server_config();
    nipc_error_t err = nipc_server_init_typed(&server, TEST_RUN_DIR, service,
                                              &scfg, 0, &full_service_handler);
    check("typed init with worker_count 0 succeeds", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("worker_count clamped to 1", server.worker_count == 1);
        check("session_capacity minimum 16", server.session_capacity == 16);
        nipc_server_destroy(&server);
    }
}

static void test_server_init_null_config_defaults(void)
{
    printf("--- Server init NULL config defaults ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_server_null_cfg");

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init_typed(&server, TEST_RUN_DIR, service,
                                              NULL, 0, &full_service_handler);
    check("typed init with NULL config succeeds", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("typed NULL config request default max",
              server.learned_request_payload_bytes == 16u);
        check("typed NULL config response default max",
              server.learned_response_payload_bytes == 65536u);
        check("typed NULL config worker_count clamped",
              server.worker_count == 1);
        nipc_server_destroy(&server);
    }
}

static void test_client_response_buffer_minimum(void)
{
    printf("--- Client response buffer minimum ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_client_resp_min");

    server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_response_payload_bytes = 1;
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_response_payload_bytes = 1;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    check("small response client reaches READY", refresh_until_ready(&client, 100, 10));
    check("small response client buffer rounded up to minimum",
          client.response_buf_size >= NIPC_HEADER_LEN + 1024u);

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_client_fault_injection_disconnects_and_recovers(void)
{
    printf("--- Client fault injection disconnects / recovers ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_resp_fault");

        server_thread_ctx_t sctx;
        HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_win_service_test_fault_set(
            NIPC_WIN_SERVICE_TEST_FAULT_CLIENT_RESPONSE_BUF_REALLOC, 0);
        check("response buffer alloc fault disconnects client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10));
        check("response buffer alloc fault leaves client not ready",
              !nipc_client_ready(&client));
        clear_test_faults();
        check("response buffer alloc fault recovers",
              refresh_until_ready(&client, 100, 10));

        nipc_client_close(&client);
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_send_fault");

        server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_win_service_test_fault_set(
            NIPC_WIN_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        check("send buffer alloc fault disconnects hybrid client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10));
        clear_test_faults();
        check("send buffer alloc fault recovers",
              refresh_until_ready(&client, 200, 10) && client.shm != NULL);

        nipc_client_close(&client);
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_shm_ctx_fault");

        server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_win_service_test_fault_set(
            NIPC_WIN_SERVICE_TEST_FAULT_CLIENT_SHM_CTX_CALLOC, 0);
        check("client SHM ctx alloc fault disconnects hybrid client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10));
        check("client SHM ctx alloc fault leaves session invalid",
              !client.session_valid);
        clear_test_faults();
        check("client SHM ctx alloc fault recovers",
              refresh_until_ready(&client, 200, 10) && client.shm != NULL);

        nipc_client_close(&client);
        stop_server_drain(&sctx, server_thread);
    }
}

static void test_server_init_fault_injection(void)
{
    printf("--- Server init fault injection ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_server_init_fault");

    nipc_managed_server_t server;
    nipc_server_config_t scfg = default_typed_server_config();

    nipc_win_service_test_fault_set(
        NIPC_WIN_SERVICE_TEST_FAULT_SERVER_SESSIONS_CALLOC, 0);
    check("server session array alloc fault returns overflow",
          nipc_server_init_typed(&server, TEST_RUN_DIR, service,
                                 &scfg, 4, &full_service_handler)
          == NIPC_ERR_OVERFLOW);
    clear_test_faults();
}

static void test_refresh_from_broken_state(void)
{
    printf("--- Refresh from BROKEN state ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_broken");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client reaches READY", refresh_until_ready(&client, 100, 10));

    if (nipc_client_ready(&client)) {
        if (client.session_valid) {
            nipc_np_close_session(&client.session);
            client.session_valid = false;
        }
        client.state = NIPC_CLIENT_BROKEN;

        check("refresh from BROKEN changes state", nipc_client_refresh(&client));
        check("refresh from BROKEN returns READY", client.state == NIPC_CLIENT_READY);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("BROKEN refresh increments reconnect_count", status.reconnect_count == 1);
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_retry_on_broken_session(void)
{
    printf("--- Retry after broken session ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_retry");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
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
        check("retry increments reconnect_count", status.reconnect_count >= 1);
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_cache_refresh_without_server(void)
{
    printf("--- Cache refresh without server ---\n");

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    char service[64];
    unique_service(service, sizeof(service), "svc_cache_missing");
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("refresh without server fails", !nipc_cgroups_cache_refresh(&cache));
    check("cache not ready", !nipc_cgroups_cache_ready(&cache));

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("success_count still 0", status.refresh_success_count == 0);
    check("failure_count == 1", status.refresh_failure_count == 1);

    nipc_cgroups_cache_close(&cache);
}

static void test_handler_failure(void)
{
    printf("--- Handler failure ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hfail");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &failing_snapshot_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("client ready", refresh_until_ready(&client, 100, 10));

    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("snapshot fails when handler fails", err != NIPC_OK);
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_client_auth_failure(void)
{
    printf("--- Client auth failure mapping ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_auth");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.auth_token = 0x1111111111111111ull;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    check("state is AUTH_FAILED", client.state == NIPC_CLIENT_AUTH_FAILED);

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_client_incompatible(void)
{
    printf("--- Client incompatible profile mapping ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_incompat");

    nipc_server_config_t scfg = default_typed_server_config();
    scfg.supported_profiles = NIPC_PROFILE_SHM_HYBRID;

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_BASELINE;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    check("state is INCOMPATIBLE", client.state == NIPC_CLIENT_INCOMPATIBLE);

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_cache_refresh_rebuilds_and_linear_lookup(void)
{
    printf("--- Cache refresh rebuilds / linear lookup ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_rebuild");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

    check("first refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("item_count == 3", cache.item_count == 3);
    check("hash table built", cache.buckets != NULL && cache.bucket_count > 0);
    check("second refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("item_count still == 3", cache.item_count == 3);

    free(cache.buckets);
    cache.buckets = NULL;
    cache.bucket_count = 0;
    check("linear lookup hit",
          nipc_cgroups_cache_lookup(&cache, 2002, "k8s-pod-xyz") != NULL);
    check("linear lookup miss",
          nipc_cgroups_cache_lookup(&cache, 9999, "missing") == NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("refresh_success_count == 2", status.refresh_success_count == 2);

    nipc_cgroups_cache_close(&cache);
    stop_server_drain(&sctx, server_thread);
}

static void test_cache_empty_snapshot(void)
{
    printf("--- Cache empty snapshot ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cache_empty");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &empty_snapshot_service_handler);
    if (!server_thread)
        return;

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
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
    stop_server_drain(&sctx, server_thread);
}

static void test_server_shm_create_fault_disconnects_and_recovers(void)
{
    printf("--- Server SHM create fault falls back to baseline / recovers ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_server_shm_fault");

    server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_hybrid_server_config();
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    nipc_win_shm_test_fault_set(NIPC_WIN_SHM_TEST_FAULT_CREATE_MAPPING,
                                ERROR_ACCESS_DENIED, 0);
    check("server SHM create fault reaches READY via baseline",
          refresh_until_ready(&client, 40, 10));
    check("server SHM create fault keeps client ready",
          nipc_client_ready(&client));
    check("server SHM create fault leaves session on baseline",
          client.shm == NULL);
    clear_test_faults();

    nipc_client_close(&client);
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("server SHM create fault recovers after reconnect",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_cache_fault_injection(void)
{
    struct {
        int site;
        const char *label;
        bool expect_refresh_ok;
    } cases[] = {
        { NIPC_WIN_SERVICE_TEST_FAULT_CACHE_ITEMS_CALLOC,
          "cache items alloc fault fails refresh", false },
        { NIPC_WIN_SERVICE_TEST_FAULT_CACHE_ITEM_NAME_MALLOC,
          "cache item name alloc fault fails refresh", false },
        { NIPC_WIN_SERVICE_TEST_FAULT_CACHE_ITEM_PATH_MALLOC,
          "cache item path alloc fault fails refresh", false },
        { NIPC_WIN_SERVICE_TEST_FAULT_CACHE_BUCKETS_CALLOC,
          "cache bucket alloc fault falls back to linear lookup", true },
    };

    printf("--- Cache fault injection ---\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service), "svc_cache_fault");

        server_thread_ctx_t sctx;
        HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_service_handler);
        if (!server_thread)
            return;

        nipc_cgroups_cache_t cache;
        nipc_client_config_t ccfg = default_client_config();
        nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

        nipc_win_service_test_fault_set(cases[i].site, 0);
        bool ok = nipc_cgroups_cache_refresh(&cache);
        check(cases[i].label, ok == cases[i].expect_refresh_ok);
        if (cases[i].expect_refresh_ok) {
            check("cache bucket fault leaves buckets NULL",
                  cache.buckets == NULL && cache.bucket_count == 0);
            check("cache bucket fault still serves lookup",
                  nipc_cgroups_cache_lookup(&cache, 2002, "k8s-pod-xyz") != NULL);
        } else {
            nipc_cgroups_cache_status_t status;
            nipc_cgroups_cache_status(&cache, &status);
            check("cache allocation fault increments failure_count",
                  status.refresh_failure_count == 1);
        }

        clear_test_faults();
        check("cache refresh recovers after fault",
              nipc_cgroups_cache_refresh(&cache));
        check("cache refresh recovery lookup works",
              nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

        nipc_cgroups_cache_close(&cache);
        stop_server_drain(&sctx, server_thread);
    }
}

int main(void)
{
    printf("=== Windows Service Extra Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);
    g_test_filter = getenv("NIPC_TEST_FILTER");

    RUN_TEST(test_client_init_defaults_and_truncation);
    RUN_TEST(test_client_init_null_config_defaults);
    RUN_TEST(test_server_init_argument_validation);
    RUN_TEST(test_server_init_worker_count_clamp);
    RUN_TEST(test_server_init_null_config_defaults);
    RUN_TEST(test_client_response_buffer_minimum);
    RUN_TEST(test_client_fault_injection_disconnects_and_recovers);
    RUN_TEST(test_server_init_fault_injection);
    RUN_TEST(test_refresh_from_broken_state);
    RUN_TEST(test_retry_on_broken_session);
    RUN_TEST(test_handler_failure);
    RUN_TEST(test_client_auth_failure);
    RUN_TEST(test_client_incompatible);
    RUN_TEST(test_cache_refresh_without_server);
    RUN_TEST(test_cache_refresh_rebuilds_and_linear_lookup);
    RUN_TEST(test_cache_empty_snapshot);
    RUN_TEST(test_server_shm_create_fault_disconnects_and_recovers);
    RUN_TEST(test_cache_fault_injection);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows service extra tests skipped (not Windows)\n");
    return 0;
}

#endif
