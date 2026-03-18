/*
 * test_hardening.c - Hardening tests for managed POSIX L2/L3 paths.
 *
 * Focus:
 *   1. protocol-violation session termination on malformed outer headers
 *   2. typed server validation of required handler tables
 *   3. internal session table growth path under NIPC_INTERNAL_TESTING
 *   4. cache refresh failure on malformed snapshot responses
 *
 * Returns 0 if all tests pass, 1 otherwise.
 */

#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"
#include "netipc/netipc_uds.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR "/tmp/nipc_hardening_test"
#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
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
    snprintf(path, sizeof(path), "%s/%s.ipcshm", TEST_RUN_DIR, service);
    unlink(path);
}

static nipc_uds_server_config_t default_server_config(void)
{
    return (nipc_uds_server_config_t){
        .supported_profiles = NIPC_PROFILE_BASELINE,
        .preferred_profiles = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items = 1,
        .auth_token = AUTH_TOKEN,
        .packet_size = 0,
        .backlog = 8,
    };
}

static nipc_uds_client_config_t default_client_config(void)
{
    return (nipc_uds_client_config_t){
        .supported_profiles = NIPC_PROFILE_BASELINE,
        .preferred_profiles = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items = 1,
        .auth_token = AUTH_TOKEN,
        .packet_size = 0,
    };
}

static bool typed_increment_handler(void *user, uint64_t delta, uint64_t *new_val)
{
    (void)user;
    *new_val = delta + 1;
    return true;
}

static bool typed_string_reverse_handler(void *user,
                                         const char *str,
                                         uint32_t str_len,
                                         char *resp_buf,
                                         uint32_t resp_capacity,
                                         uint32_t *resp_len)
{
    (void)user;

    if (str_len > resp_capacity)
        return false;

    for (uint32_t i = 0; i < str_len; i++)
        resp_buf[i] = str[str_len - 1 - i];

    *resp_len = str_len;
    return true;
}

static bool typed_snapshot_handler(void *user,
                                   const nipc_cgroups_req_t *req,
                                   nipc_cgroups_builder_t *builder)
{
    (void)user;
    (void)req;

    static const struct {
        uint32_t hash;
        uint32_t enabled;
        const char *name;
        const char *path;
    } items[] = {
        { 1001, 1, "docker-abc123", "/sys/fs/cgroup/docker/abc123" },
        { 2002, 1, "k8s-pod-xyz", "/sys/fs/cgroup/kubepods/xyz" },
        { 3003, 0, "systemd-user", "/sys/fs/cgroup/user.slice/user-1000" },
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

static nipc_cgroups_handlers_t g_typed_handlers = {
    .on_increment = typed_increment_handler,
    .on_string_reverse = typed_string_reverse_handler,
    .on_cgroups_snapshot = typed_snapshot_handler,
    .snapshot_max_items = 3,
    .user = NULL,
};

typedef struct {
    const char *service;
    int worker_count;
    size_t response_buf_size;
    bool typed;
    nipc_server_handler_fn raw_handler;
    void *raw_user;
    nipc_managed_server_t server;
    int ready;
    int done;
} server_thread_ctx_t;

static void *server_thread_fn(void *arg)
{
    server_thread_ctx_t *ctx = (server_thread_ctx_t *)arg;
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_error_t err;

    scfg.max_response_payload_bytes = (uint32_t)ctx->response_buf_size;

    if (ctx->typed) {
        err = nipc_server_init_typed(&ctx->server, TEST_RUN_DIR, ctx->service,
                                     &scfg, ctx->worker_count, &g_typed_handlers);
    } else {
        err = nipc_server_init(&ctx->server, TEST_RUN_DIR, ctx->service,
                               &scfg, ctx->worker_count, ctx->response_buf_size,
                               ctx->raw_handler, ctx->raw_user);
    }

    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed for %s: %d\n", ctx->service, err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_typed_server(server_thread_ctx_t *ctx,
                               const char *service,
                               int worker_count,
                               size_t response_buf_size,
                               pthread_t *tid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->service = service;
    ctx->worker_count = worker_count;
    ctx->response_buf_size = response_buf_size;
    ctx->typed = true;
    __atomic_store_n(&ctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, server_thread_fn, ctx);

    for (int i = 0;
         i < 2000 &&
         !__atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE);
         i++) {
        usleep(500);
    }
}

static void start_raw_server(server_thread_ctx_t *ctx,
                             const char *service,
                             int worker_count,
                             size_t response_buf_size,
                             nipc_server_handler_fn raw_handler,
                             void *raw_user,
                             pthread_t *tid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->service = service;
    ctx->worker_count = worker_count;
    ctx->response_buf_size = response_buf_size;
    ctx->typed = false;
    ctx->raw_handler = raw_handler;
    ctx->raw_user = raw_user;
    __atomic_store_n(&ctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, server_thread_fn, ctx);

    for (int i = 0;
         i < 2000 &&
         !__atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE);
         i++) {
        usleep(500);
    }
}

static void stop_server(server_thread_ctx_t *ctx, pthread_t tid)
{
    nipc_server_stop(&ctx->server);
    pthread_join(tid, NULL);
}

static void verify_server_still_works(const char *service, const char *prefix)
{
    nipc_client_ctx_t client;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_cgroups_resp_view_t view;
    nipc_error_t err;
    char msg[128];

    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);

    snprintf(msg, sizeof(msg), "%s: replacement client ready", prefix);
    check(msg, nipc_client_ready(&client));

    if (nipc_client_ready(&client)) {
        err = nipc_client_call_cgroups_snapshot(&client, &view);
        snprintf(msg, sizeof(msg), "%s: replacement snapshot succeeds", prefix);
        check(msg, err == NIPC_OK);
        if (err == NIPC_OK) {
            snprintf(msg, sizeof(msg), "%s: replacement snapshot has 3 items", prefix);
            check(msg, view.item_count == 3);
        }
    }

    nipc_client_close(&client);
}

static void run_bad_header_session_test(const char *service,
                                        const char *prefix,
                                        uint32_t bad_magic,
                                        uint16_t bad_version)
{
    server_thread_ctx_t sctx;
    pthread_t server_tid;
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t uerr;
    nipc_header_t bad_hdr;
    nipc_header_t good_hdr;
    uint8_t req_buf[4];
    uint8_t recv_buf[4096];
    const void *payload = NULL;
    size_t payload_len = 0;
    nipc_header_t resp_hdr;
    char msg[128];

    cleanup_all(service);
    start_typed_server(&sctx, service, 2, RESPONSE_BUF_SIZE, &server_tid);
    snprintf(msg, sizeof(msg), "%s: server started", prefix);
    check(msg, __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    memset(&session, 0, sizeof(session));
    session.fd = -1;
    uerr = nipc_uds_connect(TEST_RUN_DIR, service, &ccfg, &session);
    snprintf(msg, sizeof(msg), "%s: raw connect", prefix);
    check(msg, uerr == NIPC_UDS_OK);

    if (uerr == NIPC_UDS_OK) {
        memset(&bad_hdr, 0, sizeof(bad_hdr));
        bad_hdr.magic = bad_magic;
        bad_hdr.version = bad_version;
        bad_hdr.header_len = NIPC_HEADER_LEN;
        bad_hdr.kind = NIPC_KIND_REQUEST;
        bad_hdr.code = NIPC_METHOD_INCREMENT;
        bad_hdr.flags = 0;
        bad_hdr.item_count = 1;
        bad_hdr.payload_len = 0;
        bad_hdr.message_id = 1;
        bad_hdr.transport_status = NIPC_STATUS_OK;

        ssize_t raw_send = send(session.fd, &bad_hdr, sizeof(bad_hdr), MSG_NOSIGNAL);
        snprintf(msg, sizeof(msg), "%s: send malformed header", prefix);
        check(msg, raw_send == (ssize_t)sizeof(bad_hdr));

        usleep(200000);

        memset(&good_hdr, 0, sizeof(good_hdr));
        good_hdr.kind = NIPC_KIND_REQUEST;
        good_hdr.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        good_hdr.flags = 0;
        good_hdr.item_count = 1;
        good_hdr.message_id = 2;
        good_hdr.transport_status = NIPC_STATUS_OK;

        nipc_cgroups_req_t req = { .layout_version = 1, .flags = 0 };
        nipc_cgroups_req_encode(&req, req_buf, sizeof(req_buf));
        (void)nipc_uds_send(&session, &good_hdr, req_buf, sizeof(req_buf));
        uerr = nipc_uds_receive(&session, recv_buf, sizeof(recv_buf),
                                &resp_hdr, &payload, &payload_len);
        snprintf(msg, sizeof(msg), "%s: recv after malformed header fails", prefix);
        check(msg, uerr != NIPC_UDS_OK);

        nipc_uds_close_session(&session);
    }

    verify_server_still_works(service, prefix);
    stop_server(&sctx, server_tid);
    cleanup_all(service);
}

static void test_bad_magic_terminates_only_the_bad_session(void)
{
    printf("--- Bad outer-header magic terminates only the bad session ---\n");
    run_bad_header_session_test("hard_bad_magic", "bad magic",
                                0xDEADBEEF, NIPC_VERSION);
}

static void test_bad_version_terminates_only_the_bad_session(void)
{
    printf("--- Bad outer-header version terminates only the bad session ---\n");
    run_bad_header_session_test("hard_bad_version", "bad version",
                                NIPC_MAGIC_MSG, 99);
}

static void test_typed_server_rejects_null_handlers(void)
{
    printf("--- Typed server rejects NULL handler table ---\n");

    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_error_t err = nipc_server_init_typed(&server, TEST_RUN_DIR,
                                              "hard_null_handlers", &scfg,
                                              1, NULL);
    check("typed init rejects NULL handlers", err == NIPC_ERR_BAD_LAYOUT);
    cleanup_all("hard_null_handlers");
}

static void test_internal_session_table_growth(void)
{
    printf("--- Internal session table growth under forced small capacity ---\n");

    const char *svc = "hard_growth";
    nipc_managed_server_t server;
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_client_ctx_t clients[3];
    pthread_t server_tid;
    nipc_error_t err;
    int ready_clients = 0;
    int observed_capacity = 0;
    int observed_count = 0;

    cleanup_all(svc);

    err = nipc_server_init_typed(&server, TEST_RUN_DIR, svc, &scfg, 4,
                                 &g_typed_handlers);
    check("growth: server init", err == NIPC_OK);
    if (err != NIPC_OK)
        return;

    nipc_session_ctx_t **shrunk = realloc(server.sessions, sizeof(*shrunk));
    check("growth: shrink session table to 1 slot", shrunk != NULL);
    if (!shrunk) {
        nipc_server_destroy(&server);
        cleanup_all(svc);
        return;
    }
    server.sessions = shrunk;
    server.session_capacity = 1;

    pthread_create(&server_tid, NULL, (void *(*)(void *))nipc_server_run, &server);
    usleep(50000);

    for (int i = 0; i < 3; i++) {
        nipc_uds_client_config_t ccfg = default_client_config();
        nipc_client_init(&clients[i], TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&clients[i]);
        if (nipc_client_ready(&clients[i]))
            ready_clients++;
    }
    check("growth: three clients connected", ready_clients == 3);

    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&server.sessions_lock);
        observed_capacity = server.session_capacity;
        observed_count = server.session_count;
        pthread_mutex_unlock(&server.sessions_lock);
        if (observed_count >= 3 && observed_capacity >= 4)
            break;
        usleep(5000);
    }

    check("growth: session table expanded", observed_capacity >= 4);
    check("growth: server tracked three sessions", observed_count >= 3);

    for (int i = 0; i < 3; i++)
        nipc_client_close(&clients[i]);

    nipc_server_stop(&server);
    pthread_join(server_tid, NULL);
    nipc_server_destroy(&server);
    cleanup_all(svc);
}

static bool malformed_snapshot_handler(void *user,
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

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size, 1, 1, 77);

    err = nipc_cgroups_builder_add(&builder,
                                   1234, 0, 1,
                                   "malformed", 9,
                                   "/sys/fs/cgroup/malformed", 25);
    if (err != NIPC_OK)
        return false;

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    if (*response_len_out == 0)
        return false;

    /* Corrupt name_offset inside the first item so outer decode succeeds,
     * but per-item decode fails inside cache_build_items(). Layout:
     * response header (24) + one dir entry (8) + item header. */
    {
        uint32_t bad_name_offset = 0;
        size_t item_start = NIPC_CGROUPS_RESP_HDR_SIZE + NIPC_CGROUPS_DIR_ENTRY_SIZE;
        memcpy(response_buf + item_start + 16, &bad_name_offset, sizeof(bad_name_offset));
    }

    return true;
}

static void test_cache_refresh_rejects_malformed_snapshot(void)
{
    printf("--- Cache refresh rejects malformed snapshot and preserves state ---\n");

    const char *svc = "hard_cache_malformed";
    server_thread_ctx_t sctx;
    pthread_t server_tid;
    nipc_cgroups_cache_t cache;
    nipc_cgroups_cache_status_t status;
    nipc_uds_client_config_t ccfg = default_client_config();
    bool updated;

    cleanup_all(svc);
    start_raw_server(&sctx, svc, 1, RESPONSE_BUF_SIZE,
                     malformed_snapshot_handler, NULL, &server_tid);
    check("cache malformed: server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_cgroups_cache_init(&cache, TEST_RUN_DIR, svc, &ccfg);
    updated = nipc_cgroups_cache_refresh(&cache);
    check("cache malformed: refresh fails", !updated);
    check("cache malformed: cache not ready", !nipc_cgroups_cache_ready(&cache));

    nipc_cgroups_cache_status(&cache, &status);
    check("cache malformed: failure_count == 1", status.refresh_failure_count == 1);
    check("cache malformed: success_count == 0", status.refresh_success_count == 0);
    check("cache malformed: item_count == 0", status.item_count == 0);

    nipc_cgroups_cache_close(&cache);
    stop_server(&sctx, server_tid);
    cleanup_all(svc);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    ensure_run_dir();
    setbuf(stdout, NULL);

    printf("=== Hardening Tests ===\n\n");

    test_bad_magic_terminates_only_the_bad_session();
    printf("\n");

    test_bad_version_terminates_only_the_bad_session();
    printf("\n");

    test_typed_server_rejects_null_handlers();
    printf("\n");

    test_internal_session_table_growth();
    printf("\n");

    test_cache_refresh_rejects_malformed_snapshot();
    printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
