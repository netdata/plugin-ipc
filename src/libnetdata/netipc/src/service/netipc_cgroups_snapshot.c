#include <netipc/netipc_cgroups_snapshot.h>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define NETIPC_CGROUPS_WINDOWS_RUNTIME 1
#include <netipc/netipc_named_pipe.h>
#else
#define NETIPC_CGROUPS_WINDOWS_RUNTIME 0
#include <netipc/netipc_uds_seqpacket.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct netipc_cgroups_snapshot_client {
    struct netipc_cgroups_snapshot_client_config config;
    char *service_namespace;
    char *service_name;
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    netipc_named_pipe_client_t *transport;
#else
    netipc_uds_seqpacket_client_t *transport;
#endif
    uint64_t next_message_id;
    uint8_t *response_message;
    size_t response_capacity;
    struct netipc_cgroups_snapshot_cache_item *items;
    uint32_t item_count;
    struct netipc_cgroups_snapshot_cache cache;
};

static char *dup_bytes(const char *src, size_t len) {
    char *copy;

    if (!src && len != 0u) {
        errno = EINVAL;
        return NULL;
    }

    copy = malloc(len + 1u);
    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }

    if (len != 0u) {
        memcpy(copy, src, len);
    }
    copy[len] = '\0';
    return copy;
}

static void clear_cache(netipc_cgroups_snapshot_client_t *client) {
    uint32_t i;

    if (!client || !client->items) {
        client->cache.generation = 0u;
        client->cache.systemd_enabled = 0u;
        client->cache.item_count = 0u;
        client->cache.items = NULL;
        client->item_count = 0u;
        return;
    }

    for (i = 0u; i < client->item_count; ++i) {
        free((char *)client->items[i].name);
        free((char *)client->items[i].path);
    }

    free(client->items);
    client->items = NULL;
    client->item_count = 0u;
    client->cache.generation = 0u;
    client->cache.systemd_enabled = 0u;
    client->cache.item_count = 0u;
    client->cache.items = NULL;
}

static void disconnect_transport(netipc_cgroups_snapshot_client_t *client) {
    if (!client) {
        return;
    }

#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    if (client->transport) {
        netipc_named_pipe_client_destroy(client->transport);
        client->transport = NULL;
    }
#else
    if (client->transport) {
        netipc_uds_seqpacket_client_destroy(client->transport);
        client->transport = NULL;
    }
#endif
}

static int transport_status_errno(uint16_t transport_status) {
    switch (transport_status) {
        case NETIPC_TRANSPORT_STATUS_OK:
            return 0;
        case NETIPC_TRANSPORT_STATUS_AUTH_FAILED:
            return EACCES;
        case NETIPC_TRANSPORT_STATUS_LIMIT_EXCEEDED:
            return EMSGSIZE;
        case NETIPC_TRANSPORT_STATUS_UNSUPPORTED:
            return ENOTSUP;
        case NETIPC_TRANSPORT_STATUS_INTERNAL_ERROR:
            return EIO;
        case NETIPC_TRANSPORT_STATUS_BAD_ENVELOPE:
        case NETIPC_TRANSPORT_STATUS_INCOMPATIBLE:
        default:
            return EPROTO;
    }
}

static size_t effective_request_payload_limit(uint32_t configured) {
    return configured != 0u ? configured : NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
}

static size_t effective_request_batch_limit(uint32_t configured) {
    return configured != 0u ? configured : 1u;
}

static size_t effective_response_payload_limit(uint32_t configured) {
    return configured != 0u ? configured : NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
}

static size_t effective_response_batch_limit(uint32_t configured) {
    return configured != 0u ? configured : NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
}

static int ensure_transport(netipc_cgroups_snapshot_client_t *client, uint32_t timeout_ms) {
    if (!client) {
        errno = EINVAL;
        return -1;
    }

    if (client->transport) {
        return 0;
    }

#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    struct netipc_named_pipe_config config = {
        .run_dir = client->service_namespace,
        .service_name = client->service_name,
        .supported_profiles = client->config.supported_profiles,
        .preferred_profiles = client->config.preferred_profiles,
        .max_request_payload_bytes = (uint32_t)effective_request_payload_limit(client->config.max_request_payload_bytes),
        .max_request_batch_items = (uint32_t)effective_request_batch_limit(client->config.max_request_batch_items),
        .max_response_payload_bytes = (uint32_t)effective_response_payload_limit(client->config.max_response_payload_bytes),
        .max_response_batch_items = (uint32_t)effective_response_batch_limit(client->config.max_response_batch_items),
        .auth_token = client->config.auth_token,
        .shm_spin_tries = NETIPC_SHM_HYBRID_DEFAULT_SPIN_TRIES,
    };

    return netipc_named_pipe_client_create(&config, &client->transport, timeout_ms);
#else
    struct netipc_uds_seqpacket_config config = {
        .run_dir = client->service_namespace,
        .service_name = client->service_name,
        .file_mode = 0660u,
        .supported_profiles = client->config.supported_profiles,
        .preferred_profiles = client->config.preferred_profiles,
        .max_request_payload_bytes = (uint32_t)effective_request_payload_limit(client->config.max_request_payload_bytes),
        .max_request_batch_items = (uint32_t)effective_request_batch_limit(client->config.max_request_batch_items),
        .max_response_payload_bytes = (uint32_t)effective_response_payload_limit(client->config.max_response_payload_bytes),
        .max_response_batch_items = (uint32_t)effective_response_batch_limit(client->config.max_response_batch_items),
        .auth_token = client->config.auth_token,
    };

    return netipc_uds_seqpacket_client_create(&config, &client->transport, timeout_ms);
#endif
}

static int transport_call_message(netipc_cgroups_snapshot_client_t *client,
                                  const uint8_t *request_message,
                                  size_t request_message_len,
                                  size_t *out_response_len,
                                  uint32_t timeout_ms) {
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    return netipc_named_pipe_client_call_message(client->transport,
                                                 request_message,
                                                 request_message_len,
                                                 client->response_message,
                                                 client->response_capacity,
                                                 out_response_len,
                                                 timeout_ms);
#else
    return netipc_uds_seqpacket_client_call_message(client->transport,
                                                    request_message,
                                                    request_message_len,
                                                    client->response_message,
                                                    client->response_capacity,
                                                    out_response_len,
                                                    timeout_ms);
#endif
}

void netipc_cgroups_snapshot_client_config_init(
    struct netipc_cgroups_snapshot_client_config *config,
    const char *service_namespace,
    const char *service_name) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->service_namespace = service_namespace;
    config->service_name = service_name;
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    config->supported_profiles = NETIPC_PROFILE_NAMED_PIPE;
    config->preferred_profiles = NETIPC_PROFILE_NAMED_PIPE;
#else
    config->supported_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    config->preferred_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
#endif
    config->max_request_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
    config->max_request_batch_items = 1u;
    config->max_response_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config->max_response_batch_items = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
}

int netipc_cgroups_snapshot_client_create(
    const struct netipc_cgroups_snapshot_client_config *config,
    netipc_cgroups_snapshot_client_t **out_client) {
    netipc_cgroups_snapshot_client_t *client;
    size_t response_capacity;

    if (!config || !out_client || !config->service_namespace || !config->service_name) {
        errno = EINVAL;
        return -1;
    }

    *out_client = NULL;

    response_capacity = netipc_msg_max_batch_total_size(
        (uint32_t)effective_response_payload_limit(config->max_response_payload_bytes),
        (uint32_t)effective_response_batch_limit(config->max_response_batch_items));
    if (response_capacity == 0u) {
        return -1;
    }

    client = calloc(1, sizeof(*client));
    if (!client) {
        errno = ENOMEM;
        return -1;
    }

    client->config = *config;
    client->service_namespace = dup_bytes(config->service_namespace, strlen(config->service_namespace));
    client->service_name = dup_bytes(config->service_name, strlen(config->service_name));
    client->response_message = malloc(response_capacity);
    client->response_capacity = response_capacity;
    client->next_message_id = 1u;

    if (!client->service_namespace || !client->service_name || !client->response_message) {
        int saved = errno;
        free(client->response_message);
        free(client->service_name);
        free(client->service_namespace);
        free(client);
        errno = saved != 0 ? saved : ENOMEM;
        return -1;
    }

    *out_client = client;
    return 0;
}

int netipc_cgroups_snapshot_client_refresh(
    netipc_cgroups_snapshot_client_t *client,
    uint32_t timeout_ms) {
    uint8_t request_message[NETIPC_MSG_HEADER_LEN + NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
    uint8_t request_payload[NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_request request = {.flags = 0u};
    struct netipc_cgroups_snapshot_view view;
    struct netipc_cgroups_snapshot_cache_item *items = NULL;
    uint32_t i;
    size_t response_len = 0u;

    if (!client) {
        errno = EINVAL;
        return -1;
    }

    if (ensure_transport(client, timeout_ms) != 0) {
        return -1;
    }

    if (netipc_encode_cgroups_snapshot_request_payload(request_payload,
                                                       sizeof(request_payload),
                                                       &request) != 0) {
        return -1;
    }

    header.magic = NETIPC_MSG_MAGIC;
    header.version = NETIPC_MSG_VERSION;
    header.header_len = NETIPC_MSG_HEADER_LEN;
    header.kind = NETIPC_MSG_KIND_REQUEST;
    header.flags = 0u;
    header.code = NETIPC_METHOD_CGROUPS_SNAPSHOT;
    header.transport_status = NETIPC_TRANSPORT_STATUS_OK;
    header.payload_len = NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
    header.item_count = 1u;
    header.message_id = client->next_message_id++;

    if (netipc_encode_msg_header(request_message, sizeof(request_message), &header) != 0) {
        return -1;
    }
    memcpy(request_message + NETIPC_MSG_HEADER_LEN, request_payload, sizeof(request_payload));

    if (transport_call_message(client,
                               request_message,
                               sizeof(request_message),
                               &response_len,
                               timeout_ms) != 0) {
        disconnect_transport(client);
        return -1;
    }

    if (netipc_decode_msg_header(client->response_message, response_len, &header) != 0) {
        disconnect_transport(client);
        return -1;
    }

    if (header.kind != NETIPC_MSG_KIND_RESPONSE ||
        header.code != NETIPC_METHOD_CGROUPS_SNAPSHOT ||
        header.message_id != client->next_message_id - 1u ||
        header.flags != NETIPC_MSG_FLAG_BATCH ||
        header.transport_status != NETIPC_TRANSPORT_STATUS_OK ||
        header.payload_len != response_len - NETIPC_MSG_HEADER_LEN) {
        errno = header.transport_status != NETIPC_TRANSPORT_STATUS_OK ?
                    transport_status_errno(header.transport_status) :
                    EPROTO;
        disconnect_transport(client);
        return -1;
    }

    if (netipc_decode_cgroups_snapshot_view(client->response_message + NETIPC_MSG_HEADER_LEN,
                                            header.payload_len,
                                            header.item_count,
                                            &view) != 0) {
        disconnect_transport(client);
        return -1;
    }

    if (header.item_count != 0u) {
        items = calloc(header.item_count, sizeof(*items));
        if (!items) {
            errno = ENOMEM;
            return -1;
        }
    }

    for (i = 0u; i < header.item_count; ++i) {
        struct netipc_cgroups_snapshot_item_view item_view;
        if (netipc_cgroups_snapshot_view_item_at(&view, i, &item_view) != 0) {
            uint32_t j;
            for (j = 0u; j < i; ++j) {
                free((char *)items[j].name);
                free((char *)items[j].path);
            }
            free(items);
            disconnect_transport(client);
            return -1;
        }

        items[i].hash = item_view.hash;
        items[i].options = item_view.options;
        items[i].enabled = item_view.enabled;
        items[i].name = dup_bytes(item_view.name_view.ptr, item_view.name_view.len);
        items[i].path = dup_bytes(item_view.path_view.ptr, item_view.path_view.len);
        if (!items[i].name || !items[i].path) {
            uint32_t j;
            int saved = errno;
            for (j = 0u; j <= i; ++j) {
                free((char *)items[j].name);
                free((char *)items[j].path);
            }
            free(items);
            errno = saved != 0 ? saved : ENOMEM;
            return -1;
        }
    }

    clear_cache(client);
    client->items = items;
    client->item_count = header.item_count;
    client->cache.generation = view.generation;
    client->cache.systemd_enabled = view.systemd_enabled;
    client->cache.item_count = header.item_count;
    client->cache.items = client->items;
    return 0;
}

const struct netipc_cgroups_snapshot_cache *netipc_cgroups_snapshot_client_cache(
    const netipc_cgroups_snapshot_client_t *client) {
    if (!client) {
        return NULL;
    }

    return &client->cache;
}

const struct netipc_cgroups_snapshot_cache_item *netipc_cgroups_snapshot_client_lookup(
    const netipc_cgroups_snapshot_client_t *client,
    uint32_t hash,
    const char *name) {
    uint32_t i;

    if (!client || !name) {
        return NULL;
    }

    for (i = 0u; i < client->item_count; ++i) {
        if (client->items[i].hash == hash && strcmp(client->items[i].name, name) == 0) {
            return &client->items[i];
        }
    }

    return NULL;
}

void netipc_cgroups_snapshot_client_destroy(netipc_cgroups_snapshot_client_t *client) {
    if (!client) {
        return;
    }

    disconnect_transport(client);
    clear_cache(client);
    free(client->response_message);
    free(client->service_name);
    free(client->service_namespace);
    free(client);
}
