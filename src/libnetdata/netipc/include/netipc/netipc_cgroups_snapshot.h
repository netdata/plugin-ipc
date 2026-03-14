#ifndef NETIPC_CGROUPS_SNAPSHOT_H
#define NETIPC_CGROUPS_SNAPSHOT_H

#include <stdint.h>
#include <stdio.h>

#include <netipc/netipc_schema.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_CGROUPS_SNAPSHOT_NAME_MAX_BYTES 255u
#define NETIPC_CGROUPS_SNAPSHOT_PATH_MAX_BYTES ((uint32_t)FILENAME_MAX)
#define NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES \
    (NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN + NETIPC_CGROUPS_SNAPSHOT_NAME_MAX_BYTES + 1u + \
     NETIPC_CGROUPS_SNAPSHOT_PATH_MAX_BYTES + 1u)
#define NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS 1000u

typedef struct netipc_cgroups_snapshot_client netipc_cgroups_snapshot_client_t;

struct netipc_cgroups_snapshot_client_config {
    const char *service_namespace;
    const char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
};

struct netipc_cgroups_snapshot_cache_item {
    uint32_t hash;
    uint32_t options;
    uint32_t enabled;
    const char *name;
    const char *path;
};

struct netipc_cgroups_snapshot_cache {
    uint64_t generation;
    uint32_t systemd_enabled;
    uint32_t item_count;
    const struct netipc_cgroups_snapshot_cache_item *items;
};

void netipc_cgroups_snapshot_client_config_init(
    struct netipc_cgroups_snapshot_client_config *config,
    const char *service_namespace,
    const char *service_name);

int netipc_cgroups_snapshot_client_config_validate(
    const struct netipc_cgroups_snapshot_client_config *config);

int netipc_cgroups_snapshot_client_create(
    const struct netipc_cgroups_snapshot_client_config *config,
    netipc_cgroups_snapshot_client_t **out_client);

int netipc_cgroups_snapshot_client_refresh(
    netipc_cgroups_snapshot_client_t *client,
    uint32_t timeout_ms);

const struct netipc_cgroups_snapshot_cache *netipc_cgroups_snapshot_client_cache(
    const netipc_cgroups_snapshot_client_t *client);

const struct netipc_cgroups_snapshot_cache_item *netipc_cgroups_snapshot_client_lookup(
    const netipc_cgroups_snapshot_client_t *client,
    uint32_t hash,
    const char *name);

void netipc_cgroups_snapshot_client_destroy(netipc_cgroups_snapshot_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
