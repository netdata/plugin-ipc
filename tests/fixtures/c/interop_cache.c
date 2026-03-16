/*
 * interop_cache.c - L3 cross-language cache interop binary.
 *
 * Usage:
 *   interop_cache server <run_dir> <service_name>
 *     Starts a managed L2 server with a cgroups handler (3 items),
 *     prints READY, handles 1 client session, then exits.
 *
 *   interop_cache client <run_dir> <service_name>
 *     Creates L3 cache, refreshes, verifies lookup, prints PASS/FAIL.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE 65536

/* ------------------------------------------------------------------ */
/*  Cgroups handler: 3 test items (same as L2 interop)                 */
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
/*  Server mode (pure L2, same as interop_service)                     */
/* ------------------------------------------------------------------ */

static uint32_t detect_profiles(void)
{
    const char *env = getenv("NIPC_PROFILE");
    if (env && strcmp(env, "shm") == 0)
        return NIPC_PROFILE_SHM_HYBRID | NIPC_PROFILE_BASELINE;
    return NIPC_PROFILE_BASELINE;
}

static int run_server(const char *run_dir, const char *service)
{
    uint32_t profiles = detect_profiles();
    nipc_uds_server_config_t scfg = {
        .supported_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .backlog                   = 4,
    };

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
/*  Client mode (L3 cache)                                             */
/* ------------------------------------------------------------------ */

static int run_client(const char *run_dir, const char *service)
{
    uint32_t profiles = detect_profiles();
    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_cgroups_cache_t cache;
    nipc_cgroups_cache_init(&cache, run_dir, service, &ccfg);

    /* Refresh to populate cache */
    bool updated = nipc_cgroups_cache_refresh(&cache);
    if (!updated || !nipc_cgroups_cache_ready(&cache)) {
        fprintf(stderr, "client: cache not ready after refresh\n");
        nipc_cgroups_cache_close(&cache);
        printf("FAIL\n");
        return 1;
    }

    int ok = 1;

    /* Verify status */
    nipc_cgroups_cache_status_t status;
    nipc_cgroups_cache_status(&cache, &status);
    if (status.item_count != 3) {
        fprintf(stderr, "client: expected 3 items, got %u\n", status.item_count);
        ok = 0;
    }
    if (status.systemd_enabled != 1) {
        fprintf(stderr, "client: expected systemd_enabled=1, got %u\n",
                status.systemd_enabled);
        ok = 0;
    }
    if (status.generation != 42) {
        fprintf(stderr, "client: expected generation=42, got %lu\n",
                (unsigned long)status.generation);
        ok = 0;
    }

    /* Verify lookups */
    const nipc_cgroups_cache_item_t *item =
        nipc_cgroups_cache_lookup(&cache, 1001, "docker-abc123");
    if (!item) {
        fprintf(stderr, "client: item 1001 not found\n");
        ok = 0;
    } else {
        if (item->hash != 1001) {
            fprintf(stderr, "client: item hash: got %u\n", item->hash);
            ok = 0;
        }
        if (strcmp(item->name, "docker-abc123") != 0) {
            fprintf(stderr, "client: item name mismatch\n");
            ok = 0;
        }
        if (strcmp(item->path, "/sys/fs/cgroup/docker/abc123") != 0) {
            fprintf(stderr, "client: item path mismatch\n");
            ok = 0;
        }
    }

    /* Verify not-found */
    if (nipc_cgroups_cache_lookup(&cache, 9999, "nonexistent") != NULL) {
        fprintf(stderr, "client: nonexistent item should be NULL\n");
        ok = 0;
    }

    nipc_cgroups_cache_close(&cache);

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
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server|client> <run_dir> <service_name>\n",
                argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *run_dir = argv[2];
    const char *service = argv[3];

    mkdir(run_dir, 0700);

    if (strcmp(mode, "server") == 0) {
        return run_server(run_dir, service);
    } else if (strcmp(mode, "client") == 0) {
        return run_client(run_dir, service);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 1;
    }
}
