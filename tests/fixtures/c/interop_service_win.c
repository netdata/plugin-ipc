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
#define LOOKUP_SCALE_ITEMS_DEFAULT 8192u
#define LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES 8192u
#define LOOKUP_SCALE_CALL_TIMEOUT_MS 120000u
#define LOOKUP_SCALE_PATH_BYTES 24u
#define LOOKUP_MIXED_ITEMS 5u

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

static uint32_t lookup_item_count(void)
{
    const char *env = getenv("NIPC_LOOKUP_SCALE_ITEMS");
    if (!env || !*env)
        return LOOKUP_SCALE_ITEMS_DEFAULT;

    char *end = NULL;
    unsigned long value = strtoul(env, &end, 10);
    if (!end || *end != '\0' || value == 0 || value > 65536ul)
        return LOOKUP_SCALE_ITEMS_DEFAULT;

    return (uint32_t)value;
}

static int view_eq(nipc_str_view_t view, const char *expected)
{
    size_t len = strlen(expected);
    if (view.len != len)
        return 0;
    if (len == 0)
        return 1;
    return view.ptr != NULL && memcmp(view.ptr, expected, len) == 0;
}

static int label_eq(nipc_lookup_label_view_t label,
                    const char *key, const char *value)
{
    return view_eq(label.key, key) && view_eq(label.value, value);
}

static nipc_server_config_t lookup_server_config(void)
{
    uint32_t profiles = detect_profiles();
    return (nipc_server_config_t){
        .supported_profiles         = profiles,
        .preferred_profiles         = profiles,
        .max_request_payload_bytes  = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        .max_request_batch_items    = 16,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
    };
}

static nipc_client_config_t lookup_client_config(uint32_t item_count)
{
    uint32_t profiles = detect_profiles();
    return (nipc_client_config_t){
        .supported_profiles                = profiles,
        .preferred_profiles                = profiles,
        .max_request_payload_bytes         = LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        .max_request_batch_items           = 16,
        .max_response_payload_bytes        = RESPONSE_BUF_SIZE,
        .auth_token                        = AUTH_TOKEN,
        .call_timeout_ms                   = LOOKUP_SCALE_CALL_TIMEOUT_MS,
        .max_logical_lookup_items          = item_count,
        .max_logical_lookup_subcalls       = 4096,
        .max_logical_lookup_response_bytes = 64u * 1024u * 1024u,
    };
}

static bool apps_lookup_handler(void *user,
                                const nipc_apps_lookup_req_view_t *request,
                                nipc_apps_lookup_builder_t *builder)
{
    (void)user;
    nipc_apps_lookup_builder_set_generation(builder, 9);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                "ok", 2, "/ok", 3, "name", 4, NULL, 0) != NIPC_OK)
            return false;
    }

    return true;
}

static bool cgroups_lookup_handler(void *user,
                                   const nipc_cgroups_lookup_req_view_t *request,
                                   nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;
    nipc_cgroups_lookup_builder_set_generation(builder, 7);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        if (nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "ok", 2, NULL, 0) !=
            NIPC_OK)
            return false;
    }

    return true;
}

static bool apps_lookup_mixed_handler(void *user,
                                      const nipc_apps_lookup_req_view_t *request,
                                      nipc_apps_lookup_builder_t *builder)
{
    (void)user;
    nipc_apps_lookup_builder_set_generation(builder, 19);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_apps_lookup_req_item_t req_item;
        if (nipc_apps_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        nipc_error_t err;
        if (req_item.pid == 1001) {
            nipc_lookup_label_view_t labels[] = {
                { .key = { .ptr = "role", .len = 4 },
                  .value = { .ptr = "api", .len = 3 } },
            };
            err = nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_KNOWN,
                NIPC_ORCHESTRATOR_DOCKER, req_item.pid, 1, 1000, 42,
                "known", 5, "/cg/known", 9, "pod-a", 5, labels, 1);
        } else if (req_item.pid == 1002) {
            err = nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN, NIPC_APPS_CGROUP_HOST_ROOT,
                0, req_item.pid, 1, 1001, 43,
                "host", 4, "", 0, "", 0, NULL, 0);
        } else if (req_item.pid == 1003) {
            err = nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_UNKNOWN, 0, 0, req_item.pid, 0,
                NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0);
        } else if (req_item.pid == 1004) {
            err = nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_OVERSIZED_ITEM, 0, 0, req_item.pid, 0,
                NIPC_UID_UNSET, 0, "", 0, "", 0, "", 0, NULL, 0);
        } else {
            err = nipc_apps_lookup_builder_add(
                builder, NIPC_PID_LOOKUP_KNOWN,
                NIPC_APPS_CGROUP_UNKNOWN_RETRY_LATER, 0, req_item.pid, 1,
                1002, 44, "retry", 5, "", 0, "", 0, NULL, 0);
        }
        if (err != NIPC_OK)
            return false;
    }

    return true;
}

static bool cgroups_lookup_mixed_handler(void *user,
                                         const nipc_cgroups_lookup_req_view_t *request,
                                         nipc_cgroups_lookup_builder_t *builder)
{
    (void)user;
    nipc_cgroups_lookup_builder_set_generation(builder, 17);

    for (uint32_t i = 0; i < request->item_count; i++) {
        nipc_cgroups_lookup_req_item_t req_item;
        if (nipc_cgroups_lookup_req_item(request, i, &req_item) != NIPC_OK)
            return false;

        nipc_error_t err;
        if (view_eq(req_item.path, "/known")) {
            nipc_lookup_label_view_t labels[] = {
                { .key = { .ptr = "role", .len = 4 },
                  .value = { .ptr = "db", .len = 2 } },
            };
            err = nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_K8S,
                req_item.path.ptr, req_item.path.len, "pod-a", 5, labels, 1);
        } else if (view_eq(req_item.path, "/retry")) {
            err = nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, 0,
                req_item.path.ptr, req_item.path.len, "", 0, NULL, 0);
        } else if (view_eq(req_item.path, "/permanent")) {
            err = nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_UNKNOWN_PERMANENT, 0,
                req_item.path.ptr, req_item.path.len, "", 0, NULL, 0);
        } else if (view_eq(req_item.path, "/oversized")) {
            err = nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM, 0,
                req_item.path.ptr, req_item.path.len, "", 0, NULL, 0);
        } else {
            err = nipc_cgroups_lookup_builder_add(
                builder, NIPC_CGROUP_LOOKUP_KNOWN, NIPC_ORCHESTRATOR_DOCKER,
                req_item.path.ptr, req_item.path.len, "pod-b", 5, NULL, 0);
        }
        if (err != NIPC_OK)
            return false;
    }

    return true;
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
        if (InterlockedCompareExchange(&session->active, 0, 0)) {
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

static int run_apps_server(const char *run_dir, const char *service)
{
    nipc_server_config_t scfg = lookup_server_config();
    nipc_apps_lookup_service_handler_t handler = {
        .handle = apps_lookup_handler,
        .user = NULL,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init_apps_lookup(
        &server, run_dir, service, &scfg, 8, &handler);
    if (err != NIPC_OK) {
        fprintf(stderr, "apps server init failed: %d\n", err);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    nipc_server_run(&server);
    nipc_server_destroy(&server);
    return 0;
}

static int run_cgroups_lookup_server(const char *run_dir, const char *service)
{
    nipc_server_config_t scfg = lookup_server_config();
    nipc_cgroups_lookup_service_handler_t handler = {
        .handle = cgroups_lookup_handler,
        .user = NULL,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init_cgroups_lookup(
        &server, run_dir, service, &scfg, 8, &handler);
    if (err != NIPC_OK) {
        fprintf(stderr, "cgroups lookup server init failed: %d\n", err);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    nipc_server_run(&server);
    nipc_server_destroy(&server);
    return 0;
}

static int run_apps_mixed_server(const char *run_dir, const char *service)
{
    nipc_server_config_t scfg = lookup_server_config();
    nipc_apps_lookup_service_handler_t handler = {
        .handle = apps_lookup_mixed_handler,
        .user = NULL,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init_apps_lookup(
        &server, run_dir, service, &scfg, 2, &handler);
    if (err != NIPC_OK) {
        fprintf(stderr, "apps mixed server init failed: %d\n", err);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    nipc_server_run(&server);
    nipc_server_destroy(&server);
    return 0;
}

static int run_cgroups_mixed_server(const char *run_dir, const char *service)
{
    nipc_server_config_t scfg = lookup_server_config();
    nipc_cgroups_lookup_service_handler_t handler = {
        .handle = cgroups_lookup_mixed_handler,
        .user = NULL,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init_cgroups_lookup(
        &server, run_dir, service, &scfg, 2, &handler);
    if (err != NIPC_OK) {
        fprintf(stderr, "cgroups mixed server init failed: %d\n", err);
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

static int wait_client_ready(nipc_client_ctx_t *client)
{
    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(client);
        if (nipc_client_ready(client))
            return 1;
        Sleep(10);
    }

    return 0;
}

static int run_apps_client(const char *run_dir, const char *service)
{
    uint32_t item_count = lookup_item_count();
    uint32_t *pids = calloc(item_count, sizeof(*pids));
    if (!pids)
        return 1;

    for (uint32_t i = 0; i < item_count; i++)
        pids[i] = 100000u + i;

    nipc_client_config_t ccfg = lookup_client_config(item_count);
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    if (!wait_client_ready(&client)) {
        fprintf(stderr, "apps client: not ready\n");
        free(pids);
        return 1;
    }

    int ok = 1;
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err = nipc_client_call_apps_lookup(&client, pids, item_count, &view);
    if (err != NIPC_OK) {
        fprintf(stderr, "apps client: call failed: %d\n", err);
        ok = 0;
    } else if (view.item_count != item_count || view.generation != 9u) {
        fprintf(stderr, "apps client: bad header count=%u generation=%llu\n",
                view.item_count, (unsigned long long)view.generation);
        ok = 0;
    } else {
        for (uint32_t i = 0; ok && i < item_count; i++) {
            nipc_apps_lookup_item_view_t item;
            if (nipc_apps_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_PID_LOOKUP_KNOWN ||
                item.pid != pids[i] ||
                item.comm.len != 2 || memcmp(item.comm.ptr, "ok", 2) != 0 ||
                item.cgroup_path.len != 3 ||
                memcmp(item.cgroup_path.ptr, "/ok", 3) != 0) {
                fprintf(stderr, "apps client: bad item %u\n", i);
                ok = 0;
            }
        }
    }

    nipc_client_close(&client);
    free(pids);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int run_cgroups_lookup_client(const char *run_dir, const char *service)
{
    uint32_t item_count = lookup_item_count();
    nipc_str_view_t *paths = calloc(item_count, sizeof(*paths));
    char *path_storage = calloc(item_count, LOOKUP_SCALE_PATH_BYTES);
    if (!paths || !path_storage) {
        free(paths);
        free(path_storage);
        return 1;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        char *slot = path_storage + ((size_t)i * LOOKUP_SCALE_PATH_BYTES);
        snprintf(slot, LOOKUP_SCALE_PATH_BYTES, "/cg/%05u", i);
        paths[i].ptr = slot;
        paths[i].len = (uint32_t)strlen(slot);
    }

    nipc_client_config_t ccfg = lookup_client_config(item_count);
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    if (!wait_client_ready(&client)) {
        fprintf(stderr, "cgroups lookup client: not ready\n");
        free(path_storage);
        free(paths);
        return 1;
    }

    int ok = 1;
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_cgroups_lookup(&client, paths, item_count, &view);
    if (err != NIPC_OK) {
        fprintf(stderr, "cgroups lookup client: call failed: %d\n", err);
        ok = 0;
    } else if (view.item_count != item_count || view.generation != 7u) {
        fprintf(stderr, "cgroups lookup client: bad header count=%u generation=%llu\n",
                view.item_count, (unsigned long long)view.generation);
        ok = 0;
    } else {
        for (uint32_t i = 0; ok && i < item_count; i++) {
            nipc_cgroups_lookup_item_view_t item;
            if (nipc_cgroups_lookup_resp_item(&view, i, &item) != NIPC_OK ||
                item.status != NIPC_CGROUP_LOOKUP_KNOWN ||
                item.path.len != paths[i].len ||
                memcmp(item.path.ptr, paths[i].ptr, paths[i].len) != 0 ||
                item.name.len != 2 || memcmp(item.name.ptr, "ok", 2) != 0) {
                fprintf(stderr, "cgroups lookup client: bad item %u\n", i);
                ok = 0;
            }
        }
    }

    nipc_client_close(&client);
    free(path_storage);
    free(paths);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int run_apps_mixed_client(const char *run_dir, const char *service)
{
    uint32_t pids[LOOKUP_MIXED_ITEMS] = {1001, 1002, 1003, 1004, 1005};
    nipc_client_config_t ccfg = lookup_client_config(LOOKUP_MIXED_ITEMS);
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    if (!wait_client_ready(&client)) {
        fprintf(stderr, "apps mixed client: not ready\n");
        return 1;
    }

    int ok = 1;
    nipc_apps_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_apps_lookup(&client, pids, LOOKUP_MIXED_ITEMS, &view);
    if (err != NIPC_OK) {
        fprintf(stderr, "apps mixed client: call failed: %d\n", err);
        ok = 0;
    } else if (view.item_count != LOOKUP_MIXED_ITEMS || view.generation != 19u) {
        fprintf(stderr, "apps mixed client: bad header count=%u generation=%llu\n",
                view.item_count, (unsigned long long)view.generation);
        ok = 0;
    } else {
        nipc_apps_lookup_item_view_t item;
        nipc_lookup_label_view_t label;
        if (nipc_apps_lookup_resp_item(&view, 0, &item) != NIPC_OK ||
            item.status != NIPC_PID_LOOKUP_KNOWN ||
            item.cgroup_status != NIPC_APPS_CGROUP_KNOWN ||
            item.pid != 1001 || !view_eq(item.comm, "known") ||
            !view_eq(item.cgroup_path, "/cg/known") ||
            item.label_count != 1 ||
            nipc_apps_lookup_item_label(&item, 0, &label) != NIPC_OK ||
            !label_eq(label, "role", "api"))
            ok = 0;
        if (ok && (nipc_apps_lookup_resp_item(&view, 1, &item) != NIPC_OK ||
                   item.status != NIPC_PID_LOOKUP_KNOWN ||
                   item.cgroup_status != NIPC_APPS_CGROUP_HOST_ROOT ||
                   item.pid != 1002 || !view_eq(item.comm, "host")))
            ok = 0;
        if (ok && (nipc_apps_lookup_resp_item(&view, 2, &item) != NIPC_OK ||
                   item.status != NIPC_PID_LOOKUP_UNKNOWN ||
                   item.pid != 1003))
            ok = 0;
        if (ok && (nipc_apps_lookup_resp_item(&view, 3, &item) != NIPC_OK ||
                   item.status != NIPC_PID_LOOKUP_OVERSIZED_ITEM ||
                   item.pid != 1004))
            ok = 0;
        if (ok && (nipc_apps_lookup_resp_item(&view, 4, &item) != NIPC_OK ||
                   item.status != NIPC_PID_LOOKUP_KNOWN ||
                   item.cgroup_status != NIPC_APPS_CGROUP_UNKNOWN_RETRY_LATER ||
                   item.pid != 1005 || !view_eq(item.comm, "retry")))
            ok = 0;
        if (!ok)
            fprintf(stderr, "apps mixed client: bad mixed response\n");
    }

    nipc_client_close(&client);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int run_cgroups_mixed_client(const char *run_dir, const char *service)
{
    const char *raw_paths[LOOKUP_MIXED_ITEMS] = {
        "/known", "/retry", "/permanent", "/oversized", "/known2"
    };
    nipc_str_view_t paths[LOOKUP_MIXED_ITEMS];
    for (uint32_t i = 0; i < LOOKUP_MIXED_ITEMS; i++) {
        paths[i].ptr = raw_paths[i];
        paths[i].len = (uint32_t)strlen(raw_paths[i]);
    }

    nipc_client_config_t ccfg = lookup_client_config(LOOKUP_MIXED_ITEMS);
    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);
    if (!wait_client_ready(&client)) {
        fprintf(stderr, "cgroups mixed client: not ready\n");
        return 1;
    }

    int ok = 1;
    nipc_cgroups_lookup_resp_view_t view;
    nipc_error_t err =
        nipc_client_call_cgroups_lookup(&client, paths, LOOKUP_MIXED_ITEMS, &view);
    if (err != NIPC_OK) {
        fprintf(stderr, "cgroups mixed client: call failed: %d\n", err);
        ok = 0;
    } else if (view.item_count != LOOKUP_MIXED_ITEMS || view.generation != 17u) {
        fprintf(stderr, "cgroups mixed client: bad header count=%u generation=%llu\n",
                view.item_count, (unsigned long long)view.generation);
        ok = 0;
    } else {
        nipc_cgroups_lookup_item_view_t item;
        nipc_lookup_label_view_t label;
        if (nipc_cgroups_lookup_resp_item(&view, 0, &item) != NIPC_OK ||
            item.status != NIPC_CGROUP_LOOKUP_KNOWN ||
            !view_eq(item.path, "/known") || !view_eq(item.name, "pod-a") ||
            item.label_count != 1 ||
            nipc_cgroups_lookup_item_label(&item, 0, &label) != NIPC_OK ||
            !label_eq(label, "role", "db"))
            ok = 0;
        if (ok && (nipc_cgroups_lookup_resp_item(&view, 1, &item) != NIPC_OK ||
                   item.status != NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER ||
                   !view_eq(item.path, "/retry")))
            ok = 0;
        if (ok && (nipc_cgroups_lookup_resp_item(&view, 2, &item) != NIPC_OK ||
                   item.status != NIPC_CGROUP_LOOKUP_UNKNOWN_PERMANENT ||
                   !view_eq(item.path, "/permanent")))
            ok = 0;
        if (ok && (nipc_cgroups_lookup_resp_item(&view, 3, &item) != NIPC_OK ||
                   item.status != NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM ||
                   !view_eq(item.path, "/oversized")))
            ok = 0;
        if (ok && (nipc_cgroups_lookup_resp_item(&view, 4, &item) != NIPC_OK ||
                   item.status != NIPC_CGROUP_LOOKUP_KNOWN ||
                   !view_eq(item.path, "/known2") || !view_eq(item.name, "pod-b")))
            ok = 0;
        if (!ok)
            fprintf(stderr, "cgroups mixed client: bad mixed response\n");
    }

    nipc_client_close(&client);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int run_client(const char *run_dir, const char *service)
{
    uint32_t profiles = detect_profiles();
    nipc_client_config_t ccfg = {0};
    ccfg.supported_profiles        = profiles;
    ccfg.max_request_batch_items   = 16;
    ccfg.max_response_payload_bytes = RESPONSE_BUF_SIZE;
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
        fprintf(stderr, "Usage: %s <server|client|apps-server|apps-client|cgroups-server|cgroups-client|apps-mixed-server|apps-mixed-client|cgroups-mixed-server|cgroups-mixed-client> <run_dir> <service_name>\n",
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
    } else if (strcmp(mode, "apps-server") == 0) {
        return run_apps_server(run_dir, service);
    } else if (strcmp(mode, "apps-client") == 0) {
        return run_apps_client(run_dir, service);
    } else if (strcmp(mode, "cgroups-server") == 0) {
        return run_cgroups_lookup_server(run_dir, service);
    } else if (strcmp(mode, "cgroups-client") == 0) {
        return run_cgroups_lookup_client(run_dir, service);
    } else if (strcmp(mode, "apps-mixed-server") == 0) {
        return run_apps_mixed_server(run_dir, service);
    } else if (strcmp(mode, "apps-mixed-client") == 0) {
        return run_apps_mixed_client(run_dir, service);
    } else if (strcmp(mode, "cgroups-mixed-server") == 0) {
        return run_cgroups_mixed_server(run_dir, service);
    } else if (strcmp(mode, "cgroups-mixed-client") == 0) {
        return run_cgroups_mixed_client(run_dir, service);
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
