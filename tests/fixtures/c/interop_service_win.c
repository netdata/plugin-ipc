/*
 * interop_service_win.c - L2 cross-language interop binary (Windows).
 *
 * Usage:
 *   interop_service_win server <run_dir> <service_name>
 *     Starts a managed server with a cgroups handler (3 items),
 *     prints READY, handles 1 client session, then exits.
 *
 *   interop_service_win client <run_dir> <service_name>
 *     Connects, calls snapshot, verifies 3 items, prints PASS/FAIL.
 */

#ifdef _WIN32

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_named_pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE 65536

/* ------------------------------------------------------------------ */
/*  Cgroups handler: 3 test items                                      */
/* ------------------------------------------------------------------ */

static bool test_handler(void *user,
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
            return false;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Server mode                                                        */
/* ------------------------------------------------------------------ */

static int run_server(const char *run_dir, const char *service)
{
    nipc_np_server_config_t scfg = {0};
    scfg.supported_profiles        = NIPC_PROFILE_BASELINE;
    scfg.max_request_payload_bytes = 4096;
    scfg.max_request_batch_items   = 1;
    scfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    scfg.max_response_batch_items  = 1;
    scfg.auth_token                = AUTH_TOKEN;

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init(&server, run_dir, service, &scfg,
                                          1, RESPONSE_BUF_SIZE,
                                          test_handler, NULL);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    nipc_server_run(&server);
    nipc_server_destroy(&server);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Client mode                                                        */
/* ------------------------------------------------------------------ */

static int run_client(const char *run_dir, const char *service)
{
    nipc_np_client_config_t ccfg = {0};
    ccfg.supported_profiles        = NIPC_PROFILE_BASELINE;
    ccfg.max_request_payload_bytes = 4096;
    ccfg.max_request_batch_items   = 1;
    ccfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    ccfg.max_response_batch_items  = 1;
    ccfg.auth_token                = AUTH_TOKEN;

    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    nipc_client_refresh(&client);

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "client: not ready\n");
        return 1;
    }

    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;

    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);

    int ok = 1;
    if (err != NIPC_OK) {
        fprintf(stderr, "client: call failed: %d\n", err);
        ok = 0;
    } else {
        if (view.item_count != 3) {
            fprintf(stderr, "client: expected 3 items, got %u\n", view.item_count);
            ok = 0;
        }
        if (view.systemd_enabled != 1) {
            fprintf(stderr, "client: expected systemd_enabled=1, got %u\n",
                    view.systemd_enabled);
            ok = 0;
        }
        if (view.generation != 42) {
            fprintf(stderr, "client: expected generation=42, got %llu\n",
                    (unsigned long long)view.generation);
            ok = 0;
        }

        nipc_cgroups_item_view_t item;
        nipc_error_t ierr = nipc_cgroups_resp_item(&view, 0, &item);
        if (ierr != NIPC_OK) {
            fprintf(stderr, "client: item 0 decode failed\n");
            ok = 0;
        } else {
            if (item.hash != 1001) {
                fprintf(stderr, "client: item 0 hash: got %u\n", item.hash);
                ok = 0;
            }
            if (item.name.len != strlen("docker-abc123") ||
                memcmp(item.name.ptr, "docker-abc123", item.name.len) != 0) {
                fprintf(stderr, "client: item 0 name mismatch\n");
                ok = 0;
            }
        }
    }

    nipc_client_close(&client);

    if (ok) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL\n");
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server|client> <run_dir> <service_name>\n",
                argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *run_dir = argv[2];
    const char *service = argv[3];

    if (strcmp(mode, "server") == 0) {
        return run_server(run_dir, service);
    } else if (strcmp(mode, "client") == 0) {
        return run_client(run_dir, service);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 1;
    }
}

#else
/* Stub for non-Windows builds */
#include <stdio.h>
int main(void) {
    fprintf(stderr, "Windows-only binary\n");
    return 1;
}
#endif
