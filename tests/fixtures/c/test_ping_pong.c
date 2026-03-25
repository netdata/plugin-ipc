/*
 * test_ping_pong.c - L2 typed ping-pong tests for the cgroups-snapshot service.
 *
 * Proves the service model: one endpoint serves one request kind, sessions are
 * long-lived, and repeated typed snapshot calls can vary response size without
 * changing the service contract.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"

#include <pthread.h>
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

#define TEST_RUN_DIR  "/tmp/nipc_pingpong_test"
#define SERVICE_NAME  "cgroups-snapshot-ping-pong"
#define AUTH_TOKEN    0x1234567890ABCDEFull
#define RESPONSE_BUF  65536

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

static void cleanup_socket(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, SERVICE_NAME);
    unlink(path);
}

/* ------------------------------------------------------------------ */
/*  Snapshot service handler                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t calls;
    uint32_t max_items;
    bool empty_snapshot;
} snapshot_service_ctx_t;

static bool snapshot_handler(void *user,
                             const nipc_cgroups_req_t *request,
                             nipc_cgroups_builder_t *builder)
{
    snapshot_service_ctx_t *ctx = (snapshot_service_ctx_t *)user;
    uint32_t round;
    uint32_t item_count;

    if (!ctx || !request)
        return false;

    if (request->layout_version != 1 || request->flags != 0)
        return false;

    round = __atomic_add_fetch(&ctx->calls, 1u, __ATOMIC_RELAXED);
    nipc_cgroups_builder_set_header(builder, 1u, round);

    if (ctx->empty_snapshot)
        return true;

    item_count = 1u + ((round - 1u) % ctx->max_items);
    for (uint32_t i = 0; i < item_count; i++) {
        char name[64];
        char path[128];

        snprintf(name, sizeof(name), "snapshot-%u-item-%u", round, i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/service/%u/%u", round, i);

        if (nipc_cgroups_builder_add(builder,
                                     1000u + round * 16u + i,
                                     i,
                                     1u,
                                     name, (uint32_t)strlen(name),
                                     path, (uint32_t)strlen(path)) != NIPC_OK)
            return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Server thread                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    nipc_managed_server_t server;
    snapshot_service_ctx_t service_ctx;
    bool started;
} server_ctx_t;

static void *server_thread(void *arg)
{
    server_ctx_t *sctx = (server_ctx_t *)arg;
    nipc_server_run(&sctx->server);
    return NULL;
}

static bool start_server(server_ctx_t *sctx)
{
    cleanup_socket();

    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 16,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_cgroups_service_handler_t service_handler = {
        .handle = snapshot_handler,
        .snapshot_max_items = sctx->service_ctx.max_items,
        .user = &sctx->service_ctx,
    };

    nipc_error_t err = nipc_server_init_typed(&sctx->server,
                                              TEST_RUN_DIR, SERVICE_NAME,
                                              &scfg, 4, &service_handler);
    if (err != NIPC_OK)
        return false;

    pthread_t tid;
    if (pthread_create(&tid, NULL, server_thread, sctx) != 0)
        return false;
    pthread_detach(tid);

    usleep(50000); /* 50ms for server to start */
    sctx->started = true;
    return true;
}

static void stop_server(server_ctx_t *sctx)
{
    if (sctx->started) {
        nipc_server_drain(&sctx->server, 2000);
        nipc_server_destroy(&sctx->server);
        sctx->started = false;
    }
    cleanup_socket();
}

/* ------------------------------------------------------------------ */
/*  Test: repeated snapshot calls                                      */
/* ------------------------------------------------------------------ */

static void test_snapshot_ping_pong(void)
{
    printf("\nTest: repeated snapshot calls over one long-lived session\n");

    server_ctx_t sctx = {0};
    sctx.service_ctx.max_items = 4;
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 16,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    int responses_received = 0;
    bool all_ok = true;

    for (int round = 0; round < 10; round++) {
        nipc_cgroups_resp_view_t view;
        nipc_cgroups_item_view_t item0;
        uint32_t expected_generation = (uint32_t)(round + 1);
        uint32_t expected_items = 1u + (round % 4u);
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

        if (err != NIPC_OK) {
            printf("  FAIL: round %d: call error %d\n", round, err);
            all_ok = false;
            break;
        }
        responses_received++;

        if (view.generation != expected_generation) {
            printf("  FAIL: round %d: generation %u != expected %u\n",
                   round, view.generation, expected_generation);
            all_ok = false;
            break;
        }

        if (view.item_count != expected_items) {
            printf("  FAIL: round %d: item_count %u != expected %u\n",
                   round, view.item_count, expected_items);
            all_ok = false;
            break;
        }

        if (nipc_cgroups_resp_item(&view, 0, &item0) != NIPC_OK) {
            printf("  FAIL: round %d: item 0 decode failed\n", round);
            all_ok = false;
            break;
        }

        if (strncmp(item0.name.ptr, "snapshot-", 9) != 0 ||
            strncmp(item0.path.ptr, "/sys/fs/cgroup/service/", 23) != 0) {
            printf("  FAIL: round %d: item 0 content mismatch\n", round);
            all_ok = false;
            break;
        }
    }

    check("every snapshot round succeeds", all_ok);
    check("responses received == 10", responses_received == 10);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Test: session stays reused in steady state                         */
/* ------------------------------------------------------------------ */

static void test_snapshot_session_reuse(void)
{
    printf("\nTest: snapshot service reuses one session in steady state\n");

    server_ctx_t sctx = {0};
    sctx.service_ctx.max_items = 3;
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 16,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    for (int round = 0; round < 6; round++) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("snapshot call ok", err == NIPC_OK);
    }

    nipc_client_status_t status;
    nipc_client_status(&client, &status);
    check("steady state uses one connect", status.connect_count == 1);
    check("steady state avoids reconnects", status.reconnect_count == 0);
    check("steady state call_count == 6", status.call_count == 6);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Test: empty snapshot                                               */
/* ------------------------------------------------------------------ */

static void test_empty_snapshot(void)
{
    printf("\nTest: empty snapshot is valid for the service kind\n");

    server_ctx_t sctx = {0};
    sctx.service_ctx.max_items = 1;
    sctx.service_ctx.empty_snapshot = true;
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 16,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
    check("empty snapshot call ok", err == NIPC_OK);
    check("empty snapshot has 0 items",
          err == NIPC_OK && view.item_count == 0);
    check("empty snapshot keeps systemd_enabled",
          err == NIPC_OK && view.systemd_enabled == 1);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== L2 Ping-Pong Tests (CGROUPS_SNAPSHOT service kind) ===\n");

    ensure_run_dir();

    test_snapshot_ping_pong();
    test_snapshot_session_reuse();
    test_empty_snapshot();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
