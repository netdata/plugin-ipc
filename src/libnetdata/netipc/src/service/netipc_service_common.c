#include "netipc_service_common.h"

#include "netipc/netipc_protocol.h"

#include <stdlib.h>
#include <string.h>

#define NIPC_CLIENT_BUF_DEFAULT 65536u

uint32_t nipc_service_common_next_power_of_2_u32(uint32_t n)
{
    if (n < 16)
        return 16;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

bool nipc_service_common_header_payload_len(size_t payload_len,
                                            size_t *msg_len_out)
{
#if SIZE_MAX <= UINT32_MAX
    if (payload_len > SIZE_MAX - NIPC_HEADER_LEN)
        return false;
#endif

    *msg_len_out = NIPC_HEADER_LEN + payload_len;
    return true;
}

bool nipc_service_common_header_payload_len_u32(uint32_t payload_len,
                                                uint32_t *msg_len_out)
{
    if (payload_len > UINT32_MAX - NIPC_HEADER_LEN)
        return false;

    *msg_len_out = payload_len + NIPC_HEADER_LEN;
    return true;
}

bool nipc_service_common_mul_would_overflow(size_t count, size_t size)
{
    return size != 0 && count > SIZE_MAX / size;
}

uint32_t nipc_service_common_cgroups_request_payload_default(void)
{
    return NIPC_MAX_PAYLOAD_DEFAULT;
}

uint32_t nipc_service_common_cgroups_response_payload_default(void)
{
    return NIPC_CLIENT_BUF_DEFAULT;
}

static void copy_cstr_field(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0)
        return;

    size_t len = strlen(src);
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void nipc_service_common_client_init(nipc_client_ctx_t *ctx,
                                     const char *run_dir,
                                     const char *service_name)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = NIPC_CLIENT_DISCONNECTED;
    ctx->session_valid = false;
    ctx->shm = NULL;
    copy_cstr_field(ctx->run_dir, sizeof(ctx->run_dir), run_dir);
    copy_cstr_field(ctx->service_name, sizeof(ctx->service_name), service_name);
}

void nipc_service_common_client_status(const nipc_client_ctx_t *ctx,
                                       nipc_client_status_t *out)
{
    out->state = ctx->state;
    out->connect_count = ctx->connect_count;
    out->reconnect_count = ctx->reconnect_count;
    out->call_count = ctx->call_count;
    out->error_count = ctx->error_count;
}

void nipc_service_common_client_close_buffers(nipc_client_ctx_t *ctx)
{
    free(ctx->response_buf);
    free(ctx->send_buf);
    ctx->response_buf = NULL;
    ctx->send_buf = NULL;
    ctx->response_buf_size = 0;
    ctx->send_buf_size = 0;
    ctx->state = NIPC_CLIENT_DISCONNECTED;
}

void nipc_service_common_client_note_request_capacity(nipc_client_ctx_t *ctx,
                                                      uint32_t payload_len)
{
    uint32_t grown = nipc_service_common_next_power_of_2_u32(payload_len);
    if (grown > NIPC_MAX_PAYLOAD_CAP)
        grown = NIPC_MAX_PAYLOAD_CAP;
    if (grown > ctx->transport_config.max_request_payload_bytes)
        ctx->transport_config.max_request_payload_bytes = grown;
}

void nipc_service_common_client_note_response_capacity(nipc_client_ctx_t *ctx,
                                                       uint32_t payload_len)
{
    uint32_t grown = nipc_service_common_next_power_of_2_u32(payload_len);
    if (grown > NIPC_MAX_PAYLOAD_CAP)
        grown = NIPC_MAX_PAYLOAD_CAP;
    if (grown > ctx->transport_config.max_response_payload_bytes)
        ctx->transport_config.max_response_payload_bytes = grown;
}

nipc_error_t nipc_service_common_response_status_to_error(nipc_client_ctx_t *ctx,
                                                          const nipc_header_t *resp_hdr)
{
    switch (resp_hdr->transport_status) {
    case NIPC_STATUS_OK:
        return NIPC_OK;
    case NIPC_STATUS_LIMIT_EXCEEDED:
        if (ctx->session.max_response_payload_bytes > 0) {
            uint32_t current = ctx->session.max_response_payload_bytes;
            nipc_service_common_client_note_response_capacity(
                ctx, current >= UINT32_MAX / 2u ? UINT32_MAX : current * 2u);
        }
        return NIPC_ERR_OVERFLOW;
    case NIPC_STATUS_UNSUPPORTED:
        return NIPC_ERR_BAD_LAYOUT;
    case NIPC_STATUS_BAD_ENVELOPE:
    case NIPC_STATUS_INTERNAL_ERROR:
    default:
        return NIPC_ERR_BAD_LAYOUT;
    }
}

bool nipc_service_common_cgroups_lookup_request_size(const nipc_str_view_t *paths,
                                                     uint32_t path_count,
                                                     size_t *size_out)
{
    if (nipc_service_common_mul_would_overflow(
            (size_t)path_count, NIPC_LOOKUP_DIR_ENTRY_SIZE))
        return false;

    size_t data = NIPC_CGROUPS_LOOKUP_REQ_HDR_SIZE +
                  (size_t)path_count * NIPC_LOOKUP_DIR_ENTRY_SIZE;
    for (uint32_t i = 0; i < path_count; i++) {
        size_t aligned = nipc_align8(data);
        if (aligned < data)
            return false;
        if (!paths || paths[i].len > SIZE_MAX - aligned - 1u)
            return false;
        data = aligned + (size_t)paths[i].len + 1u;
    }
    *size_out = data;
    return true;
}

bool nipc_service_common_apps_lookup_request_size(uint32_t pid_count,
                                                  size_t *size_out)
{
    if (nipc_service_common_mul_would_overflow(
            (size_t)pid_count, NIPC_LOOKUP_DIR_ENTRY_SIZE) ||
        nipc_service_common_mul_would_overflow(
            (size_t)pid_count, NIPC_APPS_LOOKUP_KEY_SIZE))
        return false;

    size_t dir = (size_t)pid_count * NIPC_LOOKUP_DIR_ENTRY_SIZE;
    size_t keys = (size_t)pid_count * NIPC_APPS_LOOKUP_KEY_SIZE;
#if SIZE_MAX <= UINT32_MAX
    if (NIPC_APPS_LOOKUP_REQ_HDR_SIZE > SIZE_MAX - dir ||
        NIPC_APPS_LOOKUP_REQ_HDR_SIZE + dir > SIZE_MAX - keys)
        return false;
#endif
    *size_out = NIPC_APPS_LOOKUP_REQ_HDR_SIZE + dir + keys;
    return true;
}

void nipc_service_common_server_note_request_capacity(nipc_managed_server_t *server,
                                                      uint32_t payload_len)
{
    uint32_t grown = nipc_service_common_next_power_of_2_u32(payload_len);
#if defined(_WIN32) || defined(__MSYS__)
    uint32_t current = server->learned_request_payload_bytes;
    while (grown > current) {
        uint32_t previous = (uint32_t)InterlockedCompareExchange(
            (volatile LONG *)&server->learned_request_payload_bytes,
            (LONG)grown, (LONG)current);
        if (previous == current)
            break;
        current = previous;
    }
#else
    uint32_t current = __atomic_load_n(&server->learned_request_payload_bytes,
                                       __ATOMIC_RELAXED);
    while (grown > current &&
           !__atomic_compare_exchange_n(&server->learned_request_payload_bytes,
                                        &current, grown, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
#endif
}

void nipc_service_common_server_note_response_capacity(nipc_managed_server_t *server,
                                                       uint32_t payload_len)
{
    uint32_t grown = nipc_service_common_next_power_of_2_u32(payload_len);
#if defined(_WIN32) || defined(__MSYS__)
    uint32_t current = server->learned_response_payload_bytes;
    while (grown > current) {
        uint32_t previous = (uint32_t)InterlockedCompareExchange(
            (volatile LONG *)&server->learned_response_payload_bytes,
            (LONG)grown, (LONG)current);
        if (previous == current)
            break;
        current = previous;
    }
#else
    uint32_t current = __atomic_load_n(&server->learned_response_payload_bytes,
                                       __ATOMIC_RELAXED);
    while (grown > current &&
           !__atomic_compare_exchange_n(&server->learned_response_payload_bytes,
                                        &current, grown, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
#endif
}

static uint32_t service_snapshot_max_items(
    size_t response_buf_size,
    const nipc_cgroups_service_handler_t *service_handler)
{
    if (service_handler->snapshot_max_items != 0)
        return service_handler->snapshot_max_items;
    return nipc_cgroups_builder_estimate_max_items(response_buf_size);
}

nipc_error_t nipc_service_common_typed_dispatch(void *user,
                                                const nipc_header_t *request_hdr,
                                                const uint8_t *request_payload,
                                                size_t request_len,
                                                uint8_t *response_buf,
                                                size_t response_buf_size,
                                                size_t *response_len_out)
{
    nipc_managed_server_t *server = (nipc_managed_server_t *)user;
    (void)request_hdr;

    switch (server->expected_method_code) {
    case NIPC_METHOD_CGROUPS_SNAPSHOT: {
        nipc_cgroups_service_handler_t *service_handler = &server->service_handler;
        if (!service_handler->handle)
            return NIPC_ERR_HANDLER_FAILED;
        return nipc_dispatch_cgroups_snapshot(
            request_payload, request_len,
            response_buf, response_buf_size, response_len_out,
            service_snapshot_max_items(response_buf_size, service_handler),
            service_handler->handle, service_handler->user);
    }
    case NIPC_METHOD_CGROUPS_LOOKUP:
        if (!server->cgroups_lookup_handler.handle)
            return NIPC_ERR_HANDLER_FAILED;
        return nipc_dispatch_cgroups_lookup(
            request_payload, request_len,
            response_buf, response_buf_size, response_len_out,
            server->cgroups_lookup_handler.handle,
            server->cgroups_lookup_handler.user);
    case NIPC_METHOD_APPS_LOOKUP:
        if (!server->apps_lookup_handler.handle)
            return NIPC_ERR_HANDLER_FAILED;
        return nipc_dispatch_apps_lookup(
            request_payload, request_len,
            response_buf, response_buf_size, response_len_out,
            server->apps_lookup_handler.handle,
            server->apps_lookup_handler.user);
    default:
        return NIPC_ERR_HANDLER_FAILED;
    }
}

void nipc_service_common_prepare_response_header(const nipc_header_t *request_hdr,
                                                 nipc_header_t *resp_hdr)
{
    memset(resp_hdr, 0, sizeof(*resp_hdr));
    resp_hdr->kind = NIPC_KIND_RESPONSE;
    resp_hdr->code = request_hdr->code;
    resp_hdr->message_id = request_hdr->message_id;
    if ((request_hdr->flags & NIPC_FLAG_BATCH) && request_hdr->item_count >= 1) {
        resp_hdr->item_count = request_hdr->item_count;
        resp_hdr->flags = NIPC_FLAG_BATCH;
    } else {
        resp_hdr->item_count = 1;
        resp_hdr->flags = 0;
    }
}

void nipc_service_common_apply_dispatch_result(nipc_managed_server_t *server,
                                               nipc_error_t dispatch_err,
                                               size_t resp_buf_size,
                                               uint32_t max_response_payload_bytes,
                                               bool check_header_room,
                                               nipc_header_t *resp_hdr,
                                               size_t *response_len,
                                               bool *close_after_response)
{
    *close_after_response = false;

    switch (dispatch_err) {
    case NIPC_OK:
        if (*response_len > resp_buf_size ||
            *response_len > max_response_payload_bytes ||
            (check_header_room && *response_len > SIZE_MAX - NIPC_HEADER_LEN)) {
            nipc_service_common_server_note_response_capacity(
                server, *response_len >= UINT32_MAX ? UINT32_MAX : (uint32_t)*response_len);
            resp_hdr->transport_status = NIPC_STATUS_LIMIT_EXCEEDED;
            *close_after_response = true;
            *response_len = 0;
        } else {
            if (*response_len <= UINT32_MAX)
                nipc_service_common_server_note_response_capacity(
                    server, (uint32_t)*response_len);
            resp_hdr->transport_status = NIPC_STATUS_OK;
        }
        break;
    case NIPC_ERR_OVERFLOW:
        if (max_response_payload_bytes >= UINT32_MAX / 2u)
            nipc_service_common_server_note_response_capacity(server, UINT32_MAX);
        else
            nipc_service_common_server_note_response_capacity(
                server, max_response_payload_bytes * 2u);
        resp_hdr->transport_status = NIPC_STATUS_LIMIT_EXCEEDED;
        *close_after_response = true;
        *response_len = 0;
        break;
    case NIPC_ERR_TRUNCATED:
    case NIPC_ERR_BAD_LAYOUT:
    case NIPC_ERR_OUT_OF_BOUNDS:
    case NIPC_ERR_MISSING_NUL:
    case NIPC_ERR_BAD_ALIGNMENT:
    case NIPC_ERR_BAD_ITEM_COUNT:
        resp_hdr->transport_status = NIPC_STATUS_BAD_ENVELOPE;
        *close_after_response = true;
        *response_len = 0;
        break;
    case NIPC_ERR_HANDLER_FAILED:
    default:
        resp_hdr->transport_status = NIPC_STATUS_INTERNAL_ERROR;
        *close_after_response = true;
        *response_len = 0;
        break;
    }
}

static void cache_free_items(nipc_cgroups_cache_item_t *items, uint32_t count)
{
    if (!items)
        return;

    for (uint32_t i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].path);
    }
    free(items);
}

static uint32_t cache_hash_name(const char *name)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h;
}

static bool cache_build_hashtable(nipc_cgroups_cache_t *cache,
                                  const nipc_service_common_cache_ops_t *ops)
{
    free(cache->buckets);
    cache->buckets = NULL;
    cache->bucket_count = 0;

    if (cache->item_count == 0)
        return true;

    uint32_t bcount = nipc_service_common_next_power_of_2_u32(cache->item_count * 2);
    nipc_cgroups_hash_bucket_t *buckets = ops->calloc_fn(
        bcount, sizeof(nipc_cgroups_hash_bucket_t),
        ops->cache_buckets_fault_site);
    if (!buckets)
        return false;

    uint32_t mask = bcount - 1;
    for (uint32_t i = 0; i < cache->item_count; i++) {
        uint32_t key = cache->items[i].hash ^ cache_hash_name(cache->items[i].name);
        uint32_t slot = key & mask;

        while (buckets[slot].used)
            slot = (slot + 1) & mask;

        buckets[slot].index = i;
        buckets[slot].used = true;
    }

    cache->buckets = buckets;
    cache->bucket_count = bcount;
    return true;
}

static nipc_cgroups_cache_item_t *cache_build_items(
    const nipc_cgroups_resp_view_t *view,
    const nipc_service_common_cache_ops_t *ops,
    uint32_t *count_out)
{
    uint32_t n = view->item_count;
    *count_out = 0;

    if (n == 0)
        return NULL;

    nipc_cgroups_cache_item_t *items = ops->calloc_fn(
        n, sizeof(nipc_cgroups_cache_item_t),
        ops->cache_items_fault_site);
    if (!items)
        return NULL;

    for (uint32_t i = 0; i < n; i++) {
        nipc_cgroups_item_view_t iv;
        nipc_error_t err = nipc_cgroups_resp_item(view, i, &iv);
        if (err != NIPC_OK) {
            cache_free_items(items, i);
            return NULL;
        }

        items[i].hash = iv.hash;
        items[i].options = iv.options;
        items[i].enabled = iv.enabled;

        items[i].name = ops->malloc_fn(
            iv.name.len + 1, ops->cache_item_name_fault_site);
        if (!items[i].name) {
            cache_free_items(items, i);
            return NULL;
        }
        if (iv.name.len > 0)
            memcpy(items[i].name, iv.name.ptr, iv.name.len);
        items[i].name[iv.name.len] = '\0';

        items[i].path = ops->malloc_fn(
            iv.path.len + 1, ops->cache_item_path_fault_site);
        if (!items[i].path) {
            free(items[i].name);
            cache_free_items(items, i);
            return NULL;
        }
        if (iv.path.len > 0)
            memcpy(items[i].path, iv.path.ptr, iv.path.len);
        items[i].path[iv.path.len] = '\0';
    }

    *count_out = n;
    return items;
}

void nipc_service_common_cgroups_cache_init(nipc_cgroups_cache_t *cache,
                                            const char *run_dir,
                                            const char *service_name,
                                            const nipc_client_config_t *config)
{
    memset(cache, 0, sizeof(*cache));
    nipc_client_init(&cache->client, run_dir, service_name, config);
}

bool nipc_service_common_cgroups_cache_refresh(
    nipc_cgroups_cache_t *cache,
    const nipc_service_common_cache_ops_t *ops)
{
    nipc_client_refresh(&cache->client);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&cache->client, &view);
    if (err != NIPC_OK) {
        cache->refresh_failure_count++;
        return false;
    }

    uint32_t new_count = 0;
    nipc_cgroups_cache_item_t *new_items = NULL;
    if (view.item_count > 0) {
        new_items = cache_build_items(&view, ops, &new_count);
        if (!new_items) {
            cache->refresh_failure_count++;
            return false;
        }
    }

    cache_free_items(cache->items, cache->item_count);
    cache->items = new_items;
    cache->item_count = new_count;
    cache->systemd_enabled = view.systemd_enabled;
    cache->generation = view.generation;
    cache->populated = true;
    cache->refresh_success_count++;
    cache->last_refresh_ts = ops->monotonic_ms_fn();
    cache_build_hashtable(cache, ops);

    return true;
}

const nipc_cgroups_cache_item_t *nipc_service_common_cgroups_cache_lookup(
    const nipc_cgroups_cache_t *cache,
    uint32_t hash,
    const char *name)
{
    if (!cache->populated || !cache->items || !name)
        return NULL;

    if (cache->buckets && cache->bucket_count > 0) {
        uint32_t key = hash ^ cache_hash_name(name);
        uint32_t mask = cache->bucket_count - 1;
        uint32_t slot = key & mask;

        while (cache->buckets[slot].used) {
            uint32_t idx = cache->buckets[slot].index;
            if (cache->items[idx].hash == hash &&
                strcmp(cache->items[idx].name, name) == 0)
                return &cache->items[idx];
            slot = (slot + 1) & mask;
        }
        return NULL;
    }

    for (uint32_t i = 0; i < cache->item_count; i++) {
        if (cache->items[i].hash == hash &&
            strcmp(cache->items[i].name, name) == 0)
            return &cache->items[i];
    }

    return NULL;
}

void nipc_service_common_cgroups_cache_status(const nipc_cgroups_cache_t *cache,
                                              nipc_cgroups_cache_status_t *out)
{
    out->populated = cache->populated;
    out->item_count = cache->item_count;
    out->systemd_enabled = cache->systemd_enabled;
    out->generation = cache->generation;
    out->refresh_success_count = cache->refresh_success_count;
    out->refresh_failure_count = cache->refresh_failure_count;
    out->connection_state = cache->client.state;
    out->last_refresh_ts = cache->last_refresh_ts;
}

void nipc_service_common_cgroups_cache_close(nipc_cgroups_cache_t *cache)
{
    cache_free_items(cache->items, cache->item_count);
    cache->items = NULL;
    cache->item_count = 0;
    cache->populated = false;

    free(cache->buckets);
    cache->buckets = NULL;
    cache->bucket_count = 0;

    free(cache->response_buf);
    cache->response_buf = NULL;
    cache->response_buf_size = 0;

    nipc_client_close(&cache->client);
}
