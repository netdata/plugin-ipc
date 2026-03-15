/*
 * netipc_service.h - L2 orchestration: client context and managed server.
 *
 * Pure convenience layer. Uses L1 transport + Codec exclusively.
 * Adds zero wire behavior. Provides lifecycle management, typed calls,
 * and a multi-client server with worker dispatch.
 *
 * L2 callers never see transports, handshakes, or chunking.
 */

#ifndef NETIPC_SERVICE_H
#define NETIPC_SERVICE_H

#include "netipc_protocol.h"
#include "netipc_uds.h"
#include "netipc_shm.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Client context state                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    NIPC_CLIENT_DISCONNECTED = 0,
    NIPC_CLIENT_CONNECTING,
    NIPC_CLIENT_READY,
    NIPC_CLIENT_NOT_FOUND,
    NIPC_CLIENT_AUTH_FAILED,
    NIPC_CLIENT_INCOMPATIBLE,
    NIPC_CLIENT_BROKEN,
} nipc_client_state_t;

/* ------------------------------------------------------------------ */
/*  Client status snapshot (for diagnostics, not hot path)              */
/* ------------------------------------------------------------------ */

typedef struct {
    nipc_client_state_t state;
    uint32_t connect_count;
    uint32_t reconnect_count;
    uint32_t call_count;
    uint32_t error_count;
} nipc_client_status_t;

/* ------------------------------------------------------------------ */
/*  Client context                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    /* State */
    nipc_client_state_t state;

    /* Configuration (set at init, immutable) */
    char run_dir[256];
    char service_name[128];
    nipc_uds_client_config_t transport_config;

    /* Connection (managed internally) */
    nipc_uds_session_t session;
    bool session_valid;
    nipc_shm_ctx_t *shm;  /* non-NULL if SHM profile negotiated */

    /* Stats */
    uint32_t connect_count;
    uint32_t reconnect_count;
    uint32_t call_count;
    uint32_t error_count;
} nipc_client_ctx_t;

/* ------------------------------------------------------------------ */
/*  Client API                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialize a client context. Does NOT connect. Does NOT require the
 * server to be running. State starts as DISCONNECTED.
 */
void nipc_client_init(nipc_client_ctx_t *ctx,
                      const char *run_dir,
                      const char *service_name,
                      const nipc_uds_client_config_t *config);

/*
 * Attempt connect if DISCONNECTED, reconnect if BROKEN.
 * Returns true if the state changed (e.g. DISCONNECTED -> READY),
 * false if unchanged.
 *
 * No hidden threads. Call from your own loop at your own cadence.
 */
bool nipc_client_refresh(nipc_client_ctx_t *ctx);

/*
 * Cheap cached boolean. No I/O, no syscalls.
 * Returns true only if state == READY.
 */
static inline bool nipc_client_ready(const nipc_client_ctx_t *ctx) {
    return ctx->state == NIPC_CLIENT_READY;
}

/*
 * Detailed status snapshot. For diagnostics and logging, not hot path.
 */
void nipc_client_status(const nipc_client_ctx_t *ctx,
                        nipc_client_status_t *out);

/*
 * Tear down connection and release resources. Safe on a zero-init ctx.
 */
void nipc_client_close(nipc_client_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/*  Typed cgroups snapshot call                                        */
/* ------------------------------------------------------------------ */

/*
 * Blocking typed call: encode request, send, receive, check
 * transport_status, decode response.
 *
 * request_buf: caller-owned buffer for encoding the request (>= 4 bytes).
 * response_buf/response_buf_size: caller-owned buffer for receiving
 *   the response. Must be large enough for the expected snapshot.
 * view_out: on success, filled with the ephemeral snapshot view.
 *   Valid only until the next call on this context.
 *
 * Retry policy (per spec):
 *   If the call fails and the context was previously READY, the client
 *   disconnects, reconnects (full handshake), and retries ONCE.
 *   If not previously READY, fails immediately.
 *
 * Returns NIPC_OK on success, or an error code.
 */
nipc_error_t nipc_client_call_cgroups_snapshot(
    nipc_client_ctx_t *ctx,
    uint8_t *request_buf,
    uint8_t *response_buf,
    size_t response_buf_size,
    nipc_cgroups_resp_view_t *view_out);

/* ------------------------------------------------------------------ */
/*  Managed server                                                     */
/* ------------------------------------------------------------------ */

/*
 * Server handler callback. Receives raw request payload and must
 * produce raw response payload.
 *
 * method_code: the method from the outer envelope (e.g. 2 for cgroups).
 * request_payload/request_len: the decoded request payload bytes.
 * response_buf/response_buf_size: buffer for the handler to write
 *   the response payload into.
 * response_len_out: the handler sets this to the actual response size.
 *
 * Return true on success, false on failure. Failure results in
 * transport_status = INTERNAL_ERROR with empty payload.
 */
typedef bool (*nipc_server_handler_fn)(
    void *user,
    uint16_t method_code,
    const uint8_t *request_payload, size_t request_len,
    uint8_t *response_buf, size_t response_buf_size,
    size_t *response_len_out);

typedef struct nipc_managed_server nipc_managed_server_t;

struct nipc_managed_server {
    /* Listener */
    nipc_uds_listener_t listener;

    /* Workers */
    pthread_t *workers;
    int worker_count;

    /* Callback */
    nipc_server_handler_fn handler;
    void *handler_user;

    /* Per-worker response buffers */
    uint8_t *response_buf;
    size_t   response_buf_size;

    /* State */
    volatile bool running;

    /* Configuration */
    char run_dir[256];
    char service_name[128];
};

/*
 * Initialize and start listening. Does NOT start workers.
 * Call nipc_server_run() to start the acceptor+worker loop.
 */
nipc_error_t nipc_server_init(nipc_managed_server_t *server,
                               const char *run_dir,
                               const char *service_name,
                               const nipc_uds_server_config_t *config,
                               int worker_count,
                               size_t response_buf_size,
                               nipc_server_handler_fn handler,
                               void *user);

/*
 * Run the acceptor loop. Blocking. Accepts clients, reads requests,
 * dispatches to the handler, sends responses.
 *
 * Returns when nipc_server_stop() is called or on fatal error.
 */
void nipc_server_run(nipc_managed_server_t *server);

/*
 * Signal shutdown. The acceptor loop will exit after current work.
 */
void nipc_server_stop(nipc_managed_server_t *server);

/*
 * Cleanup: close listener, free workers. Safe after stop.
 */
void nipc_server_destroy(nipc_managed_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* NETIPC_SERVICE_H */
