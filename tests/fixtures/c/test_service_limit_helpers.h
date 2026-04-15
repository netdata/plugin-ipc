#ifndef TEST_SERVICE_LIMIT_HELPERS_H
#define TEST_SERVICE_LIMIT_HELPERS_H

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR      "/tmp/nipc_service_limits"
#define RESPONSE_BUF_SIZE 65536

static int g_pass = 0;
static int g_fail = 0;
static int g_service_counter = 0;

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

static bool on_large_snapshot(void *user,
                              const nipc_cgroups_req_t *request,
                              nipc_cgroups_builder_t *builder)
{
    char *large_path = (char *)user;

    if (request->layout_version != 1 || request->flags != 0)
        return false;

    return nipc_cgroups_builder_add(builder,
                                    4004,
                                    0,
                                    1,
                                    "large-item",
                                    (uint32_t)strlen("large-item"),
                                    large_path,
                                    (uint32_t)strlen(large_path)) == NIPC_OK;
}

static nipc_cgroups_service_handler_t full_service_handler = {
    .handle = on_snapshot,
    .snapshot_max_items = 3,
    .user = NULL,
};

static nipc_cgroups_service_handler_t large_service_handler = {
    .handle = on_large_snapshot,
    .snapshot_max_items = 1,
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

static void ensure_run_dir(void)
{
    mkdir(TEST_RUN_DIR, 0700);
}

#endif
