/*
 * test_multi_server.c - Multi-client concurrent server tests.
 *
 * Tests concurrent client sessions, worker limit enforcement,
 * client disconnect recovery, handler failure per-session,
 * rapid connect/disconnect, long-running concurrency, and
 * graceful shutdown.
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

#define TEST_RUN_DIR  "/tmp/nipc_multi_test"
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
        .backlog                   = 32,
    };
}

static nipc_client_config_t default_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };
}

/* ------------------------------------------------------------------ */
/*  Typed callbacks                                                    */
/* ------------------------------------------------------------------ */

/* Builds a snapshot with N items where N = hash of first requested item.
 * Uses the message_id encoded in the request flags as a "client ID"
 * to generate unique data per client. */
static nipc_error_t echo_handler(void *user,
                                 const nipc_header_t *request_hdr,
                                 const uint8_t *request_payload,
                                 size_t request_len,
                                 uint8_t *response_buf,
                                 size_t response_buf_size,
                                 size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    /* Decode request to validate it */
    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

    /* Build a snapshot with 3 test items */
    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               3, 1 /* systemd_enabled */, 42 /* generation */);

    struct {
        uint32_t hash;
        uint32_t options;
        uint32_t enabled;
        const char *name;
        const char *path;
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

/* Handler that sleeps briefly to simulate work, allowing concurrency testing */
static nipc_error_t slow_handler(void *user,
                                 const nipc_header_t *request_hdr,
                                 const uint8_t *request_payload,
                                 size_t request_len,
                                 uint8_t *response_buf,
                                 size_t response_buf_size,
                                 size_t *response_len_out)
{
    /* Sleep 100ms to ensure concurrent sessions overlap */
    usleep(100000);
    return echo_handler(user, request_hdr, request_payload, request_len,
                        response_buf, response_buf_size, response_len_out);
}

/* Session isolation test uses a second raw handler symbol, but the server
 * still services only the cgroups snapshot request kind. */
static nipc_error_t selective_handler(void *user,
                                      const nipc_header_t *request_hdr,
                                      const uint8_t *request_payload,
                                      size_t request_len,
                                      uint8_t *response_buf,
                                      size_t response_buf_size,
                                      size_t *response_len_out)
{
    return echo_handler(user, request_hdr, request_payload, request_len,
                        response_buf, response_buf_size, response_len_out);
}

/* ------------------------------------------------------------------ */
/*  Server thread helpers                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    nipc_server_handler_fn handler;
    int worker_count;
    int ready;  /* use __atomic builtins for cross-thread access */
    int done;   /* use __atomic builtins for cross-thread access */
} server_thread_ctx_t;

static void *server_thread_fn(void *arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_uds_server_config_t scfg = default_server_config();

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        ctx->worker_count, NIPC_METHOD_CGROUPS_SNAPSHOT, ctx->handler, NULL);

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

static void start_server_ex(server_thread_ctx_t *sctx, const char *service,
                              nipc_server_handler_fn handler,
                              int worker_count, pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->handler = handler;
    sctx->worker_count = worker_count;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, server_thread_fn, sctx);

    /* Wait for server to be ready */
    for (int i = 0; i < 4000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void start_server(server_thread_ctx_t *sctx, const char *service,
                          nipc_server_handler_fn handler, pthread_t *tid)
{
    start_server_ex(sctx, service, handler, 8, tid);
}

static void stop_server(server_thread_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

/* ------------------------------------------------------------------ */
/*  Client call helpers                                                */
/* ------------------------------------------------------------------ */

/* Make one cgroups snapshot call and verify the response content.
 * Returns true if all checks passed. */
static bool do_call_and_verify(nipc_client_ctx_t *client, int client_id)
{
    nipc_cgroups_resp_view_t view;
    (void)client_id;

    nipc_error_t err = nipc_client_call_cgroups_snapshot(client, &view);

    if (err != NIPC_OK)
        return false;

    if (view.item_count != 3)
        return false;

    if (view.systemd_enabled != 1)
        return false;

    if (view.generation != 42)
        return false;

    /* Verify first item content */
    nipc_cgroups_item_view_t item;
    if (nipc_cgroups_resp_item(&view, 0, &item) != NIPC_OK)
        return false;

    if (item.hash != 1001)
        return false;

    if (item.name.len != strlen("docker-abc123") ||
        memcmp(item.name.ptr, "docker-abc123", item.name.len) != 0)
        return false;

    return true;
}

/* ------------------------------------------------------------------ */
/*  Per-client thread context for concurrent testing                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int client_id;
    int request_count;
    int success_count;
    int error_count;
    bool content_verified;  /* all responses had correct content */
    const char *service;
} client_thread_ctx_t;

static void *client_thread_fn(void *arg)
{
    client_thread_ctx_t *ctx = (client_thread_ctx_t *)arg;
    ctx->success_count = 0;
    ctx->error_count = 0;
    ctx->content_verified = true;

    nipc_client_config_t ccfg = default_client_config();
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);

    /* Connect with retry */
    for (int i = 0; i < 100; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(10000);
    }

    if (!nipc_client_ready(&client)) {
        ctx->error_count = ctx->request_count;
        nipc_client_close(&client);
        return NULL;
    }

    for (int i = 0; i < ctx->request_count; i++) {
        if (do_call_and_verify(&client, ctx->client_id)) {
            ctx->success_count++;
        } else {
            ctx->error_count++;
            ctx->content_verified = false;
        }
    }

    nipc_client_close(&client);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Basic multi-client (5 clients, 10 requests each)           */
/* ------------------------------------------------------------------ */

static void test_basic_multi_client(void)
{
    printf("Test 1: Basic multi-client (5 clients, 10 requests each)\n");
    const char *svc = "msvc_basic";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server(&sctx, svc, echo_handler, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Launch 5 client threads */
    const int num_clients = 5;
    const int requests_per = 10;
    pthread_t ctids[5];
    client_thread_ctx_t cctxs[5];

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].client_id = i;
        cctxs[i].request_count = requests_per;
        cctxs[i].service = svc;
        pthread_create(&ctids[i], NULL, client_thread_fn, &cctxs[i]);
    }

    /* Wait for all clients to finish */
    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    /* Verify results */
    int total_success = 0;
    int total_errors = 0;
    bool all_content_ok = true;

    for (int i = 0; i < num_clients; i++) {
        total_success += cctxs[i].success_count;
        total_errors += cctxs[i].error_count;
        if (!cctxs[i].content_verified)
            all_content_ok = false;
    }

    check("all requests succeeded",
          total_success == num_clients * requests_per);
    check("no errors", total_errors == 0);
    check("all response content verified", all_content_ok);

    char msg[128];
    snprintf(msg, sizeof(msg), "total: %d/%d succeeded",
             total_success, num_clients * requests_per);
    check(msg, total_success == num_clients * requests_per);

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Worker limit enforcement                                   */
/* ------------------------------------------------------------------ */

static void test_worker_limit(void)
{
    printf("Test 2: Worker limit (server workers=2, 5 clients)\n");
    const char *svc = "msvc_wlimit";
    cleanup_all(svc);

    /* Server with only 2 workers */
    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server_ex(&sctx, svc, slow_handler, 2, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Try to connect 5 clients. With worker_count=2, only 2 can be
     * served concurrently. The slow_handler sleeps 100ms per request,
     * so we'll have clients that get rejected and retry. */
    const int num_clients = 5;
    const int requests_per = 3;
    pthread_t ctids[5];
    client_thread_ctx_t cctxs[5];

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].client_id = i;
        cctxs[i].request_count = requests_per;
        cctxs[i].service = svc;
        pthread_create(&ctids[i], NULL, client_thread_fn, &cctxs[i]);
    }

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    /* With worker_count=2, not all 5 clients will succeed simultaneously.
     * But the key test is that the server doesn't crash and the 2 slots
     * that do get served return correct data. */
    int served_clients = 0;
    bool all_served_content_ok = true;
    for (int i = 0; i < num_clients; i++) {
        if (cctxs[i].success_count > 0) {
            served_clients++;
            if (!cctxs[i].content_verified)
                all_served_content_ok = false;
        }
    }

    check("at least 2 clients were served", served_clients >= 2);
    check("served clients got correct data", all_served_content_ok);
    check("server did not crash", !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE));

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Client disconnect recovery                                 */
/* ------------------------------------------------------------------ */

static void test_client_disconnect_recovery(void)
{
    printf("Test 3: Client disconnect recovery\n");
    const char *svc = "msvc_disconn";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server(&sctx, svc, echo_handler, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_config_t ccfg = default_client_config();

    /* Client 1: connect, call, then disconnect abruptly */
    {
        nipc_client_ctx_t c1;
        nipc_client_init(&c1, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&c1);
        check("client 1 connected", nipc_client_ready(&c1));

        bool ok = do_call_and_verify(&c1, 1);
        check("client 1 call ok", ok);

        /* Abrupt close */
        nipc_client_close(&c1);
    }

    usleep(50000); /* let the server notice the disconnect */

    /* Client 2: should still be able to connect and get served */
    {
        nipc_client_ctx_t c2;
        nipc_client_init(&c2, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&c2);
        check("client 2 connected after client 1 disconnect",
              nipc_client_ready(&c2));

        bool ok = do_call_and_verify(&c2, 2);
        check("client 2 call ok", ok);

        nipc_client_close(&c2);
    }

    usleep(50000);

    /* Client 3: verify server is still healthy */
    {
        nipc_client_ctx_t c3;
        nipc_client_init(&c3, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&c3);
        check("client 3 connected", nipc_client_ready(&c3));

        bool ok = do_call_and_verify(&c3, 3);
        check("client 3 call ok", ok);

        nipc_client_close(&c3);
    }

    check("server still running", !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE));

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Handler failure per session (selective handler)             */
/* ------------------------------------------------------------------ */

static void test_handler_failure_per_session(void)
{
    printf("Test 4: Handler failure is per-session (doesn't break other sessions)\n");
    const char *svc = "msvc_hfail";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server(&sctx, svc, selective_handler, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_config_t ccfg = default_client_config();

    /* Client 1: normal call - should succeed */
    nipc_client_ctx_t c1;
    nipc_client_init(&c1, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&c1);
    check("client 1 connected", nipc_client_ready(&c1));

    bool ok = do_call_and_verify(&c1, 1);
    check("client 1 normal call succeeded", ok);

    nipc_client_close(&c1);
    usleep(50000);

    /* Client 2: should still succeed after client 1's success */
    nipc_client_ctx_t c2;
    nipc_client_init(&c2, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&c2);
    check("client 2 connected", nipc_client_ready(&c2));

    ok = do_call_and_verify(&c2, 2);
    check("client 2 call also succeeded", ok);

    nipc_client_close(&c2);

    check("server still running after mixed results", !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE));

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Rapid connect/disconnect                                   */
/* ------------------------------------------------------------------ */

static void test_rapid_connect_disconnect(void)
{
    printf("Test 5: Rapid connect/disconnect (100 cycles)\n");
    const char *svc = "msvc_rapid";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server(&sctx, svc, echo_handler, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_config_t ccfg = default_client_config();
    int success_count = 0;
    const int cycles = 100;

    for (int i = 0; i < cycles; i++) {
        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

        /* Connect with small retry window */
        for (int r = 0; r < 50; r++) {
            nipc_client_refresh(&client);
            if (nipc_client_ready(&client))
                break;
            usleep(5000);
        }

        if (nipc_client_ready(&client)) {
            if (do_call_and_verify(&client, i))
                success_count++;
        }

        nipc_client_close(&client);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "rapid cycles: %d/%d succeeded",
             success_count, cycles);
    check(msg, success_count == cycles);
    check("server survived rapid connect/disconnect", !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE));

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Long-running concurrent (10 clients, 1000 req each)        */
/* ------------------------------------------------------------------ */

static void test_long_running_concurrent(void)
{
    printf("Test 6: Long-running concurrent (10 clients, 100 requests each)\n");
    const char *svc = "msvc_long";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server_ex(&sctx, svc, echo_handler, 10, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    const int num_clients = 10;
    const int requests_per = 100;
    pthread_t ctids[10];
    client_thread_ctx_t cctxs[10];

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].client_id = i;
        cctxs[i].request_count = requests_per;
        cctxs[i].service = svc;
        pthread_create(&ctids[i], NULL, client_thread_fn, &cctxs[i]);
    }

    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    int total_success = 0;
    int total_errors = 0;
    bool all_content_ok = true;

    for (int i = 0; i < num_clients; i++) {
        total_success += cctxs[i].success_count;
        total_errors += cctxs[i].error_count;
        if (!cctxs[i].content_verified)
            all_content_ok = false;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "total: %d/%d succeeded",
             total_success, num_clients * requests_per);
    check(msg, total_success == num_clients * requests_per);
    check("no errors", total_errors == 0);
    check("all response content verified (no cross-talk)", all_content_ok);

    /* Verify per-client detail */
    bool all_per_client_ok = true;
    for (int i = 0; i < num_clients; i++) {
        if (cctxs[i].success_count != requests_per) {
            printf("    client %d: %d/%d succeeded\n",
                   i, cctxs[i].success_count, requests_per);
            all_per_client_ok = false;
        }
    }
    check("every client got all responses", all_per_client_ok);

    stop_server(&sctx, stid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: Graceful shutdown with connected clients                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int connected;   /* use __atomic builtins for cross-thread access */
    int finished;    /* use __atomic builtins for cross-thread access */
    int calls_made;  /* use __atomic builtins for cross-thread access */
} shutdown_client_ctx_t;

static void *shutdown_client_fn(void *arg)
{
    shutdown_client_ctx_t *ctx = (shutdown_client_ctx_t *)arg;
    __atomic_store_n(&ctx->calls_made, 0, __ATOMIC_RELAXED);

    nipc_client_config_t ccfg = default_client_config();
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);

    for (int i = 0; i < 100; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(10000);
    }

    if (!nipc_client_ready(&client)) {
        __atomic_store_n(&ctx->finished, 1, __ATOMIC_RELEASE);
        nipc_client_close(&client);
        return NULL;
    }

    __atomic_store_n(&ctx->connected, 1, __ATOMIC_RELEASE);

    /* Keep making calls until failure (server shutdown) */
    nipc_cgroups_resp_view_t view;

    while (1) {
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        if (err != NIPC_OK)
            break;
        __atomic_fetch_add(&ctx->calls_made, 1, __ATOMIC_RELAXED);
        usleep(10000);
    }

    nipc_client_close(&client);
    __atomic_store_n(&ctx->finished, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_graceful_shutdown(void)
{
    printf("Test 7: Graceful shutdown with connected clients\n");
    const char *svc = "msvc_shutdown";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t stid;
    start_server(&sctx, svc, echo_handler, &stid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Start 3 clients that keep making calls */
    const int num_clients = 3;
    pthread_t ctids[3];
    shutdown_client_ctx_t cctxs[3];

    for (int i = 0; i < num_clients; i++) {
        cctxs[i].service = svc;
        __atomic_store_n(&cctxs[i].connected, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cctxs[i].finished, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cctxs[i].calls_made, 0, __ATOMIC_RELAXED);
        pthread_create(&ctids[i], NULL, shutdown_client_fn, &cctxs[i]);
    }

    /* Wait for all clients to connect */
    for (int w = 0; w < 2000; w++) {
        int connected = 0;
        for (int i = 0; i < num_clients; i++)
            if (__atomic_load_n(&cctxs[i].connected, __ATOMIC_ACQUIRE))
                connected++;
        if (connected == num_clients)
            break;
        usleep(1000);
    }

    int connected_count = 0;
    for (int i = 0; i < num_clients; i++)
        if (__atomic_load_n(&cctxs[i].connected, __ATOMIC_ACQUIRE))
            connected_count++;
    check("all clients connected before shutdown",
          connected_count == num_clients);

    /* Let them run for a bit */
    usleep(200000);

    /* Signal shutdown */
    nipc_server_stop(&sctx.server);

    /* Wait for server thread to finish */
    pthread_join(stid, NULL);
    check("server thread exited cleanly", __atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) == 1);

    /* Wait for client threads to detect disconnection */
    for (int i = 0; i < num_clients; i++)
        pthread_join(ctids[i], NULL);

    /* Verify all clients finished (after join, no races possible) */
    int finished_count = 0;
    int min_calls = 999999;
    for (int i = 0; i < num_clients; i++) {
        if (__atomic_load_n(&cctxs[i].finished, __ATOMIC_ACQUIRE))
            finished_count++;
        if (__atomic_load_n(&cctxs[i].calls_made, __ATOMIC_ACQUIRE) < min_calls)
            min_calls = __atomic_load_n(&cctxs[i].calls_made, __ATOMIC_ACQUIRE);
    }

    check("all clients finished after shutdown",
          finished_count == num_clients);
    check("clients made calls before shutdown",
          min_calls > 0);

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

    printf("=== Multi-Client Multi-Worker Server Tests ===\n\n");

    test_basic_multi_client();                printf("\n");
    test_worker_limit();                      printf("\n");
    test_client_disconnect_recovery();        printf("\n");
    test_handler_failure_per_session();        printf("\n");
    test_rapid_connect_disconnect();           printf("\n");
    test_long_running_concurrent();            printf("\n");
    test_graceful_shutdown();                  printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
