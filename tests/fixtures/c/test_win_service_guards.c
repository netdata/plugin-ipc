/*
 * test_win_service_guards.c - Coverage-only Windows L2 client guard tests.
 *
 * These ordinary guard paths raise Windows C service coverage, but folding
 * them into test_win_service_extra.exe perturbs the timing of the normal
 * non-coverage build. Keep them in a dedicated executable that only the
 * Windows C coverage script runs.
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

static size_t build_status_message(uint8_t *dst, size_t dst_size,
                                   uint16_t code,
                                   uint64_t message_id,
                                   uint32_t transport_status)
{
    if (dst_size < NIPC_HEADER_LEN)
        return 0;

    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_RESPONSE,
        .code = code,
        .flags = 0,
        .item_count = 1,
        .message_id = message_id,
        .payload_len = 0,
        .transport_status = transport_status,
    };

    nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    return NIPC_HEADER_LEN;
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

static nipc_error_t raw_increment_batch_handler(void *user,
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

    if (!(request_hdr->flags & NIPC_FLAG_BATCH))
        return dispatch_increment_request(request_hdr, request_payload, request_len,
                                          response_buf, response_buf_size,
                                          response_len_out, on_increment);

    if (request_hdr->item_count <= 1)
        return dispatch_increment_request(request_hdr, request_payload, request_len,
                                          response_buf, response_buf_size,
                                          response_len_out, on_increment);

    nipc_batch_builder_t builder;
    nipc_batch_builder_init(&builder, response_buf, response_buf_size,
                            request_hdr->item_count);

    for (uint32_t i = 0; i < request_hdr->item_count; i++) {
        const void *item_ptr = NULL;
        uint32_t item_len = 0;
        nipc_error_t err = nipc_batch_item_get(request_payload, request_len,
                                               request_hdr->item_count, i,
                                               &item_ptr, &item_len);
        if (err != NIPC_OK)
            return err;

        uint64_t value = 0;
        err = nipc_increment_decode(item_ptr, item_len, &value);
        if (err != NIPC_OK)
            return err;

        uint8_t item_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
        size_t encoded = nipc_increment_encode(value + 1, item_buf, sizeof(item_buf));
        if (encoded == 0)
            return NIPC_ERR_OVERFLOW;

        err = nipc_batch_builder_add(&builder, item_buf, encoded);
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_batch_builder_finish(&builder, NULL);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
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

static nipc_cgroups_service_handler_t snapshot_default_limit_service_handler = {
    .handle = on_snapshot_one,
    .snapshot_max_items = 0,
    .user = NULL,
};

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

static nipc_cgroups_service_handler_t collision_snapshot_service_handler = {
    .handle = on_snapshot_collision,
    .snapshot_max_items = 8,
    .user = NULL,
};

typedef enum {
    FAKE_HYBRID_ATTACH_BAD_PROFILE = 1,
    FAKE_HYBRID_RESP_SHORT,
    FAKE_HYBRID_RESP_BAD_MAGIC,
    FAKE_HYBRID_RESP_BAD_KIND,
    FAKE_HYBRID_RESP_BAD_CODE,
    FAKE_HYBRID_RESP_BAD_MESSAGE_ID,
    FAKE_HYBRID_RESP_LIMIT_EXCEEDED,
    FAKE_HYBRID_RESP_UNSUPPORTED,
    FAKE_HYBRID_RESP_DISCONNECT,
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
    check("guard raw server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("guard raw server ready event", wr == WAIT_OBJECT_0);
    check("guard raw server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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
    BOOL drained;
} drain_thread_ctx_t;

static DWORD WINAPI drain_thread_proc(LPVOID arg)
{
    drain_thread_ctx_t *ctx = (drain_thread_ctx_t *)arg;
    ctx->drained = nipc_server_drain(ctx->server, 1) ? TRUE : FALSE;
    SetEvent(ctx->done_event);
    return 0;
}

static DWORD WINAPI fake_hybrid_server_thread(LPVOID arg)
{
    fake_hybrid_server_ctx_t *ctx = (fake_hybrid_server_ctx_t *)arg;
    nipc_np_listener_t listener = {0};
    nipc_np_session_t session = {0};
    nipc_win_shm_ctx_t shm = {0};
    nipc_np_server_config_t scfg = default_hybrid_server_config();
    uint16_t shm_profile = NIPC_WIN_SHM_PROFILE_HYBRID;

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

    shm_profile = session.selected_profile;
    if (ctx->mode == FAKE_HYBRID_ATTACH_BAD_PROFILE)
        shm_profile = NIPC_WIN_SHM_PROFILE_BUSYWAIT;

    if (nipc_win_shm_server_create(TEST_RUN_DIR, ctx->service, AUTH_TOKEN,
                                   session.session_id,
                                   shm_profile,
                                   session.max_request_payload_bytes + NIPC_HEADER_LEN,
                                   session.max_response_payload_bytes + NIPC_HEADER_LEN,
                                   &shm) != NIPC_WIN_SHM_OK)
        goto out;

    if (ctx->mode == FAKE_HYBRID_ATTACH_BAD_PROFILE) {
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
        case FAKE_HYBRID_RESP_LIMIT_EXCEEDED: {
            uint8_t msg[NIPC_HEADER_LEN];
            size_t msg_len = build_status_message(
                msg, sizeof(msg), req_hdr.code, req_hdr.message_id,
                NIPC_STATUS_LIMIT_EXCEEDED);
            (void)nipc_win_shm_send(&shm, msg, msg_len);
            break;
        }
        case FAKE_HYBRID_RESP_UNSUPPORTED: {
            uint8_t msg[NIPC_HEADER_LEN];
            size_t msg_len = build_status_message(
                msg, sizeof(msg), req_hdr.code, req_hdr.message_id,
                NIPC_STATUS_UNSUPPORTED);
            (void)nipc_win_shm_send(&shm, msg, msg_len);
            break;
        }
        case FAKE_HYBRID_RESP_DISCONNECT:
            goto out;
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

    {
        char service[64];
        server_thread_ctx_t sctx;
        nipc_np_server_config_t scfg = default_server_config();
        HANDLE server_thread;
        nipc_client_status_t status = {0};

        unique_service(service, sizeof(service), "svc_client_guards_inc");
        scfg.max_request_payload_bytes = 8;
        scfg.max_request_batch_items = 2;
        server_thread = start_raw_server_named(&sctx, service, 4, &scfg,
                                               NIPC_METHOD_INCREMENT,
                                               raw_increment_batch_handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_client_config();
        uint64_t req[2] = { 7, 11 };
        uint64_t resp[2] = { 0, 0 };

        ccfg.max_request_payload_bytes = 8;
        ccfg.max_request_batch_items = 2;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard batch-overflow client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = test_win_client_call_increment_batch_raw(
                &client, req, 2, resp);
            check("increment batch transparently resizes and succeeds",
                  err == NIPC_OK && resp[0] == 8 && resp[1] == 12);
            nipc_client_status(&client, &status);
            check("increment batch resize reconnects once",
                  status.reconnect_count >= 1);
            check("increment batch negotiated request size grows",
                  client.session.max_request_payload_bytes >= 32u);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        server_thread_ctx_t sctx;
        nipc_np_server_config_t scfg = default_server_config();
        HANDLE server_thread;
        nipc_client_status_t status = {0};

        unique_service(service, sizeof(service), "svc_client_guards_str");
        scfg.max_request_payload_bytes = 16;
        server_thread = start_raw_server_named(&sctx, service, 4, &scfg,
                                               NIPC_METHOD_STRING_REVERSE,
                                               raw_string_reverse_handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_client_config();
        nipc_string_reverse_view_t view;

        ccfg.max_request_payload_bytes = 16;
        ccfg.max_request_batch_items = 1;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("guard string-overflow client ready",
              refresh_until_ready(&client, 100, 10));

        if (nipc_client_ready(&client)) {
            nipc_error_t err = test_win_client_call_string_reverse_raw(
                &client, "this request is intentionally too large", 38, &view);
            check("string reverse transparently resizes and succeeds",
                  err == NIPC_OK && view.str_len == 38);
            nipc_client_status(&client, &status);
            check("string reverse resize reconnects once",
                  status.reconnect_count >= 1);
            check("string reverse negotiated request size grows",
                  client.session.max_request_payload_bytes >= 64u);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, server_thread);
    }
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
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    check("hybrid attach failure eventually reaches DISCONNECTED",
          refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 5, 10));
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
        { FAKE_HYBRID_RESP_DISCONNECT, "missing SHM response after request", NIPC_ERR_TRUNCATED },
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
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check(cases[i].label, refresh_until_ready(&client, 200, 10) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            uint64_t value_out = 0;
            nipc_error_t err = test_win_client_call_increment_raw(&client, 41, &value_out);
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

static void test_hybrid_typed_snapshot_response_guards(void)
{
    struct {
        fake_hybrid_mode_t mode;
        const char *label;
        nipc_error_t expected;
        bool expect_response_capacity_growth;
    } cases[] = {
        { FAKE_HYBRID_RESP_SHORT, "typed short SHM response", NIPC_ERR_TRUNCATED, false },
        { FAKE_HYBRID_RESP_BAD_MAGIC, "typed bad SHM response header", NIPC_ERR_BAD_MAGIC, false },
        { FAKE_HYBRID_RESP_BAD_KIND, "typed wrong SHM response kind", NIPC_ERR_BAD_KIND, false },
        { FAKE_HYBRID_RESP_BAD_CODE, "typed wrong SHM response code", NIPC_ERR_BAD_LAYOUT, false },
        { FAKE_HYBRID_RESP_BAD_MESSAGE_ID, "typed wrong SHM response message_id", NIPC_ERR_BAD_LAYOUT, false },
        { FAKE_HYBRID_RESP_LIMIT_EXCEEDED, "typed SHM limit exceeded", NIPC_ERR_OVERFLOW, true },
        { FAKE_HYBRID_RESP_UNSUPPORTED, "typed SHM unsupported response", NIPC_ERR_BAD_LAYOUT, false },
        { FAKE_HYBRID_RESP_DISCONNECT, "typed missing SHM response after request", NIPC_ERR_TRUNCATED, false },
    };

    printf("--- Hybrid typed snapshot response guards ---\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service), "svc_hybrid_typed_resp");

        fake_hybrid_server_ctx_t sctx;
        HANDLE server_thread = start_fake_hybrid_server(&sctx, service, cases[i].mode);
        if (!server_thread)
            continue;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check(cases[i].label,
              refresh_until_ready(&client, 200, 10) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_cgroups_resp_view_t view;
            uint32_t before_resp_cap = client.transport_config.max_response_payload_bytes;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed malformed hybrid response returns expected error",
                  err == cases[i].expected);
            check("typed malformed hybrid response leaves client not READY",
                  !nipc_client_ready(&client));
            if (cases[i].expect_response_capacity_growth) {
                check("typed SHM limit-exceeded learns larger response capacity",
                      client.transport_config.max_response_payload_bytes > before_resp_cap);
            }
        }

        nipc_client_close(&client);
        check("fake typed hybrid response server exited",
              WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);
        CloseHandle(server_thread);
    }
}

static void test_hybrid_client_send_buffer_guard(void)
{
    printf("--- Hybrid client send buffer guard ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_send_guard");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_hybrid_server_config();
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 4, &scfg,
        NIPC_METHOD_INCREMENT, raw_increment_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("hybrid send-guard client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        uint64_t value_out = 0;
        size_t saved_send_buf_size = client.send_buf_size;

        client.send_buf_size = NIPC_HEADER_LEN + NIPC_INCREMENT_PAYLOAD_SIZE - 1;
        check("hybrid send buffer overflow returns NIPC_ERR_OVERFLOW",
              test_win_client_call_increment_raw(&client, 41, &value_out) == NIPC_ERR_OVERFLOW);
        client.send_buf_size = saved_send_buf_size;
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_hybrid_client_send_capacity_guard(void)
{
    printf("--- Hybrid client send capacity guard ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_send_cap");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_hybrid_server_config();
    scfg.max_request_payload_bytes = 16;
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 4, &scfg,
        NIPC_METHOD_STRING_REVERSE, raw_string_reverse_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    ccfg.max_request_payload_bytes = 16;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("hybrid send-capacity client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        nipc_string_reverse_view_t view;
        nipc_client_status_t status = {0};
        nipc_error_t err = test_win_client_call_string_reverse_raw(
            &client, "this request is intentionally too large", 38, &view);
        check("hybrid SHM request overflow transparently recovers",
              err == NIPC_OK && view.str_len == 38);
        nipc_client_status(&client, &status);
        check("hybrid send-capacity resize reconnects once",
              status.reconnect_count >= 1);
        check("hybrid send-capacity resize keeps client READY",
              nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_hybrid_batch_send_capacity_guard(void)
{
    printf("--- Hybrid batch send capacity guard ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_batch_send_cap");

    server_thread_ctx_t sctx;
    nipc_np_server_config_t scfg = default_hybrid_server_config();
    scfg.max_request_payload_bytes = 16;
    HANDLE server_thread = start_raw_server_named(
        &sctx, service, 4, &scfg,
        NIPC_METHOD_INCREMENT, raw_increment_batch_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    ccfg.max_request_payload_bytes = 16;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("hybrid batch send-capacity client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        uint64_t req[4] = { 41, 99, 123, 777 };
        uint64_t resp[4] = { 0, 0, 0, 0 };
        nipc_client_status_t status = {0};
        nipc_error_t err = test_win_client_call_increment_batch_raw(&client, req, 4, resp);
        check("hybrid batch request overflow transparently recovers",
              err == NIPC_OK &&
              resp[0] == 42 && resp[1] == 100 &&
              resp[2] == 124 && resp[3] == 778);
        nipc_client_status(&client, &status);
        check("hybrid batch resize reconnects once",
              status.reconnect_count >= 1);
        check("hybrid batch resize keeps client READY",
              nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, server_thread);
}

static void test_hybrid_batch_receive_failure(void)
{
    printf("--- Hybrid batch receive failure propagation ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_batch_recv");

    fake_hybrid_server_ctx_t sctx;
    HANDLE server_thread = start_fake_hybrid_server(
        &sctx, service, FAKE_HYBRID_RESP_DISCONNECT);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("hybrid batch-recv client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        uint64_t req = 41;
        uint64_t resp = 0;
        nipc_error_t err = test_win_client_call_increment_batch_raw(&client, &req, 1, &resp);
        check("hybrid batch receive failure returns NIPC_ERR_TRUNCATED",
              err == NIPC_ERR_TRUNCATED);
        check("hybrid batch receive failure leaves client not READY",
              !nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    check("fake hybrid batch receive server exited",
          WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(server_thread);
}

static void test_hybrid_string_raw_call_failure(void)
{
    printf("--- Hybrid string raw-call failure propagation ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_hybrid_string_fail");

    fake_hybrid_server_ctx_t sctx;
    HANDLE server_thread = start_fake_hybrid_server(
        &sctx, service, FAKE_HYBRID_RESP_DISCONNECT);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("hybrid string-fail client ready",
          refresh_until_ready(&client, 200, 10) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        nipc_string_reverse_view_t view;
        nipc_error_t err = test_win_client_call_string_reverse_raw(&client, "abc", 3, &view);
        check("hybrid string raw-call failure returns NIPC_ERR_TRUNCATED",
              err == NIPC_ERR_TRUNCATED);
        check("hybrid string raw-call failure leaves client not READY",
              !nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    check("fake hybrid string-fail server exited",
          WaitForSingleObject(server_thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(server_thread);
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
        HANDLE server_thread = start_raw_server_named(
            &sctx, service, 4, &scfg,
            NIPC_METHOD_INCREMENT, raw_increment_handler);
        if (!server_thread)
            continue;

        nipc_client_ctx_t bad_client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
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
            nipc_error_t err = test_win_client_call_increment_raw(&good_client, 9, &value_out);
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
    nipc_server_config_t scfg = default_typed_hybrid_server_config();
    HANDLE server_thread = start_server_named(
        &sctx, service, 4, &scfg, &snapshot_default_limit_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
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
            check("unknown method maps to internal-error response",
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
    CloseHandle(g_block_entered);
    CloseHandle(g_block_release);
    g_block_entered = NULL;
    g_block_release = NULL;
    CloseHandle(server_thread);
}

int main(void)
{
    printf("=== Windows Service Guard Coverage Tests ===\n\n");
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_client_batch_and_string_guards();
    test_hybrid_attach_failure_disconnects();
    test_hybrid_client_rejects_malformed_responses();
    test_hybrid_typed_snapshot_response_guards();
    test_hybrid_client_send_buffer_guard();
    test_hybrid_client_send_capacity_guard();
    test_hybrid_batch_send_capacity_guard();
    test_hybrid_batch_receive_failure();
    test_hybrid_string_raw_call_failure();
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
