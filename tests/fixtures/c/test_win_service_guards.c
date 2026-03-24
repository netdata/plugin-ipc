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

static nipc_np_server_config_t default_hybrid_server_config(void)
{
    nipc_np_server_config_t cfg = default_server_config();
    cfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    cfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    return cfg;
}

static nipc_np_client_config_t default_hybrid_client_config(void)
{
    nipc_np_client_config_t cfg = default_client_config();
    cfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    cfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    return cfg;
}

static size_t build_message(uint8_t *dst, size_t dst_size,
                            uint16_t kind, uint16_t code,
                            uint64_t message_id,
                            const void *payload, size_t payload_len)
{
    if (dst_size < NIPC_HEADER_LEN + payload_len)
        return 0;

    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = kind,
        .code = code,
        .flags = 0,
        .item_count = 1,
        .message_id = message_id,
        .payload_len = (uint32_t)payload_len,
        .transport_status = NIPC_STATUS_OK,
    };

    nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    if (payload_len > 0)
        memcpy(dst + NIPC_HEADER_LEN, payload, payload_len);

    return NIPC_HEADER_LEN + payload_len;
}

static bool on_increment(void *user, uint64_t request, uint64_t *response)
{
    (void)user;
    *response = request + 1;
    return true;
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

static nipc_cgroups_handlers_t guard_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = NULL,
    .on_cgroups_snapshot = NULL,
    .snapshot_max_items = 0,
    .user = NULL,
};

static nipc_cgroups_handlers_t hybrid_snapshot_handlers = {
    .on_increment = on_increment,
    .on_string_reverse = NULL,
    .on_cgroups_snapshot = on_snapshot_one,
    .snapshot_max_items = 0,
    .user = NULL,
};

typedef enum {
    FAKE_HYBRID_ATTACH_BAD_PROFILE = 1,
    FAKE_HYBRID_RESP_SHORT,
    FAKE_HYBRID_RESP_BAD_MAGIC,
    FAKE_HYBRID_RESP_BAD_KIND,
    FAKE_HYBRID_RESP_BAD_CODE,
    FAKE_HYBRID_RESP_BAD_MESSAGE_ID,
} fake_hybrid_mode_t;

typedef struct {
    char service[64];
    fake_hybrid_mode_t mode;
    HANDLE ready_event;
    volatile LONG listen_ok;
} fake_hybrid_server_ctx_t;

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

static HANDLE start_default_server_named(server_thread_ctx_t *ctx,
                                         const char *service,
                                         int worker_count)
{
    nipc_np_server_config_t scfg = default_server_config();
    return start_server_named(ctx, service, worker_count, &scfg, &guard_handlers);
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

static DWORD WINAPI fake_hybrid_server_thread(LPVOID arg)
{
    fake_hybrid_server_ctx_t *ctx = (fake_hybrid_server_ctx_t *)arg;
    nipc_np_listener_t listener = {0};
    nipc_np_session_t session = {0};
    nipc_win_shm_ctx_t shm = {0};
    nipc_np_server_config_t scfg = default_hybrid_server_config();

    listener.pipe = INVALID_HANDLE_VALUE;
    session.pipe = INVALID_HANDLE_VALUE;

    if (nipc_np_listen(TEST_RUN_DIR, ctx->service, &scfg, &listener) != NIPC_NP_OK) {
        InterlockedExchange(&ctx->listen_ok, 0);
        SetEvent(ctx->ready_event);
        return 1;
    }

    InterlockedExchange(&ctx->listen_ok, 1);
    SetEvent(ctx->ready_event);

    if (nipc_np_accept(&listener, 1, &session) != NIPC_NP_OK)
        goto out;

    if (session.selected_profile != NIPC_WIN_SHM_PROFILE_HYBRID)
        goto out;

    if (nipc_win_shm_server_create(TEST_RUN_DIR, ctx->service, AUTH_TOKEN,
                                   session.session_id,
                                   session.selected_profile,
                                   session.max_request_payload_bytes + NIPC_HEADER_LEN,
                                   session.max_response_payload_bytes + NIPC_HEADER_LEN,
                                   &shm) != NIPC_WIN_SHM_OK)
        goto out;

    if (ctx->mode == FAKE_HYBRID_ATTACH_BAD_PROFILE) {
        nipc_win_shm_region_header_t *hdr =
            (nipc_win_shm_region_header_t *)shm.base;
        hdr->profile = NIPC_WIN_SHM_PROFILE_BUSYWAIT;
        MemoryBarrier();
        Sleep(1500);
        goto out;
    }

    {
        uint8_t req_buf[4096];
        size_t req_len = 0;
        if (nipc_win_shm_receive(&shm, req_buf, sizeof(req_buf), &req_len, 10000)
            != NIPC_WIN_SHM_OK)
            goto out;

        nipc_header_t req_hdr = {0};
        if (req_len >= NIPC_HEADER_LEN)
            nipc_header_decode(req_buf, req_len, &req_hdr);

        switch (ctx->mode) {
        case FAKE_HYBRID_RESP_SHORT: {
            uint8_t msg[8] = {0};
            (void)nipc_win_shm_send(&shm, msg, sizeof(msg));
            break;
        }
        case FAKE_HYBRID_RESP_BAD_MAGIC: {
            uint8_t msg[NIPC_HEADER_LEN] = {0};
            (void)nipc_win_shm_send(&shm, msg, sizeof(msg));
            break;
        }
        case FAKE_HYBRID_RESP_BAD_KIND: {
            uint8_t msg[NIPC_HEADER_LEN];
            size_t msg_len = build_message(msg, sizeof(msg),
                                           NIPC_KIND_REQUEST,
                                           req_hdr.code,
                                           req_hdr.message_id,
                                           NULL, 0);
            (void)nipc_win_shm_send(&shm, msg, msg_len);
            break;
        }
        case FAKE_HYBRID_RESP_BAD_CODE: {
            uint8_t msg[NIPC_HEADER_LEN];
            size_t msg_len = build_message(msg, sizeof(msg),
                                           NIPC_KIND_RESPONSE,
                                           (uint16_t)(req_hdr.code + 1),
                                           req_hdr.message_id,
                                           NULL, 0);
            (void)nipc_win_shm_send(&shm, msg, msg_len);
            break;
        }
        case FAKE_HYBRID_RESP_BAD_MESSAGE_ID: {
            uint8_t msg[NIPC_HEADER_LEN];
            size_t msg_len = build_message(msg, sizeof(msg),
                                           NIPC_KIND_RESPONSE,
                                           req_hdr.code,
                                           req_hdr.message_id + 1,
                                           NULL, 0);
            (void)nipc_win_shm_send(&shm, msg, msg_len);
            break;
        }
        default:
            break;
        }
    }

out:
    nipc_win_shm_destroy(&shm);
    nipc_np_close_session(&session);
    nipc_np_close_listener(&listener);
    return 0;
}

static HANDLE start_fake_hybrid_server(fake_hybrid_server_ctx_t *ctx,
                                       const char *service,
                                       fake_hybrid_mode_t mode)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->mode = mode;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->listen_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, fake_hybrid_server_thread, ctx, 0, NULL);
    check("fake hybrid server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("fake hybrid server ready event", wr == WAIT_OBJECT_0);
    check("fake hybrid server listen ok",
          InterlockedCompareExchange(&ctx->listen_ok, 0, 0) == 1);
    CloseHandle(ctx->ready_event);
    ctx->ready_event = NULL;

    if (wr != WAIT_OBJECT_0 ||
        InterlockedCompareExchange(&ctx->listen_ok, 0, 0) != 1) {
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
        return NULL;
    }

    return thread;
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

static void test_hybrid_attach_failure_disconnects(void)
{
    printf("--- Hybrid attach failure disconnects client ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_attach");

    fake_hybrid_server_ctx_t sctx;
    HANDLE server_thread = start_fake_hybrid_server(
        &sctx, service, FAKE_HYBRID_ATTACH_BAD_PROFILE);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    (void)nipc_client_refresh(&client);
    check("hybrid attach failure leaves client not ready",
          !nipc_client_ready(&client));
    check("hybrid attach failure maps to DISCONNECTED",
          client.state == NIPC_CLIENT_DISCONNECTED);

    nipc_client_close(&client);
    check("fake hybrid attach server exited",
          WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(server_thread);
}

static void test_hybrid_client_rejects_malformed_responses(void)
{
    struct {
        fake_hybrid_mode_t mode;
        const char *label;
        nipc_error_t expected;
    } cases[] = {
        { FAKE_HYBRID_RESP_SHORT, "short SHM response", NIPC_ERR_TRUNCATED },
        { FAKE_HYBRID_RESP_BAD_MAGIC, "bad SHM response header", NIPC_ERR_BAD_MAGIC },
        { FAKE_HYBRID_RESP_BAD_KIND, "wrong SHM response kind", NIPC_ERR_BAD_KIND },
        { FAKE_HYBRID_RESP_BAD_CODE, "wrong SHM response code", NIPC_ERR_BAD_LAYOUT },
        { FAKE_HYBRID_RESP_BAD_MESSAGE_ID, "wrong SHM response message_id", NIPC_ERR_BAD_LAYOUT },
    };

    printf("--- Hybrid client rejects malformed SHM responses ---\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service), "svc_hybrid_resp");

        fake_hybrid_server_ctx_t sctx;
        HANDLE server_thread = start_fake_hybrid_server(&sctx, service, cases[i].mode);
        if (!server_thread)
            continue;

        nipc_client_ctx_t client;
        nipc_np_client_config_t ccfg = default_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check(cases[i].label, refresh_until_ready(&client, 200, 10) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            uint64_t value_out = 0;
            nipc_error_t err = nipc_client_call_increment(&client, 41, &value_out);
            check("malformed hybrid response returns expected error",
                  err == cases[i].expected);
            check("malformed hybrid response leaves client not READY",
                  !nipc_client_ready(&client));
        }

        nipc_client_close(&client);
        check("fake hybrid response server exited",
              WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);
        CloseHandle(server_thread);
    }
}

static void test_hybrid_server_rejects_malformed_requests(void)
{
    struct {
        const char *label;
        uint8_t msg[NIPC_HEADER_LEN];
        size_t msg_len;
    } cases[] = {
        { "short SHM request", {0}, 8 },
        { "bad SHM request header", {0}, NIPC_HEADER_LEN },
        { "wrong SHM request kind", {0}, NIPC_HEADER_LEN },
    };

    printf("--- Hybrid server rejects malformed SHM requests ---\n");

    cases[1].msg_len = NIPC_HEADER_LEN;
    memset(cases[1].msg, 0, sizeof(cases[1].msg));
    (void)build_message(cases[2].msg, sizeof(cases[2].msg),
                        NIPC_KIND_RESPONSE, NIPC_METHOD_INCREMENT, 7, NULL, 0);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service), "svc_hybrid_req");

        server_thread_ctx_t sctx;
        nipc_np_server_config_t scfg = default_hybrid_server_config();
        HANDLE server_thread = start_server_named(
            &sctx, service, 4, &scfg, &guard_handlers);
        if (!server_thread)
            continue;

        nipc_client_ctx_t bad_client;
        nipc_np_client_config_t ccfg = default_hybrid_client_config();
        nipc_client_init(&bad_client, TEST_RUN_DIR, service, &ccfg);
        check(cases[i].label,
              refresh_until_ready(&bad_client, 200, 10) && bad_client.shm != NULL);

        if (nipc_client_ready(&bad_client) && bad_client.shm != NULL) {
            check("send malformed hybrid request",
                  nipc_win_shm_send(bad_client.shm, cases[i].msg, cases[i].msg_len)
                  == NIPC_WIN_SHM_OK);
            Sleep(100);
        }
        nipc_client_close(&bad_client);

        nipc_client_ctx_t good_client;
        nipc_client_init(&good_client, TEST_RUN_DIR, service, &ccfg);
        check("replacement hybrid client ready",
              refresh_until_ready(&good_client, 200, 10) && good_client.shm != NULL);
        if (nipc_client_ready(&good_client) && good_client.shm != NULL) {
            uint64_t value_out = 0;
            nipc_error_t err = nipc_client_call_increment(&good_client, 9, &value_out);
            check("replacement hybrid increment succeeds",
                  err == NIPC_OK && value_out == 10);
        }
        nipc_client_close(&good_client);

        stop_server_drain(&sctx, server_thread);
    }
}

static void test_snapshot_default_max_items(void)
{
    printf("--- Snapshot default max-items path ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_snapshot_default");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_hybrid_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 4, &scfg, &hybrid_snapshot_handlers);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_np_client_config_t ccfg = default_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("snapshot default client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("snapshot default call ok", err == NIPC_OK);
        check("snapshot default item_count == 1",
              err == NIPC_OK && view.item_count == 1);
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

int main(void)
{
    printf("=== Windows Service Guard Coverage Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_client_batch_and_string_guards();
    test_hybrid_attach_failure_disconnects();
    test_hybrid_client_rejects_malformed_responses();
    test_hybrid_server_rejects_malformed_requests();
    test_snapshot_default_max_items();

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
