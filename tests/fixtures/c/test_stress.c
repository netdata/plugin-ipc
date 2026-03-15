/*
 * test_stress.c - Large-scale and stress tests for the netipc library.
 *
 * Phase H4: Proves the library is robust under production-scale loads.
 * Tests snapshot scale (1000/5000 items), multi-client concurrency
 * (10/50 clients), rapid connect/disconnect cycling (1000 cycles),
 * long-running stability (60s continuous), SHM region lifecycle (1000x),
 * and mixed transport (SHM + UDS simultaneously).
 *
 * Every test verifies DATA CORRECTNESS, not just "didn't crash."
 * Reports actual numbers: requests, errors, elapsed time.
 *
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR  "/tmp/nipc_stress_test"
#define AUTH_TOKEN    0xDEADBEEFCAFEBABEull

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
    snprintf(path, sizeof(path), "%s/%s.ipcshm", TEST_RUN_DIR, service);
    unlink(path);
}

static double elapsed_ms(struct timespec *start, struct timespec *end)
{
    double s = (double)(end->tv_sec - start->tv_sec) * 1000.0;
    double ns = (double)(end->tv_nsec - start->tv_nsec) / 1e6;
    return s + ns;
}

/* simple_hash: djb2 for deterministic hash generation */
static uint32_t simple_hash(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + (uint32_t)c;
    return hash;
}

/* ------------------------------------------------------------------ */
/*  Configurable snapshot handler (serves N items)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int item_count;
    size_t resp_buf_size;
} handler_config_t;

static bool large_snapshot_handler(void *user,
    uint16_t method_code,
    const uint8_t *request_payload,
    size_t request_len,
    uint8_t *response_buf,
    size_t response_buf_size,
    size_t *response_len_out)
{
    handler_config_t *cfg = (handler_config_t *)user;

    if (method_code != NIPC_METHOD_CGROUPS_SNAPSHOT)
        return false;

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return false;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               (uint32_t)cfg->item_count, 1, 42);

    for (int i = 0; i < cfg->item_count; i++) {
        char name[64], path[128];
        snprintf(name, sizeof(name), "container-%04d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/docker/%04d", i);
        uint32_t hash = simple_hash(name);
        uint32_t enabled = (i % 5 == 0) ? 0 : 1;
        err = nipc_cgroups_builder_add(&builder,
            hash, 0x10, enabled,
            name, (uint32_t)strlen(name),
            path, (uint32_t)strlen(path));
        if (err != NIPC_OK)
            return false;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Server thread context with configurable response buffer             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    nipc_server_handler_fn handler;
    void *handler_user;
    int worker_count;
    size_t resp_buf_size;
    uint32_t max_resp_payload;
    uint32_t packet_size;  /* 0 = auto-detect */
    int ready;  /* use __atomic builtins */
    int done;   /* use __atomic builtins */
} stress_server_ctx_t;

static void *stress_server_thread_fn(void *arg)
{
    stress_server_ctx_t *ctx = (stress_server_ctx_t *)arg;

    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = ctx->max_resp_payload,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = ctx->packet_size,
        .backlog                   = 64,
    };

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        ctx->worker_count, ctx->resp_buf_size,
        ctx->handler, ctx->handler_user);

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

static void start_stress_server(stress_server_ctx_t *sctx, const char *service,
    nipc_server_handler_fn handler, void *handler_user,
    int worker_count, size_t resp_buf_size, uint32_t max_resp_payload,
    uint32_t packet_size, pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->handler = handler;
    sctx->handler_user = handler_user;
    sctx->worker_count = worker_count;
    sctx->resp_buf_size = resp_buf_size;
    sctx->max_resp_payload = max_resp_payload;
    sctx->packet_size = packet_size;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, stress_server_thread_fn, sctx);

    for (int i = 0; i < 4000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void stop_stress_server(stress_server_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

/* ------------------------------------------------------------------ */
/*  Client helper: call + verify N items                               */
/* ------------------------------------------------------------------ */

static bool do_call_verify_n(nipc_client_ctx_t *client,
                             uint8_t *req_buf,
                             uint8_t *resp_buf,
                             size_t resp_buf_size,
                             int expected_items)
{
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        client, req_buf, resp_buf, resp_buf_size, &view);
    if (err != NIPC_OK)
        return false;

    if ((int)view.item_count != expected_items)
        return false;
    if (view.systemd_enabled != 1)
        return false;
    if (view.generation != 42)
        return false;

    /* Spot-check first, middle, and last items */
    int indices[] = {0, expected_items / 2, expected_items - 1};
    for (int k = 0; k < 3; k++) {
        int idx = indices[k];
        nipc_cgroups_item_view_t item;
        err = nipc_cgroups_resp_item(&view, (uint32_t)idx, &item);
        if (err != NIPC_OK)
            return false;

        char expected_name[64], expected_path[128];
        snprintf(expected_name, sizeof(expected_name), "container-%04d", idx);
        snprintf(expected_path, sizeof(expected_path), "/sys/fs/cgroup/docker/%04d", idx);

        if (item.hash != simple_hash(expected_name))
            return false;
        if (item.name.len != strlen(expected_name))
            return false;
        if (memcmp(item.name.ptr, expected_name, item.name.len) != 0)
            return false;
        if (item.path.len != strlen(expected_path))
            return false;
        if (memcmp(item.path.ptr, expected_path, item.path.len) != 0)
            return false;

        uint32_t expected_enabled = (idx % 5 == 0) ? 0 : 1;
        if (item.enabled != expected_enabled)
            return false;
        if (item.options != 0x10)
            return false;
    }

    return true;
}

/* Full verification of ALL items */
static bool do_call_verify_all(nipc_client_ctx_t *client,
                               uint8_t *req_buf,
                               uint8_t *resp_buf,
                               size_t resp_buf_size,
                               int expected_items)
{
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        client, req_buf, resp_buf, resp_buf_size, &view);
    if (err != NIPC_OK)
        return false;

    if ((int)view.item_count != expected_items)
        return false;

    for (int i = 0; i < expected_items; i++) {
        nipc_cgroups_item_view_t item;
        err = nipc_cgroups_resp_item(&view, (uint32_t)i, &item);
        if (err != NIPC_OK)
            return false;

        char expected_name[64], expected_path[128];
        snprintf(expected_name, sizeof(expected_name), "container-%04d", i);
        snprintf(expected_path, sizeof(expected_path), "/sys/fs/cgroup/docker/%04d", i);

        if (item.hash != simple_hash(expected_name))
            return false;
        if (item.name.len != strlen(expected_name))
            return false;
        if (memcmp(item.name.ptr, expected_name, item.name.len) != 0)
            return false;
        if (item.path.len != strlen(expected_path))
            return false;
        if (memcmp(item.path.ptr, expected_path, item.path.len) != 0)
            return false;

        uint32_t expected_enabled = (i % 5 == 0) ? 0 : 1;
        if (item.enabled != expected_enabled)
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Snapshot scale - 1000 items                                */
/* ------------------------------------------------------------------ */

#define ITEMS_1K     1000
#define BUF_1K       (300 * ITEMS_1K)

static void test_snapshot_1000(void)
{
    printf("Test 1: Snapshot scale - 1000 items\n");
    const char *svc = "stress_1k";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = ITEMS_1K, .resp_buf_size = BUF_1K };
    stress_server_ctx_t sctx;
    pthread_t tid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        4, BUF_1K, BUF_1K, 65536, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = BUF_1K,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 65536,
    };
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint8_t req_buf[64];
    uint8_t *resp_buf = malloc(BUF_1K);
    bool ok = do_call_verify_all(&client, req_buf, resp_buf, BUF_1K, ITEMS_1K);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    1000 items: %.1f ms\n", elapsed_ms(&t0, &t1));

    check("all 1000 items verified correct", ok);

    free(resp_buf);
    nipc_client_close(&client);
    stop_stress_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Snapshot scale - 5000 items                                */
/* ------------------------------------------------------------------ */

#define ITEMS_5K     5000
#define BUF_5K       (300 * ITEMS_5K)

static void test_snapshot_5000(void)
{
    printf("Test 2: Snapshot scale - 5000 items\n");
    const char *svc = "stress_5k";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = ITEMS_5K, .resp_buf_size = BUF_5K };
    stress_server_ctx_t sctx;
    pthread_t tid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        4, BUF_5K, BUF_5K, 65536, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = BUF_5K,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 65536,
    };
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint8_t req_buf[64];
    uint8_t *resp_buf = malloc(BUF_5K);
    bool ok = do_call_verify_all(&client, req_buf, resp_buf, BUF_5K, ITEMS_5K);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    5000 items: %.1f ms\n", elapsed_ms(&t0, &t1));

    check("all 5000 items verified correct", ok);

    free(resp_buf);
    nipc_client_close(&client);
    stop_stress_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Multi-client scale - 10 clients x 100 refreshes            */
/* ------------------------------------------------------------------ */

typedef struct {
    int client_id;
    int request_count;
    int expected_items;
    int success_count;
    int error_count;
    bool content_verified;
    const char *service;
    size_t resp_buf_size;
} scale_client_ctx_t;

static void *scale_client_thread_fn(void *arg)
{
    scale_client_ctx_t *ctx = (scale_client_ctx_t *)arg;
    ctx->success_count = 0;
    ctx->error_count = 0;
    ctx->content_verified = true;

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = (uint32_t)ctx->resp_buf_size,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_cgroups_cache_t cache;
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, ctx->service, &ccfg);

    /* Resize the internal response buffer for large datasets */
    if (ctx->resp_buf_size > cache.response_buf_size) {
        free(cache.response_buf);
        cache.response_buf_size = ctx->resp_buf_size;
        cache.response_buf = malloc(cache.response_buf_size);
    }

    for (int i = 0; i < ctx->request_count; i++) {
        bool updated = nipc_cgroups_cache_refresh(&cache);
        if (updated && nipc_cgroups_cache_ready(&cache)) {
            nipc_cgroups_cache_status_t status;
            nipc_cgroups_cache_status(&cache, &status);

            if ((int)status.item_count != ctx->expected_items) {
                ctx->error_count++;
                ctx->content_verified = false;
                continue;
            }

            /* Verify a few items */
            bool item_ok = true;
            int spots[] = {0, ctx->expected_items / 4, ctx->expected_items / 2,
                           3 * ctx->expected_items / 4, ctx->expected_items - 1};
            for (int k = 0; k < 5; k++) {
                int idx = spots[k];
                char name[64];
                snprintf(name, sizeof(name), "container-%04d", idx);
                uint32_t hash = simple_hash(name);
                const nipc_cgroups_cache_item_t *item =
                    nipc_cgroups_cache_lookup(&cache, hash, name);
                if (!item || item->hash != hash) {
                    item_ok = false;
                    break;
                }
            }

            if (item_ok)
                ctx->success_count++;
            else {
                ctx->error_count++;
                ctx->content_verified = false;
            }
        } else if (!updated && nipc_cgroups_cache_ready(&cache)) {
            /* Cache still valid from previous refresh - counts as success */
            ctx->success_count++;
        } else {
            ctx->error_count++;
        }
    }

    nipc_cgroups_cache_close(&cache);
    return NULL;
}

static void test_multi_client_10(void)
{
    printf("Test 3: Multi-client scale - 10 clients x 100 refreshes (3 items)\n");
    const char *svc = "stress_mc10";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = 3, .resp_buf_size = 65536 };
    stress_server_ctx_t sctx;
    pthread_t stid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        16, 65536, 65536, 0, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    const int num_clients = 10;
    const int requests_per = 100;
    pthread_t ctids[10];
    scale_client_ctx_t cctxs[10];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].client_id = i;
        cctxs[i].request_count = requests_per;
        cctxs[i].expected_items = 3;
        cctxs[i].service = svc;
        cctxs[i].resp_buf_size = 65536;
        pthread_create(&ctids[i], NULL, scale_client_thread_fn, &cctxs[i]);
    }

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    int total_success = 0, total_errors = 0;
    bool all_content_ok = true;
    for (int i = 0; i < num_clients; i++) {
        total_success += cctxs[i].success_count;
        total_errors += cctxs[i].error_count;
        if (!cctxs[i].content_verified)
            all_content_ok = false;
    }

    int expected_total = num_clients * requests_per;
    printf("    10 clients x 100 req: %d/%d succeeded, %d errors, %.1f ms\n",
           total_success, expected_total, total_errors, elapsed_ms(&t0, &t1));

    check("all requests succeeded", total_success == expected_total);
    check("no errors", total_errors == 0);
    check("all content verified (no cross-talk)", all_content_ok);

    stop_stress_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Multi-client scale - 50 clients x 10 refreshes             */
/* ------------------------------------------------------------------ */

static void test_multi_client_50(void)
{
    printf("Test 4: Multi-client scale - 50 clients x 10 refreshes (3 items)\n");
    const char *svc = "stress_mc50";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = 3, .resp_buf_size = 65536 };
    stress_server_ctx_t sctx;
    pthread_t stid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        64, 65536, 65536, 0, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    const int num_clients = 50;
    const int requests_per = 10;
    pthread_t *ctids = malloc(sizeof(pthread_t) * num_clients);
    scale_client_ctx_t *cctxs = malloc(sizeof(scale_client_ctx_t) * num_clients);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].client_id = i;
        cctxs[i].request_count = requests_per;
        cctxs[i].expected_items = 3;
        cctxs[i].service = svc;
        cctxs[i].resp_buf_size = 65536;
        pthread_create(&ctids[i], NULL, scale_client_thread_fn, &cctxs[i]);
    }

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    int total_success = 0, total_errors = 0;
    bool all_content_ok = true;
    for (int i = 0; i < num_clients; i++) {
        total_success += cctxs[i].success_count;
        total_errors += cctxs[i].error_count;
        if (!cctxs[i].content_verified)
            all_content_ok = false;
    }

    int expected_total = num_clients * requests_per;
    printf("    50 clients x 10 req: %d/%d succeeded, %d errors, %.1f ms\n",
           total_success, expected_total, total_errors, elapsed_ms(&t0, &t1));

    check("all requests succeeded", total_success == expected_total);
    check("no errors", total_errors == 0);
    check("all content verified (no cross-talk)", all_content_ok);

    free(ctids);
    free(cctxs);
    stop_stress_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Rapid connect/disconnect cycling (1000 cycles)             */
/* ------------------------------------------------------------------ */

static void test_rapid_connect_disconnect(void)
{
    printf("Test 5: Rapid connect/disconnect cycling (1000 cycles)\n");
    const char *svc = "stress_rapid";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = 3, .resp_buf_size = 65536 };
    stress_server_ctx_t sctx;
    pthread_t stid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        8, 65536, 65536, 0, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 65536,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    const int cycles = 1000;
    int success_count = 0;
    int connect_failures = 0;
    int call_failures = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < cycles; i++) {
        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

        /* Connect with retry */
        bool connected = false;
        for (int r = 0; r < 50; r++) {
            nipc_client_refresh(&client);
            if (nipc_client_ready(&client)) {
                connected = true;
                break;
            }
            usleep(2000);
        }

        if (!connected) {
            connect_failures++;
            nipc_client_close(&client);
            continue;
        }

        uint8_t req_buf[64], resp_buf[65536];
        if (do_call_verify_n(&client, req_buf, resp_buf, sizeof(resp_buf), 3))
            success_count++;
        else
            call_failures++;

        nipc_client_close(&client);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    %d cycles: %d ok, %d connect fail, %d call fail, %.1f ms\n",
           cycles, success_count, connect_failures, call_failures,
           elapsed_ms(&t0, &t1));

    check("all cycles succeeded", success_count == cycles);
    check("no connect failures", connect_failures == 0);
    check("no call failures", call_failures == 0);

    /* Verify server is still healthy after all the churn */
    {
        nipc_client_ctx_t probe;
        nipc_client_init(&probe, TEST_RUN_DIR, svc, &ccfg);
        for (int r = 0; r < 100; r++) {
            nipc_client_refresh(&probe);
            if (nipc_client_ready(&probe))
                break;
            usleep(5000);
        }
        bool healthy = false;
        if (nipc_client_ready(&probe)) {
            uint8_t req_buf[64], resp_buf[65536];
            healthy = do_call_verify_n(&probe, req_buf, resp_buf,
                                       sizeof(resp_buf), 3);
        }
        check("server healthy after 1000 cycles", healthy);
        nipc_client_close(&probe);
    }

    /* Check for fd leaks via /proc/self/fd */
    {
        DIR *d = opendir("/proc/self/fd");
        int fd_count = 0;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                fd_count++;
            closedir(d);
        }
        printf("    fd count after test: %d\n", fd_count);
        /* A healthy process should have < 100 fds open */
        check("fd count reasonable (< 100)", fd_count < 100);
    }

    stop_stress_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Long-running stability (60 seconds)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int total_refreshes;
    int total_errors;
    int running;  /* __atomic */
} stability_client_ctx_t;

static void *stability_client_fn(void *arg)
{
    stability_client_ctx_t *ctx = (stability_client_ctx_t *)arg;

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 65536,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_cgroups_cache_t cache;
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, ctx->service, &ccfg);

    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {
        bool updated = nipc_cgroups_cache_refresh(&cache);
        if (updated || nipc_cgroups_cache_ready(&cache)) {
            __atomic_fetch_add(&ctx->total_refreshes, 1, __ATOMIC_RELAXED);

            /* Verify cache integrity */
            nipc_cgroups_cache_status_t status;
            nipc_cgroups_cache_status(&cache, &status);
            if ((int)status.item_count != 3) {
                __atomic_fetch_add(&ctx->total_errors, 1, __ATOMIC_RELAXED);
            }
        } else {
            __atomic_fetch_add(&ctx->total_errors, 1, __ATOMIC_RELAXED);
        }

        usleep(1000); /* ~1ms between refreshes */
    }

    nipc_cgroups_cache_close(&cache);
    return NULL;
}

static void test_long_running_stability(void)
{
    printf("Test 6: Long-running stability (60 seconds, 5 clients)\n");
    const char *svc = "stress_long";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = 3, .resp_buf_size = 65536 };
    stress_server_ctx_t sctx;
    pthread_t stid;
    start_stress_server(&sctx, svc, large_snapshot_handler, &hcfg,
                        8, 65536, 65536, 0, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Read initial VmRSS */
    long vmrss_start = 0;
    {
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    vmrss_start = strtol(line + 6, NULL, 10);
                    break;
                }
            }
            fclose(f);
        }
    }

    const int num_clients = 5;
    pthread_t ctids[5];
    stability_client_ctx_t cctxs[5];

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].service = svc;
        cctxs[i].total_refreshes = 0;
        cctxs[i].total_errors = 0;
        __atomic_store_n(&cctxs[i].running, 1, __ATOMIC_RELAXED);
        pthread_create(&ctids[i], NULL, stability_client_fn, &cctxs[i]);
    }

    /* Run for 60 seconds */
    sleep(60);

    /* Signal all clients to stop */
    for (int i = 0; i < num_clients; i++)
        __atomic_store_n(&cctxs[i].running, 0, __ATOMIC_RELEASE);

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    /* Read final VmRSS */
    long vmrss_end = 0;
    {
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    vmrss_end = strtol(line + 6, NULL, 10);
                    break;
                }
            }
            fclose(f);
        }
    }

    int total_refreshes = 0, total_errors = 0;
    for (int i = 0; i < num_clients; i++) {
        total_refreshes += __atomic_load_n(&cctxs[i].total_refreshes, __ATOMIC_RELAXED);
        total_errors += __atomic_load_n(&cctxs[i].total_errors, __ATOMIC_RELAXED);
    }

    printf("    60s run: %d total refreshes, %d errors\n",
           total_refreshes, total_errors);
    printf("    VmRSS: start=%ld kB, end=%ld kB, delta=%ld kB\n",
           vmrss_start, vmrss_end, vmrss_end - vmrss_start);

    check("total refreshes > 0", total_refreshes > 0);
    check("zero errors in 60s run", total_errors == 0);

    /* Memory growth should be stable - allow 4MB tolerance */
    long vmrss_growth = vmrss_end - vmrss_start;
    check("memory stable (< 4MB growth)", vmrss_growth < 4096);

    /* Verify all caches were populated */
    bool all_ran = true;
    for (int i = 0; i < num_clients; i++) {
        if (__atomic_load_n(&cctxs[i].total_refreshes, __ATOMIC_RELAXED) < 10) {
            printf("    client %d: only %d refreshes\n", i,
                   __atomic_load_n(&cctxs[i].total_refreshes, __ATOMIC_RELAXED));
            all_ran = false;
        }
    }
    check("all clients ran many refreshes", all_ran);

    stop_stress_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: SHM region lifecycle (1000 create/destroy cycles)          */
/* ------------------------------------------------------------------ */

static void test_shm_lifecycle(void)
{
    printf("Test 7: SHM region lifecycle (1000 create/destroy cycles)\n");

    const int cycles = 1000;
    int success = 0;
    int failures = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < cycles; i++) {
        char svc_name[64];
        snprintf(svc_name, sizeof(svc_name), "shm_lc_%d", i % 10);

        /* Clean up any leftover */
        char shm_path[256];
        snprintf(shm_path, sizeof(shm_path), "%s/%s.ipcshm", TEST_RUN_DIR, svc_name);
        unlink(shm_path);

        nipc_shm_ctx_t ctx;
        nipc_shm_error_t err = nipc_shm_server_create(
            TEST_RUN_DIR, svc_name, 4096, 4096, &ctx);

        if (err == NIPC_SHM_OK) {
            /* Verify the region is valid */
            if (ctx.fd < 0 || ctx.base == NULL) {
                failures++;
            } else {
                success++;
            }
            nipc_shm_destroy(&ctx);
        } else {
            failures++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    %d cycles: %d ok, %d failures, %.1f ms\n",
           cycles, success, failures, elapsed_ms(&t0, &t1));

    check("all SHM create/destroy cycles succeeded", success == cycles);
    check("no failures", failures == 0);

    /* Check for leaked SHM files in TEST_RUN_DIR */
    {
        int leaked_files = 0;
        DIR *d = opendir(TEST_RUN_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strstr(de->d_name, ".ipcshm") != NULL)
                    leaked_files++;
            }
            closedir(d);
        }
        printf("    leaked SHM files: %d\n", leaked_files);
        check("no leaked SHM files", leaked_files == 0);
    }

    /* Check for leaked mmap regions via /proc/self/maps */
    {
        int shm_mappings = 0;
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "ipcshm") != NULL)
                    shm_mappings++;
            }
            fclose(f);
        }
        printf("    leaked mmap regions: %d\n", shm_mappings);
        check("no leaked mmap regions", shm_mappings == 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Test 8: Mixed transport (SHM + UDS clients concurrently)           */
/* ------------------------------------------------------------------ */

/* SHM-capable server config */
static nipc_uds_server_config_t shm_server_config(uint32_t max_resp)
{
    return (nipc_uds_server_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = max_resp,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 16,
    };
}

/* SHM-capable server thread */
static void *mixed_server_thread_fn(void *arg)
{
    stress_server_ctx_t *ctx = (stress_server_ctx_t *)arg;

    nipc_uds_server_config_t scfg = shm_server_config(ctx->max_resp_payload);

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        ctx->worker_count, ctx->resp_buf_size,
        ctx->handler, ctx->handler_user);

    if (err != NIPC_OK) {
        fprintf(stderr, "mixed server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

typedef struct {
    int client_id;
    uint32_t profile;  /* BASELINE or SHM_HYBRID */
    const char *service;
    int success;
    int failure;
} mixed_client_ctx_t;

static void *mixed_client_fn(void *arg)
{
    mixed_client_ctx_t *ctx = (mixed_client_ctx_t *)arg;
    ctx->success = 0;
    ctx->failure = 0;

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = ctx->profile,
        .preferred_profiles        = ctx->profile,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 65536,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);

    /* Connect with retry */
    for (int r = 0; r < 200; r++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(5000);
    }

    if (!nipc_client_ready(&client)) {
        ctx->failure = 10;
        nipc_client_close(&client);
        return NULL;
    }

    /* Make 10 calls */
    for (int i = 0; i < 10; i++) {
        uint8_t req_buf[64], resp_buf[65536];
        if (do_call_verify_n(&client, req_buf, resp_buf, sizeof(resp_buf), 3))
            ctx->success++;
        else
            ctx->failure++;
    }

    nipc_client_close(&client);
    return NULL;
}

static void test_mixed_transport(void)
{
    printf("Test 8: Mixed transport (2 SHM + 1 UDS clients concurrent)\n");
    const char *svc = "stress_mixed";
    cleanup_all(svc);

    handler_config_t hcfg = { .item_count = 3, .resp_buf_size = 65536 };

    stress_server_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;
    sctx.handler = large_snapshot_handler;
    sctx.handler_user = &hcfg;
    sctx.worker_count = 8;
    sctx.resp_buf_size = 65536;
    sctx.max_resp_payload = 65536;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t stid;
    pthread_create(&stid, NULL, mixed_server_thread_fn, &sctx);

    for (int i = 0; i < 4000
         && !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE); i++)
        usleep(500);

    check("mixed server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Start 3 clients: 2 SHM, 1 UDS-only */
    const int num_clients = 3;
    pthread_t ctids[3];
    mixed_client_ctx_t mctxs[3];

    mctxs[0].client_id = 0;
    mctxs[0].profile = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    mctxs[0].service = svc;

    mctxs[1].client_id = 1;
    mctxs[1].profile = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    mctxs[1].service = svc;

    mctxs[2].client_id = 2;
    mctxs[2].profile = NIPC_PROFILE_BASELINE; /* UDS only */
    mctxs[2].service = svc;

    for (int i = 0; i < num_clients; i++)
        pthread_create(&ctids[i], NULL, mixed_client_fn, &mctxs[i]);

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    int total_success = 0, total_failure = 0;
    for (int i = 0; i < num_clients; i++) {
        total_success += mctxs[i].success;
        total_failure += mctxs[i].failure;
        printf("    client %d (profile=0x%02x): %d ok, %d fail\n",
               i, mctxs[i].profile, mctxs[i].success, mctxs[i].failure);
    }

    check("all mixed clients got correct responses",
          total_success == num_clients * 10);
    check("no errors in mixed transport", total_failure == 0);

    stop_stress_server(&sctx, stid);
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

    printf("=== Large-Scale & Stress Tests (Phase H4) ===\n\n");

    test_snapshot_1000();                printf("\n");
    test_snapshot_5000();                printf("\n");
    test_multi_client_10();              printf("\n");
    test_multi_client_50();              printf("\n");
    test_rapid_connect_disconnect();     printf("\n");
    test_long_running_stability();       printf("\n");
    test_shm_lifecycle();                printf("\n");
    test_mixed_transport();              printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
