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
#define LOOKUP_TOPOLOGY_SCALE_ITEMS 8192u
#define LOOKUP_HPC_SCALE_ITEMS 32768u
#define LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES 8192u
#define LOOKUP_SCALE_PATH_BYTES 24u

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
        .max_request_payload_bytes  = 4096,
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
        .max_request_payload_bytes  = 4096,
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

static int service_str_eq(nipc_str_view_t view, const char *s)
{
    size_t len = strlen(s);
    return view.len == len && memcmp(view.ptr, s, len) == 0;
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

typedef enum {
    LOOKUP_SERVER_CGROUPS = 1,
    LOOKUP_SERVER_APPS = 2,
} lookup_server_kind_t;

typedef struct {
    char service[64];
    lookup_server_kind_t kind;
    nipc_server_config_t config;
    nipc_cgroups_lookup_service_handler_t cgroups_handler;
    nipc_apps_lookup_service_handler_t apps_handler;
    HANDLE ready_event;
    volatile LONG init_ok;
    nipc_managed_server_t server;
} lookup_server_thread_ctx_t;

static DWORD WINAPI lookup_server_thread(LPVOID arg)
{
    lookup_server_thread_ctx_t *ctx = (lookup_server_thread_ctx_t *)arg;
    nipc_error_t err;

    if (ctx->kind == LOOKUP_SERVER_CGROUPS) {
        err = nipc_server_init_cgroups_lookup(&ctx->server, TEST_RUN_DIR,
                                              ctx->service, &ctx->config, 1,
                                              &ctx->cgroups_handler);
    } else {
        err = nipc_server_init_apps_lookup(&ctx->server, TEST_RUN_DIR,
                                           ctx->service, &ctx->config, 1,
                                           &ctx->apps_handler);
    }

    if (err != NIPC_OK) {
        fprintf(stderr, "lookup server init failed for %s: %d\n",
                ctx->service, err);
        InterlockedExchange(&ctx->init_ok, 0);
        SetEvent(ctx->ready_event);
        return 1;
    }

    InterlockedExchange(&ctx->init_ok, 1);
    SetEvent(ctx->ready_event);
    nipc_server_run(&ctx->server);
    return 0;
}

static HANDLE start_lookup_server_named(
    lookup_server_thread_ctx_t *ctx,
    const char *service,
    lookup_server_kind_t kind,
    const nipc_server_config_t *config,
    const nipc_cgroups_lookup_service_handler_t *cgroups_handler,
    const nipc_apps_lookup_service_handler_t *apps_handler)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->kind = kind;
    ctx->config = *config;
    if (cgroups_handler)
        ctx->cgroups_handler = *cgroups_handler;
    if (apps_handler)
        ctx->apps_handler = *apps_handler;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&ctx->init_ok, 0);

    HANDLE thread = CreateThread(NULL, 0, lookup_server_thread, ctx, 0, NULL);
    check("lookup server thread created", thread != NULL);
    if (!thread)
        return NULL;

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("lookup server ready event", wr == WAIT_OBJECT_0);
    check("lookup server init ok", InterlockedCompareExchange(&ctx->init_ok, 0, 0) == 1);
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
    check("server thread exited", WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("server drain completed", nipc_server_drain(&ctx->server, 10000));
}

static void stop_lookup_server_drain(lookup_server_thread_ctx_t *ctx, HANDLE thread)
{
    nipc_server_stop(&ctx->server);
    check("lookup server thread exited",
          WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    check("lookup server drain completed", nipc_server_drain(&ctx->server, 10000));
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

    check("request payload default max",
          client.transport_config.max_request_payload_bytes == NIPC_MAX_PAYLOAD_DEFAULT);
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
          client.transport_config.max_request_payload_bytes == NIPC_MAX_PAYLOAD_DEFAULT);
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

    check("raw init null config",
          nipc_server_init(&server, TEST_RUN_DIR, "svc_raw_null_config",
                           NULL, 1, NIPC_METHOD_INCREMENT,
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
              server.learned_request_payload_bytes == NIPC_MAX_PAYLOAD_DEFAULT);
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

static bool cgroups_lookup_test_handler(void *user,
                                        const nipc_cgroups_lookup_req_view_t *request,
                                        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (service_str_eq(req_item.path, "/known")) {
            nipc_lookup_label_view_t labels[] = {
                { .key = { .ptr = "namespace", .len = 9 },
                  .value = { .ptr = "default", .len = 7 } },
            };
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                    req_item.path.ptr, req_item.path.len,
                    "pod-a", 5, labels, 1) != NIPC_OK)
                return false;
        } else {
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
                    req_item.path.ptr, req_item.path.len,
                    "", 0, NULL, 0) != NIPC_OK)
                return false;
        }
    }

    return true;
}

static bool apps_lookup_test_handler(void *user,
                                     const nipc_apps_lookup_req_view_t *request,
                                     nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (req_item.pid == 1234) {
            nipc_lookup_label_view_t labels[] = {
                { .key = { .ptr = "image", .len = 5 },
                  .value = { .ptr = "nginx:latest", .len = 12 } },
            };
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                    NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                    "nginx", 5, "/docker/abc", 11, "container-a", 11,
                    labels, 1) != NIPC_OK)
                return false;
        } else if (req_item.pid == 0) {
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_HOST_ROOT,
                    0, req_item.pid, 0, 0, 0,
                    "swapper", 7, "", 0, "", 0, NULL, 0) != NIPC_OK)
                return false;
        } else {
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_UNKNOWN, 0,
                    0, req_item.pid, 0, NIPC_UID_UNSET, 0,
                    "", 0, "", 0, "", 0, NULL, 0) != NIPC_OK)
                return false;
        }
    }

    return true;
}

typedef struct {
    volatile LONG calls;
    volatile LONG max_items_seen;
    volatile LONG second_call_entered;
    int mixed_generation;
    int slow_second_call;
    int signal_second_call;
} lookup_scale_state_t;

static void lookup_scale_note_items(lookup_scale_state_t *state,
                                    uint32_t item_count)
{
    LONG current = state->max_items_seen;
    while ((LONG)item_count > current) {
        LONG previous = InterlockedCompareExchange(
            &state->max_items_seen, (LONG)item_count, current);
        if (previous == current)
            break;
        current = previous;
    }
}

static void lookup_scale_after_call(lookup_scale_state_t *state, LONG call)
{
    if (call != 2)
        return;

    if (state->signal_second_call)
        InterlockedExchange(&state->second_call_entered, 1);
    if (state->slow_second_call)
        Sleep(250);
}

typedef struct {
    nipc_client_ctx_t *client;
    lookup_server_kind_t kind;
    const nipc_str_view_t *paths;
    uint32_t path_count;
    const uint32_t *pids;
    uint32_t pid_count;
    uint32_t timeout_ms;
    nipc_cgroups_lookup_resp_view_t cgroups_view;
    nipc_apps_lookup_resp_view_t apps_view;
    nipc_error_t err;
} lookup_call_thread_ctx_t;

static DWORD WINAPI lookup_call_thread(LPVOID arg)
{
    lookup_call_thread_ctx_t *ctx = (lookup_call_thread_ctx_t *)arg;

    if (ctx->kind == LOOKUP_SERVER_CGROUPS) {
        ctx->err = nipc_client_call_cgroups_lookup_timeout(
            ctx->client, ctx->paths, ctx->path_count, &ctx->cgroups_view,
            ctx->timeout_ms);
    } else {
        ctx->err = nipc_client_call_apps_lookup_timeout(
            ctx->client, ctx->pids, ctx->pid_count, &ctx->apps_view,
            ctx->timeout_ms);
    }

    return 0;
}

static int lookup_scale_wait_for_second_call(lookup_scale_state_t *state)
{
    for (int i = 0; i < 5000; i++) {
        if (InterlockedCompareExchange(&state->second_call_entered, 0, 0) != 0)
            return 1;
        Sleep(1);
    }

    return 0;
}

static bool cgroups_lookup_scale_handler(void *user,
                                          const nipc_cgroups_lookup_req_view_t *request,
                                          nipc_cgroups_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    LONG call = InterlockedIncrement(&state->calls);
    uint64_t generation = state->mixed_generation ? (uint64_t)call : 7u;
    char huge_name[512];

    lookup_scale_note_items(state, request->item_count);
    lookup_scale_after_call(state, call);
    memset(huge_name, 'x', sizeof(huge_name));
    nipc_cgroups_lookup_builder_set_generation(builder, generation);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (service_str_eq(req_item.path, "/huge")) {
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                    req_item.path.ptr, req_item.path.len,
                    huge_name, sizeof(huge_name), NULL, 0) != NIPC_OK)
                return false;
        } else {
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                    req_item.path.ptr, req_item.path.len,
                    "ok", 2, NULL, 0) != NIPC_OK)
                return false;
        }
    }

    return true;
}

static bool apps_lookup_scale_handler(void *user,
                                      const nipc_apps_lookup_req_view_t *request,
                                      nipc_apps_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    LONG call = InterlockedIncrement(&state->calls);
    uint64_t generation = state->mixed_generation ? (uint64_t)call : 9u;
    char huge_path[1025];

    lookup_scale_note_items(state, request->item_count);
    lookup_scale_after_call(state, call);
    huge_path[0] = '/';
    memset(huge_path + 1, 'x', sizeof(huge_path) - 1);
    nipc_apps_lookup_builder_set_generation(builder, generation);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (req_item.pid == 22) {
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                    NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                    "ok", 2, huge_path, sizeof(huge_path),
                    "name", 4, NULL, 0) != NIPC_OK)
                return false;
        } else {
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                    NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                    "ok", 2, "/ok", 3, "name", 4, NULL, 0) != NIPC_OK)
                return false;
        }
    }

    return true;
}

static bool cgroups_lookup_payload_exceeded_first_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED, 0,
                req_item.path.ptr, req_item.path.len, "", 0, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool apps_lookup_payload_exceeded_first_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_PAYLOAD_EXCEEDED, 0, 0,
                req_item.pid, 0, NIPC_UID_UNSET, 0, "", 0, "", 0,
                "", 0, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool cgroups_lookup_wrong_echo_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        const char *path = i == 0 ? "/wrong" : req_item.path.ptr;
        uint32_t path_len = i == 0 ? 6u : req_item.path.len;
        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                path, path_len, "wrong", 5, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool cgroups_lookup_reordered_response_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count < 2)
        return false;

    nipc_cgroups_lookup_req_item_t first;
    nipc_cgroups_lookup_req_item_t second;
    if (nipc_cgroups_lookup_req_item(request, 0, &first) != NIPC_OK ||
        nipc_cgroups_lookup_req_item(request, 1, &second) != NIPC_OK)
        return false;

    return nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
               second.path.ptr, second.path.len, "", 0, NULL, 0) == NIPC_OK &&
           nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
               first.path.ptr, first.path.len, "", 0, NULL, 0) == NIPC_OK;
}

static bool cgroups_lookup_duplicate_response_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count < 2)
        return false;

    nipc_cgroups_lookup_req_item_t first;
    if (nipc_cgroups_lookup_req_item(request, 0, &first) != NIPC_OK)
        return false;

    return nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
               first.path.ptr, first.path.len, "", 0, NULL, 0) == NIPC_OK &&
           nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
               first.path.ptr, first.path.len, "", 0, NULL, 0) == NIPC_OK;
}

static bool apps_lookup_wrong_echo_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        uint32_t pid = i == 0 ? req_item.pid + 1u : req_item.pid;
        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, pid,
                0, NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool apps_lookup_reordered_response_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count < 2)
        return false;

    nipc_apps_lookup_req_item_t first;
    nipc_apps_lookup_req_item_t second;
    if (nipc_apps_lookup_req_item(request, 0, &first) != NIPC_OK ||
        nipc_apps_lookup_req_item(request, 1, &second) != NIPC_OK)
        return false;

    return nipc_apps_lookup_builder_add(
               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, second.pid, 0,
               NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK &&
           nipc_apps_lookup_builder_add(
               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, first.pid, 0,
               NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK;
}

static bool apps_lookup_duplicate_response_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count < 2)
        return false;

    nipc_apps_lookup_req_item_t first;
    if (nipc_apps_lookup_req_item(request, 0, &first) != NIPC_OK)
        return false;

    return nipc_apps_lookup_builder_add(
               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, first.pid, 0,
               NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK &&
           nipc_apps_lookup_builder_add(
               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, first.pid, 0,
               NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK;
}

static bool corrupt_lookup_builder_item_u16(uint8_t *buf,
                                            size_t buf_len,
                                            size_t response_header_size,
                                            uint32_t item_index,
                                            size_t item_offset,
                                            uint16_t value)
{
    size_t dir_pos = response_header_size +
                     (size_t)item_index * NIPC_LOOKUP_DIR_ENTRY_SIZE;
    if (dir_pos > buf_len || buf_len - dir_pos < sizeof(nipc_lookup_dir_entry_t))
        return false;

    nipc_lookup_dir_entry_t entry;
    memcpy(&entry, buf + dir_pos, sizeof(entry));
    if (item_offset > entry.length ||
        entry.length - item_offset < sizeof(value) ||
        entry.offset > buf_len ||
        buf_len - entry.offset < item_offset + sizeof(value))
        return false;

    memcpy(buf + entry.offset + item_offset, &value, sizeof(value));
    return true;
}

static bool cgroups_lookup_invalid_status_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "db", .len = 2 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "name", 4,
                labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_CGROUPS_LOOKUP_RESP_HDR_SIZE,
        0, 2, 0xffff);
}

static bool cgroups_lookup_invalid_status_fields_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "db", .len = 2 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "name", 4,
                labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_CGROUPS_LOOKUP_RESP_HDR_SIZE,
        0, 2, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER);
}

static bool cgroups_lookup_invalid_label_table_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "db", .len = 2 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "name", 4,
                labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_CGROUPS_LOOKUP_RESP_HDR_SIZE,
        0, 24, 2);
}

static bool apps_lookup_invalid_status_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "api", .len = 3 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                "comm", 4, "/cg", 3, "name", 4, labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_APPS_LOOKUP_RESP_HDR_SIZE,
        0, 2, 0xffff);
}

static bool apps_lookup_invalid_status_fields_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "api", .len = 3 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                "comm", 4, "/cg", 3, "name", 4, labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_APPS_LOOKUP_RESP_HDR_SIZE,
        0, 2, NIPC_PID_LOOKUP_UNKNOWN);
}

static bool apps_lookup_invalid_label_table_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return false;

    const nipc_lookup_label_view_t labels[] = {
        { .key = { .ptr = "role", .len = 4 },
          .value = { .ptr = "api", .len = 3 } },
    };
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;
        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                "comm", 4, "/cg", 3, "name", 4, labels, 1) != NIPC_OK)
            return false;
    }

    return corrupt_lookup_builder_item_u16(
        builder->buf, builder->buf_len, NIPC_APPS_LOOKUP_RESP_HDR_SIZE,
        0, 56, 2);
}

static bool cgroups_lookup_malformed_followup_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    LONG call = InterlockedIncrement(&state->calls);

    nipc_cgroups_lookup_builder_set_generation(builder, 77);
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (call == 1 && i > 0) {
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED, 0,
                    req_item.path.ptr, req_item.path.len, "", 0, NULL, 0) != NIPC_OK)
                return false;
            continue;
        }

        if (call > 1 && i == 0) {
            if (nipc_cgroups_lookup_builder_add(
                    builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
                    "/wrong", 6, "", 0, NULL, 0) != NIPC_OK)
                return false;
            continue;
        }

        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "ok", 2, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool apps_lookup_malformed_followup_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    LONG call = InterlockedIncrement(&state->calls);

    nipc_apps_lookup_builder_set_generation(builder, 88);
    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (call == 1 && i > 0) {
            if (nipc_apps_lookup_builder_add(
                    builder, NIPC_PID_LOOKUP_PAYLOAD_EXCEEDED, 0, 0,
                    req_item.pid, 0, NIPC_UID_UNSET, 0, "", 0, "", 0,
                    "", 0, NULL, 0) != NIPC_OK)
                return false;
            continue;
        }

        uint32_t pid = req_item.pid;
        if (call > 1 && i == 0)
            pid = req_item.pid == 0 ? 1u : 0u;

        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, pid, 0,
                NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

typedef struct {
    char service[64];
    lookup_server_kind_t kind;
    HANDLE ready_event;
    volatile LONG ready;
    volatile LONG done;
} raw_lookup_partial_disconnect_ctx_t;

static nipc_error_t build_apps_lookup_partial_response(
        const void *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    nipc_apps_lookup_req_view_t request;
    nipc_error_t err =
        nipc_apps_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;

    nipc_apps_lookup_builder_t builder;
    nipc_apps_lookup_builder_init(&builder, response_buf, response_buf_size,
                                  request.item_count, 88);
    for (uint32_t i = 0; i < request.item_count; i++) {
        nipc_apps_lookup_req_item_t item;
        err = nipc_apps_lookup_req_item(&request, i, &item);
        if (err != NIPC_OK)
            return err;

        uint16_t status = i == 0 ? NIPC_PID_LOOKUP_UNKNOWN :
                                    NIPC_PID_LOOKUP_PAYLOAD_EXCEEDED;
        err = nipc_apps_lookup_builder_add(
            &builder, status, 0, 0, item.pid, 0, NIPC_UID_UNSET, 0,
            "", 0, "", 0, "", 0, NULL, 0);
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_apps_lookup_builder_finish(&builder);
    return *response_len_out > 0 ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

static nipc_error_t build_cgroups_lookup_partial_response(
        const void *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    nipc_cgroups_lookup_req_view_t request;
    nipc_error_t err =
        nipc_cgroups_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_lookup_builder_t builder;
    nipc_cgroups_lookup_builder_init(&builder, response_buf, response_buf_size,
                                     request.item_count, 77);
    for (uint32_t i = 0; i < request.item_count; i++) {
        nipc_cgroups_lookup_req_item_t item;
        err = nipc_cgroups_lookup_req_item(&request, i, &item);
        if (err != NIPC_OK)
            return err;

        uint16_t status = i == 0 ? NIPC_CGROUP_LOOKUP_KNOWN :
                                    NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED;
        uint16_t orchestrator = i == 0 ? NIPC_ORCHESTRATOR_K8S : 0;
        const char *name = i == 0 ? "ok" : "";
        size_t name_len = i == 0 ? 2 : 0;
        err = nipc_cgroups_lookup_builder_add(
            &builder, status, orchestrator, item.path.ptr, item.path.len,
            name, name_len, NULL, 0);
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_cgroups_lookup_builder_finish(&builder);
    return *response_len_out > 0 ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

static DWORD WINAPI raw_lookup_partial_disconnect_thread(LPVOID arg)
{
    raw_lookup_partial_disconnect_ctx_t *ctx =
        (raw_lookup_partial_disconnect_ctx_t *)arg;
    nipc_np_listener_t listener;
    nipc_np_session_t session;
    uint8_t recv_buf[4096];
    uint8_t response_buf[1024];
    nipc_np_server_config_t scfg = default_server_config();

    memset(&listener, 0, sizeof(listener));
    memset(&session, 0, sizeof(session));
    listener.pipe = INVALID_HANDLE_VALUE;
    session.pipe = INVALID_HANDLE_VALUE;

    if (nipc_np_listen(TEST_RUN_DIR, ctx->service, &scfg, &listener) !=
        NIPC_NP_OK)
        goto out;

    InterlockedExchange(&ctx->ready, 1);
    SetEvent(ctx->ready_event);

    if (nipc_np_accept(&listener, 1, &session) != NIPC_NP_OK)
        goto out;

    nipc_header_t req_hdr;
    const void *request_payload = NULL;
    size_t request_len = 0;
    if (nipc_np_receive(&session, recv_buf, sizeof(recv_buf), &req_hdr,
                        &request_payload, &request_len) != NIPC_NP_OK)
        goto out;

    size_t response_len = 0;
    nipc_error_t err = NIPC_ERR_BAD_LAYOUT;
    if (ctx->kind == LOOKUP_SERVER_APPS &&
        req_hdr.code == NIPC_METHOD_APPS_LOOKUP)
        err = build_apps_lookup_partial_response(request_payload, request_len,
                                                 response_buf,
                                                 sizeof(response_buf),
                                                 &response_len);
    else if (ctx->kind == LOOKUP_SERVER_CGROUPS &&
             req_hdr.code == NIPC_METHOD_CGROUPS_LOOKUP)
        err = build_cgroups_lookup_partial_response(request_payload,
                                                    request_len,
                                                    response_buf,
                                                    sizeof(response_buf),
                                                    &response_len);
    if (err != NIPC_OK)
        goto out;

    nipc_header_t resp_hdr = {
        .kind = NIPC_KIND_RESPONSE,
        .code = req_hdr.code,
        .flags = 0,
        .item_count = 1,
        .message_id = req_hdr.message_id,
        .transport_status = NIPC_STATUS_OK,
    };
    (void)nipc_np_send(&session, &resp_hdr, response_buf, response_len);

out:
    if (session.pipe != INVALID_HANDLE_VALUE)
        nipc_np_close_session(&session);
    if (listener.pipe != INVALID_HANDLE_VALUE)
        nipc_np_close_listener(&listener);
    InterlockedExchange(&ctx->done, 1);
    if (ctx->ready_event &&
        InterlockedCompareExchange(&ctx->ready, 0, 0) == 0)
        SetEvent(ctx->ready_event);
    return 0;
}

static HANDLE start_raw_lookup_partial_disconnect(
        raw_lookup_partial_disconnect_ctx_t *ctx,
        const char *service,
        lookup_server_kind_t kind)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->kind = kind;
    ctx->ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    check("partial-disconnect ready event", ctx->ready_event != NULL);
    if (!ctx->ready_event)
        return NULL;

    HANDLE thread =
        CreateThread(NULL, 0, raw_lookup_partial_disconnect_thread, ctx, 0,
                     NULL);
    check("partial-disconnect raw server thread created", thread != NULL);
    if (!thread) {
        CloseHandle(ctx->ready_event);
        ctx->ready_event = NULL;
        return NULL;
    }

    DWORD wr = WaitForSingleObject(ctx->ready_event, 5000);
    check("partial-disconnect raw server ready", wr == WAIT_OBJECT_0);
    CloseHandle(ctx->ready_event);
    ctx->ready_event = NULL;
    return thread;
}

static void test_lookup_typed_calls(void)
{
    printf("--- Typed lookup calls ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/known", .len = 6 },
            { .ptr = "/missing", .len = 8 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        if (err != NIPC_OK) {
            nipc_client_status_t status;
            nipc_client_status(&client, &status);
            printf("  cgroups lookup err=%d state=%d connect=%llu reconnect=%llu calls=%llu errors=%llu\n",
                   err, status.state,
                   (unsigned long long)status.connect_count,
                   (unsigned long long)status.reconnect_count,
                   (unsigned long long)status.call_count,
                   (unsigned long long)status.error_count);
            printf("  cgroups caps req_session=%u req_config=%u resp_session=%u resp_config=%u\n",
                   client.session.max_request_payload_bytes,
                   client.transport_config.max_request_payload_bytes,
                   client.session.max_response_payload_bytes,
                   client.transport_config.max_response_payload_bytes);
        }
        check("cgroups lookup call ok", err == NIPC_OK);

        if (err == NIPC_OK) {
            check("cgroups lookup item_count == 2", view.item_count == 2);
            nipc_cgroups_lookup_item_view_t item;
            check("cgroups lookup item 0 decode",
                  nipc_cgroups_lookup_resp_item(&view, 0, &item) == NIPC_OK);
            check("cgroups lookup item 0 known",
                  item.status == NIPC_CGROUP_LOOKUP_KNOWN &&
                  item.orchestrator == NIPC_ORCHESTRATOR_K8S &&
                  service_str_eq(item.path, "/known") &&
                  service_str_eq(item.name, "pod-a") &&
                  item.label_count == 1);
            check("cgroups lookup item 1 decode",
                  nipc_cgroups_lookup_resp_item(&view, 1, &item) == NIPC_OK);
            check("cgroups lookup item 1 retry",
                  item.status == NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER &&
                  service_str_eq(item.path, "/missing") &&
                  item.name.len == 0);
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps lookup client ready", refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = {1234, 0, 9999};
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
        if (err != NIPC_OK) {
            nipc_client_status_t status;
            nipc_client_status(&client, &status);
            printf("  apps lookup err=%d state=%d connect=%llu reconnect=%llu calls=%llu errors=%llu\n",
                   err, status.state,
                   (unsigned long long)status.connect_count,
                   (unsigned long long)status.reconnect_count,
                   (unsigned long long)status.call_count,
                   (unsigned long long)status.error_count);
            printf("  apps caps req_session=%u req_config=%u resp_session=%u resp_config=%u\n",
                   client.session.max_request_payload_bytes,
                   client.transport_config.max_request_payload_bytes,
                   client.session.max_response_payload_bytes,
                   client.transport_config.max_response_payload_bytes);
        }
        check("apps lookup call ok", err == NIPC_OK);

        if (err == NIPC_OK) {
            check("apps lookup item_count == 3", view.item_count == 3);
            nipc_apps_lookup_item_view_t item;
            check("apps lookup item 0 decode",
                  nipc_apps_lookup_resp_item(&view, 0, &item) == NIPC_OK);
            check("apps lookup item 0 known",
                  item.status == NIPC_PID_LOOKUP_KNOWN &&
                  item.cgroup_status == NIPC_APPS_CGROUP_KNOWN &&
                  item.pid == 1234 &&
                  service_str_eq(item.comm, "nginx") &&
                  service_str_eq(item.cgroup_path, "/docker/abc") &&
                  item.label_count == 1);
            check("apps lookup item 1 decode",
                  nipc_apps_lookup_resp_item(&view, 1, &item) == NIPC_OK);
            check("apps lookup item 1 host root",
                  item.pid == 0 &&
                  item.cgroup_status == NIPC_APPS_CGROUP_HOST_ROOT &&
                  item.cgroup_path.len == 0);
            check("apps lookup item 2 decode",
                  nipc_apps_lookup_resp_item(&view, 2, &item) == NIPC_OK);
            check("apps lookup item 2 unknown",
                  item.pid == 9999 &&
                  item.status == NIPC_PID_LOOKUP_UNKNOWN &&
                  item.uid == NIPC_UID_UNSET);
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_zero_item_calls(void)
{
    printf("--- Typed lookup zero-item calls ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_zero");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("zero cgroups lookup client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup(&client, NULL, 0, &view);
        check("zero cgroups lookup call ok", err == NIPC_OK);
        if (err == NIPC_OK)
            check("zero cgroups lookup item_count == 0", view.item_count == 0);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_zero");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("zero apps lookup client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, NULL, 0, &view);
        check("zero apps lookup call ok", err == NIPC_OK);
        if (err == NIPC_OK)
            check("zero apps lookup item_count == 0", view.item_count == 0);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_payload_exceeded_retry(void)
{
    printf("--- Typed lookup PAYLOAD_EXCEEDED retry ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_scale");

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 160;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups scale lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/huge", .len = 5 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
        check("cgroups scale lookup call ok", err == NIPC_OK);
        check("cgroups scale lookup used follow-up call", state.calls >= 2);

        if (err == NIPC_OK) {
            check("cgroups scale item_count == 3", view.item_count == 3);
            check("cgroups scale generation stable", view.generation == 7u);
            nipc_cgroups_lookup_item_view_t item;
            check("cgroups scale item 0 decode",
                  nipc_cgroups_lookup_resp_item(&view, 0, &item) == NIPC_OK);
            check("cgroups scale item 0 known",
                  item.status == NIPC_CGROUP_LOOKUP_KNOWN &&
                  service_str_eq(item.path, "/a") &&
                  service_str_eq(item.name, "ok"));
            check("cgroups scale item 1 decode",
                  nipc_cgroups_lookup_resp_item(&view, 1, &item) == NIPC_OK);
            check("cgroups scale item 1 oversized",
                  item.status == NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM &&
                  service_str_eq(item.path, "/huge") &&
                  item.name.len == 0);
            check("cgroups scale item 2 decode",
                  nipc_cgroups_lookup_resp_item(&view, 2, &item) == NIPC_OK);
            check("cgroups scale item 2 known",
                  item.status == NIPC_CGROUP_LOOKUP_KNOWN &&
                  service_str_eq(item.path, "/b") &&
                  service_str_eq(item.name, "ok"));
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_scale");

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 320;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps scale lookup client ready", refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = {11, 22, 33};
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
        check("apps scale lookup call ok", err == NIPC_OK);
        check("apps scale lookup used follow-up call", state.calls >= 2);

        if (err == NIPC_OK) {
            check("apps scale item_count == 3", view.item_count == 3);
            check("apps scale generation stable", view.generation == 9u);
            nipc_apps_lookup_item_view_t item;
            check("apps scale item 0 decode",
                  nipc_apps_lookup_resp_item(&view, 0, &item) == NIPC_OK);
            check("apps scale item 0 known",
                  item.status == NIPC_PID_LOOKUP_KNOWN &&
                  item.pid == 11 &&
                  service_str_eq(item.comm, "ok") &&
                  service_str_eq(item.cgroup_path, "/ok"));
            check("apps scale item 1 decode",
                  nipc_apps_lookup_resp_item(&view, 1, &item) == NIPC_OK);
            check("apps scale item 1 oversized",
                  item.status == NIPC_PID_LOOKUP_OVERSIZED_ITEM &&
                  item.pid == 22 &&
                  item.comm.len == 0 &&
                  item.cgroup_path.len == 0);
            check("apps scale item 2 decode",
                  nipc_apps_lookup_resp_item(&view, 2, &item) == NIPC_OK);
            check("apps scale item 2 known",
                  item.status == NIPC_PID_LOOKUP_KNOWN &&
                  item.pid == 33 &&
                  service_str_eq(item.comm, "ok") &&
                  service_str_eq(item.cgroup_path, "/ok"));
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void run_cgroups_lookup_request_boundary_case(
    const char *prefix, uint32_t request_cap, uint32_t expected_max_items,
    uint32_t min_calls, const char *label)
{
    char service[64];
    unique_service(service, sizeof(service), prefix);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_request_payload_bytes = request_cap;
    scfg.max_response_payload_bytes = 4096;
    nipc_cgroups_lookup_service_handler_t handler = {
        .handle = cgroups_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = request_cap;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("cgroups split lookup client ready", refresh_until_ready(&client, 100, 10));

    nipc_str_view_t paths[] = {
        { .ptr = "/bbbbbb", .len = 7 },
        { .ptr = "/aaaaaa", .len = 7 },
        { .ptr = "/bbbbbb", .len = 7 },
        { .ptr = "/cccccc", .len = 7 },
        { .ptr = "/aaaaaa", .len = 7 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 5, &view);
    check("cgroups split lookup call ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups split item_count == 5", view.item_count == 5);
        check("cgroups split generation == 7", view.generation == 7u);
        int ordered = view.item_count == 5;
        for (uint32_t i = 0; ordered && i < 5; i++) {
            nipc_cgroups_lookup_item_view_t item;
            if (nipc_cgroups_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_CGROUP_LOOKUP_KNOWN ||
                item.path.len != paths[i].len ||
                memcmp(item.path.ptr, paths[i].ptr, paths[i].len) != 0)
                ordered = 0;
        }
        check("cgroups split preserves duplicate unsorted order", ordered);
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "cgroups %s made expected split requests", label);
    check(msg, state.calls >= min_calls);
    snprintf(msg, sizeof(msg), "cgroups %s max fragment", label);
    check(msg, state.max_items_seen == expected_max_items);

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
}

static void run_apps_lookup_request_boundary_case(
    const char *prefix, uint32_t request_cap, uint32_t expected_max_items,
    uint32_t min_calls, const char *label)
{
    char service[64];
    unique_service(service, sizeof(service), prefix);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_request_payload_bytes = request_cap;
    scfg.max_response_payload_bytes = 4096;
    nipc_apps_lookup_service_handler_t handler = {
        .handle = apps_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = request_cap;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("apps split lookup client ready", refresh_until_ready(&client, 100, 10));

    uint32_t pids[] = {4, 1, 4, 7, 1, 9, 7};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 7, &view);
    check("apps split lookup call ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("apps split item_count == 7", view.item_count == 7);
        check("apps split generation == 9", view.generation == 9u);
        int ordered = view.item_count == 7;
        for (uint32_t i = 0; ordered && i < 7; i++) {
            nipc_apps_lookup_item_view_t item;
            if (nipc_apps_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_PID_LOOKUP_KNOWN ||
                item.pid != pids[i])
                ordered = 0;
        }
        check("apps split preserves duplicate unsorted order", ordered);
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "apps %s made expected split requests", label);
    check(msg, state.calls >= min_calls);
    snprintf(msg, sizeof(msg), "apps %s max fragment", label);
    check(msg, state.max_items_seen == expected_max_items);

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
}

static void test_lookup_proactive_request_split(void)
{
    printf("--- Typed lookup proactive request splitting ---\n");

    run_cgroups_lookup_request_boundary_case(
        "svc_cgroups_lookup_request_split_minus", 47, 1, 5, "cap-1");
    run_cgroups_lookup_request_boundary_case(
        "svc_cgroups_lookup_request_split_exact", 48, 2, 3, "cap-exact");
    run_cgroups_lookup_request_boundary_case(
        "svc_cgroups_lookup_request_split_plus", 49, 2, 3, "cap+1");

    run_apps_lookup_request_boundary_case(
        "svc_apps_lookup_request_split_minus", 63, 2, 4, "cap-1");
    run_apps_lookup_request_boundary_case(
        "svc_apps_lookup_request_split_exact", 64, 3, 3, "cap-exact");
    run_apps_lookup_request_boundary_case(
        "svc_apps_lookup_request_split_plus", 65, 3, 3, "cap+1");
}

static void test_apps_lookup_large_logical_case(const char *prefix,
                                                uint32_t item_count)
{
    uint32_t *pids = calloc(item_count, sizeof(*pids));
    check("apps large allocate pids", pids != NULL);
    if (!pids)
        return;

    for (uint32_t i = 0; i < item_count; i++)
        pids[i] = 100000u + i;

    char service[64];
    unique_service(service, sizeof(service), prefix);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    scfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    nipc_apps_lookup_service_handler_t handler = {
        .handle = apps_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
    if (!server_thread) {
        free(pids);
        return;
    }

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("apps large client ready", refresh_until_ready(&client, 100, 10));

    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_apps_lookup(&client, pids, item_count, &view);
    check("apps large call ok", err == NIPC_OK);

    if (err == NIPC_OK) {
        int ordered = view.item_count == item_count && view.generation == 9u;
        for (uint32_t i = 0; ordered && i < item_count; i++) {
            nipc_apps_lookup_item_view_t item;
            if (nipc_apps_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_PID_LOOKUP_KNOWN ||
                item.pid != pids[i] ||
                !service_str_eq(item.comm, "ok") ||
                !service_str_eq(item.cgroup_path, "/ok"))
                ordered = 0;
        }
        check("apps large ordered full response", ordered);
        check("apps large used multiple requests", state.calls > 1);
        check("apps large fragmented below total item count",
              (uint32_t)state.max_items_seen < item_count);
    }

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
    free(pids);
}

static void test_cgroups_lookup_large_logical_case(const char *prefix,
                                                   uint32_t item_count)
{
    nipc_str_view_t *paths = calloc(item_count, sizeof(*paths));
    char *path_storage = calloc(item_count, LOOKUP_SCALE_PATH_BYTES);
    check("cgroups large allocate paths", paths != NULL && path_storage != NULL);
    if (!paths || !path_storage) {
        free(paths);
        free(path_storage);
        return;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        char *slot = path_storage + ((size_t)i * LOOKUP_SCALE_PATH_BYTES);
        snprintf(slot, LOOKUP_SCALE_PATH_BYTES, "/cg/%05u", i);
        paths[i].ptr = slot;
        paths[i].len = strlen(slot);
    }

    char service[64];
    unique_service(service, sizeof(service), prefix);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    scfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    nipc_cgroups_lookup_service_handler_t handler = {
        .handle = cgroups_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
    if (!server_thread) {
        free(path_storage);
        free(paths);
        return;
    }

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("cgroups large client ready", refresh_until_ready(&client, 100, 10));

    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_cgroups_lookup(&client, paths, item_count, &view);
    check("cgroups large call ok", err == NIPC_OK);

    if (err == NIPC_OK) {
        int ordered = view.item_count == item_count && view.generation == 7u;
        for (uint32_t i = 0; ordered && i < item_count; i++) {
            nipc_cgroups_lookup_item_view_t item;
            if (nipc_cgroups_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_CGROUP_LOOKUP_KNOWN ||
                !service_str_eq(item.path, paths[i].ptr) ||
                !service_str_eq(item.name, "ok"))
                ordered = 0;
        }
        check("cgroups large ordered full response", ordered);
        check("cgroups large used multiple requests", state.calls > 1);
        check("cgroups large fragmented below total item count",
              (uint32_t)state.max_items_seen < item_count);
    }

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
    free(path_storage);
    free(paths);
}

static void test_lookup_large_logical_calls(void)
{
    printf("--- Typed lookup large logical calls ---\n");

    test_apps_lookup_large_logical_case("svc_apps_lookup_large_8192",
                                        LOOKUP_TOPOLOGY_SCALE_ITEMS);
    test_apps_lookup_large_logical_case("svc_apps_lookup_large_32768",
                                        LOOKUP_HPC_SCALE_ITEMS);
    test_cgroups_lookup_large_logical_case("svc_cgroups_lookup_large_8192",
                                           LOOKUP_TOPOLOGY_SCALE_ITEMS);
    test_cgroups_lookup_large_logical_case("svc_cgroups_lookup_large_32768",
                                           LOOKUP_HPC_SCALE_ITEMS);
}

static void test_cgroups_lookup_oversized_request_key(void)
{
    printf("--- Typed cgroups lookup oversized request key ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_cgroups_lookup_oversized_request_key");

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_request_payload_bytes = 48;
    scfg.max_response_payload_bytes = 4096;
    nipc_cgroups_lookup_service_handler_t handler = {
        .handle = cgroups_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = 48;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("cgroups oversized request-key client ready",
          refresh_until_ready(&client, 100, 10));

    nipc_str_view_t paths[] = {
        { .ptr = "/request-key-too-large-for-configured-cap", .len = 41 },
        { .ptr = "/ok", .len = 3 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
    check("cgroups oversized request-key call ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups oversized request-key item_count == 2", view.item_count == 2);
        check("cgroups oversized request-key generation == 7", view.generation == 7);

        nipc_cgroups_lookup_item_view_t item;
        check("cgroups oversized request-key item 0 decode",
              nipc_cgroups_lookup_resp_item(&view, 0, &item) == NIPC_OK);
        check("cgroups oversized request-key item 0 oversized",
              item.status == NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM &&
              service_str_eq(item.path, "/request-key-too-large-for-configured-cap"));
        check("cgroups oversized request-key item 1 decode",
              nipc_cgroups_lookup_resp_item(&view, 1, &item) == NIPC_OK);
        check("cgroups oversized request-key item 1 known",
              item.status == NIPC_CGROUP_LOOKUP_KNOWN &&
              service_str_eq(item.path, "/ok") &&
              service_str_eq(item.name, "ok"));
    }
    check("cgroups oversized request-key skipped oversized item on server",
          state.calls == 1);
    check("cgroups oversized request-key server saw one item",
          state.max_items_seen == 1);

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
}

static void test_lookup_logical_limits(void)
{
    printf("--- Typed lookup logical limits ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_lookup_logical_limits");

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    nipc_apps_lookup_service_handler_t handler = {
        .handle = apps_lookup_scale_handler,
        .user = &state,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_logical_lookup_items = 2;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("logical limit lookup client ready", refresh_until_ready(&client, 100, 10));

    uint32_t pids[] = {1, 2, 3};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
    check("apps logical item limit rejected", err == NIPC_ERR_OVERFLOW);
    check("apps logical item limit did not call handler", state.calls == 0);
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("apps logical item limit did not reconnect",
          status.reconnect_count == 0);

    nipc_client_close(&client);

    nipc_client_ctx_t cold_client;
    nipc_client_config_t cold_cfg = default_client_config();
    cold_cfg.max_request_payload_bytes = 48;
    nipc_client_init(&cold_client, TEST_RUN_DIR, "win_lookup_not_ready",
                     &cold_cfg);
    nipc_str_view_t oversized_path = {
        .ptr = "/request-key-too-large-for-configured-cap",
        .len = 41,
    };
    nipc_cgroups_lookup_resp_view_t cgroups_view;
    err = nipc_client_call_cgroups_lookup(&cold_client, &oversized_path, 1,
                                          &cgroups_view);
    check("cgroups oversized request-key still requires ready client",
          err == NIPC_ERR_NOT_READY);
    nipc_client_close(&cold_client);

    stop_lookup_server_drain(&sctx, server_thread);

    unique_service(service, sizeof(service), "svc_apps_lookup_response_limit");
    lookup_scale_state_t apps_response_state = {0};
    lookup_server_thread_ctx_t apps_response_sctx;
    nipc_server_config_t apps_response_scfg = default_typed_server_config();
    nipc_apps_lookup_service_handler_t apps_response_handler = {
        .handle = apps_lookup_scale_handler,
        .user = &apps_response_state,
    };
    server_thread = start_lookup_server_named(
        &apps_response_sctx, service, LOOKUP_SERVER_APPS, &apps_response_scfg,
        NULL, &apps_response_handler);
    if (!server_thread)
        return;

    nipc_client_config_t response_cfg = default_client_config();
    response_cfg.max_logical_lookup_response_bytes = 1;
    nipc_client_init(&client, TEST_RUN_DIR, service, &response_cfg);
    check("apps logical response-limit client ready",
          refresh_until_ready(&client, 100, 10));
    uint32_t response_pid = 1234;
    err = nipc_client_call_apps_lookup(&client, &response_pid, 1, &view);
    check("apps logical response limit rejected", err == NIPC_ERR_OVERFLOW);
    nipc_client_close(&client);
    stop_lookup_server_drain(&apps_response_sctx, server_thread);

    unique_service(service, sizeof(service), "svc_cgroups_lookup_response_limit");
    lookup_server_thread_ctx_t cgroups_sctx;
    nipc_server_config_t cgroups_scfg = default_typed_server_config();
    nipc_cgroups_lookup_service_handler_t cgroups_handler = {
        .handle = cgroups_lookup_scale_handler,
        .user = &state,
    };
    server_thread = start_lookup_server_named(
        &cgroups_sctx, service, LOOKUP_SERVER_CGROUPS, &cgroups_scfg,
        &cgroups_handler, NULL);
    if (!server_thread)
        return;

    response_cfg = default_client_config();
    response_cfg.max_logical_lookup_response_bytes = 1;
    nipc_client_init(&client, TEST_RUN_DIR, service, &response_cfg);
    check("cgroups logical response-limit client ready",
          refresh_until_ready(&client, 100, 10));
    nipc_str_view_t response_path = { .ptr = "/ok", .len = 3 };
    err = nipc_client_call_cgroups_lookup(&client, &response_path, 1,
                                          &cgroups_view);
    check("cgroups logical response limit rejected", err == NIPC_ERR_OVERFLOW);
    nipc_client_close(&client);
    stop_lookup_server_drain(&cgroups_sctx, server_thread);

    unique_service(service, sizeof(service), "svc_cgroups_lookup_subcall_limit");
    lookup_scale_state_t cgroups_subcall_state = {0};
    cgroups_scfg = default_typed_server_config();
    cgroups_scfg.max_response_payload_bytes = 160;
    cgroups_handler.handle = cgroups_lookup_scale_handler;
    cgroups_handler.user = &cgroups_subcall_state;
    server_thread = start_lookup_server_named(
        &cgroups_sctx, service, LOOKUP_SERVER_CGROUPS, &cgroups_scfg,
        &cgroups_handler, NULL);
    if (!server_thread)
        return;

    nipc_client_config_t subcall_cfg = default_client_config();
    subcall_cfg.max_logical_lookup_subcalls = 1;
    nipc_client_init(&client, TEST_RUN_DIR, service, &subcall_cfg);
    check("cgroups logical subcall-limit client ready",
          refresh_until_ready(&client, 100, 10));
    nipc_str_view_t subcall_paths[] = {
        { .ptr = "/a", .len = 2 },
        { .ptr = "/huge", .len = 5 },
        { .ptr = "/b", .len = 2 },
    };
    err = nipc_client_call_cgroups_lookup(&client, subcall_paths, 3,
                                          &cgroups_view);
    check("cgroups logical subcall limit rejected", err == NIPC_ERR_OVERFLOW);
    check("cgroups subcall limit stopped after first server call",
          cgroups_subcall_state.calls == 1);
    nipc_client_close(&client);
    stop_lookup_server_drain(&cgroups_sctx, server_thread);

    unique_service(service, sizeof(service), "svc_apps_lookup_subcall_limit");
    lookup_scale_state_t apps_subcall_state = {0};
    lookup_server_thread_ctx_t apps_sctx;
    nipc_server_config_t apps_scfg = default_typed_server_config();
    apps_scfg.max_response_payload_bytes = 320;
    nipc_apps_lookup_service_handler_t apps_handler = {
        .handle = apps_lookup_scale_handler,
        .user = &apps_subcall_state,
    };
    server_thread = start_lookup_server_named(
        &apps_sctx, service, LOOKUP_SERVER_APPS, &apps_scfg, NULL,
        &apps_handler);
    if (!server_thread)
        return;

    subcall_cfg = default_client_config();
    subcall_cfg.max_logical_lookup_subcalls = 1;
    nipc_client_init(&client, TEST_RUN_DIR, service, &subcall_cfg);
    check("apps logical subcall-limit client ready",
          refresh_until_ready(&client, 100, 10));
    uint32_t subcall_pids[] = {11, 22, 33};
    err = nipc_client_call_apps_lookup(&client, subcall_pids, 3, &view);
    check("apps logical subcall limit rejected", err == NIPC_ERR_OVERFLOW);
    check("apps subcall limit stopped after first server call",
          apps_subcall_state.calls == 1);
    nipc_client_close(&client);
    stop_lookup_server_drain(&apps_sctx, server_thread);
}

static void test_lookup_rejects_no_progress_payload_exceeded(void)
{
    printf("--- Typed lookup rejects no-progress PAYLOAD_EXCEEDED ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_no_progress");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_payload_exceeded_first_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups no-progress client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        check("cgroups lookup rejects first payload-exceeded item",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_no_progress");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_payload_exceeded_first_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps no-progress client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = {10, 11};
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects first payload-exceeded item",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void run_cgroups_lookup_bad_response_case(
        const char *suffix,
        nipc_cgroups_lookup_handler_fn handler,
        nipc_error_t expected_err)
{
    char service[64];
    char label[128];
    snprintf(label, sizeof(label), "svc_cgroups_lookup_bad_%s", suffix);
    unique_service(service, sizeof(service), label);

    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    nipc_cgroups_lookup_service_handler_t service_handler = {
        .handle = handler,
        .user = NULL,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &service_handler, NULL);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("cgroups malformed first-response client ready",
          refresh_until_ready(&client, 100, 10));

    nipc_str_view_t paths[] = {
        { .ptr = "/a", .len = 2 },
        { .ptr = "/b", .len = 2 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2,
                                                       &view);
    snprintf(label, sizeof(label), "cgroups rejects %s response", suffix);
    check(label, err == expected_err);

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
}

static void run_apps_lookup_bad_response_case(
        const char *suffix,
        nipc_apps_lookup_handler_fn handler,
        nipc_error_t expected_err)
{
    char service[64];
    char label[128];
    snprintf(label, sizeof(label), "svc_apps_lookup_bad_%s", suffix);
    unique_service(service, sizeof(service), label);

    lookup_server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_server_config();
    nipc_apps_lookup_service_handler_t service_handler = {
        .handle = handler,
        .user = NULL,
    };
    HANDLE server_thread = start_lookup_server_named(
        &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("apps malformed first-response client ready",
          refresh_until_ready(&client, 100, 10));

    uint32_t pids[] = {77, 78};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
    snprintf(label, sizeof(label), "apps rejects %s response", suffix);
    check(label, err == expected_err);

    nipc_client_close(&client);
    stop_lookup_server_drain(&sctx, server_thread);
}

static void test_lookup_rejects_malformed_first_response(void)
{
    printf("--- Typed lookup rejects malformed first response ---\n");

    run_cgroups_lookup_bad_response_case("wrong_echo",
                                         cgroups_lookup_wrong_echo_handler,
                                         NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case("reordered",
                                         cgroups_lookup_reordered_response_handler,
                                         NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case("duplicate",
                                         cgroups_lookup_duplicate_response_handler,
                                         NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case("invalid_status",
                                         cgroups_lookup_invalid_status_handler,
                                         NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case("invalid_status_fields",
                                         cgroups_lookup_invalid_status_fields_handler,
                                         NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case("invalid_label_table",
                                         cgroups_lookup_invalid_label_table_handler,
                                         NIPC_ERR_OUT_OF_BOUNDS);

    run_apps_lookup_bad_response_case("wrong_echo",
                                      apps_lookup_wrong_echo_handler,
                                      NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case("reordered",
                                      apps_lookup_reordered_response_handler,
                                      NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case("duplicate",
                                      apps_lookup_duplicate_response_handler,
                                      NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case("invalid_status",
                                      apps_lookup_invalid_status_handler,
                                      NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case("invalid_status_fields",
                                      apps_lookup_invalid_status_fields_handler,
                                      NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case("invalid_label_table",
                                      apps_lookup_invalid_label_table_handler,
                                      NIPC_ERR_OUT_OF_BOUNDS);
}

static void test_lookup_rejects_mixed_generation_retry(void)
{
    printf("--- Typed lookup rejects mixed-generation retry ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_generation");

        lookup_scale_state_t state = { .mixed_generation = 1 };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 160;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups mixed-generation client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/huge", .len = 5 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
        check("cgroups mixed-generation lookup rejected", err != NIPC_OK);
        check("cgroups mixed-generation used follow-up call", state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_generation");

        lookup_scale_state_t state = { .mixed_generation = 1 };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 320;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps mixed-generation client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = {11, 22, 33};
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
        check("apps mixed-generation lookup rejected", err != NIPC_OK);
        check("apps mixed-generation used follow-up call", state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_rejects_malformed_followup_response(void)
{
    printf("--- Typed lookup rejects malformed follow-up response ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_bad_followup");

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_malformed_followup_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups bad-followup client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
            { .ptr = "/c", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3,
                                                           &view);
        check("cgroups lookup rejects malformed follow-up",
              err == NIPC_ERR_BAD_LAYOUT);
        check("cgroups malformed follow-up used second call", state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_bad_followup");

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_malformed_followup_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps bad-followup client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = {11, 22, 33};
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3,
                                                        &view);
        check("apps lookup rejects malformed follow-up",
              err == NIPC_ERR_BAD_LAYOUT);
        check("apps malformed follow-up used second call", state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_endpoint_gone_after_partial_progress(void)
{
    printf("--- Typed lookup fails when endpoint disappears after partial progress ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_apps_lookup_partial_disconnect");

        raw_lookup_partial_disconnect_ctx_t rctx;
        HANDLE raw_thread = start_raw_lookup_partial_disconnect(
            &rctx, service, LOOKUP_SERVER_APPS);
        if (!raw_thread)
            return;

        check("apps partial-disconnect raw server started",
              InterlockedCompareExchange(&rctx.ready, 0, 0) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps partial-disconnect client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 3, &view,
                                                 1000);
        check("apps lookup fails after endpoint disappears", err != NIPC_OK);

        nipc_client_close(&client);
        check("apps partial-disconnect raw server exited",
              WaitForSingleObject(raw_thread, 10000) == WAIT_OBJECT_0);
        CloseHandle(raw_thread);
        check("apps partial-disconnect raw server done",
              InterlockedCompareExchange(&rctx.done, 0, 0) == 1);
    }

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_cgroups_lookup_partial_disconnect");

        raw_lookup_partial_disconnect_ctx_t rctx;
        HANDLE raw_thread = start_raw_lookup_partial_disconnect(
            &rctx, service, LOOKUP_SERVER_CGROUPS);
        if (!raw_thread)
            return;

        check("cgroups partial-disconnect raw server started",
              InterlockedCompareExchange(&rctx.ready, 0, 0) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups partial-disconnect client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
            { .ptr = "/c", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup_timeout(
            &client, paths, 3, &view, 1000);
        check("cgroups lookup fails after endpoint disappears", err != NIPC_OK);

        nipc_client_close(&client);
        check("cgroups partial-disconnect raw server exited",
              WaitForSingleObject(raw_thread, 10000) == WAIT_OBJECT_0);
        CloseHandle(raw_thread);
        check("cgroups partial-disconnect raw server done",
              InterlockedCompareExchange(&rctx.done, 0, 0) == 1);
    }
}

static void test_lookup_endpoint_gone_before_first_subcall(void)
{
    printf("--- Typed lookup fails when endpoint disappears before first subcall ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_apps_lookup_gone_before_call");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps before-call client ready",
              refresh_until_ready(&client, 100, 10));

        stop_lookup_server_drain(&sctx, server_thread);

        uint32_t pids[] = { 11, 22 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 2, &view,
                                                 1000);
        check("apps lookup fails after endpoint disappears before call",
              err != NIPC_OK);

        nipc_client_close(&client);
    }

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_cgroups_lookup_gone_before_call");

        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups before-call client ready",
              refresh_until_ready(&client, 100, 10));

        stop_lookup_server_drain(&sctx, server_thread);

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup_timeout(
            &client, paths, 2, &view, 1000);
        check("cgroups lookup fails after endpoint disappears before call",
              err != NIPC_OK);

        nipc_client_close(&client);
    }
}

static void test_lookup_endpoint_absent_before_call(void)
{
    printf("--- Typed lookup fails when endpoint is absent before call ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_absent");

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        bool changed = nipc_client_refresh(&client);
        check("apps absent refresh changed state", changed);
        check("apps absent client is NOT_FOUND",
              client.state == NIPC_CLIENT_NOT_FOUND);

        uint32_t pids[] = { 11, 22 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 2, &view,
                                                 1000);
        check("apps lookup absent endpoint rejected",
              err == NIPC_ERR_NOT_READY);

        nipc_client_close(&client);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_absent");

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        bool changed = nipc_client_refresh(&client);
        check("cgroups absent refresh changed state", changed);
        check("cgroups absent client is NOT_FOUND",
              client.state == NIPC_CLIENT_NOT_FOUND);

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup_timeout(
            &client, paths, 2, &view, 1000);
        check("cgroups lookup absent endpoint rejected",
              err == NIPC_ERR_NOT_READY);

        nipc_client_close(&client);
    }
}

static void test_lookup_timeout_during_followup_subcall(void)
{
    printf("--- Typed lookup timeout during follow-up subcall ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_cgroups_lookup_followup_timeout");

        lookup_scale_state_t state = { .slow_second_call = 1 };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 160;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups follow-up timeout client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/huge", .len = 5 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup_timeout(&client, paths, 3, &view,
                                                    75);
        check("cgroups follow-up timeout rejected", err == NIPC_ERR_TIMEOUT);
        check("cgroups follow-up timeout reached second subcall",
              state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_apps_lookup_followup_timeout");

        lookup_scale_state_t state = { .slow_second_call = 1 };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 320;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps follow-up timeout client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 3, &view, 75);
        check("apps follow-up timeout rejected", err == NIPC_ERR_TIMEOUT);
        check("apps follow-up timeout reached second subcall",
              state.calls >= 2);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_abort_during_followup_subcall(void)
{
    printf("--- Typed lookup abort during follow-up subcall ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_cgroups_lookup_followup_abort");

        lookup_scale_state_t state = {
            .slow_second_call = 1,
            .signal_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 160;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("cgroups follow-up abort client ready",
              refresh_until_ready(&client, 100, 10));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/huge", .len = 5 },
            { .ptr = "/b", .len = 2 },
        };
        lookup_call_thread_ctx_t call_ctx;
        memset(&call_ctx, 0, sizeof(call_ctx));
        call_ctx.client = &client;
        call_ctx.kind = LOOKUP_SERVER_CGROUPS;
        call_ctx.paths = paths;
        call_ctx.path_count = 3;
        call_ctx.timeout_ms = 5000;

        HANDLE call_thread =
            CreateThread(NULL, 0, lookup_call_thread, &call_ctx, 0, NULL);
        check("cgroups follow-up abort call thread created",
              call_thread != NULL);
        if (call_thread) {
            check("cgroups follow-up abort reached second subcall",
                  lookup_scale_wait_for_second_call(&state));
            nipc_client_abort(&client);
            check("cgroups follow-up abort call thread exited",
                  WaitForSingleObject(call_thread, 10000) == WAIT_OBJECT_0);
            CloseHandle(call_thread);
            check("cgroups follow-up abort rejected",
                  call_ctx.err == NIPC_ERR_ABORTED);
            check("cgroups follow-up abort used follow-up call",
                  state.calls >= 2);
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service),
                       "svc_apps_lookup_followup_abort");

        lookup_scale_state_t state = {
            .slow_second_call = 1,
            .signal_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 320;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_scale_handler,
            .user = &state,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("apps follow-up abort client ready",
              refresh_until_ready(&client, 100, 10));

        uint32_t pids[] = { 11, 22, 33 };
        lookup_call_thread_ctx_t call_ctx;
        memset(&call_ctx, 0, sizeof(call_ctx));
        call_ctx.client = &client;
        call_ctx.kind = LOOKUP_SERVER_APPS;
        call_ctx.pids = pids;
        call_ctx.pid_count = 3;
        call_ctx.timeout_ms = 5000;

        HANDLE call_thread =
            CreateThread(NULL, 0, lookup_call_thread, &call_ctx, 0, NULL);
        check("apps follow-up abort call thread created", call_thread != NULL);
        if (call_thread) {
            check("apps follow-up abort reached second subcall",
                  lookup_scale_wait_for_second_call(&state));
            nipc_client_abort(&client);
            check("apps follow-up abort call thread exited",
                  WaitForSingleObject(call_thread, 10000) == WAIT_OBJECT_0);
            CloseHandle(call_thread);
            check("apps follow-up abort rejected",
                  call_ctx.err == NIPC_ERR_ABORTED);
            check("apps follow-up abort used follow-up call",
                  state.calls >= 2);
        }

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
}

static void test_lookup_init_and_client_guards(void)
{
    printf("--- Typed lookup init / client guards ---\n");

    nipc_managed_server_t server;
    nipc_server_config_t scfg = default_typed_server_config();

    check("cgroups lookup init rejects null handler",
          nipc_server_init_cgroups_lookup(&server, TEST_RUN_DIR,
                                          "svc_lookup_null_cgroups",
                                          &scfg, 1, NULL) ==
              NIPC_ERR_BAD_LAYOUT);
    check("apps lookup init rejects null handler",
          nipc_server_init_apps_lookup(&server, TEST_RUN_DIR,
                                       "svc_lookup_null_apps",
                                       &scfg, 1, NULL) ==
              NIPC_ERR_BAD_LAYOUT);

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_lookup_init_cgroups");
        nipc_server_config_t zero_cfg = scfg;
        zero_cfg.max_response_payload_bytes = 0;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        nipc_error_t err = nipc_server_init_cgroups_lookup(
            &server, TEST_RUN_DIR, service, &zero_cfg, 0, &handler);
        check("cgroups lookup init defaults ok", err == NIPC_OK);
        if (err == NIPC_OK) {
            check("cgroups lookup worker floor",
                  server.worker_count == 1 &&
                  server.expected_method_code == NIPC_METHOD_CGROUPS_LOOKUP);
            nipc_server_destroy(&server);
        }
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_lookup_init_apps");
        nipc_server_config_t zero_cfg = scfg;
        zero_cfg.max_response_payload_bytes = 0;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_test_handler,
            .user = NULL,
        };
        nipc_error_t err = nipc_server_init_apps_lookup(
            &server, TEST_RUN_DIR, service, &zero_cfg, 0, &handler);
        check("apps lookup init defaults ok", err == NIPC_OK);
        if (err == NIPC_OK) {
            check("apps lookup worker floor",
                  server.worker_count == 1 &&
                  server.expected_method_code == NIPC_METHOD_APPS_LOOKUP);
            nipc_server_destroy(&server);
        }
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_lookup_null_cfg");
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        nipc_error_t err = nipc_server_init_cgroups_lookup(
            &server, TEST_RUN_DIR, service, NULL, 1, &handler);
        check("cgroups lookup init accepts null config defaults", err == NIPC_OK);
        if (err == NIPC_OK)
            nipc_server_destroy(&server);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_cgroups_lookup_bad_req");

        lookup_server_thread_ctx_t sctx;
        nipc_cgroups_lookup_service_handler_t handler = {
            .handle = cgroups_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_CGROUPS, &scfg, &handler, NULL);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("bad cgroups lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, NULL, 1, &view);
        check("null cgroups lookup path array rejected", err == NIPC_ERR_BAD_LAYOUT);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("second bad cgroups lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_str_view_t bad_paths[] = {
            { .ptr = NULL, .len = 1 },
        };
        err = nipc_client_call_cgroups_lookup(&client, bad_paths, 1, &view);
        check("bad cgroups lookup request rejected", err == NIPC_ERR_BAD_LAYOUT);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("faulted cgroups lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_str_view_t valid_paths[] = {
            { .ptr = "/known", .len = 6 },
        };
        nipc_win_service_test_fault_set(
            NIPC_WIN_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        err = nipc_client_call_cgroups_lookup(&client, valid_paths, 1, &view);
        clear_test_faults();
        check("cgroups lookup send buffer fault rejected", err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_apps_lookup_bad_req");

        lookup_server_thread_ctx_t sctx;
        nipc_apps_lookup_service_handler_t handler = {
            .handle = apps_lookup_test_handler,
            .user = NULL,
        };
        HANDLE server_thread = start_lookup_server_named(
            &sctx, service, LOOKUP_SERVER_APPS, &scfg, NULL, &handler);
        if (!server_thread)
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("bad apps lookup client ready", refresh_until_ready(&client, 100, 10));

        nipc_apps_lookup_resp_view_t view;
        uint32_t pids[] = {1234};
        nipc_win_service_test_fault_set(
            NIPC_WIN_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 1, &view);
        clear_test_faults();
        check("apps lookup send buffer fault rejected", err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("second bad apps lookup client ready", refresh_until_ready(&client, 100, 10));

        err = nipc_client_call_apps_lookup(&client, NULL, 1, &view);
        check("bad apps lookup request rejected", err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server_drain(&sctx, server_thread);
    }
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

static void test_shm_capacity_overflow_rejects_shm_only_client(void)
{
    printf("--- SHM capacity overflow rejects SHM-only client ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_shm_cap_overflow");

    nipc_server_config_t scfg = default_typed_hybrid_server_config();
    scfg.max_response_payload_bytes = UINT32_MAX;

    server_thread_ctx_t sctx;
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_SHM_HYBRID;
    ccfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    check("SHM-only client is incompatible after capacity guard",
          client.state == NIPC_CLIENT_INCOMPATIBLE);

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

static void test_server_shm_create_fault_falls_back_and_recovers(void)
{
    printf("--- Server SHM create fault falls back to baseline / recovers ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_server_shm_fault");

    server_thread_ctx_t sctx;
    nipc_server_config_t scfg = default_typed_hybrid_server_config();

    nipc_win_shm_test_fault_set(NIPC_WIN_SHM_TEST_FAULT_CREATE_MAPPING,
                                ERROR_ACCESS_DENIED, 0);
    HANDLE server_thread = start_server_named(&sctx, service, 4, &scfg, &full_service_handler);
    if (!server_thread)
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_typed_hybrid_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

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
    RUN_TEST(test_lookup_typed_calls);
    RUN_TEST(test_lookup_zero_item_calls);
    RUN_TEST(test_lookup_payload_exceeded_retry);
    RUN_TEST(test_lookup_proactive_request_split);
    RUN_TEST(test_lookup_large_logical_calls);
    RUN_TEST(test_cgroups_lookup_oversized_request_key);
    RUN_TEST(test_lookup_logical_limits);
    RUN_TEST(test_lookup_rejects_no_progress_payload_exceeded);
    RUN_TEST(test_lookup_rejects_malformed_first_response);
    RUN_TEST(test_lookup_rejects_mixed_generation_retry);
    RUN_TEST(test_lookup_rejects_malformed_followup_response);
    RUN_TEST(test_lookup_endpoint_gone_after_partial_progress);
    RUN_TEST(test_lookup_endpoint_gone_before_first_subcall);
    RUN_TEST(test_lookup_endpoint_absent_before_call);
    RUN_TEST(test_lookup_timeout_during_followup_subcall);
    RUN_TEST(test_lookup_abort_during_followup_subcall);
    RUN_TEST(test_lookup_init_and_client_guards);
    RUN_TEST(test_client_fault_injection_disconnects_and_recovers);
    RUN_TEST(test_server_init_fault_injection);
    RUN_TEST(test_refresh_from_broken_state);
    RUN_TEST(test_retry_on_broken_session);
    RUN_TEST(test_handler_failure);
    RUN_TEST(test_client_auth_failure);
    RUN_TEST(test_client_incompatible);
    RUN_TEST(test_shm_capacity_overflow_rejects_shm_only_client);
    RUN_TEST(test_cache_refresh_without_server);
    RUN_TEST(test_cache_refresh_rebuilds_and_linear_lookup);
    RUN_TEST(test_cache_empty_snapshot);
    RUN_TEST(test_server_shm_create_fault_falls_back_and_recovers);
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
