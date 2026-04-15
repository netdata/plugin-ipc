#ifndef _WIN32

#include "test_service_limit_helpers.h"

static void test_typed_unsupported_method_response_guard(void)
{
    printf("--- Typed unsupported-method response guard ---\n");

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_unsupported");

        server_thread_ctx_t sctx;
        pthread_t tid;
        if (!start_default_server_named(&sctx, service, 4, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed unsupported-method client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm == NULL);

        if (nipc_client_ready(&client)) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            sctx.server.expected_method_code = NIPC_METHOD_INCREMENT;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed unsupported method maps to bad layout",
                  err == NIPC_ERR_BAD_LAYOUT);
            nipc_client_status(&client, &status);
            check("typed unsupported method counts error",
                  status.error_count >= 1);
            check("typed unsupported method breaks client",
                  !nipc_client_ready(&client));
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }

    {
        char service[64];
        unique_service(service, sizeof(service), "svc_typed_hybrid_unsupported");

        server_thread_ctx_t sctx;
        pthread_t tid;
        nipc_server_config_t scfg = default_typed_hybrid_server_config();
        if (!start_server_named(&sctx, service, 4, &scfg, &full_service_handler, &tid))
            return;

        nipc_client_ctx_t client;
        nipc_client_config_t ccfg = default_typed_hybrid_client_config();
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        check("typed hybrid unsupported-method client ready",
              refresh_until_ready(&client, 200, 10000) && client.shm != NULL);

        if (nipc_client_ready(&client) && client.shm != NULL) {
            nipc_cgroups_resp_view_t view = {0};
            nipc_client_status_t status = {0};

            sctx.server.expected_method_code = NIPC_METHOD_INCREMENT;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("typed hybrid unsupported method maps to bad layout",
                  err == NIPC_ERR_BAD_LAYOUT);
            nipc_client_status(&client, &status);
            check("typed hybrid unsupported method counts error",
                  status.error_count >= 1);
            check("typed hybrid unsupported method breaks client",
                  !nipc_client_ready(&client));
        }

        nipc_client_close(&client);
        stop_server_drain(&sctx, tid);
    }
}

int main(void)
{
    printf("=== POSIX Service Method Limit Tests ===\n\n");
    ensure_run_dir();

    test_typed_unsupported_method_response_guard();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#else

#include <stdio.h>

int main(void)
{
    printf("POSIX service method limit tests skipped (Windows build)\n");
    return 0;
}

#endif
