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
#include <time.h>
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
        size_t msg_len;
        nipc_shm_error_t serr = nipc_shm_receive(ctx->shm, buf, buf_size,
                                                    &msg_len, 30000);
        if (serr != NIPC_SHM_OK)
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
        return NIPC_ERR_NOT_READY;
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
static int poll_with_shutdown(int fd, bool *running)
{
    while (__atomic_load_n(running, __ATOMIC_RELAXED)) {
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
 * send responses. Each session gets its own response buffer.
 * Runs until the client disconnects or server stops.
 */
static void server_handle_session(nipc_managed_server_t *server,
                                   nipc_uds_session_t *session,
                                   nipc_shm_ctx_t *shm,
                                   uint8_t *resp_buf,
                                   size_t resp_buf_size)
{
    /* Allocate recv buffer based on negotiated max request size */
    size_t recv_size = NIPC_HEADER_LEN + session->max_request_payload_bytes;
    uint8_t *recv_buf = malloc(recv_size);
    if (!recv_buf)
        return;

    while (__atomic_load_n(&server->running, __ATOMIC_RELAXED)) {
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        /* Receive request via the active transport */
        if (shm) {
            size_t msg_len;
            nipc_shm_error_t serr = nipc_shm_receive(shm, recv_buf, recv_size,
                                                       &msg_len, 500);
            if (serr == NIPC_SHM_ERR_TIMEOUT)
                continue; /* check running flag */
            if (serr != NIPC_SHM_OK)
                break;
            if (msg_len < NIPC_HEADER_LEN)
                break;

            nipc_error_t perr = nipc_header_decode(recv_buf, msg_len, &hdr);
            if (perr != NIPC_OK)
                break;

            payload = recv_buf + NIPC_HEADER_LEN;
            payload_len = msg_len - NIPC_HEADER_LEN;
        } else {
            /* Poll the session fd before blocking on receive */
            int pr = poll_with_shutdown(session->fd, &server->running);
            if (pr <= 0)
                break; /* shutdown or error */

            nipc_uds_error_t uerr = nipc_uds_receive(
                session, recv_buf, recv_size,
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

            resp_hdr.magic      = NIPC_MAGIC_MSG;
            resp_hdr.version    = NIPC_VERSION;
            resp_hdr.header_len = NIPC_HEADER_LEN;
            resp_hdr.payload_len = (uint32_t)response_len;

            /* Use a stack buffer for small responses, heap for large ones */
            uint8_t stack_msg[4096];
            uint8_t *msg = (msg_len <= sizeof(stack_msg)) ? stack_msg : malloc(msg_len);
            if (!msg)
                break;

            nipc_header_encode(&resp_hdr, msg, NIPC_HEADER_LEN);
            if (response_len > 0)
                memcpy(msg + NIPC_HEADER_LEN, resp_buf, response_len);

            nipc_shm_error_t serr = nipc_shm_send(shm, msg, msg_len);
            if (msg != stack_msg)
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

    free(recv_buf);
}

/* ------------------------------------------------------------------ */
/*  Internal: per-session handler thread                                */
/* ------------------------------------------------------------------ */

/* Thread function: handles one client session from accept to disconnect. */
static void *session_handler_thread(void *arg)
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
        nipc_shm_destroy(sctx->shm);
        free(sctx->shm);
    }
    nipc_uds_close_session(&sctx->session);

    /* Mark inactive so the acceptor's reap loop (or server destroy)
     * can join this thread and free sctx.  Do NOT remove from the
     * tracking array here — the reap/destroy path owns that. */
    __atomic_store_n(&sctx->active, false, __ATOMIC_RELEASE);
    return NULL;
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
        if (!__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) {
            pthread_join(s->thread, NULL);
            /* Swap with last, free */
            server->sessions[i] = server->sessions[server->session_count - 1];
            server->session_count--;
            free(s);
        } else {
            i++;
        }
    }

    /* If no active session has SHM, clear the flag so a new session can use it */
    bool any_shm = false;
    for (int j = 0; j < server->session_count; j++) {
        if (server->sessions[j]->shm) {
            any_shm = true;
            break;
        }
    }
    server->shm_in_use = any_shm;
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
    __atomic_store_n(&server->running, false, __ATOMIC_RELAXED);
    server->acceptor_started = false;

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

    /* Initialize session tracking */
    server->session_capacity = worker_count * 2; /* room for slots being reaped */
    if (server->session_capacity < 16)
        server->session_capacity = 16;
    server->sessions = calloc((size_t)server->session_capacity,
                              sizeof(nipc_session_ctx_t *));
    if (!server->sessions)
        return NIPC_ERR_OVERFLOW;
    server->session_count = 0;
    server->next_session_id = 0;
    pthread_mutex_init(&server->sessions_lock, NULL);

    /* Start listening via L1 */
    nipc_uds_error_t uerr = nipc_uds_listen(
        run_dir, service_name, config, &server->listener);
    if (uerr != NIPC_UDS_OK) {
        free(server->sessions);
        server->sessions = NULL;
        pthread_mutex_destroy(&server->sessions_lock);
        return NIPC_ERR_BAD_LAYOUT;
    }

    return NIPC_OK;
}

void nipc_server_run(nipc_managed_server_t *server)
{
    __atomic_store_n(&server->running, true, __ATOMIC_RELEASE);

    while (__atomic_load_n(&server->running, __ATOMIC_RELAXED)) {
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
            if (!__atomic_load_n(&server->running, __ATOMIC_RELAXED))
                break;
            usleep(10000);
            continue;
        }

        /* Enforce worker_count limit: reap finished sessions, check count */
        pthread_mutex_lock(&server->sessions_lock);
        server_reap_sessions_locked(server);

        if (server->session_count >= server->worker_count) {
            /* At capacity: reject this client by closing the session */
            pthread_mutex_unlock(&server->sessions_lock);
            nipc_uds_close_session(&session);
            continue;
        }

        /* SHM upgrade if negotiated, but only if no other session
         * already has SHM (SHM region path is per-service, not
         * per-session, so concurrent SHM sessions would contend). */
        nipc_shm_ctx_t *shm = NULL;
        if ((session.selected_profile == NIPC_PROFILE_SHM_HYBRID ||
             session.selected_profile == NIPC_PROFILE_SHM_FUTEX) &&
            !server->shm_in_use) {

            nipc_shm_ctx_t *s = calloc(1, sizeof(nipc_shm_ctx_t));
            if (s) {
                nipc_shm_error_t serr = nipc_shm_server_create(
                    server->run_dir, server->service_name,
                    session.max_request_payload_bytes + NIPC_HEADER_LEN,
                    session.max_response_payload_bytes + NIPC_HEADER_LEN,
                    s);
                if (serr == NIPC_SHM_OK) {
                    shm = s;
                    server->shm_in_use = true;
                } else {
                    free(s);
                }
            }
        }

        /* Create session context */
        nipc_session_ctx_t *sctx = calloc(1, sizeof(nipc_session_ctx_t));
        if (!sctx) {
            pthread_mutex_unlock(&server->sessions_lock);
            if (shm) { nipc_shm_destroy(shm); free(shm); }
            nipc_uds_close_session(&session);
            continue;
        }

        sctx->server = server;
        sctx->session = session;
        sctx->shm = shm;
        sctx->id = server->next_session_id++;
        __atomic_store_n(&sctx->active, true, __ATOMIC_RELAXED);

        /* Grow session array if needed */
        if (server->session_count >= server->session_capacity) {
            int new_cap = server->session_capacity * 2;
            nipc_session_ctx_t **new_arr = realloc(
                server->sessions,
                (size_t)new_cap * sizeof(nipc_session_ctx_t *));
            if (!new_arr) {
                pthread_mutex_unlock(&server->sessions_lock);
                if (shm) { nipc_shm_destroy(shm); free(shm); }
                nipc_uds_close_session(&session);
                free(sctx);
                continue;
            }
            server->sessions = new_arr;
            server->session_capacity = new_cap;
        }

        server->sessions[server->session_count++] = sctx;
        pthread_mutex_unlock(&server->sessions_lock);

        /* Spawn handler thread for this session */
        int rc = pthread_create(&sctx->thread, NULL,
                                session_handler_thread, sctx);
        if (rc != 0) {
            /* Thread creation failed: clean up */
            pthread_mutex_lock(&server->sessions_lock);
            /* Remove the sctx we just added */
            for (int i = 0; i < server->session_count; i++) {
                if (server->sessions[i] == sctx) {
                    server->sessions[i] = server->sessions[server->session_count - 1];
                    server->session_count--;
                    break;
                }
            }
            pthread_mutex_unlock(&server->sessions_lock);

            if (shm) { nipc_shm_destroy(shm); free(shm); }
            nipc_uds_close_session(&session);
            free(sctx);
        }
    }
}

void nipc_server_stop(nipc_managed_server_t *server)
{
    __atomic_store_n(&server->running, false, __ATOMIC_RELEASE);
}

bool nipc_server_drain(nipc_managed_server_t *server, uint32_t timeout_ms)
{
    /* 1. Stop accepting new clients */
    __atomic_store_n(&server->running, false, __ATOMIC_RELEASE);
    nipc_uds_close_listener(&server->listener);

    /* 2. Wait for in-flight sessions to complete */
    bool all_drained = true;
    if (server->sessions) {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        /* Poll until all sessions are inactive or timeout */
        while (1) {
            pthread_mutex_lock(&server->sessions_lock);
            int active_count = 0;
            for (int i = 0; i < server->session_count; i++) {
                if (__atomic_load_n(&server->sessions[i]->active,
                                    __ATOMIC_ACQUIRE))
                    active_count++;
            }
            pthread_mutex_unlock(&server->sessions_lock);

            if (active_count == 0)
                break;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                /* Timeout: force-close session fds to unblock recv.
                 * Closing the fd causes poll/recv to return error,
                 * which terminates the session handler loop. */
                pthread_mutex_lock(&server->sessions_lock);
                for (int i = 0; i < server->session_count; i++) {
                    nipc_session_ctx_t *s = server->sessions[i];
                    if (__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) {
                        if (s->session.fd >= 0) {
                            shutdown(s->session.fd, SHUT_RDWR);
                        }
                    }
                }
                pthread_mutex_unlock(&server->sessions_lock);
                all_drained = false;
                break;
            }

            usleep(5000); /* 5ms poll interval */
        }

        /* 3. Join all session threads (finished or not) */
        pthread_mutex_lock(&server->sessions_lock);
        for (int i = 0; i < server->session_count; i++) {
            nipc_session_ctx_t *s = server->sessions[i];
            pthread_mutex_unlock(&server->sessions_lock);
            pthread_join(s->thread, NULL);
            free(s);
            pthread_mutex_lock(&server->sessions_lock);
        }
        server->session_count = 0;
        pthread_mutex_unlock(&server->sessions_lock);

        free(server->sessions);
        server->sessions = NULL;
        server->session_capacity = 0;
        pthread_mutex_destroy(&server->sessions_lock);
    }

    server->worker_count = 0;
    server->shm_in_use = false;
    return all_drained;
}

void nipc_server_destroy(nipc_managed_server_t *server)
{
    __atomic_store_n(&server->running, false, __ATOMIC_RELEASE);
    nipc_uds_close_listener(&server->listener);

    /* Join all active session threads */
    if (server->sessions) {
        pthread_mutex_lock(&server->sessions_lock);
        for (int i = 0; i < server->session_count; i++) {
            nipc_session_ctx_t *s = server->sessions[i];
            pthread_mutex_unlock(&server->sessions_lock);
            pthread_join(s->thread, NULL);
            free(s);
            pthread_mutex_lock(&server->sessions_lock);
        }
        server->session_count = 0;
        pthread_mutex_unlock(&server->sessions_lock);

        free(server->sessions);
        server->sessions = NULL;
        server->session_capacity = 0;
        pthread_mutex_destroy(&server->sessions_lock);
    }

    server->worker_count = 0;
    server->shm_in_use = false;
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

/*
 * Build a new cache from a decoded snapshot view. Copies all strings
 * from the ephemeral view into owned heap allocations.
 *
 * Returns the new items array and sets *count_out. Returns NULL on
 * allocation failure.
 */
static nipc_cgroups_cache_item_t *cache_build_items(
    const nipc_cgroups_resp_view_t *view,
    uint32_t *count_out)
{
    uint32_t n = view->item_count;
    *count_out = 0;

    if (n == 0)
        return NULL; /* empty snapshot is valid */

    nipc_cgroups_cache_item_t *items = calloc(n, sizeof(nipc_cgroups_cache_item_t));
    if (!items)
        return NULL;

    for (uint32_t i = 0; i < n; i++) {
        nipc_cgroups_item_view_t iv;
        nipc_error_t err = nipc_cgroups_resp_item(view, i, &iv);
        if (err != NIPC_OK) {
            /* Malformed item: abort build, free partial */
            cache_free_items(items, i);
            return NULL;
        }

        items[i].hash    = iv.hash;
        items[i].options = iv.options;
        items[i].enabled = iv.enabled;

        /* Copy name (add NUL terminator) */
        items[i].name = malloc(iv.name.len + 1);
        if (!items[i].name) {
            cache_free_items(items, i);
            return NULL;
        }
        if (iv.name.len > 0)
            memcpy(items[i].name, iv.name.ptr, iv.name.len);
        items[i].name[iv.name.len] = '\0';

        /* Copy path (add NUL terminator) */
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
                              const nipc_uds_client_config_t *config)
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

    /* Allocate internal response buffer */
    cache->response_buf_size = NIPC_CGROUPS_CACHE_BUF_SIZE;
    cache->response_buf = malloc(cache->response_buf_size);
}

bool nipc_cgroups_cache_refresh(nipc_cgroups_cache_t *cache)
{
    if (!cache->response_buf) {
        cache->refresh_failure_count++;
        return false;
    }

    /* Drive L2 connection lifecycle */
    nipc_client_refresh(&cache->client);

    /* Attempt snapshot call */
    uint8_t req_buf[4];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &cache->client, req_buf,
        cache->response_buf, cache->response_buf_size,
        &view);

    if (err != NIPC_OK) {
        /* Refresh failed -- preserve previous cache */
        cache->refresh_failure_count++;
        return false;
    }

    /* Build new cache from the snapshot view */
    uint32_t new_count = 0;
    nipc_cgroups_cache_item_t *new_items = NULL;

    if (view.item_count > 0) {
        new_items = cache_build_items(&view, &new_count);
        if (!new_items && view.item_count > 0) {
            /* Build failed (allocation error) -- preserve old cache */
            cache->refresh_failure_count++;
            return false;
        }
    }

    /* Replace old cache with new one */
    cache_free_items(cache->items, cache->item_count);
    cache->items = new_items;
    cache->item_count = new_count;
    cache->systemd_enabled = view.systemd_enabled;
    cache->generation = view.generation;
    cache->populated = true;
    cache->refresh_success_count++;

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
