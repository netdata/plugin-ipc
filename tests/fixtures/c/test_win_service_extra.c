/*
 * test_win_service_extra.c - Focused Windows L2 coverage tests.
 *
 * Keeps retry/cache/init coverage separate from the main Windows service test
 * so the default suite stays stable while these slower paths remain covered.
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

static nipc_cgroups_handlers_t full_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = NULL,
    .on_cgroups_snapshot = on_snapshot,
    .snapshot_max_items = 3,
    .user = NULL,
};

static nipc_cgroups_handlers_t empty_snapshot_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = NULL,
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

static void stop_server_destroy(server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    check("server thread exited", WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    nipc_server_destroy(&ctx->server);
    check("server destroy completed", 1);
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

static void test_client_init_defaults_and_truncation(void)
{
    printf("--- Client init defaults / truncation ---\n");

    char run_dir[512];
    char service_name[256];
    memset(run_dir, 'r', sizeof(run_dir) - 1);
    memset(service_name, 's', sizeof(service_name) - 1);
    run_dir[sizeof(run_dir) - 1] = '\0';
    service_name[sizeof(service_name) - 1] = '\0';

    nipc_np_client_config_t ccfg = {0};
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service_name, &ccfg);

    check("request buffer default size", client.request_buf_size == 65536u);
    check("response buffer default size", client.response_buf_size == 65536u + NIPC_HEADER_LEN);
    check("send buffer default size", client.send_buf_size == 65536u + NIPC_HEADER_LEN);
    check("run_dir truncated", strlen(client.run_dir) == sizeof(client.run_dir) - 1);
    check("service_name truncated", strlen(client.service_name) == sizeof(client.service_name) - 1);
    check("run_dir NUL-terminated", client.run_dir[sizeof(client.run_dir) - 1] == '\0');
    check("service_name NUL-terminated", client.service_name[sizeof(client.service_name) - 1] == '\0');

    nipc_client_close(&client);
}

static void test_refresh_from_broken_state(void)
{
    printf("--- Refresh from BROKEN state ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_broken");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4, &full_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_client_config();
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
    stop_server_destroy(&sctx, server_thread);
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
        check("retry increments reconnect_count", status.reconnect_count >= 1);
    }

    nipc_client_close(&client);
    stop_server_destroy(&sctx, server_thread);
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
    stop_server_destroy(&sctx, server_thread);

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
    check("hash table built", cache.buckets != NULL && cache.bucket_count > 0);

    if (cache.client.session_valid) {
        nipc_np_close_session(&cache.client.session);
        cache.client.session_valid = false;
    }

    check("refresh after reconnect ok", nipc_cgroups_cache_refresh(&cache));
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
    stop_server_destroy(&sctx, server_thread);
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
    stop_server_destroy(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Extra Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_client_init_defaults_and_truncation();
    test_refresh_from_broken_state();
    test_retry_on_broken_session();
    test_cache_refresh_preserves();
    test_cache_reconnect_rebuilds();
    test_cache_empty_snapshot();

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
