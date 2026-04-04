/*
 * interop_service_win.c - L2 cross-language interop binary (Windows).
 *
 * Usage:
 *   interop_service_win server <run_dir> <service_name>
 *     Starts a managed server handling the cgroups-snapshot service kind
 *     only. Prints READY, then serves clients.
 *
 *   interop_service_win client <run_dir> <service_name>
 *     Connects, performs a snapshot call, verifies results, prints PASS/FAIL.
 */

#if defined(_WIN32) || defined(__MSYS__)

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

typedef struct {
    nipc_managed_server_t server;
    volatile LONG request_count;
} test_server_state_t;

static nipc_error_t handle_cgroups(const uint8_t *request_payload, size_t request_len,
                                   uint8_t *response_buf, size_t response_buf_size,
                                   size_t *response_len_out)
{
    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

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
            return err;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/* ------------------------------------------------------------------ */
/*  Snapshot-only raw handler                                           */
/* ------------------------------------------------------------------ */

static nipc_error_t test_handler(void *user,
                                 const nipc_header_t *request_hdr,
                                 const uint8_t *request_payload,
                                 size_t request_len,
                                 uint8_t *response_buf,
                                 size_t response_buf_size,
                                 size_t *response_len_out)
{
    test_server_state_t *state = (test_server_state_t *)user;
    (void)request_hdr;
    nipc_error_t err = handle_cgroups(request_payload, request_len,
                                      response_buf, response_buf_size,
                                      response_len_out);
    if (err == NIPC_OK)
        InterlockedIncrement(&state->request_count);
    return err;
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

static DWORD WINAPI server_thread_main(LPVOID arg)
{
    test_server_state_t *state = (test_server_state_t *)arg;
    nipc_server_run(&state->server);
    return 0;
}

static bool server_has_active_sessions(nipc_managed_server_t *server)
{
    bool active = false;

    EnterCriticalSection(&server->sessions_lock);
    for (int i = 0; i < server->session_count; i++) {
        nipc_session_ctx_t *session = server->sessions[i];
        if (!session)
            continue;
        if (InterlockedCompareExchange((volatile LONG *)&session->active, 0, 0)) {
            active = true;
            break;
        }
    }
    LeaveCriticalSection(&server->sessions_lock);

    return active;
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

    test_server_state_t state;
    memset(&state, 0, sizeof(state));

    nipc_error_t err = nipc_server_init(&state.server, run_dir, service, &scfg,
                                        1, NIPC_METHOD_CGROUPS_SNAPSHOT,
                                        test_handler, &state);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return 1;
    }

    HANDLE server_thread = CreateThread(NULL, 0, server_thread_main, &state, 0, NULL);
    if (!server_thread) {
        fprintf(stderr, "server thread create failed: %lu\n", (unsigned long)GetLastError());
        nipc_server_destroy(&state.server);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    for (;;) {
        DWORD wait_rc = WaitForSingleObject(server_thread, 50);
        if (wait_rc == WAIT_OBJECT_0) {
            fprintf(stderr, "server thread exited before serving a request\n");
            CloseHandle(server_thread);
            nipc_server_destroy(&state.server);
            return 1;
        }
        if (InterlockedCompareExchange(&state.request_count, 0, 0) > 0)
            break;
    }

    while (server_has_active_sessions(&state.server)) {
        DWORD wait_rc = WaitForSingleObject(server_thread, 50);
        if (wait_rc == WAIT_OBJECT_0)
            break;
    }

    nipc_server_stop(&state.server);

    if (WaitForSingleObject(server_thread, 10000) != WAIT_OBJECT_0) {
        fprintf(stderr, "server thread did not stop cleanly\n");
        CloseHandle(server_thread);
        nipc_server_destroy(&state.server);
        return 1;
    }

    CloseHandle(server_thread);
    nipc_server_destroy(&state.server);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Client mode                                                        */
/* ------------------------------------------------------------------ */

static int run_client(const char *run_dir, const char *service)
{
    uint32_t profiles = detect_profiles();
    nipc_client_config_t ccfg = {0};
    ccfg.supported_profiles        = profiles;
    ccfg.max_request_payload_bytes = 4096;
    ccfg.max_request_batch_items   = 16;
    ccfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
    ccfg.max_response_batch_items  = 16;
    ccfg.auth_token                = AUTH_TOKEN;

    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        Sleep(10);
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "client: not ready\n");
        return 1;
    }

    int ok = 1;

    /* --- Test CGROUPS_SNAPSHOT: 3 items --- */
    {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

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
