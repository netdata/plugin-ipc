/*
 * interop_service_win.c - L2 cross-language interop binary (Windows).
 *
 * Usage:
 *   interop_service_win server <run_dir> <service_name>
 *     Starts a managed server handling INCREMENT, CGROUPS_SNAPSHOT,
 *     and STRING_REVERSE methods. Prints READY, then serves clients.
 *
 *   interop_service_win client <run_dir> <service_name>
 *     Connects, calls all three methods, verifies results, prints PASS/FAIL.
 */

#ifdef _WIN32

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_win_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE 65536

/* ------------------------------------------------------------------ */
/*  Typed business-logic handlers                                      */
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

static bool handle_cgroups(const uint8_t *request_payload, size_t request_len,
                            uint8_t *response_buf, size_t response_buf_size,
                            size_t *response_len_out)
{
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
/*  Multi-method dispatcher                                            */
/* ------------------------------------------------------------------ */

static bool test_handler(void *user,
                          uint16_t method_code,
                          const uint8_t *request_payload,
                          size_t request_len,
                          uint8_t *response_buf,
                          size_t response_buf_size,
                          size_t *response_len_out)
{
    switch (method_code) {
    case NIPC_METHOD_INCREMENT:
        return nipc_dispatch_increment(request_payload, request_len,
                                        response_buf, response_buf_size,
                                        response_len_out, on_increment, user);
    case NIPC_METHOD_CGROUPS_SNAPSHOT:
        return handle_cgroups(request_payload, request_len,
                               response_buf, response_buf_size,
                               response_len_out);
    case NIPC_METHOD_STRING_REVERSE:
        return nipc_dispatch_string_reverse(request_payload, request_len,
                                             response_buf, response_buf_size,
                                             response_len_out, on_string_reverse, user);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/*  Server mode                                                        */
/* ------------------------------------------------------------------ */

/* Profile selection: NIPC_PROFILE env var ("shm" -> SHM_HYBRID|BASELINE,
 * default -> BASELINE only). */
static uint32_t detect_profiles(void)
{
    const char *env = getenv("NIPC_PROFILE");
    if (env && strcmp(env, "shm") == 0)
        return NIPC_WIN_SHM_PROFILE_HYBRID | NIPC_PROFILE_BASELINE;
    return NIPC_PROFILE_BASELINE;
}

static int run_server(const char *run_dir, const char *service)
{
    uint32_t profiles = detect_profiles();
    nipc_np_server_config_t scfg = {0};
    scfg.supported_profiles        = profiles;
    scfg.max_request_payload_bytes = 4096;
    scfg.max_request_batch_items   = 16;
    scfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    scfg.max_response_batch_items  = 16;
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
    uint32_t profiles = detect_profiles();
    nipc_np_client_config_t ccfg = {0};
    ccfg.supported_profiles        = profiles;
    ccfg.max_request_payload_bytes = 4096;
    ccfg.max_request_batch_items   = 16;
    ccfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    ccfg.max_response_batch_items  = 16;
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
    int ok = 1;

    /* --- Test INCREMENT: 42 -> 43 --- */
    {
        uint64_t inc_result = 0;
        nipc_error_t err = nipc_client_call_increment(
            &client, 42, resp_buf, sizeof(resp_buf), &inc_result);
        if (err != NIPC_OK) {
            fprintf(stderr, "client: increment call failed: %d\n", err);
            ok = 0;
        } else if (inc_result != 43) {
            fprintf(stderr, "client: increment expected 43, got %llu\n",
                    (unsigned long long)inc_result);
            ok = 0;
        }
    }

    /* --- Test CGROUPS_SNAPSHOT: 3 items --- */
    {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(
            &client, req_buf, resp_buf, sizeof(resp_buf), &view);

        if (err != NIPC_OK) {
            fprintf(stderr, "client: cgroups call failed: %d\n", err);
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
    }

    /* --- Test INCREMENT batch: [10,20,30] -> [11,21,31] --- */
    {
        uint64_t batch_in[] = { 10, 20, 30 };
        uint64_t batch_out[3] = { 0 };
        nipc_error_t err = nipc_client_call_increment_batch(
            &client, batch_in, 3, batch_out, resp_buf, sizeof(resp_buf));
        if (err != NIPC_OK) {
            fprintf(stderr, "client: increment batch call failed: %d\n", err);
            ok = 0;
        } else {
            uint64_t expected[] = { 11, 21, 31 };
            for (int i = 0; i < 3; i++) {
                if (batch_out[i] != expected[i]) {
                    fprintf(stderr, "client: batch[%d] expected %llu, got %llu\n",
                            i, (unsigned long long)expected[i],
                            (unsigned long long)batch_out[i]);
                    ok = 0;
                }
            }
        }
    }

    /* --- Test STRING_REVERSE: "hello" -> "olleh" --- */
    {
        nipc_string_reverse_view_t sr_view;
        nipc_error_t err = nipc_client_call_string_reverse(
            &client, "hello", 5, resp_buf, sizeof(resp_buf), &sr_view);
        if (err != NIPC_OK) {
            fprintf(stderr, "client: string_reverse call failed: %d\n", err);
            ok = 0;
        } else if (sr_view.str_len != 5 || memcmp(sr_view.str, "olleh", 5) != 0) {
            fprintf(stderr, "client: string_reverse expected \"olleh\", got \"%.*s\"\n",
                    (int)sr_view.str_len, sr_view.str);
            ok = 0;
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
