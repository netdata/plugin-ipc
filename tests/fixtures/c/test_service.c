/*
 * test_service.c - Integration tests for L2 orchestration layer.
 *
 * Tests client context lifecycle, typed cgroups snapshot calls,
 * retry on failure, multiple clients, handler failure, and stats.
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

#define TEST_RUN_DIR  "/tmp/nipc_svc_test"
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

static void cleanup_socket(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
}

static void cleanup_shm(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ipcshm", TEST_RUN_DIR, service);
    unlink(path);
}

static void cleanup_all(const char *service)
{
    cleanup_socket(service);
    cleanup_shm(service);
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

static nipc_uds_client_config_t default_client_config(void)
{
    return (nipc_uds_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
    };
}

/* ------------------------------------------------------------------ */
/*  Cgroups snapshot handler (server side)                              */
/* ------------------------------------------------------------------ */

/* Build a test snapshot with N items using the Codec builder. */
static bool test_cgroups_handler(void *user,
                                  uint16_t method_code,
                                  const uint8_t *request_payload,
                                  size_t request_len,
                                  uint8_t *response_buf,
                                  size_t response_buf_size,
                                  size_t *response_len_out)
{
    (void)user;

    if (method_code != NIPC_METHOD_CGROUPS_SNAPSHOT)
        return false;

    /* Decode request to validate it */
    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return false;

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
            return false;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return true;
}

/* Handler that always fails */
static bool failing_handler(void *user,
                             uint16_t method_code,
                             const uint8_t *request_payload,
                             size_t request_len,
                             uint8_t *response_buf,
                             size_t response_buf_size,
                             size_t *response_len_out)
{
    (void)user;
    (void)method_code;
    (void)request_payload;
    (void)request_len;
    (void)response_buf;
    (void)response_buf_size;
    (void)response_len_out;
    return false;
}

/* ------------------------------------------------------------------ */
/*  Server thread context                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    nipc_server_handler_fn handler;
    volatile int ready;
    volatile int done;
} server_thread_ctx_t;

static void *server_thread_fn(void *arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;

    nipc_uds_server_config_t scfg = default_server_config();

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        1, RESPONSE_BUF_SIZE, ctx->handler, NULL);

    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        ctx->done = 1;
        return NULL;
    }

    ctx->ready = 1;

    /* Blocking acceptor loop */
    nipc_server_run(&ctx->server);

    nipc_server_destroy(&ctx->server);
    ctx->done = 1;
    return NULL;
}

/* Start a managed server in a background thread. */
static void start_server(server_thread_ctx_t *sctx, const char *service,
                          nipc_server_handler_fn handler, pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->handler = handler;
    sctx->ready = 0;
    sctx->done = 0;

    pthread_create(tid, NULL, server_thread_fn, sctx);

    /* Wait for server to be ready */
    for (int i = 0; i < 2000 && !sctx->ready && !sctx->done; i++)
        usleep(500);
}

/* Stop a managed server and join its thread. */
static void stop_server(server_thread_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

/* ------------------------------------------------------------------ */
/*  Test 1: Client lifecycle                                           */
/* ------------------------------------------------------------------ */

static void test_client_lifecycle(void)
{
    printf("Test 1: Client lifecycle (init -> not ready -> refresh -> ready -> close)\n");
    const char *svc = "svc_lifecycle";
    cleanup_all(svc);

    /* Init without server running */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    check("initial state is DISCONNECTED",
          client.state == NIPC_CLIENT_DISCONNECTED);
    check("not ready before connect",
          !nipc_client_ready(&client));

    /* Refresh without server -> NOT_FOUND */
    bool changed = nipc_client_refresh(&client);
    check("state changed after first refresh", changed);
    check("state is NOT_FOUND (no server)",
          client.state == NIPC_CLIENT_NOT_FOUND);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", sctx.ready == 1);

    /* Refresh -> READY */
    changed = nipc_client_refresh(&client);
    check("state changed after server up", changed);
    check("state is READY",
          client.state == NIPC_CLIENT_READY);
    check("ready returns true",
          nipc_client_ready(&client));

    /* Status reporting */
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("connect_count == 1", status.connect_count == 1);
    check("reconnect_count == 0", status.reconnect_count == 0);

    /* Close */
    nipc_client_close(&client);
    check("state is DISCONNECTED after close",
          client.state == NIPC_CLIENT_DISCONNECTED);
    check("not ready after close",
          !nipc_client_ready(&client));

    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Typed cgroups snapshot call                                */
/* ------------------------------------------------------------------ */

static void test_cgroups_call(void)
{
    printf("Test 2: Typed cgroups snapshot call\n");
    const char *svc = "svc_cgroups";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", sctx.ready == 1);

    /* Init + connect client */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client is READY", nipc_client_ready(&client));

    /* Make a typed call */
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;

    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);
    check("call succeeded", err == NIPC_OK);

    if (err == NIPC_OK) {
        check("item_count == 3", view.item_count == 3);
        check("systemd_enabled == 1", view.systemd_enabled == 1);
        check("generation == 42", view.generation == 42);

        /* Verify first item */
        nipc_cgroups_item_view_t item;
        nipc_error_t ierr = nipc_cgroups_resp_item(&view, 0, &item);
        check("item 0 decode ok", ierr == NIPC_OK);
        if (ierr == NIPC_OK) {
            check("item 0 hash", item.hash == 1001);
            check("item 0 enabled", item.enabled == 1);
            check("item 0 name",
                  item.name.len == strlen("docker-abc123") &&
                  memcmp(item.name.ptr, "docker-abc123", item.name.len) == 0);
            check("item 0 path",
                  item.path.len == strlen("/sys/fs/cgroup/docker/abc123") &&
                  memcmp(item.path.ptr, "/sys/fs/cgroup/docker/abc123",
                         item.path.len) == 0);
        }

        /* Verify third item */
        ierr = nipc_cgroups_resp_item(&view, 2, &item);
        check("item 2 decode ok", ierr == NIPC_OK);
        if (ierr == NIPC_OK) {
            check("item 2 hash", item.hash == 3003);
            check("item 2 enabled", item.enabled == 0);
            check("item 2 name",
                  item.name.len == strlen("systemd-user") &&
                  memcmp(item.name.ptr, "systemd-user", item.name.len) == 0);
        }
    }

    /* Verify stats */
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("call_count == 1", status.call_count == 1);
    check("error_count == 0", status.error_count == 0);

    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Retry on failure                                           */
/* ------------------------------------------------------------------ */

static void test_retry_on_failure(void)
{
    printf("Test 3: Retry on failure (server restart)\n");
    const char *svc = "svc_retry";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server 1 started", sctx.ready == 1);

    /* Init + connect client */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready (1st connect)", nipc_client_ready(&client));

    /* First call succeeds */
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;

    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);
    check("first call succeeded", err == NIPC_OK);

    /* Kill server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000); /* let cleanup settle */

    /* Restart server */
    server_thread_ctx_t sctx2;
    pthread_t tid2;
    start_server(&sctx2, svc, test_cgroups_handler, &tid2);
    check("server 2 started", sctx2.ready == 1);

    /* Next call should trigger reconnect + retry (at-least-once).
     * The first attempt will fail because the old session is dead.
     * L2 must detect this, reconnect, and retry once. */
    err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);
    check("call after server restart succeeded", err == NIPC_OK);

    if (err == NIPC_OK) {
        check("item_count after retry", view.item_count == 3);
    }

    /* Verify reconnect happened */
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("reconnect_count >= 1", status.reconnect_count >= 1);

    nipc_client_close(&client);
    stop_server(&sctx2, tid2);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Multiple clients                                           */
/* ------------------------------------------------------------------ */

static void test_multiple_clients(void)
{
    printf("Test 4: Multiple clients to one managed server\n");
    const char *svc = "svc_multi";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", sctx.ready == 1);

    /* Create and connect two clients */
    nipc_client_ctx_t client1, client2;
    nipc_uds_client_config_t ccfg = default_client_config();

    nipc_client_init(&client1, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client1);
    check("client 1 ready", nipc_client_ready(&client1));

    /* Make a call from client 1 */
    uint8_t req_buf1[64], resp_buf1[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view1;
    nipc_error_t err1 = nipc_client_call_cgroups_snapshot(
        &client1, req_buf1, resp_buf1, sizeof(resp_buf1), &view1);
    check("client 1 call ok", err1 == NIPC_OK);

    /* Close client 1 so the server can accept client 2
     * (single-threaded acceptor) */
    nipc_client_close(&client1);

    nipc_client_init(&client2, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client2);
    check("client 2 ready", nipc_client_ready(&client2));

    /* Make a call from client 2 */
    uint8_t req_buf2[64], resp_buf2[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view2;
    nipc_error_t err2 = nipc_client_call_cgroups_snapshot(
        &client2, req_buf2, resp_buf2, sizeof(resp_buf2), &view2);
    check("client 2 call ok", err2 == NIPC_OK);

    if (err1 == NIPC_OK)
        check("client 1 got 3 items", view1.item_count == 3);
    if (err2 == NIPC_OK)
        check("client 2 got 3 items", view2.item_count == 3);

    nipc_client_close(&client2);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Handler failure                                            */
/* ------------------------------------------------------------------ */

static void test_handler_failure(void)
{
    printf("Test 5: Handler failure -> INTERNAL_ERROR\n");
    const char *svc = "svc_hfail";
    cleanup_all(svc);

    /* Start server with failing handler */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, failing_handler, &tid);
    check("server started", sctx.ready == 1);

    /* Connect client */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Make a call - handler fails, so we get an error.
     * The client will also attempt a retry (at-least-once) since
     * it was previously READY, but the handler will fail again. */
    uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);
    check("call fails when handler fails", err != NIPC_OK);

    /* Stats should reflect the error */
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("error_count >= 1", status.error_count >= 1);

    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Status reporting                                           */
/* ------------------------------------------------------------------ */

static void test_status_reporting(void)
{
    printf("Test 6: Status reporting (counters)\n");
    const char *svc = "svc_status";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", sctx.ready == 1);

    /* Connect client */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Initial counters */
    nipc_client_status_t s0;
    nipc_client_status(&client, &s0);
    check("initial connect_count == 1", s0.connect_count == 1);
    check("initial call_count == 0", s0.call_count == 0);
    check("initial error_count == 0", s0.error_count == 0);

    /* Make 3 successful calls */
    for (int i = 0; i < 3; i++) {
        uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(
            &client, req_buf, resp_buf, sizeof(resp_buf), &view);
        check("call succeeded", err == NIPC_OK);
    }

    /* Verify counters */
    nipc_client_status_t s1;
    nipc_client_status(&client, &s1);
    check("call_count == 3 after 3 calls", s1.call_count == 3);
    check("error_count still 0", s1.error_count == 0);

    /* Make a call to a non-ready client */
    nipc_client_close(&client);
    uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);
    check("call on disconnected fails", err != NIPC_OK);

    nipc_client_status_t s2;
    nipc_client_status(&client, &s2);
    check("error_count incremented", s2.error_count == 1);

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

    printf("=== L2 Orchestration Tests ===\n\n");

    test_client_lifecycle();       printf("\n");
    test_cgroups_call();           printf("\n");
    test_retry_on_failure();       printf("\n");
    test_multiple_clients();       printf("\n");
    test_handler_failure();        printf("\n");
    test_status_reporting();       printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
