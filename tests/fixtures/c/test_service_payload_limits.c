#ifndef _WIN32

#include "test_service_limit_helpers.h"

static void test_typed_request_overflow_reconnect_guards(void)
{
    printf("--- Typed request overflow reconnect guards ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_req_overflow");

        server_thread_ctx_t sctx;
        pthread_t tid;
        if (!start_default_server_named(&sctx, service, 4, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed baseline request-overflow client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm == NULL);

        if (nipc_client_ready(&client)) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            client.session.max_request_payload_bytes = 1;
            client.transport_config.max_request_payload_bytes = 1;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed baseline request overflow recovers",
                  err == NIPC_OK && view.item_count == 3);
            nipc_client_status(&client, &status);
            check("typed baseline request overflow reconnects",
                  status.reconnect_count >= 1);
            check("typed baseline request capacity grows",
                  client.transport_config.max_request_payload_bytes >= 16u);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_hybrid_req_overflow");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed hybrid request-overflow client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            client.session.max_request_payload_bytes = 1;
            client.transport_config.max_request_payload_bytes = 1;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed hybrid request overflow recovers",
                  err == NIPC_OK && view.item_count == 3);
            nipc_client_status(&client, &status);
            check("typed hybrid request overflow reconnects",
                  status.reconnect_count >= 1);
            check("typed hybrid request capacity grows",
                  client.transport_config.max_request_payload_bytes >= 16u);
            check("typed hybrid request overflow keeps client ready",
                  nipc_client_ready(&client) && client.shm != NULL);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_hybrid_no_growth");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed hybrid no-growth overflow client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            client.send_buf_size = NIPC_HEADER_LEN + 3;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed hybrid no-growth overflow returns overflow",
                  err == NIPC_ERR_OVERFLOW);
            nipc_client_status(&client, &status);
            check("typed hybrid no-growth overflow is counted as error",
                  status.error_count >= 1);
            check("typed hybrid no-growth overflow breaks client",
                  !nipc_client_ready(&client));
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }
}

static void test_typed_response_overflow_reconnect_guards(void)
{
    printf("--- Typed response overflow reconnect guards ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_resp_overflow");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_server_config();
        scfg.max_response_payload_bytes = 16;
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed baseline response-overflow client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm == NULL);

        if (nipc_client_ready(&client)) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed baseline response overflow recovers",
                  err == NIPC_OK && view.item_count == 3);
            nipc_client_status(&client, &status);
            check("typed baseline response overflow reconnects",
                  status.reconnect_count >= 1);
            check("typed baseline response capacity grows",
                  client.session.max_response_payload_bytes > 16u);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_hybrid_resp_overflow");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        scfg.max_response_payload_bytes = 16;
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed hybrid response-overflow client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed hybrid response overflow recovers",
                  err == NIPC_OK && view.item_count == 3);
            nipc_client_status(&client, &status);
            check("typed hybrid response overflow reconnects",
                  status.reconnect_count >= 1);
            check("typed hybrid response capacity grows",
                  client.session.max_response_payload_bytes > 16u);
            check("typed hybrid response overflow keeps client ready",
                  nipc_client_ready(&client) && client.shm != NULL);
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }
}

static void test_typed_dispatch_overflow_reconnect_guard(void)
{
    printf("--- Typed dispatch overflow reconnect guard ---\n");

    char large_path[1536];
    memset(large_path, 'x', sizeof(large_path) - 1);
    large_path[sizeof(large_path) - 1] = '\0';
    large_service_handler.user = large_path;

    char service[64];
    unique_service(service, sizeof(service), "svc_typed_dispatch_overflow");

    server_thread_ctx_t sctx;
    pthread_t tid;
    nipc_server_config_t scfg = default_typed_server_config();
    scfg.max_response_payload_bytes = 16;
    if (!start_server_named(&sctx, service, 4, &scfg, &large_service_handler, &tid))
        return;

    nipc_client_ctx_t client;
    nipc_client_config_t ccfg = default_client_config();
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    check("typed dispatch-overflow client ready",
          refresh_until_ready(&client, 200, 10000) && client.shm == NULL);

    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view = {0};
        nipc_client_status_t status = {0};

        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
        check("typed dispatch overflow recovers",
              err == NIPC_OK && view.item_count == 1);
        nipc_client_status(&client, &status);
        check("typed dispatch overflow reconnects",
              status.reconnect_count >= 1);
        check("typed dispatch response capacity grows above builder floor",
              client.session.max_response_payload_bytes > 1024u);
    }

    nipc_client_close(&client);
    stop_server_drain(&sctx, tid);
    large_service_handler.user = NULL;
}

int main(void)
{
    printf("=== POSIX Service Payload Limit Tests ===\n\n");
    ensure_run_dir();

    test_typed_request_overflow_reconnect_guards();
    test_typed_response_overflow_reconnect_guards();
    test_typed_dispatch_overflow_reconnect_guard();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("POSIX service payload limit tests skipped (Windows build)\n");
    return 0;
}

#endif
