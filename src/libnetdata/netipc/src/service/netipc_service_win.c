/*
 * netipc_service_win.c - L2 orchestration for Windows.
 *
 * Pure composition of L1 (Named Pipe / Win SHM) + Codec.
 * Identical state machine and retry logic as the POSIX implementation,
 * using Windows transport calls instead of UDS/POSIX SHM.
 *
 * Client context manages connection lifecycle with at-least-once retry.
 * Managed server handles accept, read, dispatch, respond.
 */

#ifdef _WIN32

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_win_shm.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* WaitForSingleObject timeout for server poll loops (ms) */
#define SERVER_POLL_TIMEOUT_MS 500

/* ------------------------------------------------------------------ */
/*  Internal: client connection helpers                                */
/* ------------------------------------------------------------------ */

/* Tear down the current connection (Named Pipe session + Win SHM). */
static void client_disconnect(nipc_client_ctx_t *ctx)
{
    if (ctx->shm) {
        nipc_win_shm_close(ctx->shm);
        free(ctx->shm);
        ctx->shm = NULL;
    }

    if (ctx->session_valid) {
        nipc_np_close_session(&ctx->session);
        ctx->session_valid = false;
    }
}

/* Attempt a full connection: Named Pipe connect + handshake, then
 * Win SHM upgrade if negotiated. Returns the new state. */
static nipc_client_state_t client_try_connect(nipc_client_ctx_t *ctx)
{
    nipc_np_session_t session;
    memset(&session, 0, sizeof(session));
    session.pipe = INVALID_HANDLE_VALUE;

    nipc_np_error_t err = nipc_np_connect(
        ctx->run_dir, ctx->service_name,
        &ctx->transport_config, &session);

    switch (err) {
    case NIPC_NP_OK:
        break;
    case NIPC_NP_ERR_CONNECT:
        return NIPC_CLIENT_NOT_FOUND;
    case NIPC_NP_ERR_AUTH_FAILED:
        return NIPC_CLIENT_AUTH_FAILED;
    case NIPC_NP_ERR_NO_PROFILE:
        return NIPC_CLIENT_INCOMPATIBLE;
    default:
        return NIPC_CLIENT_DISCONNECTED;
    }

    ctx->session = session;
    ctx->session_valid = true;

    /* Win SHM upgrade if negotiated */
    if (session.selected_profile == NIPC_WIN_SHM_PROFILE_HYBRID ||
        session.selected_profile == NIPC_WIN_SHM_PROFILE_BUSYWAIT) {

        nipc_win_shm_ctx_t *shm = calloc(1, sizeof(nipc_win_shm_ctx_t));
        if (shm) {
            /* Retry attach: server creates the SHM region after
             * the NP handshake, so it may not exist yet. */
            nipc_win_shm_error_t serr = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
            for (int i = 0; i < 200; i++) {
                serr = nipc_win_shm_client_attach(
                    ctx->run_dir, ctx->service_name,
                    ctx->transport_config.auth_token,
                    session.selected_profile,
                    shm);
                if (serr == NIPC_WIN_SHM_OK)
                    break;
                if (serr == NIPC_WIN_SHM_ERR_OPEN_MAPPING ||
                    serr == NIPC_WIN_SHM_ERR_OPEN_EVENT ||
                    serr == NIPC_WIN_SHM_ERR_BAD_MAGIC)
                    Sleep(5); /* 5ms retry */
                else
                    break;
            }

            if (serr == NIPC_WIN_SHM_OK) {
                ctx->shm = shm;
            } else {
                /* SHM attach failed -- fall back to Named Pipe only. */
                free(shm);
            }
        }
    }

    return NIPC_CLIENT_READY;
}

/* ------------------------------------------------------------------ */
/*  Internal: send/receive via the active transport                    */
/* ------------------------------------------------------------------ */

static nipc_error_t transport_send(nipc_client_ctx_t *ctx,
                                    nipc_header_t *hdr,
                                    const void *payload,
                                    size_t payload_len)
{
    if (ctx->shm) {
        /* SHM: build the full message (header + payload) */
        size_t msg_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *msg = malloc(msg_len);
        if (!msg)
            return NIPC_ERR_OVERFLOW;

        hdr->magic      = NIPC_MAGIC_MSG;
        hdr->version    = NIPC_VERSION;
        hdr->header_len = NIPC_HEADER_LEN;
        hdr->payload_len = (uint32_t)payload_len;

        nipc_header_encode(hdr, msg, NIPC_HEADER_LEN);
        if (payload_len > 0)
            memcpy(msg + NIPC_HEADER_LEN, payload, payload_len);

        nipc_win_shm_error_t serr = nipc_win_shm_send(ctx->shm, msg, msg_len);
        free(msg);
        return (serr == NIPC_WIN_SHM_OK) ? NIPC_OK : NIPC_ERR_OVERFLOW;
    }

    /* Named Pipe path */
    nipc_np_error_t uerr = nipc_np_send(&ctx->session, hdr,
                                          payload, payload_len);
    return (uerr == NIPC_NP_OK) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

static nipc_error_t transport_receive(nipc_client_ctx_t *ctx,
                                       void *buf, size_t buf_size,
                                       nipc_header_t *hdr_out,
                                       const void **payload_out,
                                       size_t *payload_len_out)
{
    if (ctx->shm) {
        size_t msg_len;
        nipc_win_shm_error_t serr = nipc_win_shm_receive(ctx->shm, buf, buf_size,
                                                            &msg_len, 30000);
        if (serr != NIPC_WIN_SHM_OK)
            return NIPC_ERR_TRUNCATED;

        if (msg_len < NIPC_HEADER_LEN)
            return NIPC_ERR_TRUNCATED;

        nipc_error_t perr = nipc_header_decode(buf, msg_len, hdr_out);
        if (perr != NIPC_OK)
            return perr;

        *payload_out = (const uint8_t *)buf + NIPC_HEADER_LEN;
        *payload_len_out = msg_len - NIPC_HEADER_LEN;
        return NIPC_OK;
    }

    /* Named Pipe path */
    nipc_np_error_t uerr = nipc_np_receive(&ctx->session, buf, buf_size,
                                             hdr_out, payload_out,
                                             payload_len_out);
    return (uerr == NIPC_NP_OK) ? NIPC_OK : NIPC_ERR_TRUNCATED;
}

/* ------------------------------------------------------------------ */
/*  Internal: single attempt at a cgroups snapshot call                */
/* ------------------------------------------------------------------ */

static nipc_error_t do_cgroups_call(nipc_client_ctx_t *ctx,
                                     uint8_t *request_buf,
                                     uint8_t *response_buf,
                                     size_t response_buf_size,
                                     nipc_cgroups_resp_view_t *view_out)
{
    /* 1. Encode request using Codec */
    nipc_cgroups_req_t req = { .layout_version = 1, .flags = 0 };
    size_t req_len = nipc_cgroups_req_encode(&req, request_buf, 4);
    if (req_len == 0)
        return NIPC_ERR_TRUNCATED;

    /* 2. Build outer header */
    nipc_header_t hdr = {0};
    hdr.kind             = NIPC_KIND_REQUEST;
    hdr.code             = NIPC_METHOD_CGROUPS_SNAPSHOT;
    hdr.flags            = 0;
    hdr.item_count       = 1;
    hdr.message_id       = (uint64_t)(ctx->call_count + 1);
    hdr.transport_status = NIPC_STATUS_OK;

    /* 3. Send via L1 */
    nipc_error_t err = transport_send(ctx, &hdr, request_buf, req_len);
    if (err != NIPC_OK)
        return err;

    /* 4. Receive via L1 */
    nipc_header_t resp_hdr;
    const void *payload;
    size_t payload_len;
    err = transport_receive(ctx, response_buf, response_buf_size,
                            &resp_hdr, &payload, &payload_len);
    if (err != NIPC_OK)
        return err;

    /* 5. Check transport_status BEFORE any decode (spec requirement) */
    if (resp_hdr.transport_status != NIPC_STATUS_OK) {
        switch (resp_hdr.transport_status) {
        case NIPC_STATUS_INTERNAL_ERROR:
            return NIPC_ERR_BAD_LAYOUT;
        default:
            return NIPC_ERR_BAD_LAYOUT;
        }
    }

    /* 6. Decode response using Codec */
    err = nipc_cgroups_resp_decode(payload, payload_len, view_out);
    return err;
}

/* ------------------------------------------------------------------ */
/*  Public API: client lifecycle                                       */
/* ------------------------------------------------------------------ */

void nipc_client_init(nipc_client_ctx_t *ctx,
                      const char *run_dir,
                      const char *service_name,
                      const nipc_np_client_config_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = NIPC_CLIENT_DISCONNECTED;
    ctx->session.pipe = INVALID_HANDLE_VALUE;
    ctx->session_valid = false;
    ctx->shm = NULL;

    if (run_dir) {
        size_t len = strlen(run_dir);
        if (len >= sizeof(ctx->run_dir))
            len = sizeof(ctx->run_dir) - 1;
        memcpy(ctx->run_dir, run_dir, len);
        ctx->run_dir[len] = '\0';
    }

    if (service_name) {
        size_t len = strlen(service_name);
        if (len >= sizeof(ctx->service_name))
            len = sizeof(ctx->service_name) - 1;
        memcpy(ctx->service_name, service_name, len);
        ctx->service_name[len] = '\0';
    }

    if (config)
        ctx->transport_config = *config;
}

bool nipc_client_refresh(nipc_client_ctx_t *ctx)
{
    nipc_client_state_t old_state = ctx->state;

    switch (ctx->state) {
    case NIPC_CLIENT_DISCONNECTED:
    case NIPC_CLIENT_NOT_FOUND:
        ctx->state = NIPC_CLIENT_CONNECTING;
        ctx->state = client_try_connect(ctx);
        if (ctx->state == NIPC_CLIENT_READY)
            ctx->connect_count++;
        break;

    case NIPC_CLIENT_BROKEN:
        client_disconnect(ctx);
        ctx->state = NIPC_CLIENT_CONNECTING;
        ctx->state = client_try_connect(ctx);
        if (ctx->state == NIPC_CLIENT_READY)
            ctx->reconnect_count++;
        break;

    case NIPC_CLIENT_READY:
    case NIPC_CLIENT_CONNECTING:
    case NIPC_CLIENT_AUTH_FAILED:
    case NIPC_CLIENT_INCOMPATIBLE:
        break;
    }

    return ctx->state != old_state;
}

void nipc_client_status(const nipc_client_ctx_t *ctx,
                        nipc_client_status_t *out)
{
    out->state           = ctx->state;
    out->connect_count   = ctx->connect_count;
    out->reconnect_count = ctx->reconnect_count;
    out->call_count      = ctx->call_count;
    out->error_count     = ctx->error_count;
}

void nipc_client_close(nipc_client_ctx_t *ctx)
{
    client_disconnect(ctx);
    ctx->state = NIPC_CLIENT_DISCONNECTED;
}

/* ------------------------------------------------------------------ */
/*  Public API: typed cgroups snapshot call                            */
/* ------------------------------------------------------------------ */

nipc_error_t nipc_client_call_cgroups_snapshot(
    nipc_client_ctx_t *ctx,
    uint8_t *request_buf,
    uint8_t *response_buf,
    size_t response_buf_size,
    nipc_cgroups_resp_view_t *view_out)
{
    /* Fail fast if not READY */
    if (ctx->state != NIPC_CLIENT_READY) {
        ctx->error_count++;
        return NIPC_ERR_BAD_LAYOUT;
    }

    bool was_ready = true;

    /* First attempt */
    nipc_error_t err = do_cgroups_call(ctx, request_buf,
                                        response_buf, response_buf_size,
                                        view_out);
    if (err == NIPC_OK) {
        ctx->call_count++;
        return NIPC_OK;
    }

    /* Call failed. If previously READY: disconnect, reconnect, retry ONCE */
    if (was_ready) {
        client_disconnect(ctx);
        ctx->state = NIPC_CLIENT_BROKEN;

        /* Reconnect (full handshake) */
        ctx->state = client_try_connect(ctx);
        if (ctx->state != NIPC_CLIENT_READY) {
            ctx->error_count++;
            return err;
        }
        ctx->reconnect_count++;

        /* Retry once */
        err = do_cgroups_call(ctx, request_buf,
                              response_buf, response_buf_size,
                              view_out);
        if (err == NIPC_OK) {
            ctx->call_count++;
            return NIPC_OK;
        }

        /* Retry also failed */
        client_disconnect(ctx);
        ctx->state = NIPC_CLIENT_BROKEN;
    }

    ctx->error_count++;
    return err;
}

/* ------------------------------------------------------------------ */
/*  Internal: managed server session handler                           */
/* ------------------------------------------------------------------ */

static void server_handle_session(nipc_managed_server_t *server,
                                   nipc_np_session_t *session,
                                   nipc_win_shm_ctx_t *shm)
{
    uint8_t recv_buf[65536];
    uint8_t *resp_buf = server->response_buf;
    size_t resp_buf_size = server->response_buf_size;

    while (server->running) {
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        /* Receive request via the active transport */
        if (shm) {
            size_t msg_len;
            nipc_win_shm_error_t serr = nipc_win_shm_receive(shm, recv_buf, sizeof(recv_buf),
                                                               &msg_len, SERVER_POLL_TIMEOUT_MS);
            if (serr == NIPC_WIN_SHM_ERR_TIMEOUT)
                continue;
            if (serr != NIPC_WIN_SHM_OK)
                break;
            if (msg_len < NIPC_HEADER_LEN)
                break;

            nipc_error_t perr = nipc_header_decode(recv_buf, msg_len, &hdr);
            if (perr != NIPC_OK)
                break;

            payload = recv_buf + NIPC_HEADER_LEN;
            payload_len = msg_len - NIPC_HEADER_LEN;
        } else {
            /* Named Pipe path: use WaitForSingleObject for poll-like behavior */
            DWORD wait_result = WaitForSingleObject(session->pipe, SERVER_POLL_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                continue; /* check running flag */
            }
            if (wait_result != WAIT_OBJECT_0) {
                break; /* error */
            }

            nipc_np_error_t uerr = nipc_np_receive(
                session, recv_buf, sizeof(recv_buf),
                &hdr, &payload, &payload_len);
            if (uerr != NIPC_NP_OK)
                break;
        }

        /* Skip non-request messages */
        if (hdr.kind != NIPC_KIND_REQUEST)
            continue;

        /* Dispatch to handler */
        size_t response_len = 0;
        bool ok = server->handler(
            server->handler_user,
            hdr.code,
            (const uint8_t *)payload, payload_len,
            resp_buf, resp_buf_size,
            &response_len);

        /* Build response header */
        nipc_header_t resp_hdr = {0};
        resp_hdr.kind       = NIPC_KIND_RESPONSE;
        resp_hdr.code       = hdr.code;
        resp_hdr.message_id = hdr.message_id;
        resp_hdr.item_count = 1;
        resp_hdr.flags      = 0;

        if (ok) {
            resp_hdr.transport_status = NIPC_STATUS_OK;
        } else {
            resp_hdr.transport_status = NIPC_STATUS_INTERNAL_ERROR;
            response_len = 0;
        }

        /* Send response via the active transport */
        if (shm) {
            size_t msg_len = NIPC_HEADER_LEN + response_len;

            resp_hdr.magic      = NIPC_MAGIC_MSG;
            resp_hdr.version    = NIPC_VERSION;
            resp_hdr.header_len = NIPC_HEADER_LEN;
            resp_hdr.payload_len = (uint32_t)response_len;

            uint8_t stack_msg[4096];
            uint8_t *msg = (msg_len <= sizeof(stack_msg)) ? stack_msg : malloc(msg_len);
            if (!msg)
                break;

            nipc_header_encode(&resp_hdr, msg, NIPC_HEADER_LEN);
            if (response_len > 0)
                memcpy(msg + NIPC_HEADER_LEN, resp_buf, response_len);

            nipc_win_shm_error_t serr = nipc_win_shm_send(shm, msg, msg_len);
            if (msg != stack_msg)
                free(msg);
            if (serr != NIPC_WIN_SHM_OK)
                break;
        } else {
            nipc_np_error_t uerr = nipc_np_send(
                session, &resp_hdr, resp_buf, response_len);
            if (uerr != NIPC_NP_OK)
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API: managed server                                         */
/* ------------------------------------------------------------------ */

nipc_error_t nipc_server_init(nipc_managed_server_t *server,
                               const char *run_dir,
                               const char *service_name,
                               const nipc_np_server_config_t *config,
                               int worker_count,
                               size_t response_buf_size,
                               nipc_server_handler_fn handler,
                               void *user)
{
    memset(server, 0, sizeof(*server));
    server->listener.pipe = INVALID_HANDLE_VALUE;
    server->running = false;

    if (!run_dir || !service_name || !handler || worker_count < 1)
        return NIPC_ERR_BAD_LAYOUT;

    /* Store config */
    {
        size_t len = strlen(run_dir);
        if (len >= sizeof(server->run_dir))
            len = sizeof(server->run_dir) - 1;
        memcpy(server->run_dir, run_dir, len);
        server->run_dir[len] = '\0';
    }
    {
        size_t len = strlen(service_name);
        if (len >= sizeof(server->service_name))
            len = sizeof(server->service_name) - 1;
        memcpy(server->service_name, service_name, len);
        server->service_name[len] = '\0';
    }

    server->handler = handler;
    server->handler_user = user;
    server->worker_count = worker_count;
    server->auth_token = config->auth_token;

    /* Allocate response buffer */
    server->response_buf_size = response_buf_size;
    server->response_buf = malloc(response_buf_size);
    if (!server->response_buf)
        return NIPC_ERR_OVERFLOW;

    /* Start listening via L1 */
    nipc_np_error_t uerr = nipc_np_listen(
        run_dir, service_name, config, &server->listener);
    if (uerr != NIPC_NP_OK) {
        free(server->response_buf);
        server->response_buf = NULL;
        return NIPC_ERR_BAD_LAYOUT;
    }

    return NIPC_OK;
}

void nipc_server_run(nipc_managed_server_t *server)
{
    server->running = true;

    while (server->running) {
        /* Accept one client via L1 (blocking with internal timeout) */
        nipc_np_session_t session;
        memset(&session, 0, sizeof(session));
        session.pipe = INVALID_HANDLE_VALUE;

        nipc_np_error_t uerr = nipc_np_accept(&server->listener, &session);
        if (uerr != NIPC_NP_OK) {
            if (!server->running)
                break;
            Sleep(10);
            continue;
        }

        /* Win SHM upgrade if negotiated */
        nipc_win_shm_ctx_t *shm = NULL;
        if (session.selected_profile == NIPC_WIN_SHM_PROFILE_HYBRID ||
            session.selected_profile == NIPC_WIN_SHM_PROFILE_BUSYWAIT) {

            nipc_win_shm_ctx_t *s = calloc(1, sizeof(nipc_win_shm_ctx_t));
            if (s) {
                nipc_win_shm_error_t serr = nipc_win_shm_server_create(
                    server->run_dir, server->service_name,
                    server->auth_token,
                    session.selected_profile,
                    session.max_request_payload_bytes + NIPC_HEADER_LEN,
                    session.max_response_payload_bytes + NIPC_HEADER_LEN,
                    s);
                if (serr == NIPC_WIN_SHM_OK)
                    shm = s;
                else
                    free(s);
            }
        }

        /* Handle this session (blocking, single-threaded) */
        server_handle_session(server, &session, shm);

        /* Cleanup */
        if (shm) {
            nipc_win_shm_destroy(shm);
            free(shm);
        }
        nipc_np_close_session(&session);
    }
}

void nipc_server_stop(nipc_managed_server_t *server)
{
    server->running = false;
}

void nipc_server_destroy(nipc_managed_server_t *server)
{
    server->running = false;
    nipc_np_close_listener(&server->listener);

    free(server->response_buf);
    server->response_buf = NULL;
    server->response_buf_size = 0;

    free(server->workers);
    server->workers = NULL;
    server->worker_count = 0;
}

/* ------------------------------------------------------------------ */
/*  L3: Client-side cgroups snapshot cache                             */
/* ------------------------------------------------------------------ */

/* Free all owned strings in cache items and the items array itself. */
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

static nipc_cgroups_cache_item_t *cache_build_items(
    const nipc_cgroups_resp_view_t *view,
    uint32_t *count_out)
{
    uint32_t n = view->item_count;
    *count_out = 0;

    if (n == 0)
        return NULL;

    nipc_cgroups_cache_item_t *items = calloc(n, sizeof(nipc_cgroups_cache_item_t));
    if (!items)
        return NULL;

    for (uint32_t i = 0; i < n; i++) {
        nipc_cgroups_item_view_t iv;
        nipc_error_t err = nipc_cgroups_resp_item(view, i, &iv);
        if (err != NIPC_OK) {
            cache_free_items(items, i);
            return NULL;
        }

        items[i].hash    = iv.hash;
        items[i].options = iv.options;
        items[i].enabled = iv.enabled;

        items[i].name = malloc(iv.name.len + 1);
        if (!items[i].name) {
            cache_free_items(items, i);
            return NULL;
        }
        if (iv.name.len > 0)
            memcpy(items[i].name, iv.name.ptr, iv.name.len);
        items[i].name[iv.name.len] = '\0';

        items[i].path = malloc(iv.path.len + 1);
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

void nipc_cgroups_cache_init(nipc_cgroups_cache_t *cache,
                              const char *run_dir,
                              const char *service_name,
                              const nipc_np_client_config_t *config)
{
    memset(cache, 0, sizeof(*cache));

    nipc_client_init(&cache->client, run_dir, service_name, config);

    cache->items = NULL;
    cache->item_count = 0;
    cache->systemd_enabled = 0;
    cache->generation = 0;
    cache->populated = false;
    cache->refresh_success_count = 0;
    cache->refresh_failure_count = 0;

    cache->response_buf_size = NIPC_CGROUPS_CACHE_BUF_SIZE;
    cache->response_buf = malloc(cache->response_buf_size);
}

bool nipc_cgroups_cache_refresh(nipc_cgroups_cache_t *cache)
{
    if (!cache->response_buf) {
        cache->refresh_failure_count++;
        return false;
    }

    nipc_client_refresh(&cache->client);

    uint8_t req_buf[4];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &cache->client, req_buf,
        cache->response_buf, cache->response_buf_size,
        &view);

    if (err != NIPC_OK) {
        cache->refresh_failure_count++;
        return false;
    }

    uint32_t new_count = 0;
    nipc_cgroups_cache_item_t *new_items = NULL;

    if (view.item_count > 0) {
        new_items = cache_build_items(&view, &new_count);
        if (!new_items && view.item_count > 0) {
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

    return true;
}

const nipc_cgroups_cache_item_t *nipc_cgroups_cache_lookup(
    const nipc_cgroups_cache_t *cache,
    uint32_t hash,
    const char *name)
{
    if (!cache->populated || !cache->items || !name)
        return NULL;

    for (uint32_t i = 0; i < cache->item_count; i++) {
        if (cache->items[i].hash == hash &&
            strcmp(cache->items[i].name, name) == 0) {
            return &cache->items[i];
        }
    }

    return NULL;
}

void nipc_cgroups_cache_status(const nipc_cgroups_cache_t *cache,
                                nipc_cgroups_cache_status_t *out)
{
    out->populated             = cache->populated;
    out->item_count            = cache->item_count;
    out->systemd_enabled       = cache->systemd_enabled;
    out->generation            = cache->generation;
    out->refresh_success_count = cache->refresh_success_count;
    out->refresh_failure_count = cache->refresh_failure_count;
}

void nipc_cgroups_cache_close(nipc_cgroups_cache_t *cache)
{
    cache_free_items(cache->items, cache->item_count);
    cache->items = NULL;
    cache->item_count = 0;
    cache->populated = false;

    free(cache->response_buf);
    cache->response_buf = NULL;
    cache->response_buf_size = 0;

    nipc_client_close(&cache->client);
}

#endif /* _WIN32 */
