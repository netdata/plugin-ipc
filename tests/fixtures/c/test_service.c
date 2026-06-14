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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <time.h>
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
}

static void ensure_run_dir(void)
{
    mkdir(TEST_RUN_DIR, 0700);
}

static uint64_t monotonic_msec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static nipc_uds_error_t accept_with_timeout(nipc_uds_listener_t *listener,
                                            uint64_t session_id,
                                            uint32_t timeout_ms,
                                            nipc_uds_session_t *out)
{
    struct pollfd pfd = {
        .fd = listener ? listener->fd : -1,
        .events = POLLIN,
    };

    if (pfd.fd < 0)
        return NIPC_UDS_ERR_BAD_PARAM;

    for (;;) {
        int rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc > 0)
            break;
        if (rc == 0)
            return NIPC_UDS_ERR_TIMEOUT;
        if (errno == EINTR)
            continue;
        return NIPC_UDS_ERR_ACCEPT;
    }

    return nipc_uds_accept(listener, session_id, out);
}

static void cleanup_socket(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
}

static void cleanup_shm(const char *service)
{
    nipc_shm_cleanup_stale(TEST_RUN_DIR, service);
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
    char keep[300];
    session_shm_path(path, sizeof(path), service, session_id);
    unlink(path);
    snprintf(keep, sizeof(keep), "%s/keep", path);
    unlink(keep);
    rmdir(path);
}

static void create_session_shm_obstruction_dir(const char *service,
                                               uint64_t session_id)
{
    char path[256];
    char keep[300];
    session_shm_path(path, sizeof(path), service, session_id);
    cleanup_session_shm(service, session_id);
    mkdir(path, 0700);
    /* A non-empty directory cannot be reclaimed by stale recovery, so SHM
     * prepare keeps failing and the session must fall back to baseline. */
    snprintf(keep, sizeof(keep), "%s/keep", path);
    int fd = open(keep, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0)
        close(fd);
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

static nipc_client_config_t default_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                = AUTH_TOKEN,
    };
}

static nipc_uds_client_config_t default_transport_client_config(void)
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

/* Handler that always fails */
static nipc_error_t failing_handler(void *user,
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
    (void)response_len_out;
    return NIPC_ERR_HANDLER_FAILED;
}

static nipc_error_t slow_cgroups_handler(void *user,
                                         const nipc_header_t *request_hdr,
                                         const uint8_t *request_payload,
                                         size_t request_len,
                                         uint8_t *response_buf,
                                         size_t response_buf_size,
                                         size_t *response_len_out)
{
    usleep(150000);
    return test_cgroups_handler(user, request_hdr, request_payload, request_len,
                                response_buf, response_buf_size,
                                response_len_out);
}

static int g_blocking_handler_entered;
static int g_blocking_handler_release;

static nipc_error_t blocking_cgroups_handler(void *user,
                                             const nipc_header_t *request_hdr,
                                             const uint8_t *request_payload,
                                             size_t request_len,
                                             uint8_t *response_buf,
                                             size_t response_buf_size,
                                             size_t *response_len_out)
{
    __atomic_store_n(&g_blocking_handler_entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_blocking_handler_release, __ATOMIC_ACQUIRE))
        usleep(1000);

    return test_cgroups_handler(user, request_hdr, request_payload, request_len,
                                response_buf, response_buf_size,
                                response_len_out);
}

typedef struct {
    nipc_client_ctx_t *client;
    nipc_cgroups_resp_view_t view;
    nipc_error_t err;
} snapshot_call_thread_ctx_t;

static void *snapshot_call_thread_fn(void *arg)
{
    snapshot_call_thread_ctx_t *ctx = (snapshot_call_thread_ctx_t *)arg;
    ctx->err = nipc_client_call_cgroups_snapshot_timeout(ctx->client,
                                                         &ctx->view,
                                                         5000);
    return NULL;
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
        1, NIPC_METHOD_CGROUPS_SNAPSHOT, ctx->handler, NULL);

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

typedef struct {
    const char *service;
    uint16_t method_code;
    nipc_managed_server_t server;
    nipc_server_handler_fn handler;
    int ready;
    int done;
} raw_method_server_thread_ctx_t;

static void *raw_method_server_thread_fn(void *arg)
{
    raw_method_server_thread_ctx_t *ctx =
        (raw_method_server_thread_ctx_t *)arg;

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_error_t err = nipc_server_init(&ctx->server, TEST_RUN_DIR,
                                        ctx->service, &scfg, 1,
                                        ctx->method_code, ctx->handler, NULL);
    if (err != NIPC_OK) {
        fprintf(stderr, "raw method server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_raw_method_server(raw_method_server_thread_ctx_t *sctx,
                                    const char *service,
                                    uint16_t method_code,
                                    nipc_server_handler_fn handler,
                                    pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->method_code = method_code;
    sctx->handler = handler;
    pthread_create(tid, NULL, raw_method_server_thread_fn, sctx);

    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void stop_raw_method_server(raw_method_server_thread_ctx_t *sctx,
                                   pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

static nipc_server_config_t default_service_server_config(void)
{
    return (nipc_server_config_t){
        .supported_profiles = NIPC_PROFILE_BASELINE,
        .preferred_profiles = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token = AUTH_TOKEN,
    };
}

static int service_str_eq(nipc_str_view_t view, const char *s)
{
    size_t len = strlen(s);
    return view.len == len && memcmp(view.ptr, s, len) == 0;
}

typedef enum {
    LOOKUP_SERVER_CGROUPS = 1,
    LOOKUP_SERVER_APPS = 2,
} lookup_server_kind_t;

typedef struct {
    const char *service;
    lookup_server_kind_t kind;
    nipc_managed_server_t server;
    nipc_server_config_t config;
    int has_config;
    int worker_count;
    nipc_cgroups_lookup_service_handler_t cgroups_handler;
    nipc_apps_lookup_service_handler_t apps_handler;
    int ready;
    int done;
} lookup_server_thread_ctx_t;

static void *lookup_server_thread_fn(void *arg)
{
    lookup_server_thread_ctx_t *ctx = (lookup_server_thread_ctx_t *)arg;
    nipc_server_config_t scfg = ctx->has_config ? ctx->config : default_service_server_config();
    int worker_count = ctx->worker_count > 0 ? ctx->worker_count : 1;
    nipc_error_t err;

    if (ctx->kind == LOOKUP_SERVER_CGROUPS) {
        err = nipc_server_init_cgroups_lookup(&ctx->server, TEST_RUN_DIR,
                                              ctx->service, &scfg, worker_count,
                                              &ctx->cgroups_handler);
    } else {
        err = nipc_server_init_apps_lookup(&ctx->server, TEST_RUN_DIR,
                                           ctx->service, &scfg, worker_count,
                                           &ctx->apps_handler);
    }

    if (err != NIPC_OK) {
        fprintf(stderr, "lookup server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_lookup_server(lookup_server_thread_ctx_t *sctx,
                                const char *service,
                                lookup_server_kind_t kind,
                                pthread_t *tid)
{
    nipc_cgroups_lookup_service_handler_t cgroups_handler = sctx->cgroups_handler;
    nipc_apps_lookup_service_handler_t apps_handler = sctx->apps_handler;
    nipc_server_config_t config = sctx->config;
    int has_config = sctx->has_config;
    int worker_count = sctx->worker_count;
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    sctx->kind = kind;
    sctx->config = config;
    sctx->has_config = has_config;
    sctx->worker_count = worker_count;
    sctx->cgroups_handler = cgroups_handler;
    sctx->apps_handler = apps_handler;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);
    pthread_create(tid, NULL, lookup_server_thread_fn, sctx);
    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void stop_lookup_server(lookup_server_thread_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

typedef struct {
    const char *service;
    lookup_server_kind_t kind;
    int ready;
    int done;
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

static void *raw_lookup_partial_disconnect_thread(void *arg)
{
    raw_lookup_partial_disconnect_ctx_t *ctx =
        (raw_lookup_partial_disconnect_ctx_t *)arg;
    nipc_uds_listener_t listener;
    nipc_uds_session_t session;
    uint8_t recv_buf[4096];
    uint8_t response_buf[1024];
    nipc_uds_server_config_t scfg = default_server_config();

    memset(&listener, 0, sizeof(listener));
    memset(&session, 0, sizeof(session));
    listener.fd = -1;
    session.fd = -1;

    cleanup_all(ctx->service);
    if (nipc_uds_listen(TEST_RUN_DIR, ctx->service, &scfg, &listener) !=
        NIPC_UDS_OK)
        goto out;

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    if (accept_with_timeout(&listener, 1, 30000, &session) != NIPC_UDS_OK)
        goto out;

    nipc_header_t req_hdr;
    const void *request_payload = NULL;
    size_t request_len = 0;
    if (nipc_uds_receive(&session, recv_buf, sizeof(recv_buf), &req_hdr,
                         &request_payload, &request_len) != NIPC_UDS_OK)
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
        err = build_cgroups_lookup_partial_response(request_payload, request_len,
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
    (void)nipc_uds_send(&session, &resp_hdr, response_buf, response_len);

out:
    if (session.fd != -1)
        nipc_uds_close_session(&session);
    if (listener.fd != -1)
        nipc_uds_close_listener(&listener);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_raw_lookup_partial_disconnect(
        raw_lookup_partial_disconnect_ctx_t *ctx,
        const char *service,
        lookup_server_kind_t kind,
        pthread_t *tid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->service = service;
    ctx->kind = kind;
    pthread_create(tid, NULL, raw_lookup_partial_disconnect_thread, ctx);
    for (int i = 0; i < 2000 &&
                    !__atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE) &&
                    !__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static nipc_server_config_t default_shm_service_server_config(void)
{
    return (nipc_server_config_t){
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_FUTEX,
        .preferred_profiles = NIPC_PROFILE_SHM_FUTEX,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token = AUTH_TOKEN,
    };
}

static nipc_client_config_t default_shm_service_client_config(void)
{
    return (nipc_client_config_t){
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_FUTEX,
        .preferred_profiles = NIPC_PROFILE_SHM_FUTEX,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token = AUTH_TOKEN,
    };
}

typedef struct {
    const char *service;
    int ready;
    int done;
    int accepted;
} raw_hello_ack_server_ctx_t;

static void build_socket_path(char *dst, size_t dst_len, const char *service)
{
    snprintf(dst, dst_len, "%s/%s.sock", TEST_RUN_DIR, service);
}

static void *raw_hello_ack_version_server_thread(void *arg)
{
    raw_hello_ack_server_ctx_t *ctx = (raw_hello_ack_server_ctx_t *)arg;
    int fd = -1;
    int cfd = -1;
    char path[256];
    struct sockaddr_un addr;

    build_socket_path(path, sizeof(path), ctx->service);
    unlink(path);

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        goto out;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto out;
    if (listen(fd, 4) != 0)
        goto out;

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    cfd = accept(fd, NULL, NULL);
    if (cfd < 0)
        goto out;

    __atomic_store_n(&ctx->accepted, 1, __ATOMIC_RELEASE);

    {
        uint8_t hello_buf[128];
        ssize_t n = recv(cfd, hello_buf, sizeof(hello_buf), 0);
        if (n <= 0)
            goto out;
    }

    {
        nipc_hello_ack_t ack = {
            .layout_version = 1,
            .flags = 0,
            .server_supported_profiles = NIPC_PROFILE_BASELINE,
            .intersection_profiles = NIPC_PROFILE_BASELINE,
            .selected_profile = NIPC_PROFILE_BASELINE,
            .agreed_max_request_payload_bytes = NIPC_MAX_PAYLOAD_DEFAULT,
            .agreed_max_request_batch_items = 1,
            .agreed_max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .agreed_max_response_batch_items = 1,
            .agreed_packet_size = 0,
            ._reserved = 0,
            .session_id = 77,
        };
        uint8_t payload[sizeof(ack)];
        uint8_t packet[NIPC_HEADER_LEN + sizeof(ack)];
        nipc_header_t hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION + 1u,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_CONTROL,
            .flags = 0,
            .code = NIPC_CODE_HELLO_ACK,
            .transport_status = NIPC_STATUS_OK,
            .payload_len = (uint32_t)sizeof(ack),
            .item_count = 1,
            .message_id = 0,
        };

        if (nipc_hello_ack_encode(&ack, payload, sizeof(payload)) != sizeof(payload))
            goto out;
        if (nipc_header_encode(&hdr, packet, sizeof(packet)) != NIPC_HEADER_LEN)
            goto out;
        memcpy(packet + NIPC_HEADER_LEN, payload, sizeof(payload));
        if (send(cfd, packet, sizeof(packet), 0) != (ssize_t)sizeof(packet))
            goto out;
    }

out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_raw_hello_ack_version_server(raw_hello_ack_server_ctx_t *ctx,
                                               const char *service,
                                               pthread_t *tid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->service = service;
    pthread_create(tid, NULL, raw_hello_ack_version_server_thread, ctx);

    for (int i = 0; i < 2000
         && !__atomic_load_n(&ctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
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
    nipc_client_config_t ccfg = default_client_config();
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
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client is READY", nipc_client_ready(&client));

    /* Make a typed call */
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

static void test_client_call_timeout_on_wedged_peer(void)
{
    printf("Test: Client call timeout on wedged peer\n");
    const char *svc = "svc_call_timeout";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, slow_cgroups_handler, &tid);
    check("timeout test server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    for (int i = 0; i < 2000 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        usleep(500);
    }
    check("timeout test client is READY", nipc_client_ready(&client));

    nipc_cgroups_resp_view_t view;
    uint64_t start = monotonic_msec();
    nipc_error_t err = nipc_client_call_cgroups_snapshot_timeout(&client,
                                                                 &view,
                                                                 30);
    uint64_t elapsed = monotonic_msec() - start;
    check("wedged peer returns timeout", err == NIPC_ERR_TIMEOUT);
    check("timeout returns promptly", elapsed < 1000u);

    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_client_abort_unblocks_call(void)
{
    printf("Test: Client abort unblocks in-flight call\n");
    const char *svc = "svc_call_abort";
    cleanup_all(svc);

    __atomic_store_n(&g_blocking_handler_entered, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_blocking_handler_release, 0, __ATOMIC_RELEASE);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, blocking_cgroups_handler, &tid);
    check("abort test server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    for (int i = 0; i < 2000 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        usleep(500);
    }
    check("abort test client is READY", nipc_client_ready(&client));

    snapshot_call_thread_ctx_t call_ctx;
    memset(&call_ctx, 0, sizeof(call_ctx));
    call_ctx.client = &client;

    pthread_t call_tid;
    check("snapshot call thread created",
          pthread_create(&call_tid, NULL, snapshot_call_thread_fn,
                         &call_ctx) == 0);

    for (int i = 0; i < 2000
         && !__atomic_load_n(&g_blocking_handler_entered, __ATOMIC_ACQUIRE);
         i++)
        usleep(500);
    check("blocking handler received request",
          __atomic_load_n(&g_blocking_handler_entered,
                          __ATOMIC_ACQUIRE) == 1);

    uint64_t start = monotonic_msec();
    nipc_client_abort(&client);
    check("snapshot call thread exited", pthread_join(call_tid, NULL) == 0);
    uint64_t elapsed = monotonic_msec() - start;
    check("aborted call returns aborted", call_ctx.err == NIPC_ERR_ABORTED);
    check("abort returns promptly", elapsed < 1000u);

    __atomic_store_n(&g_blocking_handler_release, 1, __ATOMIC_RELEASE);
    nipc_client_close(&client);
    stop_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Typed lookup calls                                        */
/* ------------------------------------------------------------------ */

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
    uint32_t calls;
    uint32_t max_items_seen;
    int mixed_generation;
    int slow_second_call;
    int signal_second_call;
    int second_call_entered;
} lookup_scale_state_t;

static void lookup_scale_note_items(lookup_scale_state_t *state,
                                    uint32_t item_count)
{
    uint32_t current = __atomic_load_n(&state->max_items_seen, __ATOMIC_ACQUIRE);
    while (item_count > current &&
           !__atomic_compare_exchange_n(&state->max_items_seen, &current,
                                        item_count, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
    }
}

static void lookup_scale_after_call(lookup_scale_state_t *state, uint32_t call)
{
    if (call != 2)
        return;

    if (state->signal_second_call)
        __atomic_store_n(&state->second_call_entered, 1, __ATOMIC_RELEASE);
    if (state->slow_second_call)
        usleep(250000);
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

static void *lookup_call_thread_fn(void *arg)
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

    return NULL;
}

static int lookup_scale_wait_for_second_call(lookup_scale_state_t *state)
{
    for (int i = 0; i < 5000; i++) {
        if (__atomic_load_n(&state->second_call_entered,
                            __ATOMIC_ACQUIRE) != 0)
            return 1;
        usleep(500);
    }

    return 0;
}

static bool cgroups_lookup_scale_handler(void *user,
                                          const nipc_cgroups_lookup_req_view_t *request,
                                          nipc_cgroups_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    uint32_t call = __atomic_add_fetch(&state->calls, 1, __ATOMIC_ACQ_REL);
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
    uint32_t call = __atomic_add_fetch(&state->calls, 1, __ATOMIC_ACQ_REL);
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
    (void)request;

    return nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
	               "/wrong", 6, "wrong", 5, NULL, 0) == NIPC_OK;
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

static bool apps_lookup_wrong_echo_handler(void *user,
                                           const nipc_apps_lookup_req_view_t *request,
                                           nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    nipc_apps_lookup_req_item_t req_item;
    if (request->item_count == 0 ||
        nipc_apps_lookup_req_item(request, 0, &req_item) != NIPC_OK)
        return false;

    return nipc_apps_lookup_builder_add(
	               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, req_item.pid + 1u,
	               0, NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK;
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

static bool cgroups_lookup_short_response_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return true;

    nipc_cgroups_lookup_req_item_t req_item;
    if (nipc_cgroups_lookup_req_item(request, 0, &req_item) != NIPC_OK)
        return false;

    return nipc_cgroups_lookup_builder_add(
               builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
               req_item.path.ptr, req_item.path.len, "ok", 2, NULL, 0) == NIPC_OK;
}

static bool apps_lookup_short_response_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    (void)user;

    if (request->item_count == 0)
        return true;

    nipc_apps_lookup_req_item_t req_item;
    if (nipc_apps_lookup_req_item(request, 0, &req_item) != NIPC_OK)
        return false;

    return nipc_apps_lookup_builder_add(
               builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, req_item.pid, 0,
               NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0) == NIPC_OK;
}

static bool cgroups_lookup_slow_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    usleep(150000);
    return cgroups_lookup_test_handler(user, request, builder);
}

static bool apps_lookup_slow_handler(
        void *user,
        const nipc_apps_lookup_req_view_t *request,
        nipc_apps_lookup_builder_t *builder)
{
    usleep(150000);
    return apps_lookup_test_handler(user, request, builder);
}

static nipc_error_t raw_lookup_bad_payload_handler(
        void *user,
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

    if (response_buf_size < 8)
        return NIPC_ERR_OVERFLOW;
    memset(response_buf, 0, 8);
    *response_len_out = 8;
    return NIPC_OK;
}

static nipc_error_t raw_cgroups_lookup_short_response_handler(
        void *user,
        const nipc_header_t *request_hdr,
        const uint8_t *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_cgroups_lookup_req_view_t request;
    nipc_error_t err =
        nipc_cgroups_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_lookup_builder_t builder;
    nipc_cgroups_lookup_builder_init(&builder, response_buf, response_buf_size,
                                     request.item_count > 0 ? 1 : 0, 77);
    if (request.item_count > 0) {
        nipc_cgroups_lookup_req_item_t item;
        err = nipc_cgroups_lookup_req_item(&request, 0, &item);
        if (err != NIPC_OK)
            return err;
        err = nipc_cgroups_lookup_builder_add(
            &builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
            item.path.ptr, item.path.len, "ok", 2, NULL, 0);
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_cgroups_lookup_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

static nipc_error_t raw_apps_lookup_short_response_handler(
        void *user,
        const nipc_header_t *request_hdr,
        const uint8_t *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_apps_lookup_req_view_t request;
    nipc_error_t err =
        nipc_apps_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;

    nipc_apps_lookup_builder_t builder;
    nipc_apps_lookup_builder_init(&builder, response_buf, response_buf_size,
                                  request.item_count > 0 ? 1 : 0, 88);
    if (request.item_count > 0) {
        nipc_apps_lookup_req_item_t item;
        err = nipc_apps_lookup_req_item(&request, 0, &item);
        if (err != NIPC_OK)
            return err;
        err = nipc_apps_lookup_builder_add(
            &builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, item.pid, 0,
            NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0);
        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_apps_lookup_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

static nipc_error_t build_one_cgroups_raw_item(
        uint16_t status,
        nipc_str_view_t path,
        uint8_t *dst,
        size_t dst_size,
        uint32_t *len_out)
{
    uint8_t tmp[512];
    nipc_cgroups_lookup_builder_t builder;
    nipc_cgroups_lookup_builder_init(&builder, tmp, sizeof(tmp), 1, 1);
    nipc_error_t err = nipc_cgroups_lookup_builder_add(
        &builder, status, status == NIPC_CGROUP_LOOKUP_KNOWN ?
        NIPC_ORCHESTRATOR_K8S : 0, path.ptr, path.len,
        status == NIPC_CGROUP_LOOKUP_KNOWN ? "ok" : "",
        status == NIPC_CGROUP_LOOKUP_KNOWN ? 2 : 0, NULL, 0);
    if (err != NIPC_OK)
        return err;

    size_t encoded = nipc_cgroups_lookup_builder_finish(&builder);
    nipc_cgroups_lookup_resp_view_t view;
    err = nipc_cgroups_lookup_resp_decode(tmp, encoded, &view);
    if (err != NIPC_OK)
        return err;

    const uint8_t *raw;
    uint32_t raw_len;
    err = nipc_cgroups_lookup_resp_raw_item(&view, 0, &raw, &raw_len);
    if (err != NIPC_OK)
        return err;
    if (raw_len > dst_size)
        return NIPC_ERR_OVERFLOW;
    memcpy(dst, raw, raw_len);
    *len_out = raw_len;
    return NIPC_OK;
}

static nipc_error_t build_one_apps_raw_item(
        uint16_t status,
        uint32_t pid,
        uint8_t *dst,
        size_t dst_size,
        uint32_t *len_out)
{
    uint8_t tmp[512];
    nipc_apps_lookup_builder_t builder;
    nipc_apps_lookup_builder_init(&builder, tmp, sizeof(tmp), 1, 1);
    nipc_error_t err = nipc_apps_lookup_builder_add(
        &builder, status,
        status == NIPC_PID_LOOKUP_KNOWN ? NIPC_APPS_CGROUP_KNOWN : 0,
        status == NIPC_PID_LOOKUP_KNOWN ? NIPC_ORCHESTRATOR_DOCKER : 0,
        pid, status == NIPC_PID_LOOKUP_KNOWN ? 1 : 0,
        status == NIPC_PID_LOOKUP_KNOWN ? 1000 : NIPC_UID_UNSET,
        status == NIPC_PID_LOOKUP_KNOWN ? 42 : 0,
        status == NIPC_PID_LOOKUP_KNOWN ? "ok" : "",
        status == NIPC_PID_LOOKUP_KNOWN ? 2 : 0,
        status == NIPC_PID_LOOKUP_KNOWN ? "/ok" : "",
        status == NIPC_PID_LOOKUP_KNOWN ? 3 : 0,
        status == NIPC_PID_LOOKUP_KNOWN ? "name" : "",
        status == NIPC_PID_LOOKUP_KNOWN ? 4 : 0, NULL, 0);
    if (err != NIPC_OK)
        return err;

    size_t encoded = nipc_apps_lookup_builder_finish(&builder);
    nipc_apps_lookup_resp_view_t view;
    err = nipc_apps_lookup_resp_decode(tmp, encoded, &view);
    if (err != NIPC_OK)
        return err;

    const uint8_t *raw;
    uint32_t raw_len;
    err = nipc_apps_lookup_resp_raw_item(&view, 0, &raw, &raw_len);
    if (err != NIPC_OK)
        return err;
    if (raw_len > dst_size)
        return NIPC_ERR_OVERFLOW;
    memcpy(dst, raw, raw_len);
    *len_out = raw_len;
    return NIPC_OK;
}

static nipc_error_t raw_cgroups_lookup_payload_gap_handler(
        void *user,
        const nipc_header_t *request_hdr,
        const uint8_t *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_cgroups_lookup_req_view_t request;
    nipc_error_t err =
        nipc_cgroups_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;
    if (request.item_count != 3)
        return NIPC_ERR_BAD_ITEM_COUNT;

    uint8_t raw_storage[3][256];
    const uint8_t *items[3];
    uint32_t item_lens[3];
    for (uint32_t i = 0; i < 3; i++) {
        nipc_cgroups_lookup_req_item_t item;
        err = nipc_cgroups_lookup_req_item(&request, i, &item);
        if (err != NIPC_OK)
            return err;
        uint16_t status = (i == 1) ? NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED :
                                     NIPC_CGROUP_LOOKUP_KNOWN;
        err = build_one_cgroups_raw_item(status, item.path, raw_storage[i],
                                         sizeof(raw_storage[i]), &item_lens[i]);
        if (err != NIPC_OK)
            return err;
        items[i] = raw_storage[i];
    }

    return nipc_cgroups_lookup_raw_resp_encode(
        items, item_lens, 3, 77, response_buf, response_buf_size,
        response_len_out);
}

static nipc_error_t raw_apps_lookup_payload_gap_handler(
        void *user,
        const nipc_header_t *request_hdr,
        const uint8_t *request_payload,
        size_t request_len,
        uint8_t *response_buf,
        size_t response_buf_size,
        size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_apps_lookup_req_view_t request;
    nipc_error_t err =
        nipc_apps_lookup_req_decode(request_payload, request_len, &request);
    if (err != NIPC_OK)
        return err;
    if (request.item_count != 3)
        return NIPC_ERR_BAD_ITEM_COUNT;

    uint8_t raw_storage[3][256];
    const uint8_t *items[3];
    uint32_t item_lens[3];
    for (uint32_t i = 0; i < 3; i++) {
        nipc_apps_lookup_req_item_t item;
        err = nipc_apps_lookup_req_item(&request, i, &item);
        if (err != NIPC_OK)
            return err;
        uint16_t status = (i == 1) ? NIPC_PID_LOOKUP_PAYLOAD_EXCEEDED :
                                     NIPC_PID_LOOKUP_KNOWN;
        err = build_one_apps_raw_item(status, item.pid, raw_storage[i],
                                      sizeof(raw_storage[i]), &item_lens[i]);
        if (err != NIPC_OK)
            return err;
        items[i] = raw_storage[i];
    }

    return nipc_apps_lookup_raw_resp_encode(
        items, item_lens, 3, 88, response_buf, response_buf_size,
        response_len_out);
}

static bool cgroups_lookup_malformed_followup_handler(
        void *user,
        const nipc_cgroups_lookup_req_view_t *request,
        nipc_cgroups_lookup_builder_t *builder)
{
    lookup_scale_state_t *state = (lookup_scale_state_t *)user;
    uint32_t call = __atomic_add_fetch(&state->calls, 1, __ATOMIC_ACQ_REL);

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
    uint32_t call = __atomic_add_fetch(&state->calls, 1, __ATOMIC_ACQ_REL);

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

static void test_cgroups_lookup_call(void)
{
    printf("Test 3a: Typed cgroups lookup call\n");
    const char *svc = "svc_cgroups_lookup";
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
    sctx.cgroups_handler.user = NULL;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups lookup server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups lookup client ready", nipc_client_ready(&client));

    nipc_str_view_t paths[] = {
        { .ptr = "/known", .len = 6 },
        { .ptr = "/missing", .len = 8 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
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
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_apps_lookup_call(void)
{
    printf("Test 3b: Typed apps lookup call\n");
    const char *svc = "svc_apps_lookup";
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.apps_handler.handle = apps_lookup_test_handler;
    sctx.apps_handler.user = NULL;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps lookup server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps lookup client ready", nipc_client_ready(&client));

    uint32_t pids[] = {1234, 0, 9999};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
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
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_lookup_zero_item_calls(void)
{
    printf("Test 3b.1: Typed lookup zero-item calls\n");

    {
        const char *svc = "svc_cgroups_lookup_zero";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        sctx.cgroups_handler.user = NULL;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("zero cgroups lookup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("zero cgroups lookup client ready", nipc_client_ready(&client));

        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup(&client, NULL, 0, &view);
        check("zero cgroups lookup call ok", err == NIPC_OK);
        if (err == NIPC_OK)
            check("zero cgroups lookup item_count == 0", view.item_count == 0);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_zero";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        sctx.apps_handler.user = NULL;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("zero apps lookup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("zero apps lookup client ready", nipc_client_ready(&client));

        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup(&client, NULL, 0, &view);
        check("zero apps lookup call ok", err == NIPC_OK);
        if (err == NIPC_OK)
            check("zero apps lookup item_count == 0", view.item_count == 0);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_cgroups_lookup_payload_exceeded_retry(void)
{
    printf("Test 3c: Cgroups lookup transparent PAYLOAD_EXCEEDED retry\n");
    const char *svc = "svc_cgroups_lookup_scale";
    cleanup_all(svc);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_response_payload_bytes = 160;
    sctx.has_config = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
    sctx.cgroups_handler.user = &state;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups scale server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups scale client ready", nipc_client_ready(&client));

    nipc_str_view_t paths[] = {
        { .ptr = "/a", .len = 2 },
        { .ptr = "/huge", .len = 5 },
        { .ptr = "/b", .len = 2 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
    check("cgroups scale call ok", err == NIPC_OK);
    check("cgroups scale used follow-up call",
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

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
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_apps_lookup_payload_exceeded_retry(void)
{
    printf("Test 3d: Apps lookup transparent PAYLOAD_EXCEEDED retry\n");
    const char *svc = "svc_apps_lookup_scale";
    cleanup_all(svc);

    lookup_scale_state_t state = {0};
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_response_payload_bytes = 320;
    sctx.has_config = 1;
    sctx.apps_handler.handle = apps_lookup_scale_handler;
    sctx.apps_handler.user = &state;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps scale server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps scale client ready", nipc_client_ready(&client));

    uint32_t pids[] = {11, 22, 33};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
    check("apps scale call ok", err == NIPC_OK);
    check("apps scale used follow-up call",
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

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
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void run_cgroups_lookup_request_boundary_case(
    const char *svc, uint32_t request_cap, uint32_t expected_max_items,
    uint32_t min_calls, const char *label)
{
    cleanup_all(svc);

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_request_payload_bytes = request_cap;
    sctx.config.max_response_payload_bytes = 4096;
    sctx.has_config = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
    sctx.cgroups_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups split server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = request_cap;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups split client ready", nipc_client_ready(&client));

    nipc_str_view_t paths[] = {
        { .ptr = "/bbbbbb", .len = 7 },
        { .ptr = "/aaaaaa", .len = 7 },
        { .ptr = "/bbbbbb", .len = 7 },
        { .ptr = "/cccccc", .len = 7 },
        { .ptr = "/aaaaaa", .len = 7 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_cgroups_lookup(&client, paths, 5, &view);
    check("cgroups split call ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups split item_count == 5", view.item_count == 5);
        check("cgroups split generation == 7", view.generation == 7);
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
    check(msg, __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= min_calls);
    snprintf(msg, sizeof(msg), "cgroups %s max fragment", label);
    check(msg, __atomic_load_n(&state.max_items_seen, __ATOMIC_ACQUIRE) ==
                   expected_max_items);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void run_apps_lookup_request_boundary_case(
    const char *svc, uint32_t request_cap, uint32_t expected_max_items,
    uint32_t min_calls, const char *label)
{
    cleanup_all(svc);

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_request_payload_bytes = request_cap;
    sctx.config.max_response_payload_bytes = 4096;
    sctx.has_config = 1;
    sctx.apps_handler.handle = apps_lookup_scale_handler;
    sctx.apps_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps split server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = request_cap;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps split client ready", nipc_client_ready(&client));

    uint32_t pids[] = { 4, 1, 4, 7, 1, 9, 7 };
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 7, &view);
    check("apps split call ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("apps split item_count == 7", view.item_count == 7);
        check("apps split generation == 9", view.generation == 9);
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
    check(msg, __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= min_calls);
    snprintf(msg, sizeof(msg), "apps %s max fragment", label);
    check(msg, __atomic_load_n(&state.max_items_seen, __ATOMIC_ACQUIRE) ==
                   expected_max_items);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_lookup_proactive_request_split(void)
{
    printf("Test 3e: Lookup proactive request splitting\n");

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

static void test_apps_lookup_large_logical_case(const char *svc,
                                                uint32_t item_count)
{
    cleanup_all(svc);

    uint32_t *pids = calloc(item_count, sizeof(*pids));
    check("apps large allocate pids", pids != NULL);
    if (!pids)
        return;

    for (uint32_t i = 0; i < item_count; i++)
        pids[i] = 100000u + i;

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    sctx.config.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    sctx.has_config = 1;
    sctx.apps_handler.handle = apps_lookup_scale_handler;
    sctx.apps_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps large server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps large client ready", nipc_client_ready(&client));

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
        check("apps large used multiple requests",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) > 1);
        check("apps large fragmented below total item count",
              __atomic_load_n(&state.max_items_seen, __ATOMIC_ACQUIRE) <
                  item_count);
    }

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
    free(pids);
}

static void test_cgroups_lookup_large_logical_case(const char *svc,
                                                   uint32_t item_count)
{
    cleanup_all(svc);

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

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    sctx.config.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    sctx.has_config = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
    sctx.cgroups_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups large server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups large client ready", nipc_client_ready(&client));

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
        check("cgroups large used multiple requests",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) > 1);
        check("cgroups large fragmented below total item count",
              __atomic_load_n(&state.max_items_seen, __ATOMIC_ACQUIRE) <
                  item_count);
    }

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
    free(path_storage);
    free(paths);
}

static void test_lookup_large_logical_calls(void)
{
    printf("Test 3e2: Lookup large logical calls\n");

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
    printf("Test 3f: Cgroups lookup oversized request key\n");
    const char *svc = "svc_cgroups_lookup_oversized_request_key";
    cleanup_all(svc);

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_request_payload_bytes = 48;
    sctx.config.max_response_payload_bytes = 4096;
    sctx.has_config = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
    sctx.cgroups_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups oversized request-key server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = 48;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups oversized request-key client ready", nipc_client_ready(&client));

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
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 1);
    check("cgroups oversized request-key server saw one item",
          __atomic_load_n(&state.max_items_seen, __ATOMIC_ACQUIRE) == 1);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_lookup_logical_limits(void)
{
    printf("Test 3g: Lookup logical limits\n");

    const char *svc = "svc_lookup_logical_limits";
    cleanup_all(svc);

    lookup_scale_state_t state;
    memset(&state, 0, sizeof(state));
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.has_config = 1;
    sctx.apps_handler.handle = apps_lookup_scale_handler;
    sctx.apps_handler.user = &state;

    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("logical limit server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    ccfg.max_logical_lookup_items = 2;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("logical limit client ready", nipc_client_ready(&client));

    uint32_t pids[] = { 1, 2, 3 };
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
    check("apps logical item limit rejected", err == NIPC_ERR_OVERFLOW);
    check("apps logical item limit did not call handler",
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 0);
    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("apps logical item limit did not reconnect",
          status.reconnect_count == 0);

    nipc_client_close(&client);

    nipc_client_ctx_t cold_client;
    nipc_client_config_t cold_cfg = default_client_config();
    cold_cfg.max_request_payload_bytes = 48;
    nipc_client_init(&cold_client, TEST_RUN_DIR, "svc_lookup_not_ready",
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

    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_cgroups_lookup_rejects_mixed_generation_retry(void)
{
    printf("Test 3e: Cgroups lookup rejects mixed-generation retry\n");
    const char *svc = "svc_cgroups_lookup_mixed_generation";
    cleanup_all(svc);

    lookup_scale_state_t state = {
        .mixed_generation = 1,
    };
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_response_payload_bytes = 160;
    sctx.has_config = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
    sctx.cgroups_handler.user = &state;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("mixed-generation cgroups server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("mixed-generation cgroups client ready", nipc_client_ready(&client));

    nipc_str_view_t paths[] = {
        { .ptr = "/a", .len = 2 },
        { .ptr = "/huge", .len = 5 },
        { .ptr = "/b", .len = 2 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
    check("mixed-generation cgroups call rejected", err != NIPC_OK);
    check("mixed-generation cgroups used follow-up call",
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_apps_lookup_rejects_mixed_generation_retry(void)
{
    printf("Test 3f: Apps lookup rejects mixed-generation retry\n");
    const char *svc = "svc_apps_lookup_mixed_generation";
    cleanup_all(svc);

    lookup_scale_state_t state = {
        .mixed_generation = 1,
    };
    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_service_server_config();
    sctx.config.max_response_payload_bytes = 320;
    sctx.has_config = 1;
    sctx.apps_handler.handle = apps_lookup_scale_handler;
    sctx.apps_handler.user = &state;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("mixed-generation apps server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("mixed-generation apps client ready", nipc_client_ready(&client));

    uint32_t pids[] = {11, 22, 33};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
    check("mixed-generation apps call rejected", err != NIPC_OK);
    check("mixed-generation apps used follow-up call",
          __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_cgroups_lookup_call_shm(void)
{
    printf("Test 3g: Typed cgroups lookup call over SHM/futex\n");
    const char *svc = "svc_cgroups_lookup_shm";
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_shm_service_server_config();
    sctx.has_config = 1;
    sctx.worker_count = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
    sctx.cgroups_handler.user = NULL;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups lookup SHM server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_shm_service_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups lookup SHM client ready",
          nipc_client_ready(&client) &&
          client.shm != NULL &&
          client.session.selected_profile == NIPC_PROFILE_SHM_FUTEX);

    nipc_str_view_t paths[] = {
        { .ptr = "/known", .len = 6 },
        { .ptr = "/missing", .len = 8 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
    check("cgroups lookup SHM call ok", err == NIPC_OK);

    if (err == NIPC_OK) {
        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("cgroups lookup SHM has no resize reconnect",
              status.reconnect_count == 0);
        check("cgroups lookup SHM item_count == 2", view.item_count == 2);
        nipc_cgroups_lookup_item_view_t item;
        check("cgroups lookup SHM item 0 decode",
              nipc_cgroups_lookup_resp_item(&view, 0, &item) == NIPC_OK);
        check("cgroups lookup SHM item 0 known",
              item.status == NIPC_CGROUP_LOOKUP_KNOWN &&
              service_str_eq(item.path, "/known") &&
              service_str_eq(item.name, "pod-a"));
        check("cgroups lookup SHM item 1 decode",
              nipc_cgroups_lookup_resp_item(&view, 1, &item) == NIPC_OK);
        check("cgroups lookup SHM item 1 retry",
              item.status == NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER &&
              service_str_eq(item.path, "/missing"));
    }

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_apps_lookup_call_shm(void)
{
    printf("Test 3h: Typed apps lookup call over SHM/futex\n");
    const char *svc = "svc_apps_lookup_shm";
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_shm_service_server_config();
    sctx.has_config = 1;
    sctx.worker_count = 1;
    sctx.apps_handler.handle = apps_lookup_test_handler;
    sctx.apps_handler.user = NULL;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps lookup SHM server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_shm_service_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps lookup SHM client ready",
          nipc_client_ready(&client) &&
          client.shm != NULL &&
          client.session.selected_profile == NIPC_PROFILE_SHM_FUTEX);

    uint32_t pids[] = {1234, 0, 9999};
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
    check("apps lookup SHM call ok", err == NIPC_OK);

    if (err == NIPC_OK) {
        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("apps lookup SHM has no resize reconnect",
              status.reconnect_count == 0);
        check("apps lookup SHM item_count == 3", view.item_count == 3);
        nipc_apps_lookup_item_view_t item;
        check("apps lookup SHM item 0 decode",
              nipc_apps_lookup_resp_item(&view, 0, &item) == NIPC_OK);
        check("apps lookup SHM item 0 known",
              item.status == NIPC_PID_LOOKUP_KNOWN &&
              item.cgroup_status == NIPC_APPS_CGROUP_KNOWN &&
              item.pid == 1234 &&
              service_str_eq(item.comm, "nginx"));
        check("apps lookup SHM item 1 decode",
              nipc_apps_lookup_resp_item(&view, 1, &item) == NIPC_OK);
        check("apps lookup SHM item 1 host root",
              item.pid == 0 &&
              item.cgroup_status == NIPC_APPS_CGROUP_HOST_ROOT);
        check("apps lookup SHM item 2 decode",
              nipc_apps_lookup_resp_item(&view, 2, &item) == NIPC_OK);
        check("apps lookup SHM item 2 unknown",
              item.pid == 9999 &&
              item.status == NIPC_PID_LOOKUP_UNKNOWN);
    }

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_shm_lookup_session_closes_after_client_disconnect(void)
{
    printf("Test 3i: SHM lookup session closes after client disconnect\n");
    const char *svc = "svc_lookup_shm_disconnect";
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.config = default_shm_service_server_config();
    sctx.has_config = 1;
    sctx.worker_count = 1;
    sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
    sctx.cgroups_handler.user = NULL;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("disconnect SHM lookup server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_config_t ccfg = default_shm_service_client_config();
    nipc_client_ctx_t first;
    nipc_client_init(&first, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&first);
    check("first SHM lookup client ready",
          nipc_client_ready(&first) && first.shm != NULL);

    usleep(150000);
    if (nipc_client_ready(&first) && first.session.fd >= 0) {
        char control_byte = 0;
        check("first SHM lookup control fd accepts readable byte",
              send(first.session.fd, &control_byte, sizeof(control_byte), 0) ==
              (ssize_t)sizeof(control_byte));
        usleep(150000);
    }

    nipc_client_close(&first);

    usleep(300000);

    nipc_client_ctx_t second;
    nipc_client_init(&second, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&second);
    check("replacement SHM lookup client ready",
          nipc_client_ready(&second) && second.shm != NULL);

    if (nipc_client_ready(&second) && second.shm != NULL) {
        nipc_str_view_t paths[] = {
            { .ptr = "/known", .len = 6 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&second, paths, 1, &view);
        check("replacement SHM lookup call succeeds", err == NIPC_OK);
        if (err == NIPC_OK)
            check("replacement SHM lookup item_count == 1", view.item_count == 1);
    }

    nipc_client_close(&second);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_lookup_init_guards(void)
{
    printf("Test 3j: Typed lookup server init guards\n");

    nipc_managed_server_t server;
    nipc_server_config_t scfg = default_service_server_config();

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

    const char *c_svc = "svc_lookup_init_cgroups";
    cleanup_all(c_svc);
    nipc_cgroups_lookup_service_handler_t c_handler = {
        .handle = cgroups_lookup_test_handler,
        .user = NULL,
    };
    nipc_server_config_t zero_payload_cfg = scfg;
    zero_payload_cfg.max_response_payload_bytes = 0;
    nipc_error_t err = nipc_server_init_cgroups_lookup(
        &server, TEST_RUN_DIR, c_svc, &zero_payload_cfg, 0, &c_handler);
    check("cgroups lookup init defaults ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups lookup worker floor",
              server.worker_count == 1 &&
              server.expected_method_code == NIPC_METHOD_CGROUPS_LOOKUP);
        nipc_server_destroy(&server);
    }
    cleanup_all(c_svc);

    const char *a_svc = "svc_lookup_init_apps";
    cleanup_all(a_svc);
    nipc_apps_lookup_service_handler_t a_handler = {
        .handle = apps_lookup_test_handler,
        .user = NULL,
    };
    err = nipc_server_init_apps_lookup(
        &server, TEST_RUN_DIR, a_svc, &zero_payload_cfg, 0, &a_handler);
    check("apps lookup init defaults ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("apps lookup worker floor",
              server.worker_count == 1 &&
              server.expected_method_code == NIPC_METHOD_APPS_LOOKUP);
        nipc_server_destroy(&server);
    }
    cleanup_all(a_svc);

    const char *null_cfg_svc = "svc_lookup_init_null_cfg";
    cleanup_all(null_cfg_svc);
    err = nipc_server_init_cgroups_lookup(
        &server, TEST_RUN_DIR, null_cfg_svc, NULL, 1, &c_handler);
    check("cgroups lookup init accepts null config defaults", err == NIPC_OK);
    if (err == NIPC_OK)
        nipc_server_destroy(&server);
    cleanup_all(null_cfg_svc);

    err = nipc_server_init_cgroups_lookup(
        &server, "/tmp/nipc_lookup_missing_dir_99999",
        "svc_lookup_cgroups_bad_dir", &scfg, 1, &c_handler);
    check("cgroups lookup init reports listen failure", err != NIPC_OK);

    err = nipc_server_init_apps_lookup(
        &server, "/tmp/nipc_lookup_missing_dir_99999",
        "svc_lookup_apps_bad_dir", &scfg, 1, &a_handler);
    check("apps lookup init reports listen failure", err != NIPC_OK);
}

static void test_lookup_client_invalid_requests(void)
{
    printf("Test 3k: Typed lookup client invalid requests\n");

    {
        const char *svc = "svc_cgroups_lookup_bad_req";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        sctx.cgroups_handler.user = NULL;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("bad cgroups lookup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("bad cgroups lookup client ready", nipc_client_ready(&client));

        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, NULL, 1, &view);
        check("null cgroups lookup path array rejected", err == NIPC_ERR_BAD_LAYOUT);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("second bad cgroups lookup client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = NULL, .len = 1 },
        };
        err = nipc_client_call_cgroups_lookup(&client, paths, 1, &view);
        check("bad cgroups lookup request rejected", err == NIPC_ERR_BAD_LAYOUT);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("faulted cgroups lookup client ready", nipc_client_ready(&client));

        nipc_str_view_t valid_paths[] = {
            { .ptr = "/known", .len = 6 },
        };
        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        err = nipc_client_call_cgroups_lookup(&client, valid_paths, 1, &view);
        nipc_posix_service_test_fault_clear();
        check("cgroups lookup send buffer fault rejected", err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("resize cgroups lookup client ready", nipc_client_ready(&client));

        client.session.max_request_payload_bytes = 1;
        err = nipc_client_call_cgroups_lookup(&client, valid_paths, 1, &view);
        check("cgroups lookup request preflight resize recovers", err == NIPC_OK);
        if (err == NIPC_OK) {
            nipc_client_status_t status;
            nipc_client_status(&client, &status);
            check("cgroups lookup request preflight reconnects",
                  status.reconnect_count >= 1);
            check("cgroups lookup request preflight learns size",
                  client.transport_config.max_request_payload_bytes >= 32);
        }

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_bad_req";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        sctx.apps_handler.user = NULL;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("bad apps lookup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("bad apps lookup client ready", nipc_client_ready(&client));

        nipc_apps_lookup_resp_view_t view;
        uint32_t pids[] = {1234};
        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_SEND_BUF_REALLOC, 0);
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 1, &view);
        nipc_posix_service_test_fault_clear();
        check("apps lookup send buffer fault rejected", err == NIPC_ERR_OVERFLOW);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("second bad apps lookup client ready", nipc_client_ready(&client));

        err = nipc_client_call_apps_lookup(&client, NULL, 1, &view);
        check("bad apps lookup request rejected", err == NIPC_ERR_BAD_LAYOUT);
        nipc_client_close(&client);

        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("resize apps lookup client ready", nipc_client_ready(&client));

        client.session.max_request_payload_bytes = 1;
        err = nipc_client_call_apps_lookup(&client, pids, 1, &view);
        check("apps lookup request preflight resize recovers", err == NIPC_OK);
        if (err == NIPC_OK) {
            nipc_client_status_t status;
            nipc_client_status(&client, &status);
            check("apps lookup request preflight reconnects",
                  status.reconnect_count >= 1);
            check("apps lookup request preflight learns size",
                  client.transport_config.max_request_payload_bytes >= 32);
        }

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void run_cgroups_lookup_bad_response_case_posix(
        const char *suffix,
        nipc_cgroups_lookup_handler_fn handler,
        nipc_error_t expected_err)
{
    char svc[128];
    snprintf(svc, sizeof(svc), "svc_cgroups_lookup_%s", suffix);
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.cgroups_handler.handle = handler;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
    check("cgroups bad-response server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("cgroups bad-response client ready", nipc_client_ready(&client));

    nipc_str_view_t paths[] = {
        { .ptr = "/a", .len = 2 },
        { .ptr = "/b", .len = 2 },
    };
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);

    char label[160];
    snprintf(label, sizeof(label), "cgroups lookup rejects %s response", suffix);
    check(label, err == expected_err);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void run_apps_lookup_bad_response_case_posix(
        const char *suffix,
        nipc_apps_lookup_handler_fn handler,
        nipc_error_t expected_err)
{
    char svc[128];
    snprintf(svc, sizeof(svc), "svc_apps_lookup_%s", suffix);
    cleanup_all(svc);

    lookup_server_thread_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.apps_handler.handle = handler;
    pthread_t tid;
    start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
    check("apps bad-response server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("apps bad-response client ready", nipc_client_ready(&client));

    uint32_t pids[] = { 77, 78 };
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);

    char label[160];
    snprintf(label, sizeof(label), "apps lookup rejects %s response", suffix);
    check(label, err == expected_err);

    nipc_client_close(&client);
    stop_lookup_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_lookup_rejects_bad_server_responses(void)
{
    printf("Test 3l: Typed lookup rejects bad server responses\n");

    {
        const char *svc = "svc_cgroups_lookup_bad_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_payload_exceeded_first_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("bad cgroups response server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("bad cgroups response client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        check("cgroups lookup rejects first payload-exceeded item",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_bad_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_payload_exceeded_first_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("bad apps response server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("bad apps response client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 10, 11 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects first payload-exceeded item",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_wrong_echo";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_wrong_echo_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("wrong-echo cgroups server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("wrong-echo cgroups client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/expected", .len = 9 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1, &view);
        check("cgroups lookup rejects wrong echoed key", err == NIPC_ERR_BAD_LAYOUT);

	        nipc_client_close(&client);
	        stop_lookup_server(&sctx, tid);
	        cleanup_all(svc);
	    }

    {
        const char *svc = "svc_cgroups_lookup_reordered_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_reordered_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("reordered cgroups server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("reordered cgroups client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        check("cgroups lookup rejects reordered response items",
              err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_duplicate_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_duplicate_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("duplicate cgroups server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("duplicate cgroups client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        check("cgroups lookup rejects duplicate response items",
              err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    run_cgroups_lookup_bad_response_case_posix(
        "invalid_status", cgroups_lookup_invalid_status_handler,
        NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case_posix(
        "invalid_status_fields", cgroups_lookup_invalid_status_fields_handler,
        NIPC_ERR_BAD_LAYOUT);
    run_cgroups_lookup_bad_response_case_posix(
        "invalid_label_table", cgroups_lookup_invalid_label_table_handler,
        NIPC_ERR_OUT_OF_BOUNDS);

	    {
	        const char *svc = "svc_cgroups_lookup_short_response";
	        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_short_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("short-response cgroups server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("short-response cgroups client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2, &view);
        check("cgroups lookup rejects short response", err != NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_wrong_echo";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_wrong_echo_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("wrong-echo apps server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("wrong-echo apps client ready", nipc_client_ready(&client));

        uint32_t pid = 77;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup rejects wrong echoed key", err == NIPC_ERR_BAD_LAYOUT);

	        nipc_client_close(&client);
	        stop_lookup_server(&sctx, tid);
	        cleanup_all(svc);
	    }

    {
        const char *svc = "svc_apps_lookup_reordered_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_reordered_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("reordered apps server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("reordered apps client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 77, 78 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects reordered response items",
              err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_duplicate_response";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_duplicate_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("duplicate apps server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("duplicate apps client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 77, 78 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects duplicate response items",
              err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    run_apps_lookup_bad_response_case_posix(
        "invalid_status", apps_lookup_invalid_status_handler,
        NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case_posix(
        "invalid_status_fields", apps_lookup_invalid_status_fields_handler,
        NIPC_ERR_BAD_LAYOUT);
    run_apps_lookup_bad_response_case_posix(
        "invalid_label_table", apps_lookup_invalid_label_table_handler,
        NIPC_ERR_OUT_OF_BOUNDS);

	    {
	        const char *svc = "svc_apps_lookup_short_response";
	        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_short_response_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("short-response apps server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("short-response apps client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 77, 78 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects short response", err != NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_lookup_logical_ceiling_failures(void)
{
    printf("Test 3m: Typed lookup logical ceiling failures\n");

    {
        const char *svc = "svc_cgroups_lookup_response_limit";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups response-limit server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_logical_lookup_response_bytes = 32;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups response-limit client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/known", .len = 6 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1, &view);
        check("cgroups logical response limit rejected", err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_response_limit";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps response-limit server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_logical_lookup_response_bytes = 64;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps response-limit client ready", nipc_client_ready(&client));

        uint32_t pid = 1234;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps logical response limit rejected", err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_subcall_limit";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 160;
        sctx.has_config = 1;
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups subcall-limit server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_logical_lookup_subcalls = 1;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups subcall-limit client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/huge", .len = 5 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
        check("cgroups logical subcall limit rejected", err == NIPC_ERR_OVERFLOW);
        check("cgroups subcall limit stopped after first server call",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 1);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_subcall_limit";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 320;
        sctx.has_config = 1;
        sctx.apps_handler.handle = apps_lookup_scale_handler;
        sctx.apps_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps subcall-limit server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_logical_lookup_subcalls = 1;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps subcall-limit client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
        check("apps logical subcall limit rejected", err == NIPC_ERR_OVERFLOW);
        check("apps subcall limit stopped after first server call",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 1);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

}

static void test_lookup_rejects_malformed_followup_response(void)
{
    printf("Test 3l2: Typed lookup rejects malformed follow-up response\n");

    {
        const char *svc = "svc_cgroups_lookup_bad_followup";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_malformed_followup_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups bad-followup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups bad-followup client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
            { .ptr = "/c", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup(&client, paths, 3, &view);
        check("cgroups lookup rejects malformed follow-up",
              err == NIPC_ERR_BAD_LAYOUT);
        check("cgroups malformed follow-up used second call",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_bad_followup";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_malformed_followup_handler;
        sctx.apps_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps bad-followup server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps bad-followup client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3,
                                                        &view);
        check("apps lookup rejects malformed follow-up",
              err == NIPC_ERR_BAD_LAYOUT);
        check("apps malformed follow-up used second call",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_lookup_endpoint_gone_after_partial_progress(void)
{
    printf("Test 3l3: Typed lookup fails when endpoint disappears after partial progress\n");

    {
        const char *svc = "svc_apps_lookup_partial_disconnect";
        raw_lookup_partial_disconnect_ctx_t rctx;
        pthread_t tid;
        start_raw_lookup_partial_disconnect(&rctx, svc, LOOKUP_SERVER_APPS,
                                            &tid);
        check("apps partial-disconnect raw server started",
              __atomic_load_n(&rctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps partial-disconnect client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 3, &view, 1000);
        check("apps lookup fails after endpoint disappears",
              err != NIPC_OK);

        nipc_client_close(&client);
        pthread_join(tid, NULL);
        check("apps partial-disconnect raw server done",
              __atomic_load_n(&rctx.done, __ATOMIC_ACQUIRE) == 1);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_partial_disconnect";
        raw_lookup_partial_disconnect_ctx_t rctx;
        pthread_t tid;
        start_raw_lookup_partial_disconnect(&rctx, svc, LOOKUP_SERVER_CGROUPS,
                                            &tid);
        check("cgroups partial-disconnect raw server started",
              __atomic_load_n(&rctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups partial-disconnect client ready",
              nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
            { .ptr = "/c", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup_timeout(
            &client, paths, 3, &view, 1000);
        check("cgroups lookup fails after endpoint disappears",
              err != NIPC_OK);

        nipc_client_close(&client);
        pthread_join(tid, NULL);
        check("cgroups partial-disconnect raw server done",
              __atomic_load_n(&rctx.done, __ATOMIC_ACQUIRE) == 1);
        cleanup_all(svc);
    }
}

static void test_lookup_endpoint_gone_before_first_subcall(void)
{
    printf("Test 3l4: Typed lookup fails when endpoint disappears before first subcall\n");

    {
        const char *svc = "svc_apps_lookup_gone_before_call";
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps before-call server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps before-call client ready", nipc_client_ready(&client));

        stop_lookup_server(&sctx, tid);

        uint32_t pids[] = { 11, 22 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 2, &view, 1000);
        check("apps lookup fails after endpoint disappears before call",
              err != NIPC_OK);

        nipc_client_close(&client);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_gone_before_call";
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups before-call server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups before-call client ready", nipc_client_ready(&client));

        stop_lookup_server(&sctx, tid);

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
        cleanup_all(svc);
    }
}

static void test_lookup_endpoint_absent_before_call(void)
{
    printf("Test 3l5: Typed lookup fails when endpoint is absent before call\n");

    {
        const char *svc = "svc_apps_lookup_absent";
        cleanup_all(svc);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

        bool changed = nipc_client_refresh(&client);
        check("apps absent refresh changed state", changed);
        check("apps absent client is NOT_FOUND",
              client.state == NIPC_CLIENT_NOT_FOUND);

        uint32_t pids[] = { 11, 22 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 2, &view, 1000);
        check("apps lookup absent endpoint rejected",
              err == NIPC_ERR_NOT_READY);

        nipc_client_close(&client);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_absent";
        cleanup_all(svc);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

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
        cleanup_all(svc);
    }
}

static void test_lookup_timeout_cap_and_raw_guards(void)
{
    printf("Test 3n: Typed lookup timeout, cap fallback, and raw guards\n");

    {
        const char *svc = "svc_cgroups_lookup_timeout";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_slow_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups timeout server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups timeout client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/known", .len = 6 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_cgroups_lookup_timeout(&client, &path, 1, &view, 20);
        check("cgroups lookup timeout rejected", err == NIPC_ERR_TIMEOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_timeout";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_slow_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps timeout server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps timeout client ready", nipc_client_ready(&client));

        uint32_t pid = 1234;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, &pid, 1, &view, 20);
        check("apps lookup timeout rejected", err == NIPC_ERR_TIMEOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_default_cap";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups default-cap server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups default-cap client ready", nipc_client_ready(&client));
        client.session.max_request_payload_bytes = 0;
        client.transport_config.max_request_payload_bytes = 0;

        nipc_str_view_t path = { .ptr = "/known", .len = 6 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup falls back to default request cap", err == NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_default_cap";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps default-cap server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps default-cap client ready", nipc_client_ready(&client));
        client.session.max_request_payload_bytes = 0;
        client.transport_config.max_request_payload_bytes = 0;

        uint32_t pid = 1234;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup falls back to default request cap", err == NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_item_limit";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups item-limit server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_logical_lookup_items = 1;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups item-limit client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2,
                                                           &view);
        check("cgroups logical item limit rejected", err == NIPC_ERR_OVERFLOW);
        check("cgroups item limit did not call handler",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 0);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_response_buf_fault";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_request_payload_bytes = 64;
        sctx.config.max_response_payload_bytes = 256;
        sctx.has_config = 1;
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups response-buffer-fault server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        ccfg.max_request_payload_bytes = 64;
        ccfg.max_response_payload_bytes = 256;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups response-buffer-fault client ready", nipc_client_ready(&client));

        client.response_buf_size = 512;

        char path_storage[80][16];
        nipc_str_view_t paths[80];
        for (uint32_t i = 0; i < 80; i++) {
            int n = snprintf(path_storage[i], sizeof(path_storage[i]), "/cg-%02u", i);
            paths[i].ptr = path_storage[i];
            paths[i].len = (uint32_t)n;
        }

        nipc_posix_service_test_fault_set(
            NIPC_POSIX_SERVICE_TEST_FAULT_CLIENT_RESPONSE_BUF_REALLOC, 0);
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 80,
                                                           &view);
        nipc_posix_service_test_fault_clear();
        check("cgroups final response buffer fault rejected",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_request_cap_fail";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps request-cap fail server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps request-cap fail client ready", nipc_client_ready(&client));
        client.session.max_request_payload_bytes = NIPC_APPS_LOOKUP_REQ_HDR_SIZE;
        client.transport_config.max_request_payload_bytes = 0;

        uint32_t pid = 1234;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup request cap failure rejected", err == NIPC_ERR_OVERFLOW);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_bad_payload";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_CGROUPS_LOOKUP,
                                raw_lookup_bad_payload_handler, &tid);
        check("raw cgroups bad-payload server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw cgroups bad-payload client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/known", .len = 6 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup rejects malformed raw payload", err != NIPC_OK);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_bad_payload";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_APPS_LOOKUP,
                                raw_lookup_bad_payload_handler, &tid);
        check("raw apps bad-payload server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw apps bad-payload client ready", nipc_client_ready(&client));

        uint32_t pid = 1234;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup rejects malformed raw payload", err != NIPC_OK);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_raw_short";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_CGROUPS_LOOKUP,
                                raw_cgroups_lookup_short_response_handler, &tid);
        check("raw cgroups short-response server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw cgroups short-response client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 2,
                                                           &view);
        check("cgroups lookup rejects raw short response", err != NIPC_OK);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_raw_short";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_APPS_LOOKUP,
                                raw_apps_lookup_short_response_handler, &tid);
        check("raw apps short-response server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw apps short-response client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 2, &view);
        check("apps lookup rejects raw short response", err != NIPC_OK);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_payload_gap";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_CGROUPS_LOOKUP,
                                raw_cgroups_lookup_payload_gap_handler, &tid);
        check("raw cgroups payload-gap server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw cgroups payload-gap client ready", nipc_client_ready(&client));

        nipc_str_view_t paths[] = {
            { .ptr = "/a", .len = 2 },
            { .ptr = "/b", .len = 2 },
            { .ptr = "/c", .len = 2 },
        };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, paths, 3,
                                                           &view);
        check("cgroups lookup rejects payload-exceeded gap", err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_payload_gap";
        cleanup_all(svc);

        raw_method_server_thread_ctx_t sctx;
        pthread_t tid;
        start_raw_method_server(&sctx, svc, NIPC_METHOD_APPS_LOOKUP,
                                raw_apps_lookup_payload_gap_handler, &tid);
        check("raw apps payload-gap server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("raw apps payload-gap client ready", nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, 3, &view);
        check("apps lookup rejects payload-exceeded gap", err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_raw_method_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_null_path_encode";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups null-path server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups null-path client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = NULL, .len = 2 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup rejects null path at encode", err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_null_oversized";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups null-oversized server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups null-oversized client ready", nipc_client_ready(&client));
        client.session.max_request_payload_bytes = NIPC_CGROUPS_LOOKUP_REQ_HDR_SIZE;
        client.transport_config.max_request_payload_bytes = 0;

        nipc_str_view_t path = { .ptr = NULL, .len = 2 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup rejects null oversized path", err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_bad_oversized_key";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_test_handler;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups bad-oversized-key server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups bad-oversized-key client ready", nipc_client_ready(&client));
        client.session.max_request_payload_bytes = NIPC_CGROUPS_LOOKUP_REQ_HDR_SIZE;
        client.transport_config.max_request_payload_bytes = 0;

        char bad_path[] = { '/', 'b', '\0', 'd' };
        nipc_str_view_t path = { .ptr = bad_path, .len = sizeof(bad_path) };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup rejects malformed oversized key",
              err == NIPC_ERR_BAD_LAYOUT);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_lookup_timeout_during_followup_subcall(void)
{
    printf("Test 3n2: Typed lookup timeout during follow-up subcall\n");

    {
        const char *svc = "svc_cgroups_lookup_followup_timeout";
        cleanup_all(svc);

        lookup_scale_state_t state = {
            .slow_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 160;
        sctx.has_config = 1;
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups follow-up timeout server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups follow-up timeout client ready",
              nipc_client_ready(&client));

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
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_followup_timeout";
        cleanup_all(svc);

        lookup_scale_state_t state = {
            .slow_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 320;
        sctx.has_config = 1;
        sctx.apps_handler.handle = apps_lookup_scale_handler;
        sctx.apps_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps follow-up timeout server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps follow-up timeout client ready",
              nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err =
            nipc_client_call_apps_lookup_timeout(&client, pids, 3, &view, 75);
        check("apps follow-up timeout rejected", err == NIPC_ERR_TIMEOUT);
        check("apps follow-up timeout reached second subcall",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_lookup_abort_during_followup_subcall(void)
{
    printf("Test 3n3: Typed lookup abort during follow-up subcall\n");

    {
        const char *svc = "svc_cgroups_lookup_followup_abort";
        cleanup_all(svc);

        lookup_scale_state_t state = {
            .slow_second_call = 1,
            .signal_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 160;
        sctx.has_config = 1;
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups follow-up abort server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups follow-up abort client ready",
              nipc_client_ready(&client));

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

        pthread_t call_tid;
        int thread_created =
            pthread_create(&call_tid, NULL, lookup_call_thread_fn,
                           &call_ctx) == 0;
        check("cgroups follow-up abort call thread created", thread_created);
        if (thread_created) {
            check("cgroups follow-up abort reached second subcall",
                  lookup_scale_wait_for_second_call(&state));
            nipc_client_abort(&client);
            pthread_join(call_tid, NULL);
            check("cgroups follow-up abort rejected",
                  call_ctx.err == NIPC_ERR_ABORTED);
            check("cgroups follow-up abort used follow-up call",
                  __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);
        }

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_followup_abort";
        cleanup_all(svc);

        lookup_scale_state_t state = {
            .slow_second_call = 1,
            .signal_second_call = 1,
        };
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.config = default_service_server_config();
        sctx.config.max_response_payload_bytes = 320;
        sctx.has_config = 1;
        sctx.apps_handler.handle = apps_lookup_scale_handler;
        sctx.apps_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps follow-up abort server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps follow-up abort client ready",
              nipc_client_ready(&client));

        uint32_t pids[] = { 11, 22, 33 };
        lookup_call_thread_ctx_t call_ctx;
        memset(&call_ctx, 0, sizeof(call_ctx));
        call_ctx.client = &client;
        call_ctx.kind = LOOKUP_SERVER_APPS;
        call_ctx.pids = pids;
        call_ctx.pid_count = 3;
        call_ctx.timeout_ms = 5000;

        pthread_t call_tid;
        int thread_created =
            pthread_create(&call_tid, NULL, lookup_call_thread_fn,
                           &call_ctx) == 0;
        check("apps follow-up abort call thread created", thread_created);
        if (thread_created) {
            check("apps follow-up abort reached second subcall",
                  lookup_scale_wait_for_second_call(&state));
            nipc_client_abort(&client);
            pthread_join(call_tid, NULL);
            check("apps follow-up abort rejected",
                  call_ctx.err == NIPC_ERR_ABORTED);
            check("apps follow-up abort used follow-up call",
                  __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) >= 2);
        }

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

static void test_lookup_abort_and_handler_failure(void)
{
    printf("Test 3o: Typed lookup abort and handler failure\n");

    {
        const char *svc = "svc_apps_lookup_abort";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.apps_handler.handle = apps_lookup_scale_handler;
        sctx.apps_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps abort server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps abort client ready", nipc_client_ready(&client));

        uint32_t pid = 1;
        nipc_apps_lookup_resp_view_t view;
        nipc_client_abort(&client);
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup abort fails before request", err == NIPC_ERR_ABORTED);
        check("apps lookup abort did not call handler",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 0);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_cgroups_lookup_abort";
        cleanup_all(svc);

        lookup_scale_state_t state = {0};
        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.cgroups_handler.handle = cgroups_lookup_scale_handler;
        sctx.cgroups_handler.user = &state;
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups abort server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups abort client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/a", .len = 2 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_client_abort(&client);
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup abort fails before request", err == NIPC_ERR_ABORTED);
        check("cgroups lookup abort did not call handler",
              __atomic_load_n(&state.calls, __ATOMIC_ACQUIRE) == 0);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_not_ready";
        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

        uint32_t pid = 1;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1, &view);
        check("apps lookup not-ready client rejected", err == NIPC_ERR_NOT_READY);
        nipc_client_close(&client);
    }

    {
        const char *svc = "svc_cgroups_lookup_no_handler";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_CGROUPS, &tid);
        check("cgroups no-handler server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("cgroups no-handler client ready", nipc_client_ready(&client));

        nipc_str_view_t path = { .ptr = "/known", .len = 6 };
        nipc_cgroups_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_lookup(&client, &path, 1,
                                                           &view);
        check("cgroups lookup no-handler rejected", err != NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }

    {
        const char *svc = "svc_apps_lookup_no_handler";
        cleanup_all(svc);

        lookup_server_thread_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        pthread_t tid;
        start_lookup_server(&sctx, svc, LOOKUP_SERVER_APPS, &tid);
        check("apps no-handler server started",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
        nipc_client_refresh(&client);
        check("apps no-handler client ready", nipc_client_ready(&client));

        uint32_t pid = 1;
        nipc_apps_lookup_resp_view_t view;
        nipc_error_t err = nipc_client_call_apps_lookup(&client, &pid, 1,
                                                        &view);
        check("apps lookup no-handler rejected", err != NIPC_OK);

        nipc_client_close(&client);
        stop_lookup_server(&sctx, tid);
        cleanup_all(svc);
    }
}

/* ------------------------------------------------------------------ */
/*  Test 4: Retry on failure                                           */
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
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready (1st connect)", nipc_client_ready(&client));

    /* First call succeeds */
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
     * L2 must detect this and reconnect before retrying. */
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
    nipc_client_config_t ccfg = default_client_config();

    nipc_client_init(&client1, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client1);
    check("client 1 ready", nipc_client_ready(&client1));

    /* Make a call from client 1 */
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
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Make a call - handler fails, so we get an error.
     * The client will also attempt a retry (at-least-once) since
     * it was previously READY, but the handler will fail again. */
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
    nipc_client_config_t ccfg = default_client_config();
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

    nipc_client_config_t ccfg = default_client_config();
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
        4, NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL);
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
                                         &scfg, 2,
                                         NIPC_METHOD_CGROUPS_SNAPSHOT,
                                         test_cgroups_handler, NULL);
    check("server init", err == NIPC_OK);

    pthread_t server_tid;
    pthread_create(&server_tid, NULL,
                   (void *(*)(void *))nipc_server_run, &server);
    usleep(50000);

    /* Connect via raw UDS session */
    nipc_uds_client_config_t ccfg = default_transport_client_config();
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
    nipc_client_config_t verify_cfg = default_client_config();
    nipc_client_init(&verify_client, TEST_RUN_DIR, svc, &verify_cfg);
    nipc_client_refresh(&verify_client);
    check("server still alive after bad client",
          nipc_client_ready(&verify_client));

    if (nipc_client_ready(&verify_client)) {
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
/*  SHM-mode snapshot service coverage                                 */
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
        2, NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL);

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
        2, NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL);

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

typedef struct {
    const char *service;
    int ready;
    int done;
    uint32_t first_selected_profile;
    uint32_t second_selected_profile;
    nipc_uds_error_t first_receive_err;
    nipc_uds_error_t second_receive_err;
} fake_shm_attach_fallback_ctx_t;

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

static void *fake_shm_attach_fallback_server_thread_fn(void *arg)
{
    fake_shm_attach_fallback_ctx_t *ctx = (fake_shm_attach_fallback_ctx_t *)arg;
    nipc_uds_listener_t listener;
    nipc_uds_session_t first;
    nipc_uds_session_t second;
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
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    listener.fd = -1;
    first.fd = -1;
    second.fd = -1;
    ctx->first_receive_err = NIPC_UDS_ERR_BAD_PARAM;
    ctx->second_receive_err = NIPC_UDS_ERR_BAD_PARAM;

    if (nipc_uds_listen(TEST_RUN_DIR, ctx->service, &scfg, &listener) != NIPC_UDS_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    if (accept_with_timeout(&listener, 1, 30000, &first) != NIPC_UDS_OK)
        goto out;
    ctx->first_selected_profile = first.selected_profile;

    {
        uint8_t recv_buf[4096];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        ctx->first_receive_err = nipc_uds_receive(
            &first, recv_buf, sizeof(recv_buf), &hdr, &payload, &payload_len);
    }
    nipc_uds_close_session(&first);
    first.fd = -1;

    if (accept_with_timeout(&listener, 2, 30000, &second) != NIPC_UDS_OK)
        goto out;
    ctx->second_selected_profile = second.selected_profile;

    {
        uint8_t recv_buf[4096];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        ctx->second_receive_err = nipc_uds_receive(
            &second, recv_buf, sizeof(recv_buf), &hdr, &payload, &payload_len);
    }

out:
    if (first.fd != -1)
        nipc_uds_close_session(&first);
    if (second.fd != -1)
        nipc_uds_close_session(&second);
    if (listener.fd != -1)
        nipc_uds_close_listener(&listener);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_shm_l2_service(void)
{
    printf("Test: SHM-mode snapshot service\n");
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
    nipc_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("SHM client ready", nipc_client_ready(&client));

    /* CGROUPS_SNAPSHOT call */
    nipc_cgroups_resp_view_t cg_view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &cg_view);
    check("cgroups_snapshot ok", err == NIPC_OK);
    if (err == NIPC_OK) {
        check("cgroups item_count == 3", cg_view.item_count == 3);
        check("cgroups generation == 42", cg_view.generation == 42);
        check("cgroups systemd_enabled == 1", cg_view.systemd_enabled == 1);
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
        nipc_client_config_t ccfg = {
            .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
            .max_request_batch_items = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .auth_token = AUTH_TOKEN,
        };
        nipc_client_ctx_t client;
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
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
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
        nipc_client_config_t ccfg = {
            .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
            .max_request_batch_items = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .auth_token = AUTH_TOKEN,
        };
        nipc_client_ctx_t bad_client;
        nipc_client_ctx_t good_client;
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
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&good_client, &view);
            snprintf(msg, sizeof(msg), "%s: replacement snapshot succeeds", cases[i].label);
            check(msg, err == NIPC_OK && view.item_count == 3);
        }

        nipc_client_close(&good_client);
        nipc_server_stop(&sctx.server);
        pthread_join(tid, NULL);
        cleanup_all(cases[i].service);
        cleanup_session_shm(cases[i].service, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Coverage: SHM resize + retry paths                                 */
/* ------------------------------------------------------------------ */

static void test_shm_request_resize_retry(void)
{
    printf("Test: SHM request resize retry until stable\n");

    const char *svc = "svc_shm_request_resize";
    shm_limit_server_ctx_t sctx;
    pthread_t tid;
    nipc_client_config_t ccfg = {
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token = AUTH_TOKEN,
    };
    nipc_client_ctx_t client;

    cleanup_all(svc);
    cleanup_session_shm(svc, 1);

    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;
    sctx.max_request_payload_bytes = 1;
    sctx.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    pthread_create(&tid, NULL, shm_limit_server_thread_fn, &sctx);

    for (int spin = 0;
         spin < 2000 &&
         !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
         spin++) {
        usleep(500);
    }

    check("request resize: server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    client.transport_config.max_request_payload_bytes = 1;
    nipc_client_refresh(&client);
    check("request resize: client negotiated SHM",
          nipc_client_ready(&client) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("request resize: explicit cap returns overflow",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("request resize: error_count >= 1", status.error_count >= 1);
        check("request resize: learned request size stays capped",
              client.session.max_request_payload_bytes <= 1);
        check("request resize: client leaves READY", !nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_all(svc);
    cleanup_session_shm(svc, 1);
}

static void test_shm_response_resize_retry(void)
{
    printf("Test: SHM response resize retry until stable\n");

    const char *svc = "svc_shm_response_resize";
    shm_limit_server_ctx_t sctx;
    pthread_t tid;
    nipc_client_config_t ccfg = {
        .supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles = NIPC_PROFILE_SHM_HYBRID,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = 32,
        .auth_token = AUTH_TOKEN,
    };
    nipc_client_ctx_t client;

    cleanup_all(svc);
    cleanup_session_shm(svc, 1);

    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;
    sctx.max_request_payload_bytes = 4096;
    sctx.max_response_payload_bytes = 32;
    pthread_create(&tid, NULL, shm_limit_server_thread_fn, &sctx);

    for (int spin = 0;
         spin < 2000 &&
         !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE);
         spin++) {
        usleep(500);
    }

    check("response resize: server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("response resize: client negotiated SHM",
          nipc_client_ready(&client) && client.shm != NULL);

    if (nipc_client_ready(&client) && client.shm != NULL) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("response resize: explicit cap returns overflow",
              err == NIPC_ERR_OVERFLOW);

        nipc_client_status_t status;
        nipc_client_status(&client, &status);
        check("response resize: reconnect_count >= 1", status.reconnect_count >= 1);
        check("response resize: learned response size stays capped",
              client.session.max_response_payload_bytes <= 32);
        check("response resize: client leaves READY", !nipc_client_ready(&client));
    }

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_all(svc);
    cleanup_session_shm(svc, 1);
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
    nipc_client_config_t ccfg = default_client_config();
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
static nipc_error_t garbage_handler(void *user,
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

    /* Write garbage that won't decode as a valid cgroups response */
    if (response_buf_size < 16)
        return NIPC_ERR_OVERFLOW;

    memset(response_buf, 0xFF, 16);
    *response_len_out = 16;
    return NIPC_OK;
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
    nipc_client_config_t ccfg = default_client_config();
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
    nipc_client_config_t ccfg = default_client_config();
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
    nipc_client_config_t ccfg = default_client_config();
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

static void test_client_protocol_version_incompatible(void)
{
    printf("Test: Client protocol version mismatch maps to INCOMPATIBLE\n");
    const char *svc = "svc_proto_incompat";
    cleanup_all(svc);

    raw_hello_ack_server_ctx_t sctx;
    pthread_t tid;
    start_raw_hello_ack_version_server(&sctx, svc, &tid);
    check("raw hello_ack server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    bool changed = nipc_client_refresh(&client);
    check("refresh changed state", changed);
    check("state is INCOMPATIBLE after protocol mismatch",
          client.state == NIPC_CLIENT_INCOMPATIBLE);
    check("client not ready after protocol mismatch", !nipc_client_ready(&client));

    pthread_join(tid, NULL);
    check("raw hello_ack server accepted exactly one client",
          __atomic_load_n(&sctx.accepted, __ATOMIC_ACQUIRE) == 1);

    changed = nipc_client_refresh(&client);
    check("refresh from INCOMPATIBLE is no-op", !changed);
    check("state remains INCOMPATIBLE",
          client.state == NIPC_CLIENT_INCOMPATIBLE);

    nipc_client_close(&client);
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
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    /* Kill server */
    stop_server(&sctx, tid);
    cleanup_all(svc);
    usleep(50000);

    /* Make a call - should fail and put client in BROKEN state */
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
/*  Coverage: server rejects wrong request kind                         */
/* ------------------------------------------------------------------ */

static void test_server_rejects_wrong_request_kind(void)
{
    printf("Test: Server rejects wrong request kind\n");
    const char *svc = "svc_wrong_kind";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_session_t session;
    memset(&session, 0, sizeof(session));
    session.fd = -1;

    nipc_uds_client_config_t ccfg = default_transport_client_config();
    nipc_uds_error_t uerr = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("raw connect", uerr == NIPC_UDS_OK);

    if (uerr == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .flags = 0,
            .item_count = 1,
            .message_id = 1,
            .transport_status = NIPC_STATUS_OK,
        };
        uint8_t recv_buf[256];
        nipc_header_t resp_hdr = {0};
        const void *payload = NULL;
        size_t payload_len = 0;

        check("send wrong-kind request",
              nipc_uds_send(&session, &hdr, NULL, 0) == NIPC_UDS_OK);
        check("receive wrong-kind response",
              nipc_uds_receive(&session, recv_buf, sizeof(recv_buf),
                               &resp_hdr, &payload, &payload_len) == NIPC_UDS_OK);
        check("wrong-kind status unsupported",
              resp_hdr.transport_status == NIPC_STATUS_UNSUPPORTED);
        check("wrong-kind empty payload", payload_len == 0);

        nipc_uds_close_session(&session);
    }

    stop_server(&sctx, tid);
    cleanup_all(svc);
}

static void test_server_closes_after_terminal_error_response(void)
{
    printf("Test: Server closes after terminal error response\n");
    const char *svc = "svc_terminal_error_close";
    cleanup_all(svc);

    server_thread_ctx_t sctx;
    pthread_t tid;
    start_server(&sctx, svc, test_cgroups_handler, &tid);
    check("terminal close server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_session_t bad_session;
    memset(&bad_session, 0, sizeof(bad_session));
    bad_session.fd = -1;

    nipc_uds_client_config_t raw_cfg = default_transport_client_config();
    nipc_uds_error_t uerr = nipc_uds_connect(TEST_RUN_DIR, svc, &raw_cfg, &bad_session);
    check("terminal close raw connect", uerr == NIPC_UDS_OK);

    if (uerr == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_CGROUPS_SNAPSHOT,
            .flags = 0,
            .item_count = 1,
            .message_id = 1,
            .transport_status = NIPC_STATUS_OK,
        };
        uint8_t bad_payload = 0;
        uint8_t recv_buf[256];
        nipc_header_t resp_hdr = {0};
        const void *payload = NULL;
        size_t payload_len = 0;

        check("terminal close send bad request",
              nipc_uds_send(&bad_session, &hdr, &bad_payload, sizeof(bad_payload)) ==
              NIPC_UDS_OK);
        check("terminal close receive bad-envelope response",
              nipc_uds_receive(&bad_session, recv_buf, sizeof(recv_buf),
                               &resp_hdr, &payload, &payload_len) == NIPC_UDS_OK);
        check("terminal close status bad-envelope",
              resp_hdr.transport_status == NIPC_STATUS_BAD_ENVELOPE);
        check("terminal close response empty", payload_len == 0);
    }

    usleep(300000);

    nipc_client_ctx_t good_client;
    nipc_client_config_t good_cfg = default_client_config();
    nipc_client_init(&good_client, TEST_RUN_DIR, svc, &good_cfg);
    nipc_client_refresh(&good_client);
    check("terminal close replacement client ready",
          nipc_client_ready(&good_client));
    if (nipc_client_ready(&good_client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&good_client, &view);
        check("terminal close replacement call succeeds", err == NIPC_OK);
        if (err == NIPC_OK)
            check("terminal close replacement item_count == 3", view.item_count == 3);
    }

    nipc_client_close(&good_client);
    if (bad_session.fd != -1)
        nipc_uds_close_session(&bad_session);
    stop_server(&sctx, tid);
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
                           NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL)
              != NIPC_OK);

    /* NULL service_name */
    check("null service_name",
          nipc_server_init(&server, TEST_RUN_DIR, NULL, &scfg, 1,
                           NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL)
              != NIPC_OK);

    /* NULL raw config */
    check("null raw config",
          nipc_server_init(&server, TEST_RUN_DIR, "svc", NULL, 1,
                           NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL)
              == NIPC_ERR_BAD_LAYOUT);

    /* NULL handler */
    check("null handler",
          nipc_server_init(&server, TEST_RUN_DIR, "svc", &scfg, 1,
                           NIPC_METHOD_CGROUPS_SNAPSHOT, NULL, NULL)
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
                           &scfg, 1, NIPC_METHOD_CGROUPS_SNAPSHOT,
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
        4, NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL);
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
    /* With fast service callbacks and 3s timeout, drain should succeed */
    check("drain completed", drained);

    for (int i = 0; i < 2; i++)
        pthread_join(ctids[i], NULL);
    pthread_join(server_tid, NULL);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: cache with empty snapshot, linear scan fallback           */
/* ------------------------------------------------------------------ */

static nipc_error_t empty_cgroups_handler(void *user,
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

    /* Return an empty snapshot (0 items) */
    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               0, 0, 77);
    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return NIPC_OK;
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
        1, NIPC_METHOD_CGROUPS_SNAPSHOT, empty_cgroups_handler, NULL);
    check("server init", serr == NIPC_OK);

    pthread_create(&tid, NULL, (void *(*)(void *))nipc_server_run, &sctx.server);
    usleep(50000);

    /* Create cache and refresh */
    nipc_cgroups_cache_t cache;
    nipc_client_config_t ccfg = default_client_config();
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
    nipc_client_config_t ccfg = default_client_config();
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
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    /* Client is DISCONNECTED, call should fail immediately */
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("call on DISCONNECTED fails", err == NIPC_ERR_NOT_READY);

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
                                          &scfg, 1, NIPC_METHOD_CGROUPS_SNAPSHOT,
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
                                        &scfg, 0, NIPC_METHOD_CGROUPS_SNAPSHOT,
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
                           &scfg, 1, NIPC_METHOD_CGROUPS_SNAPSHOT,
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
    nipc_client_config_t zero_cfg = {0};

    nipc_client_init(&client, TEST_RUN_DIR, "svc_client_defaults", &zero_cfg);
    check("default request payload size",
          client.transport_config.max_request_payload_bytes == NIPC_MAX_PAYLOAD_DEFAULT);
    check("default response payload size",
          client.transport_config.max_response_payload_bytes == 65536u);
    check("default response buffer size",
          client.response_buf_size == 0u);
    check("default send buffer size", client.send_buf_size == 0u);
    check("buffers allocated lazily",
          client.response_buf == NULL && client.send_buf == NULL);
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

    nipc_client_init(&client, TEST_RUN_DIR, "svc_client_null_config", NULL);
    check("null client config request default",
          client.transport_config.max_request_payload_bytes == NIPC_MAX_PAYLOAD_DEFAULT);
    check("null client config response default",
          client.transport_config.max_response_payload_bytes == 65536u);
    nipc_client_close(&client);
}

/* ------------------------------------------------------------------ */
/*  Coverage: negotiated SHM obstruction rejects the session            */
/* ------------------------------------------------------------------ */

static void test_shm_negotiation_falls_back_to_baseline_on_obstructed_region(void)
{
    printf("Test: Negotiated SHM obstruction falls back to baseline\n");
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

    nipc_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    bool changed = nipc_client_refresh(&client);
    check("refresh reports READY transition", changed);
    check("client is ready after obstructed SHM negotiation",
          nipc_client_ready(&client));
    check("client state falls back to READY baseline",
          client.state == NIPC_CLIENT_READY);
    check("failed SHM negotiation keeps baseline session",
          client.session_valid && client.shm == NULL);
    check("selected profile falls back to baseline",
          client.session.selected_profile == NIPC_PROFILE_BASELINE);

    nipc_client_close(&client);
    nipc_server_stop(&sctx.server);
    pthread_join(tid, NULL);
    cleanup_session_shm(svc, 1);
    cleanup_all(svc);
}

static void test_client_shm_attach_failure_falls_back_to_baseline(void)
{
    printf("Test: Client-side SHM attach failure falls back to baseline\n");
    const char *svc = "svc_shm_attach_fallback";
    cleanup_all(svc);

    fake_shm_attach_fallback_ctx_t sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.service = svc;

    pthread_t tid;
    pthread_create(&tid, NULL, fake_shm_attach_fallback_server_thread_fn, &sctx);
    for (int i = 0; i < 2000
         && !__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
    check("attach-fallback server started",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles         = NIPC_PROFILE_SHM_HYBRID,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);
    bool changed = nipc_client_refresh(&client);
    check("refresh reports READY transition after attach failure fallback", changed);
    check("client is ready after attach failure fallback", nipc_client_ready(&client));
    check("client state is READY after attach failure fallback",
          client.state == NIPC_CLIENT_READY);
    check("fallback session keeps no SHM attachment",
          client.session_valid && client.shm == NULL);
    check("fallback selected profile is baseline",
          client.session.selected_profile == NIPC_PROFILE_BASELINE);
    check("fallback strips SHM from future supported profiles",
          (client.transport_config.supported_profiles &
           (NIPC_PROFILE_SHM_HYBRID | NIPC_PROFILE_SHM_FUTEX)) == 0);
    check("fallback strips SHM from future preferred profiles",
          (client.transport_config.preferred_profiles &
           (NIPC_PROFILE_SHM_HYBRID | NIPC_PROFILE_SHM_FUTEX)) == 0);

    nipc_client_close(&client);
    pthread_join(tid, NULL);

    check("first negotiated profile was SHM hybrid",
          sctx.first_selected_profile == NIPC_PROFILE_SHM_HYBRID);
    check("second negotiated profile was baseline",
          sctx.second_selected_profile == NIPC_PROFILE_BASELINE);
    check("first session disconnected after SHM attach failure",
          sctx.first_receive_err != NIPC_UDS_OK);
    check("second baseline session closed after client shutdown",
          sctx.second_receive_err != NIPC_UDS_OK);

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
    test_client_call_timeout_on_wedged_peer(); printf("\n");
    test_client_abort_unblocks_call(); printf("\n");
    test_cgroups_lookup_call();    printf("\n");
    test_apps_lookup_call();       printf("\n");
    test_lookup_zero_item_calls(); printf("\n");
    test_cgroups_lookup_payload_exceeded_retry(); printf("\n");
    test_apps_lookup_payload_exceeded_retry(); printf("\n");
    test_lookup_proactive_request_split(); printf("\n");
    test_lookup_large_logical_calls(); printf("\n");
    test_cgroups_lookup_oversized_request_key(); printf("\n");
    test_lookup_logical_limits(); printf("\n");
    test_cgroups_lookup_rejects_mixed_generation_retry(); printf("\n");
    test_apps_lookup_rejects_mixed_generation_retry(); printf("\n");
    test_cgroups_lookup_call_shm(); printf("\n");
    test_apps_lookup_call_shm();   printf("\n");
    test_shm_lookup_session_closes_after_client_disconnect(); printf("\n");
    test_lookup_init_guards();     printf("\n");
    test_lookup_client_invalid_requests(); printf("\n");
    test_lookup_rejects_bad_server_responses(); printf("\n");
    test_lookup_rejects_malformed_followup_response(); printf("\n");
    test_lookup_endpoint_gone_after_partial_progress(); printf("\n");
    test_lookup_endpoint_gone_before_first_subcall(); printf("\n");
    test_lookup_endpoint_absent_before_call(); printf("\n");
    test_lookup_logical_ceiling_failures(); printf("\n");
    test_lookup_timeout_cap_and_raw_guards(); printf("\n");
    test_lookup_timeout_during_followup_subcall(); printf("\n");
    test_lookup_abort_during_followup_subcall(); printf("\n");
    test_lookup_abort_and_handler_failure(); printf("\n");
    test_retry_on_failure();       printf("\n");
    test_multiple_clients();       printf("\n");
    test_handler_failure();        printf("\n");
    test_status_reporting();       printf("\n");
    test_graceful_drain();         printf("\n");
    test_non_request_terminates_session(); printf("\n");
    test_shm_l2_service();         printf("\n");
    test_shm_client_rejects_malformed_responses(); printf("\n");
    test_shm_server_rejects_malformed_requests(); printf("\n");
    test_shm_request_resize_retry(); printf("\n");
    test_shm_response_resize_retry(); printf("\n");
    test_cache_refresh_failure_preserves(); printf("\n");
    test_malformed_response_handling(); printf("\n");

    /* Coverage gap tests */
    test_client_auth_failure();         printf("\n");
    test_client_incompatible();         printf("\n");
    test_client_protocol_version_incompatible(); printf("\n");
    test_client_broken_refresh();       printf("\n");
    test_server_rejects_wrong_request_kind(); printf("\n");
    test_server_closes_after_terminal_error_response(); printf("\n");
    test_server_init_null();            printf("\n");
    test_server_init_listen_failure();  printf("\n");
    test_drain_timeout();               printf("\n");
    test_cache_empty_snapshot();        printf("\n");
    test_cache_linear_scan();           printf("\n");
    test_client_call_disconnected();    printf("\n");
    test_server_init_long_strings();    printf("\n");
    test_server_init_worker_floor_and_long_run_dir(); printf("\n");
    test_client_init_defaults_and_truncation(); printf("\n");
    test_shm_negotiation_falls_back_to_baseline_on_obstructed_region(); printf("\n");
    test_client_shm_attach_failure_falls_back_to_baseline(); printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
