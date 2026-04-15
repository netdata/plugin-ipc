/*
 * test_win_service_payload_limits.c - Windows managed-service payload limits.
 *
 * This target covers server-side payload-limit paths that are hard to hit from
 * the broad guard fixtures without making those already-large files bigger.
 */

#if defined(_WIN32) || defined(__MSYS__)

#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "C:\\Temp\\nipc_win_service_payload_limits"
#define RESPONSE_BUF_SIZE 65536

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;

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

static nipc_client_config_t default_typed_client_config(void)
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
    nipc_client_config_t cfg = default_typed_client_config();
    cfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    cfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    return cfg;
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

static nipc_cgroups_service_handler_t snapshot_one_service_handler = {
    .handle = on_snapshot_one,
    .snapshot_max_items = 1,
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
                                              ctx->worker_count,
                                              &ctx->service_handler);
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
                                 const nipc_server_config_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = 4;
    ctx->config = *config;
    ctx->service_handler = snapshot_one_service_handler;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->init_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, managed_server_thread, ctx, 0, NULL);
    check("payload-limit server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("payload-limit server ready event", wr == WAIT_OBJECT_0);
    check("payload-limit server init ok",
          InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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
    check("payload-limit server thread exited",
          WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("payload-limit server drain completed",
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

static void test_response_limit_reconnect(bool hybrid)
{
    printf("--- Windows %s response payload limit reconnect ---\n",
           hybrid ? "hybrid" : "baseline");

    char service[64];
    unique_service(service, sizeof(service),
                   hybrid ? "svc_win_hybrid_resp_limit" : "svc_win_resp_limit");

    server_thread_ctx_t sctx;
    nipc_server_config_t scfg = hybrid
        ? default_typed_hybrid_server_config()
        : default_typed_server_config();
    scfg.max_response_payload_bytes = 16;

    HANDLE server_thread = start_server_named(&sctx, service, &scfg);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = hybrid
        ? default_typed_hybrid_client_config()
        : default_typed_client_config();
    ccfg.max_response_payload_bytes = 16;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    check("payload-limit client negotiated expected profile",
          refresh_until_ready(&client, 200, 10) &&
          (hybrid ? client.shm != NULL : client.shm == NULL));

    if (nipc_client_ready(&client)) {
        uint32_t before_response_cap = client.session.max_response_payload_bytes;
        nipc_cgroups_resp_view_t view = {0};
        nipc_client_status_t status = {0};

        check("payload-limit initial response cap is constrained",
              before_response_cap == 16u);

        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("payload-limit snapshot recovers after limit response",
              err == NIPC_OK && view.item_count == 1);

        nipc_client_status(&client, &status);
        check("payload-limit reconnects after limit response",
              status.reconnect_count >= 1);
        check("payload-limit negotiated response cap grows",
              client.session.max_response_payload_bytes > before_response_cap);
        check("payload-limit client remains ready on selected profile",
              nipc_client_ready(&client) &&
              (hybrid ? client.shm != NULL : client.shm == NULL));
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Payload Limit Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_response_limit_reconnect(false);
    test_response_limit_reconnect(true);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows service payload limit tests skipped (not Windows)\n");
    return 0;
}

#endif
