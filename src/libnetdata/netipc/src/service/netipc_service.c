/*
 * netipc_service.c - L2 orchestration implementation.
 *
 * Pure composition of L1 (UDS/SHM) + Codec. No direct socket/mmap calls
 * for data framing. Uses poll() for timeout-based shutdown detection
 * in the managed server.
 *
 * Client context manages connection lifecycle with at-least-once retry.
 * Managed server handles accept, read, dispatch, respond.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Poll timeout for server loops: 500ms between shutdown checks */
#define SERVER_POLL_TIMEOUT_MS 500

/* ------------------------------------------------------------------ */
/*  Internal: client connection helpers                                */
/* ------------------------------------------------------------------ */

/* Tear down the current connection (UDS session + SHM if any). */
static void client_disconnect(nipc_client_ctx_t *ctx)
{
    if (ctx->shm) {
        nipc_shm_close(ctx->shm);
        free(ctx->shm);
        ctx->shm = NULL;
    }

    if (ctx->session_valid) {
        nipc_uds_close_session(&ctx->session);
        ctx->session_valid = false;
    }
}

/* Attempt a full connection: UDS connect + handshake, then SHM upgrade
 * if negotiated. Returns the new state. */
static nipc_client_state_t client_try_connect(nipc_client_ctx_t *ctx)
{
    nipc_uds_session_t session;
    memset(&session, 0, sizeof(session));
    session.fd = -1;

    nipc_uds_error_t err = nipc_uds_connect(
        ctx->run_dir, ctx->service_name,
        &ctx->transport_config, &session);

    switch (err) {
    case NIPC_UDS_OK:
        break;
    case NIPC_UDS_ERR_CONNECT:
        return NIPC_CLIENT_NOT_FOUND;
    case NIPC_UDS_ERR_AUTH_FAILED:
        return NIPC_CLIENT_AUTH_FAILED;
    case NIPC_UDS_ERR_NO_PROFILE:
        return NIPC_CLIENT_INCOMPATIBLE;
    default:
        return NIPC_CLIENT_DISCONNECTED;
    }

    ctx->session = session;
    ctx->session_valid = true;

    /* SHM upgrade if negotiated */
    if (session.selected_profile == NIPC_PROFILE_SHM_HYBRID ||
        session.selected_profile == NIPC_PROFILE_SHM_FUTEX) {

        nipc_shm_ctx_t *shm = calloc(1, sizeof(nipc_shm_ctx_t));
        if (shm) {
            /* Retry attach: server creates the SHM region after
             * the UDS handshake, so it may not exist yet. */
            nipc_shm_error_t serr = NIPC_SHM_ERR_NOT_READY;
            for (int i = 0; i < 200; i++) {
                serr = nipc_shm_client_attach(
                    ctx->run_dir, ctx->service_name, shm);
                if (serr == NIPC_SHM_OK)
                    break;
                if (serr == NIPC_SHM_ERR_NOT_READY ||
                    serr == NIPC_SHM_ERR_OPEN ||
                    serr == NIPC_SHM_ERR_BAD_MAGIC)
                    usleep(5000); /* 5ms retry */
                else
                    break;
            }

            if (serr == NIPC_SHM_OK) {
                ctx->shm = shm;
            } else {
                /* SHM attach failed -- fall back to UDS only.
                 * The session is still valid for baseline transport. */
                free(shm);
            }
        }
    }

    return NIPC_CLIENT_READY;
}

/* ------------------------------------------------------------------ */
/*  Internal: send/receive via the active transport                    */
/* ------------------------------------------------------------------ */

/*
 * Send a complete message (header + payload) using whichever transport
 * is active: SHM if negotiated, UDS otherwise.
 */
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

        /* Fill envelope fields */
        hdr->magic      = NIPC_MAGIC_MSG;
        hdr->version    = NIPC_VERSION;
        hdr->header_len = NIPC_HEADER_LEN;
        hdr->payload_len = (uint32_t)payload_len;

        nipc_header_encode(hdr, msg, NIPC_HEADER_LEN);
        if (payload_len > 0)
            memcpy(msg + NIPC_HEADER_LEN, payload, payload_len);

        nipc_shm_error_t serr = nipc_shm_send(ctx->shm, msg, msg_len);
        free(msg);
        return (serr == NIPC_SHM_OK) ? NIPC_OK : NIPC_ERR_OVERFLOW;
    }

    /* UDS path */
    nipc_uds_error_t uerr = nipc_uds_send(&ctx->session, hdr,
                                            payload, payload_len);
    return (uerr == NIPC_UDS_OK) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/*
 * Receive a complete message. For SHM, reads from the SHM region.
 * For UDS, reads from the socket into the caller's buffer.
 *
 * On success, hdr_out is filled, and payload_out + payload_len_out
 * point to the payload bytes (valid until next receive).
 */
static nipc_error_t transport_receive(nipc_client_ctx_t *ctx,
                                       void *buf, size_t buf_size,
                                       nipc_header_t *hdr_out,
                                       const void **payload_out,
                                       size_t *payload_len_out)
{
    if (ctx->shm) {
        const void *msg;
        size_t msg_len;
        nipc_shm_error_t serr = nipc_shm_receive(ctx->shm, &msg, &msg_len, 30000);
        if (serr != NIPC_SHM_OK)
            return NIPC_ERR_TRUNCATED;

        if (msg_len < NIPC_HEADER_LEN)
            return NIPC_ERR_TRUNCATED;

        nipc_error_t perr = nipc_header_decode(msg, msg_len, hdr_out);
        if (perr != NIPC_OK)
            return perr;

        *payload_out = (const uint8_t *)msg + NIPC_HEADER_LEN;
        *payload_len_out = msg_len - NIPC_HEADER_LEN;
        return NIPC_OK;
    }

    /* UDS path */
    nipc_uds_error_t uerr = nipc_uds_receive(&ctx->session, buf, buf_size,
                                               hdr_out, payload_out,
                                               payload_len_out);
    return (uerr == NIPC_UDS_OK) ? NIPC_OK : NIPC_ERR_TRUNCATED;
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
        /* Map transport_status to error */
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
                      const nipc_uds_client_config_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = NIPC_CLIENT_DISCONNECTED;
    ctx->session.fd = -1;
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
        /* Attempt to connect */
        ctx->state = NIPC_CLIENT_CONNECTING;
        ctx->state = client_try_connect(ctx);
        if (ctx->state == NIPC_CLIENT_READY)
            ctx->connect_count++;
        break;

    case NIPC_CLIENT_BROKEN:
        /* Reconnect: tear down old connection first */
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
        /* No action needed */
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

    /* Call failed. If previously READY: disconnect, reconnect, retry ONCE
     * (spec: at-least-once, mandatory). */
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

/*
 * Wait for data on a file descriptor with periodic shutdown checks.
 * Returns: 1 = data ready, 0 = server stopping, -1 = error/hangup.
 */
static int poll_with_shutdown(int fd, volatile bool *running)
{
    while (*running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, SERVER_POLL_TIMEOUT_MS);

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (ret == 0)
            continue; /* timeout, check running flag */

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;

        if (pfd.revents & POLLIN)
            return 1;
    }
    return 0;
}

/*
 * Handle one client session: read requests, dispatch to handler,
 * send responses. Runs until the client disconnects or server stops.
 */
static void server_handle_session(nipc_managed_server_t *server,
                                   nipc_uds_session_t *session,
                                   nipc_shm_ctx_t *shm)
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
            const void *msg;
            size_t msg_len;
            nipc_shm_error_t serr = nipc_shm_receive(shm, &msg, &msg_len, 500);
            if (serr == NIPC_SHM_ERR_TIMEOUT)
                continue; /* check running flag */
            if (serr != NIPC_SHM_OK)
                break;
            if (msg_len < NIPC_HEADER_LEN)
                break;

            nipc_error_t perr = nipc_header_decode(msg, msg_len, &hdr);
            if (perr != NIPC_OK)
                break;

            payload = (const uint8_t *)msg + NIPC_HEADER_LEN;
            payload_len = msg_len - NIPC_HEADER_LEN;
        } else {
            /* Poll the session fd before blocking on receive */
            int pr = poll_with_shutdown(session->fd, &server->running);
            if (pr <= 0)
                break; /* shutdown or error */

            nipc_uds_error_t uerr = nipc_uds_receive(
                session, recv_buf, sizeof(recv_buf),
                &hdr, &payload, &payload_len);
            if (uerr != NIPC_UDS_OK)
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
            /* Handler failure: INTERNAL_ERROR + empty payload (spec) */
            resp_hdr.transport_status = NIPC_STATUS_INTERNAL_ERROR;
            response_len = 0;
        }

        /* Send response via the active transport */
        if (shm) {
            size_t msg_len = NIPC_HEADER_LEN + response_len;
            uint8_t *msg = malloc(msg_len);
            if (!msg)
                break;

            resp_hdr.magic      = NIPC_MAGIC_MSG;
            resp_hdr.version    = NIPC_VERSION;
            resp_hdr.header_len = NIPC_HEADER_LEN;
            resp_hdr.payload_len = (uint32_t)response_len;

            nipc_header_encode(&resp_hdr, msg, NIPC_HEADER_LEN);
            if (response_len > 0)
                memcpy(msg + NIPC_HEADER_LEN, resp_buf, response_len);

            nipc_shm_error_t serr = nipc_shm_send(shm, msg, msg_len);
            free(msg);
            if (serr != NIPC_SHM_OK)
                break;
        } else {
            nipc_uds_error_t uerr = nipc_uds_send(
                session, &resp_hdr, resp_buf, response_len);
            if (uerr != NIPC_UDS_OK)
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
                               const nipc_uds_server_config_t *config,
                               int worker_count,
                               size_t response_buf_size,
                               nipc_server_handler_fn handler,
                               void *user)
{
    memset(server, 0, sizeof(*server));
    server->listener.fd = -1;
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

    /* Allocate response buffer */
    server->response_buf_size = response_buf_size;
    server->response_buf = malloc(response_buf_size);
    if (!server->response_buf)
        return NIPC_ERR_OVERFLOW;

    /* Start listening via L1 */
    nipc_uds_error_t uerr = nipc_uds_listen(
        run_dir, service_name, config, &server->listener);
    if (uerr != NIPC_UDS_OK) {
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
        /* Poll the listener fd before blocking on accept */
        int pr = poll_with_shutdown(server->listener.fd, &server->running);
        if (pr <= 0)
            break; /* shutdown or error */

        /* Accept one client via L1 */
        nipc_uds_session_t session;
        memset(&session, 0, sizeof(session));
        session.fd = -1;

        nipc_uds_error_t uerr = nipc_uds_accept(
            &server->listener, &session);
        if (uerr != NIPC_UDS_OK) {
            if (!server->running)
                break;
            usleep(10000);
            continue;
        }

        /* SHM upgrade if negotiated */
        nipc_shm_ctx_t *shm = NULL;
        if (session.selected_profile == NIPC_PROFILE_SHM_HYBRID ||
            session.selected_profile == NIPC_PROFILE_SHM_FUTEX) {

            nipc_shm_ctx_t *s = calloc(1, sizeof(nipc_shm_ctx_t));
            if (s) {
                nipc_shm_error_t serr = nipc_shm_server_create(
                    server->run_dir, server->service_name,
                    session.max_request_payload_bytes + NIPC_HEADER_LEN,
                    session.max_response_payload_bytes + NIPC_HEADER_LEN,
                    s);
                if (serr == NIPC_SHM_OK)
                    shm = s;
                else
                    free(s);
            }
        }

        /* Handle this session (blocking, single-threaded for now) */
        server_handle_session(server, &session, shm);

        /* Cleanup */
        if (shm) {
            nipc_shm_destroy(shm);
            free(shm);
        }
        nipc_uds_close_session(&session);
    }
}

void nipc_server_stop(nipc_managed_server_t *server)
{
    server->running = false;
}

void nipc_server_destroy(nipc_managed_server_t *server)
{
    server->running = false;
    nipc_uds_close_listener(&server->listener);

    free(server->response_buf);
    server->response_buf = NULL;
    server->response_buf_size = 0;

    free(server->workers);
    server->workers = NULL;
    server->worker_count = 0;
}
