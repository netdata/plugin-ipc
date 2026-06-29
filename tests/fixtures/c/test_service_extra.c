#ifndef _WIN32

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc_service_common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;
static int g_service_counter = 0;

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "/tmp/nipc_service_extra"
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
}

static void unique_service(char *buf, size_t len, const char *prefix)
{
    int n = __sync_add_and_fetch(&g_service_counter, 1);
    snprintf(buf, len, "%s_%d_%ld", prefix, n, (long)getpid());
}

static int cache_has(nipc_cgroups_cache_t *cache, uint32_t hash, const char *name)
{
    nipc_cgroups_cache_read_guard_t guard;
    if (!nipc_cgroups_cache_read_lock(cache, &guard))
        return 0;

    const nipc_cgroups_cache_item_view_t *item =
        nipc_cgroups_cache_get(&guard, hash, name);
    int found = item != NULL;
    nipc_cgroups_cache_read_unlock(&guard);
    return found;
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

static nipc_cgroups_service_handler_t full_service_handler = {
    .handle = on_snapshot,
    .snapshot_max_items = 3,
    .user = NULL,
};

typedef struct {
    char service[64];
    int worker_count;
    nipc_server_config_t config;
    nipc_cgroups_service_handler_t service_handler;
    nipc_managed_server_t server;
    int ready;
    int done;
    int init_ok;
} server_thread_ctx_t;

static void *managed_server_thread(void *arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_error_t err = nipc_server_init_typed(&ctx->server, TEST_RUN_DIR,
                                              ctx->service, &ctx->config,
                                              ctx->worker_count,
                                              &ctx->service_handler);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed for %s: %d\n", ctx->service, err);
        __atomic_store_n(&ctx->init_ok, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->init_ok, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int start_server_named(server_thread_ctx_t *ctx,
                              const char *service,
                              int worker_count,
                              const nipc_server_config_t *config,
                              const nipc_cgroups_service_handler_t *service_handler,
                              pthread_t *tid)
{
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->service, service, sizeof(ctx->service) - 1);
    ctx->worker_count = worker_count;
    ctx->config = *config;
    ctx->service_handler = *service_handler;

    if (pthread_create(tid, NULL, managed_server_thread, ctx) != 0) {
        check("server thread created", 0);
        return 0;
    }

    check("server thread created", 1);
    for (int i = 0; i < 2000
         && !__atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);

    check("server ready", __atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE) == 1);
    check("server init ok", __atomic_load_n(&ctx->init_ok, __ATOMIC_ACQUIRE) == 1);
    return __atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE) == 1 &&
           __atomic_load_n(&ctx->init_ok, __ATOMIC_ACQUIRE) == 1;
}

static int start_default_server_named(server_thread_ctx_t *ctx,
                                      const char *service,
                                      int worker_count,
                                      pthread_t *tid)
{
    nipc_server_config_t config = default_typed_server_config();
    return start_server_named(ctx, service, worker_count, &config,
                              &full_service_handler, tid);
}

static void stop_server_drain(server_thread_ctx_t *ctx, pthread_t tid)
{
    nipc_server_stop(&ctx->server);
    check("server thread exited", pthread_join(tid, NULL) == 0);
    check("server drain completed", nipc_server_drain(&ctx->server, 10000));
    nipc_server_destroy(&ctx->server);
}

static int refresh_until_ready(nipc_client_ctx_t *client, int max_tries, useconds_t sleep_us)
{
    for (int i = 0; i < max_tries; i++) {
        nipc_client_refresh(client);
        if (nipc_client_ready(client))
            return 1;
        usleep(sleep_us);
    }

    return 0;
}

static int refresh_until_state(nipc_client_ctx_t *client,
                               nipc_client_state_t target_state,
                               int max_tries,
                               useconds_t sleep_us)
{
    for (int i = 0; i < max_tries; i++) {
        nipc_client_refresh(client);
        if (client->state == target_state)
            return 1;
        usleep(sleep_us);
    }

    return client->state == target_state;
}

static void clear_test_faults(void)
{
    nipc_posix_service_test_fault_clear();
}

static void ensure_run_dir(void)
{
    mkdir(TEST_RUN_DIR, 0700);
}

static void test_common_service_helpers(void)
{
    printf("--- Common service helpers ---\n");

    {
        uint32_t msg_len = 0;
        check("u32 header length rejects overflow",
              !nipc_service_common_header_payload_len_u32(UINT32_MAX,
                                                          &msg_len));
    }

    {
        char dst[4] = "xxx";

        nipc_service_common_copy_cstr_field(NULL, sizeof(dst), "abc");
        check("copy cstr tolerates null destination", 1);

        nipc_service_common_copy_cstr_field(dst, 0, "abc");
        check("copy cstr tolerates zero destination size", 1);

        nipc_service_common_copy_cstr_field(dst, sizeof(dst), NULL);
        check("copy cstr null source clears destination", dst[0] == '\0');

        nipc_service_common_copy_cstr_field(dst, sizeof(dst), "abcdef");
        check("copy cstr truncates and terminates",
              strcmp(dst, "abc") == 0);
    }

    {
        nipc_client_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));

        check("call timeout uses library default",
              nipc_service_common_client_call_timeout_ms(&ctx, 0)
              == NIPC_CLIENT_CALL_TIMEOUT_DEFAULT_MS);
        ctx.call_timeout_ms = 1234;
        check("call timeout uses context default",
              nipc_service_common_client_call_timeout_ms(&ctx, 0) == 1234);
        check("call timeout uses explicit timeout",
              nipc_service_common_client_call_timeout_ms(&ctx, 7) == 7);
    }

    {
        nipc_header_t req = {
            .code = NIPC_METHOD_INCREMENT,
            .flags = NIPC_FLAG_BATCH,
            .item_count = 3,
            .message_id = 42,
        };
        nipc_header_t resp;

        nipc_service_common_prepare_response_header(&req, &resp);
        check("batch response header preserves item count",
              resp.item_count == req.item_count);
        check("batch response header preserves batch flag",
              (resp.flags & NIPC_FLAG_BATCH) != 0);
    }

    {
        nipc_client_ctx_t ctx;
        nipc_header_t hdr;

        memset(&ctx, 0, sizeof(ctx));
        memset(&hdr, 0, sizeof(hdr));
        hdr.transport_status = NIPC_STATUS_BAD_ENVELOPE;
        check("bad envelope status maps to bad layout",
              nipc_service_common_response_status_to_error(&ctx, &hdr)
              == NIPC_ERR_BAD_LAYOUT);

        hdr.transport_status = 999;
        check("unknown response status maps to bad layout",
              nipc_service_common_response_status_to_error(&ctx, &hdr)
              == NIPC_ERR_BAD_LAYOUT);
    }

    {
        nipc_client_ctx_t ctx;
        nipc_header_t hdr;
        uint8_t *msg = NULL;
        size_t msg_len = 0;

        memset(&ctx, 0, sizeof(ctx));
        check("prepare shm request rejects u32 overflow",
              nipc_service_common_client_prepare_shm_request(
                  &ctx, &hdr, NULL, (size_t)UINT32_MAX + 1u,
                  &msg, &msg_len) == NIPC_ERR_OVERFLOW);
    }

    {
        nipc_managed_server_t server;
        nipc_header_t resp;
        size_t response_len = 32;
        bool close_after_response = false;

        memset(&server, 0, sizeof(server));
        memset(&resp, 0, sizeof(resp));
        nipc_service_common_apply_dispatch_result(
            &server, NIPC_OK, 8, RESPONSE_BUF_SIZE, true,
            &resp, &response_len, &close_after_response);
        check("oversize dispatch result returns limit status",
              resp.transport_status == NIPC_STATUS_LIMIT_EXCEEDED);
        check("oversize dispatch result closes session",
              close_after_response && response_len == 0);

        response_len = 16;
        close_after_response = false;
        memset(&resp, 0, sizeof(resp));
        nipc_service_common_apply_dispatch_result(
            &server, NIPC_ERR_OVERFLOW, RESPONSE_BUF_SIZE,
            UINT32_MAX / 2u, true, &resp, &response_len,
            &close_after_response);
        check("overflow dispatch result returns limit status",
              resp.transport_status == NIPC_STATUS_LIMIT_EXCEEDED);
        check("overflow dispatch result closes session",
              close_after_response && response_len == 0);
    }
}

static void test_lookup_remaining_timeout_helpers(void)
{
    printf("--- Lookup remaining timeout helpers ---\n");

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, "svc_lookup_timeout_helpers", &ccfg);

    uint32_t timeout = 0;
    nipc_client_abort(&client);
    check("apps lookup remaining timeout reports abort",
          nipc_apps_lookup_remaining_timeout_for_tests(&client, 1, &timeout)
          == NIPC_ERR_ABORTED);
    check("cgroups lookup remaining timeout reports abort",
          nipc_cgroups_lookup_remaining_timeout_for_tests(&client, 1, &timeout)
          == NIPC_ERR_ABORTED);

    nipc_client_clear_abort(&client);
    check("apps lookup remaining timeout reports expired deadline",
          nipc_apps_lookup_remaining_timeout_for_tests(&client, 0, &timeout)
          == NIPC_ERR_TIMEOUT);
    check("cgroups lookup remaining timeout reports expired deadline",
          nipc_cgroups_lookup_remaining_timeout_for_tests(&client, 0, &timeout)
          == NIPC_ERR_TIMEOUT);

    nipc_client_close(&client);
}

static void test_client_fault_injection_disconnects_and_recovers(void)
{
    printf("--- Client fault injection disconnects / recovers ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_resp_fault");

        server_thread_ctx_t sctx;
        pthread_t tid;
        if (!start_default_server_named(&sctx, service, 4, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_RESPONSE_BUF_REALLOC, 0);
        check("response buffer alloc fault disconnects client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10000));
        check("response buffer alloc fault leaves client not ready",
              !nipc_client_ready(&client));
        clear_test_faults();
        check("response buffer alloc fault recovers",
              refresh_until_ready(&client, 100, 10000));

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_send_fault");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        check("send buffer alloc fault disconnects hybrid client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10000));
        clear_test_faults();
        check("send buffer alloc fault recovers",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_client_shm_ctx_fault");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_SHM_CTX_CALLOC, 0);
        check("client SHM ctx alloc fault disconnects hybrid client",
              refresh_until_state(&client, NIPC_CLIENT_DISCONNECTED, 20, 10000));
        check("client SHM ctx alloc fault leaves session invalid",
              !client.session_valid && client.shm == NULL);
        clear_test_faults();
        check("client SHM ctx alloc fault recovers",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }
}

static void test_server_init_fault_injection(void)
{
    printf("--- Server init fault injection ---\n");

    char service[64];
    unique_service(service, sizeof(service), "svc_server_init_fault");

    nipc_managed_server_t server;
    nipc_server_config_t scfg = default_typed_server_config();

    nipc_posix_service_test_fault_set(
        NIPC_POSIX_SERVICE_TEST_FAULT_SERVER_SESSIONS_CALLOC, 0);
    check("server session array alloc fault returns overflow",
          nipc_server_init_typed(&server, TEST_RUN_DIR, service,
                                 &scfg, 4, &full_service_handler)
          == NIPC_ERR_OVERFLOW);
    clear_test_faults();

    check("server init recovers after sessions alloc fault",
          nipc_server_init_typed(&server, TEST_RUN_DIR, service,
                                 &scfg, 4, &full_service_handler)
          == NIPC_OK);
    nipc_server_destroy(&server);
}

static void test_cache_fault_injection(void)
{
    struct {
        int site;
        const char *label;
        int expect_refresh_ok;
    } cases[] = {
        { NIPC_POSIX_SERVICE_TEST_FAULT_CACHE_ITEMS_CALLOC,
          "cache items alloc fault fails refresh", 0 },
        { NIPC_POSIX_SERVICE_TEST_FAULT_CACHE_ITEM_NAME_MALLOC,
          "cache item name alloc fault fails refresh", 0 },
        { NIPC_POSIX_SERVICE_TEST_FAULT_CACHE_ITEM_PATH_MALLOC,
          "cache item path alloc fault fails refresh", 0 },
        { NIPC_POSIX_SERVICE_TEST_FAULT_CACHE_BUCKETS_CALLOC,
          "cache bucket alloc fault falls back to linear lookup", 1 },
    };

    printf("--- Cache fault injection ---\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char service[64];
        unique_service(service, sizeof(service), "svc_cache_fault");

        server_thread_ctx_t sctx;
        pthread_t tid;
        if (!start_default_server_named(&sctx, service, 4, &tid))
            return;

        nipc_cgroups_cache_t cache;
        nipc_client_config_t ccfg = default_client_config();
        nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, service, &ccfg);

        nipc_posix_service_test_fault_set(cases[i].site, 0);
        {
            int ok = nipc_cgroups_cache_refresh(&cache);
            check(cases[i].label, ok == cases[i].expect_refresh_ok);
            if (cases[i].expect_refresh_ok) {
                nipc_cgroups_cache_status_t status;
                nipc_cgroups_cache_status(&cache, &status);
                check("cache bucket fault still populates snapshot",
                      status.populated && status.item_count == 3);
                check("cache bucket fault still serves lookup",
                      cache_has(&cache, 2002, "k8s-pod-xyz"));
            } else {
                nipc_cgroups_cache_status_t status;
                nipc_cgroups_cache_status(&cache, &status);
                check("cache allocation fault increments failure_count",
                      status.refresh_failure_count == 1);
            }
        }

        clear_test_faults();
        check("cache refresh recovers after fault",
              nipc_cgroups_cache_refresh(&cache));
        check("cache refresh recovery lookup works",
              cache_has(&cache, 1001, "docker-abc123"));

        nipc_cgroups_cache_close(&cache);
        stop_server_drain(&sctx, tid);
    }
}

int main(void)
{
    printf("=== POSIX Service Extra Tests ===\n\n");
    ensure_run_dir();

    test_common_service_helpers();
    test_lookup_remaining_timeout_helpers();
    test_client_fault_injection_disconnects_and_recovers();
    test_server_init_fault_injection();
    test_cache_fault_injection();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("POSIX service extra tests skipped (Windows build)\n");
    return 0;
}

#endif
