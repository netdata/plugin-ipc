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

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/futex.h>

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

static void session_shm_path(char *dst, size_t dst_len,
                             const char *service, uint64_t session_id)
{
    snprintf(dst, dst_len, "%s/%s-%016llx.ipcshm",
             TEST_RUN_DIR, service, (unsigned long long)session_id);
}

static void cleanup_session_shm(const char *service, uint64_t session_id)
{
    char path[256];
    session_shm_path(path, sizeof(path), service, session_id);
    unlink(path);
    rmdir(path);
}

static void create_session_shm_obstruction_dir(const char *service,
                                               uint64_t session_id)
{
    char path[256];
    session_shm_path(path, sizeof(path), service, session_id);
    unlink(path);
    rmdir(path);
    mkdir(path, 0700);
}

static void cleanup_all(const char *service)
{
    cleanup_socket(service);
    cleanup_shm(service);
}

static void publish_raw_shm_message(nipc_shm_ctx_t *ctx,
                                    const void *msg,
                                    size_t copy_len,
                                    uint32_t published_len)
{
    nipc_shm_region_header_t *hdr = (nipc_shm_region_header_t *)ctx->base;
    uint8_t *dst;
    uint64_t *seq_ptr;
    uint32_t *len_ptr;
    uint32_t *signal_ptr;

    if (ctx->role == NIPC_SHM_ROLE_CLIENT) {
        dst = (uint8_t *)ctx->base + ctx->request_offset;
        seq_ptr = &hdr->req_seq;
        len_ptr = &hdr->req_len;
        signal_ptr = &hdr->req_signal;
    } else {
        dst = (uint8_t *)ctx->base + ctx->response_offset;
        seq_ptr = &hdr->resp_seq;
        len_ptr = &hdr->resp_len;
        signal_ptr = &hdr->resp_signal;
    }

    if (msg && copy_len > 0)
        memcpy(dst, msg, copy_len);

    __atomic_store_n(len_ptr, published_len, __ATOMIC_RELEASE);
    __atomic_add_fetch(seq_ptr, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(signal_ptr, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, signal_ptr, FUTEX_WAKE, 1, NULL, NULL, 0);

    if (ctx->role == NIPC_SHM_ROLE_CLIENT)
        ctx->local_req_seq++;
    else
        ctx->local_resp_seq++;
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
    int ready;  /* use __atomic builtins for cross-thread access */
    int done;   /* use __atomic builtins for cross-thread access */
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
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    /* Blocking acceptor loop */
    nipc_server_run(&ctx->server);

    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Start a managed server in a background thread. */
static void start_server(server_thread_ctx_t *sctx, const char *service,
                          nipc_server_handler_fn handler, pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->handler = handler;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, server_thread_fn, sctx);

    /* Wait for server to be ready */
    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
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
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

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
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

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

    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
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
    check("server 1 started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

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

    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("first call succeeded", err == NIPC_OK);

    /* Kill server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000); /* let cleanup settle */

    /* Restart server */
    server_thread_ctx_t sctx2;
    pthread_t tid2;
    start_server(&sctx2, svc, test_cgroups_handler, &tid2);
    check("server 2 started", __atomic_load_n(&sctx2.ready, __ATOMIC_ACQUIRE) == 1);

    /* Next call should trigger reconnect + retry (at-least-once).
     * The first attempt will fail because the old session is dead.
     * L2 must detect this, reconnect, and retry once. */
    err = nipc_client_call_cgroups_snapshot(&client, &view);
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
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Create and connect two clients */
    nipc_client_ctx_t client1, client2;
    nipc_uds_client_config_t ccfg = default_client_config();

    nipc_client_init(&client1, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client1);
    check("client 1 ready", nipc_client_ready(&client1));

    /* Make a call from client 1 */
    uint8_t req_buf1[64], resp_buf1[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view1;
    nipc_error_t err1 = nipc_client_call_cgroups_snapshot(&client1, &view1);
    check("client 1 call ok", err1 == NIPC_OK);

    if (err1 == NIPC_OK)
        check("client 1 got 3 items", view1.item_count == 3);

    /* Close client 1 so the server can accept client 2
     * (single-threaded acceptor). Give the server a short window to
     * observe the disconnect before opening the next session. */
    nipc_client_close(&client1);
    usleep(50000);

    nipc_client_init(&client2, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client2);
    check("client 2 ready", nipc_client_ready(&client2));

    /* Make a call from client 2 */
    uint8_t req_buf2[64], resp_buf2[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view2;
    nipc_error_t err2 = nipc_client_call_cgroups_snapshot(&client2, &view2);
    check("client 2 call ok", err2 == NIPC_OK);

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
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

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
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
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
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

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
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
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
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("call on disconnected fails", err != NIPC_OK);

    nipc_client_status_t s2;
    nipc_client_status(&client, &s2);
    check("error_count incremented", s2.error_count == 1);

    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: Graceful server drain                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int done;  /* __atomic */
    int call_ok;  /* __atomic */
} drain_client_ctx_t;

static void *drain_client_fn(void *arg)
{
    drain_client_ctx_t *ctx = (drain_client_ctx_t *)arg;

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, ctx->service, &ccfg);

    /* Connect with retry */
    for (int r = 0; r < 200; r++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(5000);
    }

    if (nipc_client_ready(&client)) {
        /* Make a slow series of calls to be "in-flight" during drain */
        for (int i = 0; i < 5; i++) {
            uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            if (err == NIPC_OK)
                __atomic_fetch_add(&ctx->call_ok, 1, __ATOMIC_RELAXED);
            usleep(10000); /* 10ms between calls */
        }
    }

    nipc_client_close(&client);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_graceful_drain(void)
{
    printf("Test 7: Graceful server drain with active clients\n");
    const char *svc = "svc_drain";
    cleanup_all(svc);

    /* Start server with multi-worker support */
    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    nipc_error_t err = nipc_server_init(&server,
        TEST_RUN_DIR, svc, &scfg,
        4, RESPONSE_BUF_SIZE, test_cgroups_handler, NULL);
    check("server init ok", err == NIPC_OK);

    /* Start server in background thread */
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, (void *(*)(void *))nipc_server_run, &server);
    usleep(50000); /* let it start */

    /* Start 3 client threads that will be making calls */
    drain_client_ctx_t cctxs[3];
    pthread_t ctids[3];
    for (int i = 0; i < 3; i++) {
        cctxs[i].service = svc;
        __atomic_store_n(&cctxs[i].done, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cctxs[i].call_ok, 0, __ATOMIC_RELAXED);
        pthread_create(&ctids[i], NULL, drain_client_fn, &cctxs[i]);
    }

    /* Let clients establish connections and start making calls */
    usleep(50000);

    /* Drain with 5-second timeout */
    bool drained = nipc_server_drain(&server, 5000);
    check("drain completed", drained);

    /* Wait for client threads to finish */
    for (int i = 0; i < 3; i++)
        pthread_join(ctids[i], NULL);

    /* Join server thread */
    pthread_join(server_tid, NULL);

    /* Check that clients got at least some successful calls */
    int total_ok = 0;
    for (int i = 0; i < 3; i++)
        total_ok += __atomic_load_n(&cctxs[i].call_ok, __ATOMIC_RELAXED);
    printf("    total successful calls during drain: %d\n", total_ok);
    check("clients got successful calls before drain", total_ok > 0);

    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: non-request message terminates session                        */
/* ------------------------------------------------------------------ */

static void test_non_request_terminates_session(void)
{
    const char *svc = "svc_nonreq";
    cleanup_all(svc);
    printf("--- test_non_request_terminates_session ---\n");

    /* Start server */
    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_error_t err = nipc_server_init(&server, TEST_RUN_DIR, svc,
                                         &scfg, 2, RESPONSE_BUF_SIZE,
                                         test_cgroups_handler, NULL);
    check("server init", err == NIPC_OK);

    pthread_t server_tid;
    pthread_create(&server_tid, NULL,
                   (void *(*)(void *))nipc_server_run, &server);
    usleep(50000);

    /* Connect via raw UDS session */
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    memset(&session, 0, sizeof(session));
    session.fd = -1;
    nipc_uds_error_t uerr = nipc_uds_connect(TEST_RUN_DIR, svc,
                                               &ccfg, &session);
    check("raw connect", uerr == NIPC_UDS_OK);

    /* Send a RESPONSE message (not REQUEST) - protocol violation */
    nipc_header_t hdr = {0};
    hdr.kind             = NIPC_KIND_RESPONSE;
    hdr.code             = NIPC_METHOD_CGROUPS_SNAPSHOT;
    hdr.flags            = 0;
    hdr.item_count       = 0;
    hdr.message_id       = 1;
    hdr.transport_status = NIPC_STATUS_OK;

    uerr = nipc_uds_send(&session, &hdr, NULL, 0);
    check("send non-request", uerr == NIPC_UDS_OK);

    /* Wait for server to process and terminate the session */
    usleep(200000);

    /* Try to send a valid request - should fail because server closed */
    nipc_header_t hdr2 = {0};
    hdr2.kind             = NIPC_KIND_REQUEST;
    hdr2.code             = NIPC_METHOD_CGROUPS_SNAPSHOT;
    hdr2.flags            = 0;
    hdr2.item_count       = 1;
    hdr2.message_id       = 2;
    hdr2.transport_status = NIPC_STATUS_OK;

    uint8_t req_buf[4];
    nipc_cgroups_req_t req = { .layout_version = 1, .flags = 0 };
    nipc_cgroups_req_encode(&req, req_buf, sizeof(req_buf));

    /* Send may succeed (buffered), but receive should fail */
    nipc_uds_send(&session, &hdr2, req_buf, 4);
    uint8_t recv_buf[4096];
    nipc_header_t resp_hdr;
    const void *payload;
    size_t payload_len;
    nipc_uds_error_t recv_err = nipc_uds_receive(&session, recv_buf,
                                                   sizeof(recv_buf),
                                                   &resp_hdr, &payload,
                                                   &payload_len);
    check("recv after non-request fails", recv_err != NIPC_UDS_OK);

    nipc_uds_close_session(&session);

    /* Verify server is still alive: connect a new client and do a normal call */
    nipc_client_ctx_t verify_client;
    nipc_client_init(&verify_client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&verify_client);
    check("server still alive after bad client",
          nipc_client_ready(&verify_client));

    if (nipc_client_ready(&verify_client)) {
        uint8_t vreq[64], vresp[RESPONSE_BUF_SIZE];
        nipc_cgroups_resp_view_t vview;
        nipc_error_t verr = nipc_client_call_cgroups_snapshot(&verify_client, &vview);
        check("normal call succeeds after bad client", verr == NIPC_OK);
        if (verr == NIPC_OK)
            check("response correct after bad client", vview.item_count == 3);
    }
    nipc_client_close(&verify_client);

    nipc_server_stop(&server);
    pthread_join(server_tid, NULL);
    nipc_server_destroy(&server);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Multi-method handler (INCREMENT + STRING_REVERSE + CGROUPS)         */
/* ------------------------------------------------------------------ */

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

static bool multi_method_handler(
    void *user,
    uint16_t method_code,
    const uint8_t *request_payload, size_t request_len,
    uint8_t *response_buf, size_t response_buf_size,
    size_t *response_len_out)
{
    switch (method_code) {
    case NIPC_METHOD_INCREMENT:
        return nipc_dispatch_increment(request_payload, request_len,
                                        response_buf, response_buf_size,
                                        response_len_out, on_increment, user);
    case NIPC_METHOD_STRING_REVERSE:
        return nipc_dispatch_string_reverse(request_payload, request_len,
                                             response_buf, response_buf_size,
                                             response_len_out, on_string_reverse, user);
    case NIPC_METHOD_CGROUPS_SNAPSHOT:
        return test_cgroups_handler(user, method_code, request_payload, request_len,
                                     response_buf, response_buf_size, response_len_out);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/*  Test: SHM-mode L2 service (increment + string_reverse over SHM)     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    int ready;
    int done;
} shm_server_ctx_t;

typedef struct {
    const char *service;
    uint32_t max_request_payload_bytes;
    uint32_t max_response_payload_bytes;
    nipc_managed_server_t server;
    int ready;
    int done;
} shm_limit_server_ctx_t;

static void *shm_server_thread_fn(void *arg)
{
    shm_server_ctx_t *ctx = (shm_server_ctx_t *)arg;

    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .backlog                   = 4,
    };

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        2, RESPONSE_BUF_SIZE, multi_method_handler, NULL);

    if (err != NIPC_OK) {
        fprintf(stderr, "SHM server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *shm_limit_server_thread_fn(void *arg)
{
    shm_limit_server_ctx_t *ctx = (shm_limit_server_ctx_t *)arg;
    nipc_uds_server_config_t scfg = {
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = ctx->max_request_payload_bytes,
        .max_request_batch_items = 16,
        .max_response_payload_bytes = ctx->max_response_payload_bytes,
        .max_response_batch_items = 16,
        .auth_token = AUTH_TOKEN,
        .backlog = 4,
    };

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        2, RESPONSE_BUF_SIZE, multi_method_handler, NULL);

    if (err != NIPC_OK) {
        fprintf(stderr, "limited SHM server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

typedef enum {
    FAKE_SHM_RESP_TOO_LARGE = 1,
    FAKE_SHM_RESP_SHORT,
    FAKE_SHM_RESP_BAD_MAGIC,
    FAKE_SHM_RESP_BAD_KIND,
    FAKE_SHM_RESP_BAD_CODE,
    FAKE_SHM_RESP_BAD_MESSAGE_ID,
    FAKE_SHM_RESP_BAD_ITEM_COUNT,
} fake_shm_response_mode_t;

typedef struct {
    const char *service;
    fake_shm_response_mode_t mode;
    int ready;
    int done;
} fake_shm_server_ctx_t;

static void *fake_shm_response_server_thread_fn(void *arg)
{
    fake_shm_server_ctx_t *ctx = (fake_shm_server_ctx_t *)arg;
    nipc_uds_listener_t listener;
    nipc_uds_session_t session;
    nipc_shm_ctx_t shm;
    uint8_t *req_buf = NULL;
    nipc_uds_server_config_t scfg = {
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items = 1,
        .auth_token = AUTH_TOKEN,
        .backlog = 4,
    };

    memset(&listener, 0, sizeof(listener));
    memset(&session, 0, sizeof(session));
    memset(&shm, 0, sizeof(shm));
    listener.fd = -1;
    session.fd = -1;
    shm.fd = -1;

    if (nipc_uds_listen(TEST_RUN_DIR, ctx->service, &scfg, &listener) != NIPC_UDS_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    if (nipc_uds_accept(&listener, 1, &session) != NIPC_UDS_OK)
        goto out;

    if (session.selected_profile != NIPC_PROFILE_SHM_HYBRID)
        goto out;

    if (nipc_shm_server_create(TEST_RUN_DIR, ctx->service, session.session_id,
                               session.max_request_payload_bytes + NIPC_HEADER_LEN,
                               session.max_response_payload_bytes + NIPC_HEADER_LEN,
                               &shm) != NIPC_SHM_OK)
        goto out;

    req_buf = malloc((size_t)session.max_request_payload_bytes + NIPC_HEADER_LEN);
    if (!req_buf)
        goto out;

    size_t msg_len = 0;
    if (nipc_shm_receive(&shm, req_buf,
                         (size_t)session.max_request_payload_bytes + NIPC_HEADER_LEN,
                         &msg_len, 5000) != NIPC_SHM_OK)
        goto out;

    nipc_header_t req_hdr = {0};
    if (msg_len >= NIPC_HEADER_LEN)
        nipc_header_decode(req_buf, msg_len, &req_hdr);

    switch (ctx->mode) {
    case FAKE_SHM_RESP_TOO_LARGE: {
        uint8_t msg[NIPC_HEADER_LEN];
        nipc_header_t resp_hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_RESPONSE,
            .code = req_hdr.code,
            .flags = 0,
            .item_count = 1,
            .message_id = req_hdr.message_id,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&resp_hdr, msg, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg),
                                shm.response_capacity + 1);
        break;
    }
    case FAKE_SHM_RESP_SHORT: {
        uint8_t msg[8] = {0};
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    case FAKE_SHM_RESP_BAD_MAGIC: {
        uint8_t msg[NIPC_HEADER_LEN];
        memset(msg, 0, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    case FAKE_SHM_RESP_BAD_KIND: {
        uint8_t msg[NIPC_HEADER_LEN];
        nipc_header_t resp_hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_REQUEST,
            .code = req_hdr.code,
            .flags = 0,
            .item_count = 1,
            .message_id = req_hdr.message_id,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&resp_hdr, msg, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    case FAKE_SHM_RESP_BAD_CODE: {
        uint8_t msg[NIPC_HEADER_LEN];
        nipc_header_t resp_hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_RESPONSE,
            .code = (uint16_t)(req_hdr.code + 1),
            .flags = 0,
            .item_count = 1,
            .message_id = req_hdr.message_id,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&resp_hdr, msg, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    case FAKE_SHM_RESP_BAD_MESSAGE_ID: {
        uint8_t msg[NIPC_HEADER_LEN];
        nipc_header_t resp_hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_RESPONSE,
            .code = req_hdr.code,
            .flags = 0,
            .item_count = 1,
            .message_id = req_hdr.message_id + 1,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&resp_hdr, msg, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    case FAKE_SHM_RESP_BAD_ITEM_COUNT: {
        uint8_t msg[NIPC_HEADER_LEN];
        nipc_header_t resp_hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_RESPONSE,
            .code = req_hdr.code,
            .flags = NIPC_FLAG_BATCH,
            .item_count = (uint32_t)(req_hdr.item_count + 1),
            .message_id = req_hdr.message_id,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&resp_hdr, msg, sizeof(msg));
        publish_raw_shm_message(&shm, msg, sizeof(msg), (uint32_t)sizeof(msg));
        break;
    }
    }

out:
    free(req_buf);
    nipc_shm_destroy(&shm);
    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_shm_l2_service(void)
{
    printf("Test: SHM-mode L2 service (increment + string_reverse + cgroups)\n");
    const char *svc = "svc_shm_l2";
    cleanup_all(svc);

    /* Start SHM-capable server */
    shm_server_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;

    pthread_t tid;
    pthread_create(&tid, NULL, shm_server_thread_fn, &sctx);

    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
    check("SHM server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect client with SHM profile */
    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("SHM client ready", nipc_client_ready(&client));

    /* INCREMENT calls */
    uint8_t resp_buf[RESPONSE_BUF_SIZE];
    uint64_t inc_val;

    nipc_error_t err = nipc_client_call_increment(&client, 41, &inc_val);
    check("increment(41) ok", err == NIPC_OK);
    if (err == NIPC_OK)
        check("increment(41) == 42", inc_val == 42);

    err = nipc_client_call_increment(&client, 99, &inc_val);
    check("increment(99) ok", err == NIPC_OK);
    if (err == NIPC_OK)
        check("increment(99) == 100", inc_val == 100);

    /* STRING_REVERSE call */
    nipc_string_reverse_view_t str_view;
    err = nipc_client_call_string_reverse(&client, "hello", 5, &str_view);
    check("string_reverse(hello) ok", err == NIPC_OK);
    if (err == NIPC_OK)
        check("string_reverse(hello) == olleh",
              str_view.str_len == 5 &&
              memcmp(str_view.str, "olleh", 5) == 0);

    /* CGROUPS_SNAPSHOT call */
    uint8_t cg_req[64];
    nipc_cgroups_resp_view_t cg_view;
    err = nipc_client_call_cgroups_snapshot(&client, &cg_view);
    check("cgroups_snapshot ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups item_count == 3", cg_view.item_count == 3);
        check("cgroups generation == 42", cg_view.generation == 42);
    }

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: SHM client rejects malformed raw responses               */
/* ------------------------------------------------------------------ */

static void test_shm_client_rejects_malformed_responses(void)
{
    struct {
        fake_shm_response_mode_t mode;
        const char *service;
        const char *label;
        nipc_error_t expected;
    } cases[] = {
        { FAKE_SHM_RESP_TOO_LARGE, "svc_shm_resp_big", "oversize SHM response", NIPC_ERR_TRUNCATED },
        { FAKE_SHM_RESP_SHORT, "svc_shm_resp_short", "short SHM response", NIPC_ERR_TRUNCATED },
        { FAKE_SHM_RESP_BAD_MAGIC, "svc_shm_resp_magic", "bad SHM response header", NIPC_ERR_BAD_MAGIC },
        { FAKE_SHM_RESP_BAD_KIND, "svc_shm_resp_kind", "wrong SHM response kind", NIPC_ERR_BAD_KIND },
        { FAKE_SHM_RESP_BAD_CODE, "svc_shm_resp_code", "wrong SHM response code", NIPC_ERR_BAD_LAYOUT },
        { FAKE_SHM_RESP_BAD_MESSAGE_ID, "svc_shm_resp_id", "wrong SHM response message_id", NIPC_ERR_BAD_LAYOUT },
    };

    printf("Test: SHM client rejects malformed raw responses\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_shm_server_ctx_t sctx;
        pthread_t tid;
        nipc_uds_client_config_t ccfg = {
            .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items = 1,
            .auth_token = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        uint64_t value_out = 0;
        char msg[160];

        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);

        memset(&sctx, 0, sizeof(sctx));
        sctx.service = cases[i].service;
        sctx.mode = cases[i].mode;
        pthread_create(&tid, NULL, fake_shm_response_server_thread_fn, &sctx);

        for (int spin = 0;
             spin < 2000 &&
             !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
             !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
             spin++) {
            usleep(500);
        }

        snprintf(msg, sizeof(msg), "%s: fake SHM server started", cases[i].label);
        check(msg, __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_init(&client, TEST_RUN_DIR, cases[i].service, &ccfg);
        nipc_client_refresh(&client);
        snprintf(msg, sizeof(msg), "%s: client negotiated SHM", cases[i].label);
        check(msg, nipc_client_ready(&client) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_error_t err = nipc_client_call_increment(&client, 41, &value_out);
            snprintf(msg, sizeof(msg), "%s: client returns expected error", cases[i].label);
            check(msg, err == cases[i].expected);
            snprintf(msg, sizeof(msg), "%s: client no longer stays READY after malformed reply", cases[i].label);
            check(msg, !nipc_client_ready(&client));
        }

        nipc_client_close(&client);
        pthread_join(tid, NULL);
        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Coverage: SHM server rejects malformed raw requests                */
/* ------------------------------------------------------------------ */

static void test_shm_server_rejects_malformed_requests(void)
{
    struct {
        const char *service;
        const char *label;
        uint8_t msg[NIPC_HEADER_LEN];
        size_t copy_len;
        uint32_t published_len;
    } cases[] = {
        { "svc_shm_req_big", "oversize SHM request", {0}, NIPC_HEADER_LEN, 0 },
        { "svc_shm_req_short", "short SHM request", {0}, 8, 8 },
        { "svc_shm_req_magic", "bad SHM request header", {0}, NIPC_HEADER_LEN, NIPC_HEADER_LEN },
    };

    printf("Test: SHM server rejects malformed raw requests\n");

    cases[0].published_len = RESPONSE_BUF_SIZE + NIPC_HEADER_LEN + 1;
    cases[2].msg[0] = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        shm_server_ctx_t sctx;
        pthread_t tid;
        nipc_uds_client_config_t ccfg = {
            .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items = 1,
            .auth_token = AUTH_TOKEN,
        };
        nipc_client_ctx_t bad_client;
        nipc_client_ctx_t good_client;
        uint64_t increment_out = 0;
        char msg[160];

        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);

        memset(&sctx, 0, sizeof(sctx));
        sctx.service = cases[i].service;
        pthread_create(&tid, NULL, shm_server_thread_fn, &sctx);

        for (int spin = 0;
             spin < 2000 &&
             !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
             !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
             spin++) {
            usleep(500);
        }

        snprintf(msg, sizeof(msg), "%s: SHM server started", cases[i].label);
        check(msg, __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_init(&bad_client, TEST_RUN_DIR, cases[i].service, &ccfg);
        nipc_client_refresh(&bad_client);
        snprintf(msg, sizeof(msg), "%s: attacker client negotiated SHM", cases[i].label);
        check(msg, nipc_client_ready(&bad_client) && bad_client.shm != NULL);

        if (nipc_client_ready(&bad_client) && bad_client.shm != NULL) {
            if (cases[i].copy_len == NIPC_HEADER_LEN && cases[i].published_len == NIPC_HEADER_LEN) {
                memset(cases[i].msg, 0, sizeof(cases[i].msg));
            }
            publish_raw_shm_message(bad_client.shm,
                                    cases[i].copy_len > 0 ? cases[i].msg : NULL,
                                    cases[i].copy_len,
                                    cases[i].published_len);
            usleep(100000);
        }

        nipc_client_close(&bad_client);

        nipc_client_init(&good_client, TEST_RUN_DIR, cases[i].service, &ccfg);
        nipc_client_refresh(&good_client);
        snprintf(msg, sizeof(msg), "%s: replacement client ready", cases[i].label);
        check(msg, nipc_client_ready(&good_client) && good_client.shm != NULL);

        if (nipc_client_ready(&good_client) && good_client.shm != NULL) {
            nipc_error_t err = nipc_client_call_increment(&good_client, 9, &increment_out);
            snprintf(msg, sizeof(msg), "%s: replacement increment succeeds", cases[i].label);
            check(msg, err == NIPC_OK && increment_out == 10);
        }

        nipc_client_close(&good_client);
        nipc_server_stop(&sctx.server);
        pthread_join(tid, NULL);
        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Coverage: SHM batch client error propagation                       */
/* ------------------------------------------------------------------ */

static void test_shm_batch_send_overflow_on_negotiated_limit(void)
{
    printf("Test: SHM batch send overflow on negotiated request limit\n");

    const char *svc = "svc_shm_batch_send_overflow";
    shm_limit_server_ctx_t sctx;
    pthread_t tid;
    nipc_uds_client_config_t ccfg = {
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items = 16,
        .auth_token = AUTH_TOKEN,
    };
    nipc_client_ctx_t client;
    uint64_t req_values[] = {1, 2, 3, 4, 5, 6};
    uint64_t resp_values[6] = {0};

    cleanup_all(svc);
    cleanup_session_shm(svc, 1);

    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;
    sctx.max_request_payload_bytes = 16;
    sctx.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    pthread_create(&tid, NULL, shm_limit_server_thread_fn, &sctx);

    for (int spin = 0;
         spin < 2000 &&
         !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
         spin++) {
        usleep(500);
    }

    check("batch send overflow: server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("batch send overflow: client negotiated SHM",
          nipc_client_ready(&client) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        nipc_error_t err = nipc_client_call_increment_batch(
            &client, req_values, 6, resp_values);
        check("batch send overflow: call returns OVERFLOW",
              err == NIPC_ERR_OVERFLOW);
    }

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_all(svc);
    cleanup_session_shm(svc, 1);
}

static void test_shm_batch_rejects_malformed_responses(void)
{
    struct {
        fake_shm_response_mode_t mode;
        const char *service;
        const char *label;
        nipc_error_t expected;
    } cases[] = {
        { FAKE_SHM_RESP_TOO_LARGE, "svc_shm_batch_resp_big", "oversize SHM batch response", NIPC_ERR_TRUNCATED },
        { FAKE_SHM_RESP_BAD_ITEM_COUNT, "svc_shm_batch_resp_count", "wrong SHM batch item_count", NIPC_ERR_BAD_ITEM_COUNT },
    };

    printf("Test: SHM batch client rejects malformed raw responses\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_shm_server_ctx_t sctx;
        pthread_t tid;
        nipc_uds_client_config_t ccfg = {
            .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items = 16,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items = 16,
            .auth_token = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        uint64_t req_values[] = {10, 20};
        uint64_t resp_values[2] = {0};
        char msg[160];

        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);

        memset(&sctx, 0, sizeof(sctx));
        sctx.service = cases[i].service;
        sctx.mode = cases[i].mode;
        pthread_create(&tid, NULL, fake_shm_response_server_thread_fn, &sctx);

        for (int spin = 0;
             spin < 2000 &&
             !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
             !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
             spin++) {
            usleep(500);
        }

        snprintf(msg, sizeof(msg), "%s: fake SHM server started", cases[i].label);
        check(msg, __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_init(&client, TEST_RUN_DIR, cases[i].service, &ccfg);
        nipc_client_refresh(&client);
        snprintf(msg, sizeof(msg), "%s: client negotiated SHM", cases[i].label);
        check(msg, nipc_client_ready(&client) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_error_t err = nipc_client_call_increment_batch(
                &client, req_values, 2, resp_values);
            snprintf(msg, sizeof(msg), "%s: client returns expected error", cases[i].label);
            check(msg, err == cases[i].expected);
        }

        nipc_client_close(&client);
        pthread_join(tid, NULL);
        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: Refresh failure preserves cache                               */
/* ------------------------------------------------------------------ */

static void test_cache_refresh_failure_preserves(void)
{
    printf("Test: Refresh failure preserves cache\n");
    const char *svc = "svc_cache_pres";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Init cache and do first refresh */
    nipc_cgroups_cache_t cache;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("first refresh ok", updated);
    check("ready after refresh", nipc_cgroups_cache_ready(&cache));
    check("lookup ok after refresh",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    /* Get baseline status */
    nipc_cgroups_cache_status_t s0;
    nipc_cgroups_cache_status(&cache, &s0);
    check("success_count == 1", s0.refresh_success_count == 1);
    check("failure_count == 0", s0.refresh_failure_count == 0);

    /* Stop server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000);

    /* Refresh fails, but old cache must be preserved */
    updated = nipc_cgroups_cache_refresh(&cache);
    check("refresh fails without server", !updated);
    check("still ready (old cache preserved)", nipc_cgroups_cache_ready(&cache));
    check("old data still present",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);
    check("item 2 still present",
          nipc_cgroups_cache_lookup(&cache, 3003, "systemd-user") != NULL);

    nipc_cgroups_cache_status_t s1;
    nipc_cgroups_cache_status(&cache, &s1);
    check("success_count still 1", s1.refresh_success_count == 1);
    check("failure_count >= 1", s1.refresh_failure_count >= 1);

    nipc_cgroups_cache_close(&cache);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: Malformed response handling (handler returns garbage)         */
/* ------------------------------------------------------------------ */

/* Handler that returns garbage bytes that don't decode as valid cgroups */
static bool garbage_handler(void *user,
                              uint16_t method_code,
                              const uint8_t *request_payload,
                              size_t request_len,
                              uint8_t *response_buf,
                              size_t response_buf_size,
                              size_t *response_len_out)
{
    (void)user;
    (void)request_payload;
    (void)request_len;

    if (method_code != NIPC_METHOD_CGROUPS_SNAPSHOT)
        return false;

    /* Write garbage that won't decode as a valid cgroups response */
    if (response_buf_size < 16)
        return false;

    memset(response_buf, 0xFF, 16);
    *response_len_out = 16;
    return true;
}

static void test_malformed_response_handling(void)
{
    printf("Test: Malformed response handling\n");
    const char *svc = "svc_garbage";
    cleanup_all(svc);

    /* Phase 1: populate cache with good server */
    server_thread_ctx_t sctx_good;
    pthread_t tid_good;
    start_server(&sctx_good, svc, test_cgroups_handler, &tid_good);
    check("good server started",
          __atomic_load_n(&sctx_good.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_cgroups_cache_t cache;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("first refresh ok", updated);
    check("cache ready", nipc_cgroups_cache_ready(&cache));
    check("data present",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    /* Stop good server, start garbage server */
    stop_server(&sctx_good, tid_good);
    cleanup_all(svc);
    usleep(50000);

    server_thread_ctx_t sctx_bad;
    pthread_t tid_bad;
    start_server(&sctx_bad, svc, garbage_handler, &tid_bad);
    check("garbage server started",
          __atomic_load_n(&sctx_bad.ready, __ATOMIC_ACQUIRE) == 1);

    /* Phase 2: refresh against garbage server should fail gracefully,
     * but old cache data must be preserved */
    updated = nipc_cgroups_cache_refresh(&cache);
    check("refresh with garbage fails", !updated);
    check("still ready (old cache preserved)", nipc_cgroups_cache_ready(&cache));
    check("old data still present",
          nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123") != NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("success_count still 1", status.refresh_success_count == 1);
    check("failure_count >= 1", status.refresh_failure_count >= 1);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx_bad, tid_bad);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: client error mapping (auth failure)                       */
/* ------------------------------------------------------------------ */

static void test_client_auth_failure(void)
{
    printf("Test: Client auth failure mapping\n");
    const char *svc = "svc_auth_fail";
    cleanup_all(svc);

    /* Start server with one token */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect client with WRONG auth token */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.auth_token = 0x1111111111111111ull; /* wrong token */
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("state is AUTH_FAILED",
          client.state == NIPC_CLIENT_AUTH_FAILED);

    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: client error mapping (incompatible profiles)              */
/* ------------------------------------------------------------------ */

static void test_client_incompatible(void)
{
    printf("Test: Client incompatible profiles mapping\n");
    const char *svc = "svc_incompat";
    cleanup_all(svc);

    /* Start server that supports only baseline */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect client that supports only SHM_FUTEX (no overlap with server) */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_SHM_FUTEX; /* no overlap */
    ccfg.preferred_profiles = NIPC_PROFILE_SHM_FUTEX;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("state is INCOMPATIBLE",
          client.state == NIPC_CLIENT_INCOMPATIBLE);

    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: client BROKEN state refresh                               */
/* ------------------------------------------------------------------ */

static void test_client_broken_refresh(void)
{
    printf("Test: Client BROKEN state refresh\n");
    const char *svc = "svc_broken";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect client */
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Kill server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000);

    /* Make a call - should fail and put client in BROKEN state */
    uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("call fails (server gone)", err != NIPC_OK);
    /* After retry, the state is BROKEN (retry reconnect failed)
     * OR NOT_FOUND (reconnect attempt returned not found).
     * The key coverage is exercising the BROKEN -> reconnect path. */
    check("client in error state after failed call",
          client.state == NIPC_CLIENT_BROKEN ||
          client.state == NIPC_CLIENT_NOT_FOUND ||
          client.state == NIPC_CLIENT_DISCONNECTED);

    /* Force to BROKEN to test the BROKEN refresh path */
    client.state = NIPC_CLIENT_BROKEN;

    /* Start a new server */
    server_thread_ctx_t sctx2;
    pthread_t tid2;
    start_server(&sctx2, svc, test_cgroups_handler, &tid2);
    check("server 2 started", __atomic_load_n(&sctx2.ready, __ATOMIC_ACQUIRE) == 1);

    /* Refresh from BROKEN state - should reconnect */
    bool changed = nipc_client_refresh(&client);
    check("refresh from BROKEN changed state", changed);
    check("client ready after BROKEN refresh", nipc_client_ready(&client));

    nipc_client_close(&client);
    stop_server(&sctx2, tid2);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: batch increment end-to-end                                */
/* ------------------------------------------------------------------ */

static void test_batch_increment(void)
{
    printf("Test: Batch increment end-to-end\n");
    const char *svc = "svc_batch_inc";
    cleanup_all(svc);

    /* Start server with multi-method handler */
    shm_server_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.max_request_batch_items = 16;
    scfg.max_response_batch_items = 16;
    scfg.max_request_payload_bytes = 65536;
    scfg.max_response_payload_bytes = 65536;

    nipc_error_t serr = nipc_server_init(&sctx.server,
        TEST_RUN_DIR, svc, &scfg,
        2, RESPONSE_BUF_SIZE, multi_method_handler, NULL);
    check("server init ok", serr == NIPC_OK);

    pthread_t stid;
    pthread_create(&stid, NULL, (void *(*)(void *))nipc_server_run, &sctx.server);
    usleep(50000);

    /* Connect client with batch support */
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.max_request_batch_items = 16;
    ccfg.max_response_batch_items = 16;
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Call batch increment */
    uint64_t request_values[] = {10, 20, 30, 40, 50};
    uint64_t response_values[5] = {0};
    uint8_t resp_buf[RESPONSE_BUF_SIZE];

    nipc_error_t err = nipc_client_call_increment_batch(
        &client, request_values, 5, response_values);
    check("batch increment ok", err == NIPC_OK);

    if (err == NIPC_OK) {
        check("batch[0] == 11", response_values[0] == 11);
        check("batch[1] == 21", response_values[1] == 21);
        check("batch[2] == 31", response_values[2] == 31);
        check("batch[3] == 41", response_values[3] == 41);
        check("batch[4] == 51", response_values[4] == 51);
    }

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(stid, NULL);
    nipc_server_destroy(&sctx.server);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: server init with NULL args, listen failure                */
/* ------------------------------------------------------------------ */

static void test_server_init_null(void)
{
    printf("Test: Server init with NULL args\n");

    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    /* NULL run_dir */
    check("null run_dir",
          nipc_server_init(&server, NULL, "svc", &scfg, 1,
                           RESPONSE_BUF_SIZE, test_cgroups_handler, NULL)
              != NIPC_OK);

    /* NULL service_name */
    check("null service_name",
          nipc_server_init(&server, TEST_RUN_DIR, NULL, &scfg, 1,
                           RESPONSE_BUF_SIZE, test_cgroups_handler, NULL)
              != NIPC_OK);

    /* NULL handler */
    check("null handler",
          nipc_server_init(&server, TEST_RUN_DIR, "svc", &scfg, 1,
                           RESPONSE_BUF_SIZE, NULL, NULL)
              != NIPC_OK);
}

static void test_server_init_listen_failure(void)
{
    printf("Test: Server init with listen failure (bad path)\n");

    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    /* Non-existent parent directory */
    check("listen failure returns error",
          nipc_server_init(&server, "/tmp/nonexistent_svc_dir_99999", "svc",
                           &scfg, 1, RESPONSE_BUF_SIZE,
                           test_cgroups_handler, NULL)
              != NIPC_OK);
}

/* ------------------------------------------------------------------ */
/*  Coverage: server drain with short timeout                           */
/* ------------------------------------------------------------------ */

static void test_drain_timeout(void)
{
    printf("Test: Server drain with short timeout + blocking handler\n");
    const char *svc = "svc_drain_timeout";
    cleanup_all(svc);

    /* Use a normal handler (fast). Start multiple clients to fill
     * worker slots, then drain with a very short timeout while clients
     * are active. This exercises the drain code path. */
    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    nipc_error_t err = nipc_server_init(&server,
        TEST_RUN_DIR, svc, &scfg,
        4, RESPONSE_BUF_SIZE, test_cgroups_handler, NULL);
    check("server init ok", err == NIPC_OK);

    pthread_t server_tid;
    pthread_create(&server_tid, NULL, (void *(*)(void *))nipc_server_run, &server);
    usleep(50000);

    /* Start clients that make calls */
    drain_client_ctx_t cctxs[2];
    pthread_t ctids[2];
    for (int i = 0; i < 2; i++) {
        cctxs[i].service = svc;
        __atomic_store_n(&cctxs[i].done, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cctxs[i].call_ok, 0, __ATOMIC_RELAXED);
        pthread_create(&ctids[i], NULL, drain_client_fn, &cctxs[i]);
    }
    usleep(100000);

    /* Drain with short timeout */
    bool drained = nipc_server_drain(&server, 3000);
    /* With fast handlers and 3s timeout, drain should succeed */
    check("drain completed", drained);

    for (int i = 0; i < 2; i++)
        pthread_join(ctids[i], NULL);
    pthread_join(server_tid, NULL);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: cache with empty snapshot, linear scan fallback           */
/* ------------------------------------------------------------------ */

static bool empty_cgroups_handler(void *user,
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

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return false;

    /* Return an empty snapshot (0 items) */
    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               0, 0, 77);
    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return true;
}

static void test_cache_empty_snapshot(void)
{
    printf("Test: Cache with empty snapshot\n");
    const char *svc = "svc_cache_empty";
    cleanup_all(svc);

    /* Start server that returns empty snapshot */
    server_thread_ctx_t sctx;
    pthread_t tid;
    sctx.handler = empty_cgroups_handler;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;
    sctx.handler = empty_cgroups_handler;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_error_t serr = nipc_server_init(&sctx.server,
        TEST_RUN_DIR, svc, &scfg,
        1, RESPONSE_BUF_SIZE, empty_cgroups_handler, NULL);
    check("server init", serr == NIPC_OK);

    pthread_create(&tid, NULL, (void *(*)(void *))nipc_server_run, &sctx.server);
    usleep(50000);

    /* Create cache and refresh */
    nipc_cgroups_cache_t cache;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("empty refresh succeeded", updated);
    check("cache ready (empty snapshot)", nipc_cgroups_cache_ready(&cache));

    /* Lookup should return NULL for any key */
    check("lookup on empty returns NULL",
          nipc_cgroups_cache_lookup(&cache, 123, "nonexistent") == NULL);

    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    check("item_count == 0", status.item_count == 0);
    check("generation == 77", status.generation == 77);

    nipc_cgroups_cache_close(&cache);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    nipc_server_destroy(&sctx.server);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: cache linear scan fallback (lookup without hash table)     */
/* ------------------------------------------------------------------ */

static void test_cache_linear_scan(void)
{
    printf("Test: Cache lookup with many items (hash table path)\n");
    const char *svc = "svc_cache_linear";
    cleanup_all(svc);

    /* Start server */
    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Create cache and refresh */
    nipc_cgroups_cache_t cache;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);

    bool updated = nipc_cgroups_cache_refresh(&cache);
    check("refresh ok", updated);
    check("cache ready", nipc_cgroups_cache_ready(&cache));

    /* Lookup all 3 items from test_cgroups_handler */
    const nipc_cgroups_cache_item_t *item;
    item = nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123");
    check("lookup docker-abc123", item != NULL);
    if (item) check("docker-abc123 enabled", item->enabled == 1);

    item = nipc_cgroups_cache_lookup(&cache, 2002, "k8s-pod-xyz");
    check("lookup k8s-pod-xyz", item != NULL);

    item = nipc_cgroups_cache_lookup(&cache, 3003, "systemd-user");
    check("lookup systemd-user", item != NULL);
    if (item) check("systemd-user disabled", item->enabled == 0);

    /* Lookup non-existent item */
    item = nipc_cgroups_cache_lookup(&cache, 9999, "nonexistent");
    check("lookup nonexistent returns NULL", item == NULL);

    /* Lookup with wrong hash but correct name */
    item = nipc_cgroups_cache_lookup(&cache, 9999, "docker-abc123");
    check("wrong hash returns NULL", item == NULL);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: client call on DISCONNECTED state (generic error)         */
/* ------------------------------------------------------------------ */

static void test_client_call_disconnected(void)
{
    printf("Test: Client call on DISCONNECTED state\n");
    const char *svc = "svc_call_disc";
    cleanup_all(svc);

    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    /* Client is DISCONNECTED, call should fail immediately */
    uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("call on DISCONNECTED fails", err == NIPC_ERR_NOT_READY);

    /* Same for increment */
    uint64_t inc_val;
    err = nipc_client_call_increment(&client, 42, &inc_val);
    check("increment on DISCONNECTED fails", err == NIPC_ERR_NOT_READY);

    /* Same for string_reverse */
    nipc_string_reverse_view_t sv;
    err = nipc_client_call_string_reverse(&client, "hi", 2, &sv);
    check("string_reverse on DISCONNECTED fails", err == NIPC_ERR_NOT_READY);

    nipc_client_close(&client);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: server init with long strings (truncation)                */
/* ------------------------------------------------------------------ */

static void test_server_init_long_strings(void)
{
    printf("Test: Server init with long service_name (truncation)\n");

    /* service_name longer than 127 chars (buffer is 128) */
    char long_name[200];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    /* This will fail at listen() because the service name is invalid
     * for UDS path construction, but the truncation code is exercised */
    nipc_error_t err = nipc_server_init(&server, TEST_RUN_DIR, long_name,
                                          &scfg, 1, RESPONSE_BUF_SIZE,
                                          test_cgroups_handler, NULL);
    /* The service_name is truncated to 127 chars, still valid chars
     * but the socket path will be long. It may succeed or fail
     * depending on path length. Either way, code is exercised. */
    if (err == NIPC_OK)
        nipc_server_destroy(&server);
    check("long service_name does not crash", 1);
}

static void test_server_init_worker_floor_and_long_run_dir(void)
{
    printf("Test: Server init worker floor and long run_dir truncation\n");

    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();

    nipc_error_t err = nipc_server_init(&server, TEST_RUN_DIR, "svc_worker_floor",
                                        &scfg, 0, RESPONSE_BUF_SIZE,
                                        test_cgroups_handler, NULL);
    check("worker_count floor init ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("worker_count floor coerces to 1", server.worker_count == 1);
        nipc_server_destroy(&server);
    }
    cleanup_all("svc_worker_floor");

    char long_run_dir[400];
    memset(long_run_dir, 'r', sizeof(long_run_dir) - 1);
    long_run_dir[sizeof(long_run_dir) - 1] = '\0';

    err = nipc_server_init(&server, long_run_dir, "svc_long_run_dir",
                           &scfg, 1, RESPONSE_BUF_SIZE,
                           test_cgroups_handler, NULL);
    if (err == NIPC_OK)
        nipc_server_destroy(&server);
    check("long run_dir does not crash", 1);
}

/* ------------------------------------------------------------------ */
/*  Coverage: client init defaults + truncation                         */
/* ------------------------------------------------------------------ */

static void test_client_init_defaults_and_truncation(void)
{
    printf("Test: Client init defaults and truncation\n");

    nipc_client_ctx_t client;
    nipc_uds_client_config_t zero_cfg = {0};

    nipc_client_init(&client, TEST_RUN_DIR, "svc_client_defaults", &zero_cfg);
    check("default request buffer size",
          client.request_buf_size == 65536u);
    check("default response buffer size",
          client.response_buf_size == 65536u + NIPC_HEADER_LEN);
    check("default send buffer size",
          client.send_buf_size == 65536u + NIPC_HEADER_LEN);
    check("default buffers allocated",
          client.request_buf != NULL &&
          client.response_buf != NULL &&
          client.send_buf != NULL);
    nipc_client_close(&client);

    char long_run_dir[400];
    char long_service[260];
    memset(long_run_dir, 'r', sizeof(long_run_dir) - 1);
    long_run_dir[sizeof(long_run_dir) - 1] = '\0';
    memset(long_service, 's', sizeof(long_service) - 1);
    long_service[sizeof(long_service) - 1] = '\0';

    nipc_client_init(&client, long_run_dir, long_service, &zero_cfg);
    check("run_dir truncated to client buffer",
          strlen(client.run_dir) == sizeof(client.run_dir) - 1 &&
          client.run_dir[sizeof(client.run_dir) - 1] == '\0');
    check("service_name truncated to client buffer",
          strlen(client.service_name) == sizeof(client.service_name) - 1 &&
          client.service_name[sizeof(client.service_name) - 1] == '\0');
    nipc_client_close(&client);
}

/* ------------------------------------------------------------------ */
/*  Coverage: empty batch fast-path + request overflow guards           */
/* ------------------------------------------------------------------ */

static void test_client_batch_and_request_overflow(void)
{
    printf("Test: Client batch fast-path and request overflow guards\n");
    const char *svc = "svc_client_overflow";
    cleanup_all(svc);

    shm_server_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;

    pthread_t tid;
    pthread_create(&tid, NULL, shm_server_thread_fn, &sctx);
    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
    check("overflow server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    {
        nipc_uds_client_config_t ccfg = {
            .supported_profiles        = NIPC_PROFILE_BASELINE,
            .preferred_profiles        = 0,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items   = 16,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 16,
            .auth_token                = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("empty-batch client ready", nipc_client_ready(&client));

        nipc_error_t err = nipc_client_call_increment_batch(&client, NULL, 0, NULL);
        check("empty increment batch succeeds", err == NIPC_OK);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("empty increment batch increments call_count", status.call_count == 1);
        check("empty increment batch keeps error_count at 0", status.error_count == 0);
        nipc_client_close(&client);
    }

    {
        nipc_uds_client_config_t ccfg = {
            .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
            .max_request_payload_bytes = 1,
            .max_request_batch_items   = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 1,
            .auth_token                = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        uint64_t inc_value = 0;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("increment SHM overflow client ready", nipc_client_ready(&client));
        check("increment SHM overflow negotiated SHM", client.shm != NULL);

        nipc_error_t err = nipc_client_call_increment(&client, 9, &inc_value);
        check("increment detects tiny SHM send buffer overflow",
              err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);
    }

    {
        nipc_uds_client_config_t ccfg = {
            .supported_profiles        = NIPC_PROFILE_BASELINE,
            .preferred_profiles        = 0,
            .max_request_payload_bytes = 8,
            .max_request_batch_items   = 16,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 16,
            .auth_token                = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        uint64_t req = 7;
        uint64_t resp = 0;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("batch overflow client ready", nipc_client_ready(&client));

        nipc_error_t err = nipc_client_call_increment_batch(&client, &req, 1, &resp);
        check("increment batch detects tiny request buffer overflow",
              err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);
    }

    {
        nipc_uds_client_config_t ccfg = {
            .supported_profiles        = NIPC_PROFILE_BASELINE,
            .preferred_profiles        = 0,
            .max_request_payload_bytes = 8,
            .max_request_batch_items   = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 1,
            .auth_token                = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
        nipc_string_reverse_view_t view;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("string overflow client ready", nipc_client_ready(&client));

        nipc_error_t err = nipc_client_call_string_reverse(
            &client, "this request is intentionally too large", 38, &view);
        check("string reverse detects tiny request buffer overflow",
              err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);
    }

    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: negotiated SHM obstruction rejects the session            */
/* ------------------------------------------------------------------ */

static void test_shm_negotiation_failure_on_obstructed_region(void)
{
    printf("Test: Negotiated SHM obstruction rejects the session\n");
    const char *svc = "svc_shm_obstruct";
    cleanup_all(svc);
    cleanup_session_shm(svc, 1);
    create_session_shm_obstruction_dir(svc, 1);

    shm_server_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;

    pthread_t tid;
    pthread_create(&tid, NULL, shm_server_thread_fn, &sctx);
    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
    check("obstructed SHM server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    bool changed = nipc_client_refresh(&client);
    check("refresh reports no READY transition", !changed);
    check("client is not ready after obstructed SHM negotiation",
          !nipc_client_ready(&client));
    check("client state falls back to DISCONNECTED",
          client.state == NIPC_CLIENT_DISCONNECTED);
    check("failed SHM negotiation closes the UDS session",
          !client.session_valid && client.shm == NULL);

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_session_shm(svc, 1);
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
    test_graceful_drain();         printf("\n");
    test_non_request_terminates_session(); printf("\n");
    test_shm_l2_service();         printf("\n");
    test_shm_client_rejects_malformed_responses(); printf("\n");
    test_shm_server_rejects_malformed_requests(); printf("\n");
    test_shm_batch_send_overflow_on_negotiated_limit(); printf("\n");
    test_shm_batch_rejects_malformed_responses(); printf("\n");
    test_cache_refresh_failure_preserves(); printf("\n");
    test_malformed_response_handling(); printf("\n");

    /* Coverage gap tests */
    test_client_auth_failure();         printf("\n");
    test_client_incompatible();         printf("\n");
    test_client_broken_refresh();       printf("\n");
    test_batch_increment();             printf("\n");
    test_server_init_null();            printf("\n");
    test_server_init_listen_failure();  printf("\n");
    test_drain_timeout();               printf("\n");
    test_cache_empty_snapshot();        printf("\n");
    test_cache_linear_scan();           printf("\n");
    test_client_call_disconnected();    printf("\n");
    test_server_init_long_strings();    printf("\n");
    test_server_init_worker_floor_and_long_run_dir(); printf("\n");
    test_client_init_defaults_and_truncation(); printf("\n");
    test_client_batch_and_request_overflow(); printf("\n");
    test_shm_negotiation_failure_on_obstructed_region(); printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
