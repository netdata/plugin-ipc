/*
 * test_cache.c - L3 client-side cgroups snapshot cache tests.
 *
 * Tests: full round-trip, refresh failure preserves cache, reconnect
 * rebuilds, lookup not found, empty cache, large dataset.
 *
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR  "/tmp/nipc_cache_test"
#define AUTH_TOKEN    0xDEADBEEFCAFEBABEull
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

static void ensure_run_dir(void)
{
    mkdir(TEST_RUN_DIR, 0700);
}

static void cleanup_all(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
    nipc_shm_cleanup_stale(TEST_RUN_DIR, service);
}

static nipc_uds_server_config_t default_server_config(void)
{
    return (nipc_uds_server_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 4,
    };
}

static nipc_client_config_t default_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                = AUTH_TOKEN,
    };
}

/* ------------------------------------------------------------------ */
/*  Cgroups snapshot handler (server side)                              */
/* ------------------------------------------------------------------ */

static nipc_error_t test_cgroups_handler(void *user,
                                         const nipc_header_t *request_hdr,
                                         const uint8_t *request_payload,
                                         size_t request_len,
                                         uint8_t *response_buf,
                                         size_t response_buf_size,
                                         size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               3, 1, 42);

    struct {
        uint32_t hash, options, enabled;
        const char *name, *path;
    } items[] = {
        { 1001, 0, 1, "docker-abc123", "/sys/fs/cgroup/docker/abc123" },
        { 2002, 0, 1, "k8s-pod-xyz",   "/sys/fs/cgroup/kubepods/xyz" },
        { 3003, 0, 0, "systemd-user",  "/sys/fs/cgroup/user.slice/user-1000" },
    };

    for (int i = 0; i < 3; i++) {
        err = nipc_cgroups_builder_add(&builder,
            items[i].hash, items[i].options, items[i].enabled,
            items[i].name, (uint32_t)strlen(items[i].name),
            items[i].path, (uint32_t)strlen(items[i].path));
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/* ------------------------------------------------------------------ */
/*  Large dataset handler                                              */
/* ------------------------------------------------------------------ */

#define LARGE_N 1000
#define LARGE_BUF_SIZE (256 * LARGE_N)

static nipc_error_t large_handler(void *user,
                                  const nipc_header_t *request_hdr,
                                  const uint8_t *request_payload,
                                  size_t request_len,
                                  uint8_t *response_buf,
                                  size_t response_buf_size,
                                  size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               LARGE_N, 1, 100);

    for (int i = 0; i < LARGE_N; i++) {
        char name[64], path[128];
        snprintf(name, sizeof(name), "cgroup-%d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/test/%d", i);

        uint32_t enabled = (i % 3 == 0) ? 0 : 1;
        err = nipc_cgroups_builder_add(&builder,
            (uint32_t)(i + 1000), 0, enabled,
            name, (uint32_t)strlen(name),
            path, (uint32_t)strlen(path));
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/* ------------------------------------------------------------------ */
/*  Server thread context                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    nipc_server_handler_fn handler;
    size_t resp_buf_size;
    int ready;  /* use __atomic builtins for cross-thread access */
    int done;   /* use __atomic builtins for cross-thread access */
} server_thread_ctx_t;

static void *server_thread_fn(void *arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.max_response_payload_bytes = (uint32_t)ctx->resp_buf_size;

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        1, NIPC_METHOD_CGROUPS_SNAPSHOT, ctx->handler, NULL);

    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_server(server_thread_ctx_t *sctx, const char *service,
                          nipc_server_handler_fn handler,
                          size_t resp_buf_size,
                          pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->handler = handler;
    sctx->resp_buf_size = resp_buf_size;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, server_thread_fn, sctx);

    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void stop_server(server_thread_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

/* ------------------------------------------------------------------ */
/*  Test 1: Full round-trip                                            */
/* ------------------------------------------------------------------ */

static void test_full_round_trip(void)
{
    printf("Test 1: L3 cache full round-trip\n");
    const char *svc = "cache_rt";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, RESPONSE_BUF_SIZE, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    check("not ready before refresh", !nipc_cgroups_cache_ready(&cache));

    /* Refresh populates the cache */
    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("refresh updated cache", updated);
    check("ready after refresh", nipc_cgroups_cache_ready(&cache));

    /* Lookup by hash + name */
    const nipc_cgroups_cache_item_t *item =
        nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123");
    check("lookup found item", item != NULL);
    if (item) {
        check("item hash", item->hash == 1001);
        check("item options", item->options == 0);
        check("item enabled", item->enabled == 1);
        check("item name", strcmp(item->name, "docker-abc123") == 0);
        check("item path", strcmp(item->path, "/sys/fs/cgroup/docker/abc123") == 0);
    }

    const nipc_cgroups_cache_item_t *item2 =
        nipc_cgroups_cache_lookup(&cache, 3003, "systemd-user");
    check("lookup item 2 found", item2 != NULL);
    if (item2) {
        check("item 2 enabled == 0", item2->enabled == 0);
    }

    /* Status */
    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("status populated", status.populated);
    check("status item_count == 3", status.item_count == 3);
    check("status systemd_enabled == 1", status.systemd_enabled == 1);
    check("status generation == 42", status.generation == 42);
    check("status success_count == 1", status.refresh_success_count == 1);
    check("status failure_count == 0", status.refresh_failure_count == 0);
    check("status connection_state == READY", status.connection_state == NIPC_CLIENT_READY);
    check("status last_refresh_ts > 0", status.last_refresh_ts > 0);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Refresh failure preserves cache                            */
/* ------------------------------------------------------------------ */

static void test_refresh_failure_preserves(void)
{
    printf("Test 2: L3 refresh failure preserves cache\n");
    const char *svc = "cache_preserve";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, RESPONSE_BUF_SIZE, &tid);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    /* First refresh populates cache */
    check("first refresh ok", nipc_cgroups_cache_refresh(&cache));
    check("ready", nipc_cgroups_cache_ready(&cache));
    check("lookup ok", nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    /* Kill server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000);

    /* Refresh fails, old cache preserved */
    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("refresh fails", !updated);
    check("still ready (old cache)", nipc_cgroups_cache_ready(&cache));
    check("old item still found",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("success_count == 1", status.refresh_success_count == 1);
    check("failure_count >= 1", status.refresh_failure_count >= 1);

    nipc_cgroups_cache_close(&cache);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Reconnect rebuilds cache                                   */
/* ------------------------------------------------------------------ */

static void test_reconnect_rebuilds(void)
{
    printf("Test 3: L3 reconnect rebuilds cache\n");
    const char *svc = "cache_reconn";
    cleanup_all(svc);

    server_thread_ctx_t sctx1;
    pthread_t tid1;
    start_server(&sctx1, svc, test_cgroups_handler, RESPONSE_BUF_SIZE, &tid1);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    check("first refresh ok", nipc_cgroups_cache_refresh(&cache));

    nipc_cgroups_cache_status_t s1;
    nipc_cgroups_cache_status(&cache, &s1);
    check("item_count == 3", s1.item_count == 3);

    /* Kill and restart */
    stop_server(&sctx1, tid1);
    cleanup_all(svc);
    usleep(50000);

    server_thread_ctx_t sctx2;
    pthread_t tid2;
    start_server(&sctx2, svc, test_cgroups_handler, RESPONSE_BUF_SIZE, &tid2);

    /* Refresh reconnects and rebuilds */
    check("refresh after reconnect", nipc_cgroups_cache_refresh(&cache));
    check("still ready", nipc_cgroups_cache_ready(&cache));

    nipc_cgroups_cache_status_t s2;
    nipc_cgroups_cache_status(&cache, &s2);
    check("item_count still 3", s2.item_count == 3);
    check("success_count == 2", s2.refresh_success_count == 2);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx2, tid2);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Lookup not found                                           */
/* ------------------------------------------------------------------ */

static void test_lookup_not_found(void)
{
    printf("Test 4: L3 lookup not found\n");
    const char *svc = "cache_notfound";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, RESPONSE_BUF_SIZE, &tid);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);
    nipc_cgroups_cache_refresh(&cache);

    /* Non-existent hash */
    check("nonexistent -> NULL",
          nipc_cgroups_cache_lookup(&cache, 9999, "nonexistent") == NULL);

    /* Correct hash, wrong name */
    check("wrong name -> NULL",
          nipc_cgroups_cache_lookup(&cache, 1001, "wrong-name") == NULL);

    /* Correct name, wrong hash */
    check("wrong hash -> NULL",
          nipc_cgroups_cache_lookup(&cache, 9999, "docker-abc123") == NULL);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Empty cache                                                */
/* ------------------------------------------------------------------ */

static void test_empty_cache(void)
{
    printf("Test 5: L3 empty cache\n");
    const char *svc = "cache_empty";
    cleanup_all(svc);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    check("not ready", !nipc_cgroups_cache_ready(&cache));
    check("lookup -> NULL",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") == NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("not populated", !status.populated);
    check("item_count == 0", status.item_count == 0);
    check("success_count == 0", status.refresh_success_count == 0);
    check("failure_count == 0", status.refresh_failure_count == 0);

    nipc_cgroups_cache_close(&cache);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Large dataset (1000+ items)                                */
/* ------------------------------------------------------------------ */

static void test_large_dataset(void)
{
    printf("Test 6: L3 large dataset (%d items)\n", LARGE_N);
    const char *svc = "cache_large";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, large_handler, LARGE_BUF_SIZE, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_response_payload_bytes = LARGE_BUF_SIZE;
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    /* Resize the internal response buffer for the large dataset */
    free(cache.response_buf);
    cache.response_buf_size = LARGE_BUF_SIZE;
    cache.response_buf = malloc(cache.response_buf_size);

    check("refresh ok", nipc_cgroups_cache_refresh(&cache));

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("item_count == 1000", status.item_count == LARGE_N);

    /* Verify all lookups */
    int all_ok = 1;
    for (int i = 0; i < LARGE_N; i++) {
        char name[64], path[128];
        snprintf(name, sizeof(name), "cgroup-%d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/test/%d", i);

        const nipc_cgroups_cache_item_t *item =
            nipc_cgroups_cache_lookup(&cache, (uint32_t)(i + 1000), name);
        if (!item) {
            printf("  FAIL: item %d not found\n", i);
            all_ok = 0;
            break;
        }
        if (item->hash != (uint32_t)(i + 1000) ||
            strcmp(item->path, path) != 0) {
            printf("  FAIL: item %d data mismatch\n", i);
            all_ok = 0;
            break;
        }
    }
    check("all 1000 lookups correct", all_ok);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    ensure_run_dir();
    setbuf(stdout, NULL);

    printf("=== L3 Cache Tests ===\n\n");

    test_full_round_trip();          printf("\n");
    test_refresh_failure_preserves(); printf("\n");
    test_reconnect_rebuilds();       printf("\n");
    test_lookup_not_found();         printf("\n");
    test_empty_cache();              printf("\n");
    test_large_dataset();            printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
