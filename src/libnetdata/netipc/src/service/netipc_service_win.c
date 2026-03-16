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
#include <process.h>
#include <windows.h>

/* WaitForSingleObject timeout for server poll loops (ms) */
#define SERVER_POLL_TIMEOUT_MS 100

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
                    session.session_id,
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
                /* SHM attach failed after retries. The handshake selected
                 * SHM but we can't use it. Fail the session to avoid
                 * transport desync (server on SHM, client on NP). */
                free(shm);
                nipc_np_close_session(&ctx->session);
                ctx->session_valid = false;
                return NIPC_CLIENT_DISCONNECTED;
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
/*  Internal: generic raw call (send request, receive response)        */
/* ------------------------------------------------------------------ */

/*
 * Single-attempt raw call: build envelope, send, receive, validate
 * envelope. The caller handles encode before and decode after.
 *
 * On success, response_payload_out and response_len_out point into
 * response_buf (valid until next call on this context).
 */
static nipc_error_t do_raw_call(nipc_client_ctx_t *ctx,
                                 uint16_t method_code,
                                 const void *request_payload,
                                 size_t request_len,
                                 void *response_buf,
                                 size_t response_buf_size,
                                 const void **response_payload_out,
                                 size_t *response_len_out)
{
    nipc_header_t hdr = {0};
    hdr.kind             = NIPC_KIND_REQUEST;
    hdr.code             = method_code;
    hdr.flags            = 0;
    hdr.item_count       = 1;
    hdr.message_id       = (uint64_t)(ctx->call_count + 1);
    hdr.transport_status = NIPC_STATUS_OK;

    nipc_error_t err = transport_send(ctx, &hdr, request_payload, request_len);
    if (err != NIPC_OK)
        return err;

    nipc_header_t resp_hdr;
    err = transport_receive(ctx, response_buf, response_buf_size,
                            &resp_hdr, response_payload_out, response_len_out);
    if (err != NIPC_OK)
        return err;

    if (resp_hdr.kind != NIPC_KIND_RESPONSE)
        return NIPC_ERR_BAD_KIND;
    if (resp_hdr.code != method_code)
        return NIPC_ERR_BAD_LAYOUT;
    if (resp_hdr.message_id != hdr.message_id)
        return NIPC_ERR_BAD_LAYOUT;
    if (resp_hdr.transport_status != NIPC_STATUS_OK)
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

/*
 * Generic call-with-retry: try once, if it fails and previously READY,
 * disconnect, reconnect, retry once. The caller provides a function
 * pointer for the single-attempt logic.
 */
typedef nipc_error_t (*nipc_attempt_fn)(nipc_client_ctx_t *ctx, void *state);

static nipc_error_t call_with_retry(nipc_client_ctx_t *ctx,
                                     nipc_attempt_fn attempt,
                                     void *state)
{
    if (ctx->state != NIPC_CLIENT_READY) {
        ctx->error_count++;
        return NIPC_ERR_NOT_READY;
    }

    nipc_error_t err = attempt(ctx, state);
    if (err == NIPC_OK) {
        ctx->call_count++;
        return NIPC_OK;
    }

    /* Retry once: disconnect, reconnect, resend */
    client_disconnect(ctx);
    ctx->state = NIPC_CLIENT_BROKEN;
    ctx->state = client_try_connect(ctx);
    if (ctx->state != NIPC_CLIENT_READY) {
        ctx->error_count++;
        return err;
    }
    ctx->reconnect_count++;

    err = attempt(ctx, state);
    if (err == NIPC_OK) {
        ctx->call_count++;
        return NIPC_OK;
    }

    client_disconnect(ctx);
    ctx->state = NIPC_CLIENT_BROKEN;
    ctx->error_count++;
    return err;
}

/* ------------------------------------------------------------------ */
/*  Internal: single attempt at a cgroups snapshot call                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *response_buf;
    size_t   response_buf_size;
    nipc_cgroups_resp_view_t *view_out;
} cgroups_call_state_t;

static nipc_error_t do_cgroups_attempt(nipc_client_ctx_t *ctx, void *state)
{
    cgroups_call_state_t *s = (cgroups_call_state_t *)state;

    nipc_cgroups_req_t req = { .layout_version = 1, .flags = 0 };
    uint8_t req_buf[4];
    size_t req_len = nipc_cgroups_req_encode(&req, req_buf, sizeof(req_buf));
    if (req_len == 0)
        return NIPC_ERR_TRUNCATED;

    const void *payload;
    size_t payload_len;
    nipc_error_t err = do_raw_call(ctx, NIPC_METHOD_CGROUPS_SNAPSHOT,
                                     req_buf, req_len,
                                     s->response_buf, s->response_buf_size,
                                     &payload, &payload_len);
    if (err != NIPC_OK)
        return err;

    return nipc_cgroups_resp_decode(payload, payload_len, s->view_out);
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
    (void)request_buf; /* request is encoded internally */

    cgroups_call_state_t state = {
        .response_buf      = response_buf,
        .response_buf_size = response_buf_size,
        .view_out          = view_out,
    };
    return call_with_retry(ctx, do_cgroups_attempt, &state);
}

/* ------------------------------------------------------------------ */
/*  Public API: typed INCREMENT call                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t  request_value;
    uint8_t  *response_buf;
    size_t    response_buf_size;
    uint64_t *value_out;
} increment_call_state_t;

static nipc_error_t do_increment_attempt(nipc_client_ctx_t *ctx, void *state)
{
    increment_call_state_t *s = (increment_call_state_t *)state;

    uint8_t req_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
    size_t req_len = nipc_increment_encode(s->request_value,
                                            req_buf, sizeof(req_buf));
    if (req_len == 0)
        return NIPC_ERR_TRUNCATED;

    const void *payload;
    size_t payload_len;
    nipc_error_t err = do_raw_call(ctx, NIPC_METHOD_INCREMENT,
                                     req_buf, req_len,
                                     s->response_buf, s->response_buf_size,
                                     &payload, &payload_len);
    if (err != NIPC_OK)
        return err;

    return nipc_increment_decode(payload, payload_len, s->value_out);
}

nipc_error_t nipc_client_call_increment(
    nipc_client_ctx_t *ctx,
    uint64_t request_value,
    uint8_t *response_buf,
    size_t response_buf_size,
    uint64_t *value_out)
{
    increment_call_state_t state = {
        .request_value     = request_value,
        .response_buf      = response_buf,
        .response_buf_size = response_buf_size,
        .value_out         = value_out,
    };
    return call_with_retry(ctx, do_increment_attempt, &state);
}

/* ------------------------------------------------------------------ */
/*  Public API: batch INCREMENT call                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint64_t *request_values;
    uint32_t        count;
    uint64_t       *response_values;
    uint8_t        *response_buf;
    size_t          response_buf_size;
} increment_batch_state_t;

static nipc_error_t do_increment_batch_attempt(nipc_client_ctx_t *ctx,
                                                 void *state)
{
    increment_batch_state_t *s = (increment_batch_state_t *)state;

    /* Build batch request payload using batch builder */
    size_t req_buf_size = s->count * (8 + NIPC_INCREMENT_PAYLOAD_SIZE) + 64;
    uint8_t *req_buf = malloc(req_buf_size);
    if (!req_buf) return NIPC_ERR_OVERFLOW;

    nipc_batch_builder_t bb;
    nipc_batch_builder_init(&bb, req_buf, req_buf_size, s->count);

    for (uint32_t i = 0; i < s->count; i++) {
        uint8_t item[NIPC_INCREMENT_PAYLOAD_SIZE];
        nipc_increment_encode(s->request_values[i], item, sizeof(item));
        nipc_error_t err = nipc_batch_builder_add(&bb, item, sizeof(item));
        if (err != NIPC_OK) { free(req_buf); return err; }
    }

    uint32_t out_count;
    size_t req_len = nipc_batch_builder_finish(&bb, &out_count);

    /* Send as batch */
    nipc_header_t hdr = {0};
    hdr.kind       = NIPC_KIND_REQUEST;
    hdr.code       = NIPC_METHOD_INCREMENT;
    hdr.flags      = NIPC_FLAG_BATCH;
    hdr.item_count = s->count;
    hdr.message_id = (uint64_t)(ctx->call_count + 1);
    hdr.transport_status = NIPC_STATUS_OK;

    nipc_error_t err = transport_send(ctx, &hdr, req_buf, req_len);
    free(req_buf);
    if (err != NIPC_OK) return err;

    /* Receive batch response */
    nipc_header_t resp_hdr;
    const void *payload;
    size_t payload_len;
    err = transport_receive(ctx, s->response_buf, s->response_buf_size,
                            &resp_hdr, &payload, &payload_len);
    if (err != NIPC_OK) return err;

    if (resp_hdr.kind != NIPC_KIND_RESPONSE) return NIPC_ERR_BAD_KIND;
    if (resp_hdr.code != NIPC_METHOD_INCREMENT) return NIPC_ERR_BAD_LAYOUT;
    if (resp_hdr.transport_status != NIPC_STATUS_OK) return NIPC_ERR_BAD_LAYOUT;
    if (resp_hdr.item_count != s->count) return NIPC_ERR_BAD_ITEM_COUNT;

    /* Extract and decode each response item */
    for (uint32_t i = 0; i < s->count; i++) {
        const void *item_ptr;
        uint32_t item_len;
        err = nipc_batch_item_get(payload, payload_len, s->count, i,
                                   &item_ptr, &item_len);
        if (err != NIPC_OK) return err;

        err = nipc_increment_decode(item_ptr, item_len,
                                     &s->response_values[i]);
        if (err != NIPC_OK) return err;
    }

    return NIPC_OK;
}

nipc_error_t nipc_client_call_increment_batch(
    nipc_client_ctx_t *ctx,
    const uint64_t *request_values, uint32_t count,
    uint64_t *response_values,
    uint8_t *response_buf, size_t response_buf_size)
{
    increment_batch_state_t state = {
        .request_values    = request_values,
        .count             = count,
        .response_values   = response_values,
        .response_buf      = response_buf,
        .response_buf_size = response_buf_size,
    };
    return call_with_retry(ctx, do_increment_batch_attempt, &state);
}

/* ------------------------------------------------------------------ */
/*  Public API: typed STRING_REVERSE call                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *request_str;
    uint32_t    request_str_len;
    uint8_t    *response_buf;
    size_t      response_buf_size;
    nipc_string_reverse_view_t *view_out;
} string_reverse_call_state_t;

static nipc_error_t do_string_reverse_attempt(nipc_client_ctx_t *ctx,
                                                void *state)
{
    string_reverse_call_state_t *s = (string_reverse_call_state_t *)state;

    /* Encode request -- worst case buffer: header + string + NUL */
    size_t req_buf_size = NIPC_STRING_REVERSE_HDR_SIZE + s->request_str_len + 1;
    uint8_t stack_buf[256];
    uint8_t *req_buf = (req_buf_size <= sizeof(stack_buf))
                            ? stack_buf : malloc(req_buf_size);
    if (!req_buf)
        return NIPC_ERR_OVERFLOW;

    size_t req_len = nipc_string_reverse_encode(s->request_str,
                                                  s->request_str_len,
                                                  req_buf, req_buf_size);
    if (req_len == 0) {
        if (req_buf != stack_buf) free(req_buf);
        return NIPC_ERR_TRUNCATED;
    }

    const void *payload;
    size_t payload_len;
    nipc_error_t err = do_raw_call(ctx, NIPC_METHOD_STRING_REVERSE,
                                     req_buf, req_len,
                                     s->response_buf, s->response_buf_size,
                                     &payload, &payload_len);
    if (req_buf != stack_buf) free(req_buf);
    if (err != NIPC_OK)
        return err;

    return nipc_string_reverse_decode(payload, payload_len, s->view_out);
}

nipc_error_t nipc_client_call_string_reverse(
    nipc_client_ctx_t *ctx,
    const char *request_str,
    uint32_t request_str_len,
    uint8_t *response_buf,
    size_t response_buf_size,
    nipc_string_reverse_view_t *view_out)
{
    string_reverse_call_state_t state = {
        .request_str       = request_str,
        .request_str_len   = request_str_len,
        .response_buf      = response_buf,
        .response_buf_size = response_buf_size,
        .view_out          = view_out,
    };
    return call_with_retry(ctx, do_string_reverse_attempt, &state);
}

/* ------------------------------------------------------------------ */
/*  Internal: managed server session handler                           */
/* ------------------------------------------------------------------ */

/*
 * Handle one client session: read requests, dispatch to handler,
 * send responses. Each session gets its own response buffer.
 * Runs until the client disconnects or server stops.
 */
static void server_handle_session(nipc_managed_server_t *server,
                                   nipc_np_session_t *session,
                                   nipc_win_shm_ctx_t *shm,
                                   uint8_t *resp_buf,
                                   size_t resp_buf_size)
{
    /* Dynamically allocate recv buffer based on negotiated max */
    size_t recv_size = NIPC_HEADER_LEN + session->max_request_payload_bytes;
    uint8_t *recv_buf = malloc(recv_size);
    if (!recv_buf)
        return;

    while (InterlockedCompareExchange(&server->running, 0, 0)) {
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        /* Receive request via the active transport */
        if (shm) {
            size_t msg_len;
            nipc_win_shm_error_t serr = nipc_win_shm_receive(shm, recv_buf, recv_size,
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
                session, recv_buf, recv_size,
                &hdr, &payload, &payload_len);
            if (uerr != NIPC_NP_OK)
                break;
        }

        /* Protocol violation: unexpected message kind terminates session */
        if (hdr.kind != NIPC_KIND_REQUEST)
            break;

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

    free(recv_buf);
}

/* ------------------------------------------------------------------ */
/*  Internal: per-session handler thread                                */
/* ------------------------------------------------------------------ */

/* Thread function: handles one client session from accept to disconnect. */
static unsigned __stdcall session_handler_thread(void *arg)
{
    nipc_session_ctx_t *sctx = (nipc_session_ctx_t *)arg;
    nipc_managed_server_t *server = sctx->server;

    /* Allocate a per-session response buffer */
    uint8_t *resp_buf = malloc(server->response_buf_size);
    if (resp_buf) {
        server_handle_session(server, &sctx->session, sctx->shm,
                              resp_buf, server->response_buf_size);
        free(resp_buf);
    }

    /* Cleanup SHM and session */
    if (sctx->shm) {
        nipc_win_shm_destroy(sctx->shm);
        free(sctx->shm);
    }
    nipc_np_close_session(&sctx->session);

    /* Mark inactive; the reap/destroy path owns removal from the array */
    InterlockedExchange((volatile LONG *)&sctx->active, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Internal: reap finished session threads                             */
/* ------------------------------------------------------------------ */

/* Reap all finished (inactive) session threads. Called with lock held. */
static void server_reap_sessions_locked(nipc_managed_server_t *server)
{
    int i = 0;
    while (i < server->session_count) {
        nipc_session_ctx_t *s = server->sessions[i];
        if (!InterlockedCompareExchange((volatile LONG *)&s->active, 0, 0)) {
            WaitForSingleObject(s->thread, INFINITE);
            CloseHandle(s->thread);
            /* Swap with last, free */
            server->sessions[i] = server->sessions[server->session_count - 1];
            server->session_count--;
            free(s);
        } else {
            i++;
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
    InterlockedExchange(&server->running, 0);

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
    server->response_buf_size = response_buf_size;
    server->auth_token = config->auth_token;


    /* Initialize session tracking */
    server->session_capacity = worker_count * 2;
    if (server->session_capacity < 16)
        server->session_capacity = 16;
    server->sessions = calloc((size_t)server->session_capacity,
                              sizeof(nipc_session_ctx_t *));
    if (!server->sessions)
        return NIPC_ERR_OVERFLOW;
    server->session_count = 0;
    server->next_session_id = 1; /* spec: monotonic counter starting at 1 */
    InitializeCriticalSection(&server->sessions_lock);

    /* Clean up stale SHM kernel objects from previous crashes (no-op on
     * Windows but maintains API symmetry with the POSIX transport). */
    nipc_win_shm_cleanup_stale(run_dir, service_name);

    /* Start listening via L1 */
    nipc_np_error_t uerr = nipc_np_listen(
        run_dir, service_name, config, &server->listener);
    if (uerr != NIPC_NP_OK) {
        free(server->sessions);
        server->sessions = NULL;
        DeleteCriticalSection(&server->sessions_lock);
        return NIPC_ERR_BAD_LAYOUT;
    }

    return NIPC_OK;
}

void nipc_server_run(nipc_managed_server_t *server)
{
    InterlockedExchange(&server->running, 1);

    while (InterlockedCompareExchange(&server->running, 0, 0)) {
        /* Accept one client via L1 (blocking with internal timeout) */
        nipc_np_session_t session;
        memset(&session, 0, sizeof(session));
        session.pipe = INVALID_HANDLE_VALUE;

        uint64_t sid = server->next_session_id++;
        nipc_np_error_t uerr = nipc_np_accept(&server->listener, sid, &session);
        if (uerr != NIPC_NP_OK) {
            if (!InterlockedCompareExchange(&server->running, 0, 0))
                break;
            Sleep(10);
            continue;
        }

        /* Enforce worker_count limit: reap finished sessions, check count */
        EnterCriticalSection(&server->sessions_lock);
        server_reap_sessions_locked(server);

        if (server->session_count >= server->worker_count) {
            /* At capacity: reject this client by closing the session */
            LeaveCriticalSection(&server->sessions_lock);
            nipc_np_close_session(&session);
            continue;
        }

        /* Win SHM upgrade if negotiated. Each session gets its own
         * SHM region (per-session kernel objects via session_id). */
        nipc_win_shm_ctx_t *shm = NULL;
        if (session.selected_profile == NIPC_WIN_SHM_PROFILE_HYBRID ||
            session.selected_profile == NIPC_WIN_SHM_PROFILE_BUSYWAIT) {

            nipc_win_shm_ctx_t *s = calloc(1, sizeof(nipc_win_shm_ctx_t));
            if (s) {
                nipc_win_shm_error_t serr = nipc_win_shm_server_create(
                    server->run_dir, server->service_name,
                    server->auth_token,
                    session.session_id,
                    session.selected_profile,
                    session.max_request_payload_bytes + NIPC_HEADER_LEN,
                    session.max_response_payload_bytes + NIPC_HEADER_LEN,
                    s);
                if (serr == NIPC_WIN_SHM_OK) {
                    shm = s;
                } else {
                    /* SHM create failed for a session that negotiated SHM.
                     * Reject the session to avoid transport desync. */
                    free(s);
                    LeaveCriticalSection(&server->sessions_lock);
                    nipc_np_close_session(&session);
                    continue;
                }
            }
        }

        /* Create session context */
        nipc_session_ctx_t *sctx = calloc(1, sizeof(nipc_session_ctx_t));
        if (!sctx) {
            LeaveCriticalSection(&server->sessions_lock);
            if (shm) { nipc_win_shm_destroy(shm); free(shm); }
            nipc_np_close_session(&session);
            continue;
        }

        sctx->server = server;
        sctx->session = session;
        sctx->shm = shm;
        sctx->id = sid;
        InterlockedExchange((volatile LONG *)&sctx->active, 1);

        /* Grow session array if needed */
        if (server->session_count >= server->session_capacity) {
            int new_cap = server->session_capacity * 2;
            nipc_session_ctx_t **new_arr = realloc(
                server->sessions,
                (size_t)new_cap * sizeof(nipc_session_ctx_t *));
            if (!new_arr) {
                LeaveCriticalSection(&server->sessions_lock);
                if (shm) { nipc_win_shm_destroy(shm); free(shm); }
                nipc_np_close_session(&session);
                free(sctx);
                continue;
            }
            server->sessions = new_arr;
            server->session_capacity = new_cap;
        }

        server->sessions[server->session_count++] = sctx;
        LeaveCriticalSection(&server->sessions_lock);

        /* Spawn handler thread for this session */
        unsigned tid_unused;
        sctx->thread = (HANDLE)_beginthreadex(
            NULL, 0, session_handler_thread, sctx, 0, &tid_unused);
        if (sctx->thread == 0) {
            /* Thread creation failed: clean up */
            EnterCriticalSection(&server->sessions_lock);
            for (int i = 0; i < server->session_count; i++) {
                if (server->sessions[i] == sctx) {
                    server->sessions[i] = server->sessions[server->session_count - 1];
                    server->session_count--;
                    break;
                }
            }
            LeaveCriticalSection(&server->sessions_lock);

            if (shm) { nipc_win_shm_destroy(shm); free(shm); }
            nipc_np_close_session(&session);
            free(sctx);
        }
    }
}

void nipc_server_stop(nipc_managed_server_t *server)
{
    InterlockedExchange(&server->running, 0);
}

bool nipc_server_drain(nipc_managed_server_t *server, uint32_t timeout_ms)
{
    /* 1. Stop accepting new clients */
    InterlockedExchange(&server->running, 0);
    nipc_np_close_listener(&server->listener);

    /* 2. Wait for in-flight sessions to complete */
    bool all_drained = true;
    if (server->sessions) {
        DWORD deadline = GetTickCount() + timeout_ms;

        /* Poll until all sessions are inactive or timeout */
        while (1) {
            EnterCriticalSection(&server->sessions_lock);
            int active_count = 0;
            for (int i = 0; i < server->session_count; i++) {
                if (InterlockedCompareExchange(
                        (volatile LONG *)&server->sessions[i]->active, 0, 0))
                    active_count++;
            }
            LeaveCriticalSection(&server->sessions_lock);

            if (active_count == 0)
                break;

            if (GetTickCount() >= deadline) {
                /* Timeout: force-close session pipes to unblock threads */
                EnterCriticalSection(&server->sessions_lock);
                for (int i = 0; i < server->session_count; i++) {
                    nipc_session_ctx_t *s = server->sessions[i];
                    if (InterlockedCompareExchange(
                            (volatile LONG *)&s->active, 0, 0)) {
                        /* Close the pipe to unblock the recv call */
                        if (s->session.pipe != INVALID_HANDLE_VALUE) {
                            CancelIoEx(s->session.pipe, NULL);
                        }
                    }
                }
                LeaveCriticalSection(&server->sessions_lock);
                all_drained = false;
                break;
            }

            Sleep(5); /* 5ms poll interval */
        }

        /* 3. Join all session threads */
        EnterCriticalSection(&server->sessions_lock);
        for (int i = 0; i < server->session_count; i++) {
            nipc_session_ctx_t *s = server->sessions[i];
            LeaveCriticalSection(&server->sessions_lock);
            WaitForSingleObject(s->thread, INFINITE);
            CloseHandle(s->thread);
            free(s);
            EnterCriticalSection(&server->sessions_lock);
        }
        server->session_count = 0;
        LeaveCriticalSection(&server->sessions_lock);

        free(server->sessions);
        server->sessions = NULL;
        server->session_capacity = 0;
        DeleteCriticalSection(&server->sessions_lock);
    }

    server->worker_count = 0;

    return all_drained;
}

void nipc_server_destroy(nipc_managed_server_t *server)
{
    InterlockedExchange(&server->running, 0);
    nipc_np_close_listener(&server->listener);

    /* Join all active session threads */
    if (server->sessions) {
        EnterCriticalSection(&server->sessions_lock);
        for (int i = 0; i < server->session_count; i++) {
            nipc_session_ctx_t *s = server->sessions[i];
            LeaveCriticalSection(&server->sessions_lock);
            WaitForSingleObject(s->thread, INFINITE);
            CloseHandle(s->thread);
            free(s);
            EnterCriticalSection(&server->sessions_lock);
        }
        server->session_count = 0;
        LeaveCriticalSection(&server->sessions_lock);

        free(server->sessions);
        server->sessions = NULL;
        server->session_capacity = 0;
        DeleteCriticalSection(&server->sessions_lock);
    }

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

/* Round up to the next power of 2. Minimum 16. */
static uint32_t next_power_of_2(uint32_t n)
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

/* Hash a name string (djb2). Combined with item hash for bucket index. */
static uint32_t cache_hash_name(const char *name)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h;
}

/*
 * Build the open-addressing hash table from the items array.
 * Uses (item.hash ^ name_hash) as the probe key.
 * Load factor <= 0.5 (bucket_count >= 2 * item_count).
 */
static bool cache_build_hashtable(nipc_cgroups_cache_t *cache)
{
    free(cache->buckets);
    cache->buckets = NULL;
    cache->bucket_count = 0;

    if (cache->item_count == 0)
        return true;

    uint32_t bcount = next_power_of_2(cache->item_count * 2);
    nipc_cgroups_hash_bucket_t *buckets = calloc(bcount,
        sizeof(nipc_cgroups_hash_bucket_t));
    if (!buckets)
        return false;

    uint32_t mask = bcount - 1;
    for (uint32_t i = 0; i < cache->item_count; i++) {
        uint32_t key = cache->items[i].hash ^ cache_hash_name(cache->items[i].name);
        uint32_t slot = key & mask;

        /* Linear probe for an empty bucket */
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
    cache->buckets = NULL;
    cache->bucket_count = 0;
    cache->refresh_success_count = 0;
    cache->refresh_failure_count = 0;

    /* Allocate internal response buffer sized from negotiated limits.
     * Falls back to default if config specifies 0. */
    uint32_t max_resp = config ? config->max_response_payload_bytes : 0;
    cache->response_buf_size = (max_resp > 0)
        ? (size_t)max_resp + NIPC_HEADER_LEN
        : NIPC_CGROUPS_CACHE_BUF_SIZE_DEFAULT;
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

    /* Record monotonic timestamp (GetTickCount64 is always available on Windows) */
    cache->last_refresh_ts = GetTickCount64();

    /* Rebuild hash table for O(1) lookup */
    cache_build_hashtable(cache);

    return true;
}

const nipc_cgroups_cache_item_t *nipc_cgroups_cache_lookup(
    const nipc_cgroups_cache_t *cache,
    uint32_t hash,
    const char *name)
{
    if (!cache->populated || !cache->items || !name)
        return NULL;

    /* Use hash table if available, fall back to linear scan */
    if (cache->buckets && cache->bucket_count > 0) {
        uint32_t key = hash ^ cache_hash_name(name);
        uint32_t mask = cache->bucket_count - 1;
        uint32_t slot = key & mask;

        while (cache->buckets[slot].used) {
            uint32_t idx = cache->buckets[slot].index;
            if (cache->items[idx].hash == hash &&
                strcmp(cache->items[idx].name, name) == 0) {
                return &cache->items[idx];
            }
            slot = (slot + 1) & mask;
        }
        return NULL;
    }

    /* Fallback linear scan (hash table allocation failed) */
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
    out->connection_state      = cache->client.state;
    out->last_refresh_ts       = cache->last_refresh_ts;
}

void nipc_cgroups_cache_close(nipc_cgroups_cache_t *cache)
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

#endif /* _WIN32 */
