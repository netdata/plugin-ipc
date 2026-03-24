/*
 * test_win_service_guards.c - Coverage-only Windows L2 client guard tests.
 *
 * These ordinary guard paths raise Windows C service coverage, but folding
 * them into test_win_service_extra.exe perturbs the timing of the normal
 * non-coverage build. Keep them in a dedicated executable that only the
 * Windows C coverage script runs.
 */

#ifdef _WIN32

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;

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

static bool on_increment(void *user, uint64_t request, uint64_t *response)
{
    (void)user;
    *response = request + 1;
    return true;
}

static nipc_cgroups_handlers_t guard_handlers = {
    .on_increment = on_increment,
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

static HANDLE start_default_server_named(server_thread_ctx_t *ctx,
                                         const char *service,
                                         int worker_count)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = worker_count;
    ctx->config = default_server_config();
    ctx->handlers = guard_handlers;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->init_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, managed_server_thread, ctx, 0, NULL);
    check("guard server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("guard server ready event", wr == WAIT_OBJECT_0);
    check("guard server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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
    check("guard server thread exited",
          WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("guard server drain completed",
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

static void test_client_batch_and_string_guards(void)
{
    printf("--- Client batch / string guard coverage ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_client_guards");

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_default_server_named(&sctx, service, 4);
    if (!server_thread)
        return;

    {
        nipc_client_ctx_t client;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard empty-batch client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = nipc_client_call_increment_batch(&client, NULL, 0, NULL);
            check("empty increment batch succeeds", err == NIPC_OK);

            nipc_client_status_t status;
            nipc_client_status(&client, &status);
            check("empty increment batch increments call_count",
                  status.call_count == 1);
            check("empty increment batch keeps error_count at 0",
                  status.error_count == 0);
        }

        nipc_client_close(&client);
    }

    {
        nipc_client_ctx_t client;
        nipc_np_client_config_t ccfg = default_client_config();
        uint64_t req = 7;
        uint64_t resp = 0;

        ccfg.max_request_payload_bytes = 8;
        ccfg.max_request_batch_items = 1;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard batch-overflow client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = nipc_client_call_increment_batch(&client, &req, 1, &resp);
            check("increment batch detects tiny request buffer overflow",
                  err == NIPC_ERR_OVERFLOW);
        }

        nipc_client_close(&client);
    }

    {
        nipc_client_ctx_t client;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_string_reverse_view_t view;

        ccfg.max_request_payload_bytes = 8;
        ccfg.max_request_batch_items = 1;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard string-overflow client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = nipc_client_call_string_reverse(
                &client, "this request is intentionally too large", 38, &view);
            check("string reverse detects tiny request buffer overflow",
                  err == NIPC_ERR_OVERFLOW);
        }

        nipc_client_close(&client);
    }

    {
        nipc_client_ctx_t client;
        nipc_np_client_config_t ccfg = default_client_config();
        nipc_string_reverse_view_t view;

        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard missing-string-handler client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = nipc_client_call_string_reverse(
                &client, "abc", 3, &view);
            check("missing string handler propagates failure", err != NIPC_OK);
        }

        nipc_client_close(&client);
    }

    stop_server_drain(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Guard Coverage Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_client_batch_and_string_guards();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows service guard coverage tests skipped (not Windows)\n");
    return 0;
}

#endif
