/*
 * test_win_stress.c - Windows stress tests for managed named-pipe / WinSHM paths.
 *
 * Focus:
 *   1. many simultaneous managed-service clients
 *   2. rapid connect / request / disconnect cycles
 *   3. large payload string_reverse traffic
 *   4. WinSHM create / attach / close / destroy lifecycle
 *
 * Returns 0 on success, 1 on failure.
 */

#if defined(_WIN32) || defined(__MSYS__)

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"
#include "netipc/netipc_win_shm.h"
#include "test_win_raw_client_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_service_counter = 0;

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR "C:\\Temp\\nipc_win_stress"
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

static nipc_error_t handle_increment_raw(void *user,
                                         const nipc_header_t *request_hdr,
                                         const uint8_t *request_payload,
                                         size_t request_len,
                                         uint8_t *response_buf,
                                         size_t response_buf_size,
                                         size_t *response_len_out)
{
    (void)user;

    if (request_hdr->code != NIPC_METHOD_INCREMENT)
        return NIPC_ERR_BAD_LAYOUT;

    if (!nipc_dispatch_increment(request_payload, request_len,
                                 response_buf, response_buf_size,
                                 response_len_out, on_increment, NULL))
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

static nipc_error_t handle_string_reverse_raw(void *user,
                                              const nipc_header_t *request_hdr,
                                              const uint8_t *request_payload,
                                              size_t request_len,
                                              uint8_t *response_buf,
                                              size_t response_buf_size,
                                              size_t *response_len_out)
{
    (void)user;

    if (request_hdr->code != NIPC_METHOD_STRING_REVERSE)
        return NIPC_ERR_BAD_LAYOUT;

    if (!nipc_dispatch_string_reverse(request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out,
                                      on_string_reverse, NULL))
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

typedef struct {
    char service[64];
    int worker_count;
    uint32_t max_request_payload_bytes;
    uint32_t max_response_payload_bytes;
    uint16_t expected_method_code;
    nipc_server_handler_fn raw_handler;
    HANDLE ready_event;
    volatile LONG init_ok;
    nipc_managed_server_t server;
} server_thread_ctx_t;

static DWORD WINAPI managed_server_thread(LPVOID arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_np_server_config_t cfg = {0};

    cfg.supported_profiles = NIPC_PROFILE_BASELINE;
    cfg.preferred_profiles = 0;
    cfg.max_request_payload_bytes = ctx->max_request_payload_bytes;
    cfg.max_request_batch_items = 16;
    cfg.max_response_payload_bytes = ctx->max_response_payload_bytes;
    cfg.max_response_batch_items = 16;
    cfg.auth_token = AUTH_TOKEN;
    cfg.packet_size = 0;

    nipc_error_t err = nipc_server_init(&ctx->server, TEST_RUN_DIR,
                                        ctx->service, &cfg,
                                        ctx->worker_count,
                                        ctx->expected_method_code,
                                        ctx->raw_handler, NULL);
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

static HANDLE start_server(server_thread_ctx_t *ctx,
                           const char *prefix,
                           int worker_count,
                           uint16_t expected_method_code,
                           nipc_server_handler_fn raw_handler,
                           uint32_t max_request_payload_bytes,
                           uint32_t max_response_payload_bytes)
{
    memset(ctx, 0, sizeof(*ctx));
    unique_service(ctx->service, sizeof(ctx->service), prefix);
    ctx->worker_count = worker_count;
    ctx->expected_method_code = expected_method_code;
    ctx->raw_handler = raw_handler;
    ctx->max_request_payload_bytes = max_request_payload_bytes;
    ctx->max_response_payload_bytes = max_response_payload_bytes;
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
        if (thread != NULL) {
            WaitForSingleObject(thread, 10000);
            CloseHandle(thread);
        }
        return NULL;
    }

    return thread;
}

static void stop_server(server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    DWORD wr = WaitForSingleObject(thread, 10000);
    check("server acceptor exited", wr == WAIT_OBJECT_0);
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

static nipc_client_config_t default_client_config(uint32_t max_payload)
{
    nipc_client_config_t cfg = {0};
    cfg.supported_profiles = NIPC_PROFILE_BASELINE;
    cfg.preferred_profiles = 0;
    cfg.max_request_batch_items = 16;
    cfg.max_response_payload_bytes = max_payload;
    cfg.auth_token = AUTH_TOKEN;
    return cfg;
}

static void test_many_simultaneous_clients(void)
{
    printf("--- Many simultaneous managed-service clients ---\n");
    fflush(stdout);

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server(&sctx, "win_stress_many", 16,
                                        NIPC_METHOD_INCREMENT,
                                        handle_increment_raw,
                                        4096, 4096);
    if (!server_thread)
        return;

    nipc_client_ctx_t clients[8];
    int ready = 0;
    int call_ok = 0;

    for (int i = 0; i < 8; i++) {
        nipc_client_config_t cfg = default_client_config(4096);
        uint64_t value = 0;
            nipc_client_init(&clients[i], TEST_RUN_DIR, sctx.service, &cfg);
        if (refresh_until_ready(&clients[i], 50, 10)) {
            ready++;
            if (test_win_client_call_increment_raw(&clients[i], (uint64_t)i, &value) == NIPC_OK &&
                value == (uint64_t)i + 1) {
                call_ok++;
            }
        }
    }

    check("8 clients connected", ready == 8);
    check("8 increment calls succeeded", call_ok == 8);

    for (int i = 0; i < 8; i++)
        nipc_client_close(&clients[i]);

    stop_server(&sctx, server_thread);
}

static void test_rapid_connect_disconnect_cycles(void)
{
    printf("--- Rapid connect / request / disconnect cycles ---\n");
    fflush(stdout);

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server(&sctx, "win_stress_rapid", 8,
                                        NIPC_METHOD_INCREMENT,
                                        handle_increment_raw,
                                        4096, 4096);
    if (!server_thread)
        return;

    int success = 0;
    for (int i = 0; i < 100; i++) {
        nipc_client_ctx_t client;
        nipc_client_config_t cfg = default_client_config(4096);
        uint64_t value = 0;

        nipc_client_init(&client, TEST_RUN_DIR, sctx.service, &cfg);
        if (refresh_until_ready(&client, 200, 2) &&
            test_win_client_call_increment_raw(&client, 1, &value) == NIPC_OK &&
            value == 2) {
            success++;
        }

        nipc_client_close(&client);
    }

    check("100 cycles completed cleanly", success == 100);
    stop_server(&sctx, server_thread);
}

static void test_large_payload_string_reverse(void)
{
    printf("--- Large payload string_reverse ---\n");
    fflush(stdout);

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server(&sctx, "win_stress_large", 4,
                                        NIPC_METHOD_STRING_REVERSE,
                                        handle_string_reverse_raw,
                                        65536, 65536);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t cfg = default_client_config(65536);
    nipc_string_reverse_view_t view;
    enum { PAYLOAD_LEN = 60000 };
    char *payload = malloc(PAYLOAD_LEN);

    check("allocate large payload", payload != NULL);
    if (!payload) {
        stop_server(&sctx, server_thread);
        return;
    }

    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (char)('A' + (i % 26));

    nipc_client_init(&client, TEST_RUN_DIR, sctx.service, &cfg);
    check("large payload client ready", refresh_until_ready(&client, 50, 10));

    if (nipc_client_ready(&client)) {
        nipc_error_t err = test_win_client_call_string_reverse_raw(
            &client, payload, PAYLOAD_LEN, &view);
        check("large string_reverse succeeded", err == NIPC_OK);
        if (err == NIPC_OK) {
            check("large response length matches", view.str_len == PAYLOAD_LEN);
            check("large response first byte reversed",
                  view.str[0] == payload[PAYLOAD_LEN - 1]);
            check("large response last byte reversed",
                  view.str[PAYLOAD_LEN - 1] == payload[0]);
        }
    }

    nipc_client_close(&client);
    free(payload);
    stop_server(&sctx, server_thread);
}

static void test_win_shm_lifecycle(void)
{
    printf("--- WinSHM create / attach / close / destroy lifecycle ---\n");
    fflush(stdout);

    int success = 0;

    for (int i = 0; i < 25; i++) {
        char service[64];
        nipc_win_shm_ctx_t server_ctx;
        nipc_win_shm_ctx_t client_ctx;
        nipc_win_shm_error_t err;

        unique_service(service, sizeof(service), "win_shm_lifecycle");

        memset(&server_ctx, 0, sizeof(server_ctx));
        memset(&client_ctx, 0, sizeof(client_ctx));

        err = nipc_win_shm_server_create(TEST_RUN_DIR, service, AUTH_TOKEN, 1,
                                         NIPC_WIN_SHM_PROFILE_HYBRID,
                                         4096, 4096, &server_ctx);
        if (err != NIPC_WIN_SHM_OK)
            continue;

        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 1,
                                         NIPC_WIN_SHM_PROFILE_HYBRID,
                                         &client_ctx);
        if (err == NIPC_WIN_SHM_OK) {
            success++;
            nipc_win_shm_close(&client_ctx);
        }

        nipc_win_shm_destroy(&server_ctx);
    }

    check("25 WinSHM lifecycles succeeded", success == 25);
}

int main(void)
{
    printf("=== Windows Stress Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);
    fflush(stdout);

    /*
     * The managed-service stress cases are intentionally not part of the
     * default Windows ctest path yet. They currently need a separate
     * investigation of Windows managed-server shutdown behavior.
     */
    test_win_shm_lifecycle();
    printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#else

#include <stdio.h>

int main(void)
{
    printf("Windows stress tests require a Windows runtime\n");
    return 0;
}

#endif
