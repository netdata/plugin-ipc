/*
 * test_uds.c - Integration tests for L1 POSIX UDS SEQPACKET transport.
 *
 * Forks server processes and exercises the transport API directly.
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_uds.h"
#include "netipc/netipc_protocol.h"
#include "test_sync.h"
#include "uds_test_socket.h"
#include "netipc_uds_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR  "/tmp/nipc_test"
#define AUTH_TOKEN    0xDEADBEEFCAFEBABEull

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", name);
        g_fail++;
    }
}

static void ensure_run_dir(void)
{
    mkdir(TEST_RUN_DIR, 0700);
}

/* Clean up any leftover socket file */
static void cleanup_socket(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
}

static void build_socket_path(char *path, size_t path_len, const char *service)
{
    snprintf(path, path_len, "%s/%s.sock", TEST_RUN_DIR, service);
}

/* Default server config */
static nipc_uds_server_config_t default_server_config(void)
{
    return (nipc_uds_server_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 4,
    };
}

/* Default client config */
static nipc_uds_client_config_t default_client_config(void)
{
    return (nipc_uds_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
    };
}

/* ------------------------------------------------------------------ */
/*  Server thread helper                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char              *service;
    nipc_uds_server_config_t config;
    int                      accept_count; /* how many clients to accept */
    int                      echo_count;   /* messages to echo per client */
    int                      ready;        /* set to 1 when listening */
    int                      done;         /* set to 1 when finished */
} server_ctx_t;

typedef enum {
    MALFORMED_ACK_SHORT = 1,
    MALFORMED_ACK_BAD_VERSION,
    MALFORMED_ACK_WRONG_KIND,
    MALFORMED_ACK_BAD_STATUS,
    MALFORMED_ACK_INCOMPATIBLE_STATUS,
    MALFORMED_ACK_BAD_LAYOUT,
    MALFORMED_ACK_BAD_PAYLOAD,
    MALFORMED_ACK_PACKET_ABOVE_PROPOSAL,
    MALFORMED_ACK_SELECTED_NOT_OFFERED,
    MALFORMED_ACK_SELECTED_NONE,
    MALFORMED_ACK_SELECTED_TWO,
    MALFORMED_ACK_TRAILING_BYTES,
    MALFORMED_ACK_LONG_PAYLOAD,
} malformed_ack_mode_t;

typedef struct {
    const char *service;
    malformed_ack_mode_t mode;
    int ready;
    int done;
} malformed_ack_server_ctx_t;

typedef struct {
    const char *service;
    nipc_uds_server_config_t config;
    nipc_uds_error_t accept_err;
    int ready;
    int done;
} accept_result_server_ctx_t;

typedef enum {
    RAW_RESP_SHORT_PACKET = 1,
    RAW_RESP_BAD_BATCH_DIR_SHORT,
    RAW_RESP_SHORT_CONTINUATION,
    RAW_RESP_PAYLOAD_EXCEEDED,
    RAW_RESP_BATCH_COUNT_EXCEEDED,
    RAW_RESP_BAD_CONTINUATION_HEADER,
    RAW_RESP_MISSING_CONTINUATION,
    RAW_RESP_TRAILING_PACKET,
} raw_response_mode_t;

typedef struct {
    const char *service;
    nipc_uds_server_config_t config;
    raw_response_mode_t mode;
    int ready;
    int done;
} raw_response_server_ctx_t;

static void send_short_continuation_response(int fd, uint32_t packet_size,
                                             const nipc_header_t *request)
{
    uint8_t payload_buf[160];
    memset(payload_buf, 0xAB, sizeof(payload_buf));
    size_t first_payload = (size_t)packet_size - NIPC_HEADER_LEN;
    if (first_payload > sizeof(payload_buf))
        first_payload = sizeof(payload_buf);

    uint8_t hdr_buf[NIPC_HEADER_LEN];
    nipc_header_t resp = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_RESPONSE,
        .code = request->code,
        .message_id = request->message_id,
        .item_count = 1,
        .transport_status = NIPC_STATUS_OK,
        .payload_len = sizeof(payload_buf),
    };
    nipc_header_encode(&resp, hdr_buf, sizeof(hdr_buf));

    struct iovec iov[2];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = hdr_buf;
    iov[0].iov_len = sizeof(hdr_buf);
    iov[1].iov_base = payload_buf;
    iov[1].iov_len = first_payload;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    sendmsg(fd, &msg, MSG_NOSIGNAL);

    uint8_t short_chunk[8] = {0};
    send(fd, short_chunk, sizeof(short_chunk), MSG_NOSIGNAL);
}

static void send_bad_or_missing_continuation_response(int fd,
                                                      uint32_t packet_size,
                                                      const nipc_header_t *request,
                                                      int send_bad_header)
{
    uint8_t payload_buf[160];
    memset(payload_buf, 0xBC, sizeof(payload_buf));
    size_t first_payload = (size_t)packet_size - NIPC_HEADER_LEN;
    if (first_payload > sizeof(payload_buf))
        first_payload = sizeof(payload_buf);

    uint8_t hdr_buf[NIPC_HEADER_LEN];
    nipc_header_t resp = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_RESPONSE,
        .code = request->code,
        .message_id = request->message_id,
        .item_count = 1,
        .transport_status = NIPC_STATUS_OK,
        .payload_len = sizeof(payload_buf),
    };
    nipc_header_encode(&resp, hdr_buf, sizeof(hdr_buf));

    struct iovec iov[2];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    iov[0].iov_base = hdr_buf;
    iov[0].iov_len = sizeof(hdr_buf);
    iov[1].iov_base = payload_buf;
    iov[1].iov_len = first_payload;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    sendmsg(fd, &msg, MSG_NOSIGNAL);

    if (send_bad_header) {
        uint8_t chunk_buf[NIPC_HEADER_LEN] = {0};
        nipc_chunk_header_t chk = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .flags = 0,
            .message_id = request->message_id,
            .total_message_len = (uint32_t)(NIPC_HEADER_LEN + sizeof(payload_buf)),
            .chunk_index = 1,
            .chunk_count = 2,
            .chunk_payload_len = 64,
        };
        nipc_chunk_header_encode(&chk, chunk_buf, sizeof(chunk_buf));
        send(fd, chunk_buf, sizeof(chunk_buf), MSG_NOSIGNAL);
    }
}

/* Simple echo server: accepts clients, for each one reads echo_count
 * messages and sends them back with kind=RESPONSE. */
static void *echo_server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server listen failed: %d\n", err);
        test_flag_set(&ctx->done);
        return NULL;
    }

    test_flag_set(&ctx->ready);

    for (int c = 0; c < ctx->accept_count; c++) {
        nipc_uds_session_t session;
        err = nipc_uds_accept(&listener, 0, &session);
        if (err != NIPC_UDS_OK)
            continue;

        for (int m = 0; m < ctx->echo_count; m++) {
            uint8_t buf[8192];
            nipc_header_t hdr;
            const void *payload;
            size_t payload_len;

            err = nipc_uds_receive(&session, buf, sizeof(buf),
                                    &hdr, &payload, &payload_len);
            if (err != NIPC_UDS_OK) {
                /* Client may have disconnected */
                break;
            }

            /* Echo back as response */
            nipc_header_t resp = hdr;
            resp.kind = NIPC_KIND_RESPONSE;
            resp.transport_status = NIPC_STATUS_OK;

            err = nipc_uds_send(&session, &resp, payload, payload_len);
            if (err != NIPC_UDS_OK)
                break;
        }

        nipc_uds_close_session(&session);
    }

    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void *malformed_ack_server_thread(void *arg)
{
    malformed_ack_server_ctx_t *ctx = (malformed_ack_server_ctx_t *)arg;
    char path[256];
    int lfd = -1;
    int cfd = -1;

    build_socket_path(path, sizeof(path), ctx->service);
    unlink(path);

    lfd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    if (lfd < 0) {
        test_flag_set(&ctx->done);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 4) != 0) {
        close(lfd);
        unlink(path);
        test_flag_set(&ctx->done);
        return NULL;
    }

    test_flag_set(&ctx->ready);

    cfd = accept(lfd, NULL, NULL);
    if (cfd >= 0) {
        uint8_t buf[128];
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n > 0) {
            switch (ctx->mode) {
            case MALFORMED_ACK_SHORT: {
                uint8_t short_buf[8] = {0};
                send(cfd, short_buf, sizeof(short_buf), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_BAD_VERSION: {
                nipc_hello_ack_t ack = { .layout_version = 1 };
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf)];
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION + 1,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = sizeof(ack_buf),
                    .item_count = 1,
                };
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_WRONG_KIND: {
                nipc_hello_ack_t ack = { .layout_version = 1 };
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf)];
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_RESPONSE,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = sizeof(ack_buf),
                    .item_count = 1,
                };
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_BAD_STATUS: {
                nipc_hello_ack_t ack = { .layout_version = 1 };
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf)];
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_INTERNAL_ERROR,
                    .payload_len = sizeof(ack_buf),
                    .item_count = 1,
                };
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_INCOMPATIBLE_STATUS: {
                nipc_hello_ack_t ack = { .layout_version = 1 };
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf)];
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_INCOMPATIBLE,
                    .payload_len = sizeof(ack_buf),
                    .item_count = 1,
                };
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_BAD_LAYOUT: {
                nipc_hello_ack_t ack = { .layout_version = 2 };
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf)];
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = sizeof(ack_buf),
                    .item_count = 1,
                };
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_BAD_PAYLOAD: {
                uint8_t pkt[NIPC_HEADER_LEN + 4] = {0};
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_OK,
                    .item_count = 1,
                    .payload_len = 4,
                };
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                send(cfd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case MALFORMED_ACK_PACKET_ABOVE_PROPOSAL:
            case MALFORMED_ACK_SELECTED_NOT_OFFERED:
            case MALFORMED_ACK_SELECTED_NONE:
            case MALFORMED_ACK_SELECTED_TWO:
            case MALFORMED_ACK_TRAILING_BYTES:
            case MALFORMED_ACK_LONG_PAYLOAD: {
                nipc_hello_t hello;
                if (n < NIPC_HEADER_LEN ||
                    nipc_hello_decode(buf + NIPC_HEADER_LEN, (size_t)n - NIPC_HEADER_LEN,
                                      &hello) != NIPC_OK)
                    break;
                nipc_hello_ack_t ack = {
                    .layout_version = 1,
                    .server_supported_profiles = NIPC_PROFILE_BASELINE,
                    .intersection_profiles = NIPC_PROFILE_BASELINE,
                    .selected_profile = NIPC_PROFILE_BASELINE,
                    .agreed_max_request_payload_bytes = hello.max_request_payload_bytes,
                    .agreed_max_request_batch_items = hello.max_request_batch_items,
                    .agreed_max_response_payload_bytes = hello.max_response_payload_bytes,
                    .agreed_max_response_batch_items = hello.max_response_batch_items,
                    .agreed_packet_size = hello.packet_size + 1,
                    .session_id = 1,
                };
                if (ctx->mode == MALFORMED_ACK_TRAILING_BYTES ||
                    ctx->mode == MALFORMED_ACK_LONG_PAYLOAD) {
                    /* A valid ACK, then 16 more bytes in the same packet. */
                    ack.agreed_packet_size = hello.packet_size;
                } else if (ctx->mode != MALFORMED_ACK_PACKET_ABOVE_PROPOSAL) {
                    /* The client offers baseline only. */
                    ack.agreed_packet_size = hello.packet_size;
                    ack.selected_profile =
                        ctx->mode == MALFORMED_ACK_SELECTED_NOT_OFFERED ? NIPC_PROFILE_SHM_HYBRID
                        : ctx->mode == MALFORMED_ACK_SELECTED_NONE
                            ? 0
                            : (NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID);
                }
                uint8_t ack_buf[48];
                uint8_t pkt[NIPC_HEADER_LEN + sizeof(ack_buf) + 16];
                size_t extra = (ctx->mode == MALFORMED_ACK_TRAILING_BYTES ||
                                ctx->mode == MALFORMED_ACK_LONG_PAYLOAD) ? 16 : 0;
                nipc_header_t hdr = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_CONTROL,
                    .code = NIPC_CODE_HELLO_ACK,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = sizeof(ack_buf) +
                        (ctx->mode == MALFORMED_ACK_LONG_PAYLOAD ? 16 : 0),
                    .item_count = 1,
                };
                memset(pkt, 0, sizeof(pkt));
                nipc_hello_ack_encode(&ack, ack_buf, sizeof(ack_buf));
                nipc_header_encode(&hdr, pkt, sizeof(pkt));
                memcpy(pkt + NIPC_HEADER_LEN, ack_buf, sizeof(ack_buf));
                send(cfd, pkt, NIPC_HEADER_LEN + sizeof(ack_buf) + extra,
                     MSG_NOSIGNAL);
                break;
            }
            }
        }
        close(cfd);
    }

    close(lfd);
    unlink(path);
    test_flag_set(&ctx->done);
    return NULL;
}

static void *accept_result_server_thread(void *arg)
{
    accept_result_server_ctx_t *ctx = (accept_result_server_ctx_t *)arg;
    nipc_uds_listener_t listener;

    ctx->accept_err = NIPC_UDS_OK;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                           &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        ctx->accept_err = err;
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    ctx->accept_err = nipc_uds_accept(&listener, 1, &session);
    if (ctx->accept_err == NIPC_UDS_OK)
        nipc_uds_close_session(&session);

    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void *raw_response_server_thread(void *arg)
{
    raw_response_server_ctx_t *ctx = (raw_response_server_ctx_t *)arg;
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                           &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 1, &session);
    if (err == NIPC_UDS_OK) {
        uint8_t buf[65536];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                 &hdr, &payload, &payload_len);
        if (rerr == NIPC_UDS_OK) {
            switch (ctx->mode) {
            case RAW_RESP_SHORT_PACKET: {
                uint8_t short_buf[8] = {0};
                send(session.fd, short_buf, sizeof(short_buf), MSG_NOSIGNAL);
                break;
            }
            case RAW_RESP_BAD_BATCH_DIR_SHORT: {
                uint8_t pkt[NIPC_HEADER_LEN + 8] = {0};
                nipc_header_t resp = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_RESPONSE,
                    .code = hdr.code,
                    .flags = NIPC_FLAG_BATCH,
                    .item_count = 3,
                    .message_id = hdr.message_id,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = 8,
                };
                nipc_header_encode(&resp, pkt, sizeof(pkt));
                send(session.fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case RAW_RESP_SHORT_CONTINUATION: {
                send_short_continuation_response(session.fd, session.packet_size, &hdr);
                break;
            }
            case RAW_RESP_PAYLOAD_EXCEEDED: {
                uint8_t pkt[NIPC_HEADER_LEN + 1] = {0};
                nipc_header_t resp = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_RESPONSE,
                    .code = hdr.code,
                    .item_count = 1,
                    .message_id = hdr.message_id,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = session.max_response_payload_bytes + 1,
                };
                pkt[NIPC_HEADER_LEN] = 0xAC;
                nipc_header_encode(&resp, pkt, sizeof(pkt));
                send(session.fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case RAW_RESP_BATCH_COUNT_EXCEEDED: {
                uint8_t pkt[NIPC_HEADER_LEN + 1] = {0};
                nipc_header_t resp = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_RESPONSE,
                    .code = hdr.code,
                    .item_count = session.max_response_batch_items + 1,
                    .message_id = hdr.message_id,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = 1,
                };
                pkt[NIPC_HEADER_LEN] = 0xAD;
                nipc_header_encode(&resp, pkt, sizeof(pkt));
                send(session.fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case RAW_RESP_TRAILING_PACKET: {
                uint8_t pkt[NIPC_HEADER_LEN + 2] = {0};
                nipc_header_t resp = {
                    .magic = NIPC_MAGIC_MSG,
                    .version = NIPC_VERSION,
                    .header_len = NIPC_HEADER_LEN,
                    .kind = NIPC_KIND_RESPONSE,
                    .code = hdr.code,
                    .item_count = 1,
                    .message_id = hdr.message_id,
                    .transport_status = NIPC_STATUS_OK,
                    .payload_len = 1,
                };
                pkt[NIPC_HEADER_LEN] = 0xBE;
                pkt[NIPC_HEADER_LEN + 1] = 0xEF;
                nipc_header_encode(&resp, pkt, NIPC_HEADER_LEN);
                send(session.fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
                break;
            }
            case RAW_RESP_BAD_CONTINUATION_HEADER:
            case RAW_RESP_MISSING_CONTINUATION: {
                send_bad_or_missing_continuation_response(
                    session.fd, session.packet_size, &hdr,
                    ctx->mode == RAW_RESP_BAD_CONTINUATION_HEADER);
                break;
            }
            }
        }
        nipc_uds_close_session(&session);
    }

    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

/* Start echo server in a thread. Waits until it's ready. */
static pthread_t start_echo_server(server_ctx_t *ctx)
{
    pthread_t tid;
    __atomic_store_n(&ctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->done, 0, __ATOMIC_RELAXED);
    pthread_create(&tid, NULL, echo_server_thread, ctx);

    /* Wait for the server to be ready */
    (void)test_flag_wait_any(&ctx->ready, &ctx->done);
    return tid;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Single client ping-pong                                    */
/* ------------------------------------------------------------------ */

static void test_ping_pong(void)
{
    printf("Test 1: Single client ping-pong\n");
    const char *svc = "test_ping";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 1,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("client connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        check("selected profile is baseline",
              session.selected_profile == NIPC_PROFILE_BASELINE);

        /* Send a request */
        uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
        nipc_header_t hdr = {
            .kind       = NIPC_KIND_REQUEST,
            .code       = NIPC_METHOD_INCREMENT,
            .flags      = 0,
            .item_count = 1,
            .message_id = 42,
        };

        err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request", err == NIPC_UDS_OK);

        /* Receive response */
        uint8_t rbuf[4096];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;
        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rpayload, &rpayload_len);
        check("receive response", err == NIPC_UDS_OK);
        check("response kind", rhdr.kind == NIPC_KIND_RESPONSE);
        check("response message_id", rhdr.message_id == 42);
        check("response payload matches",
              rpayload_len == sizeof(payload) &&
              memcmp(rpayload, payload, sizeof(payload)) == 0);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

#ifdef NIPC_TEST_EINTR_WRAP
#include "uds_eintr_wrap.h"

/* A signal that interrupts a socket call before data moved must not fail
 * the handshake or the session: every send, sendmsg and recv below fails
 * once with EINTR first. */
static void test_eintr_retried(void)
{
    printf("Test: EINTR on accept, send, sendmsg and recv is retried\n");
    const char *svc = "test_eintr";
    cleanup_socket(svc);

    /* The server's first accept is interrupted before a client arrives. */
    uds_eintr_wrap_set_accept(1);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 1,
    };
    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* HELLO and HELLO_ACK go through send and recv, the request and the
     * response through sendmsg and recv; on a stream every packet goes
     * through sendmsg. */
#if TEST_UDS_STREAM
    uds_eintr_wrap_set(0, 4, 4);
#else
    uds_eintr_wrap_set(2, 2, 4);
#endif

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect survives EINTR", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        uint8_t payload[] = {0x05, 0x06};
        nipc_header_t hdr = {
            .kind       = NIPC_KIND_REQUEST,
            .code       = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7,
        };
        err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send survives EINTR", err == NIPC_UDS_OK);

        uint8_t rbuf[4096];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;
        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rpayload, &rpayload_len);
        check("receive survives EINTR", err == NIPC_UDS_OK);
        check("response matches",
              err == NIPC_UDS_OK && rhdr.message_id == 7 &&
              rpayload_len == sizeof(payload) &&
              memcmp(rpayload, payload, sizeof(payload)) == 0);
        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    check("every injected EINTR was hit", uds_eintr_wrap_pending() == 0);
    check("the injected accept EINTR was hit",
          uds_eintr_wrap_accept_pending() == 0);
    uds_eintr_wrap_set(0, 0, 0);
    uds_eintr_wrap_set_accept(0);
    cleanup_socket(svc);
}

/* The umask is process-global: a file another thread creates while the
 * listener binds must keep the process umask, and the socket must still be
 * owner-only. */
static void test_listen_keeps_process_umask(void)
{
    printf("Test: Listen leaves the process umask alone\n");
    const char *svc = "test_listen_umask";
    cleanup_socket(svc);

    char probe[256];
    snprintf(probe, sizeof(probe), "%s/%s.probe", TEST_RUN_DIR, svc);
    unlink(probe);

    mode_t old_mask = umask(022);
    uds_bind_probe_arm(probe);
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    umask(old_mask);
    check("listen succeeds", err == NIPC_UDS_OK);
    check("file created during bind keeps the process umask (0644)",
          uds_bind_probe_mode() == 0644);

    if (err == NIPC_UDS_OK) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);
        struct stat st;
        check("socket mode is owner-only",
              stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
        nipc_uds_close_listener(&listener);
    }
    cleanup_socket(svc);
}
#endif

/* ------------------------------------------------------------------ */
/*  Test 2: Multi-client concurrent sessions                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char              *service;
    nipc_uds_client_config_t config;
    uint8_t                  payload_byte;
    uint64_t                 message_id;
    int                      ok;   /* set by thread */
    int                      match; /* payload round-tripped */
} client_ctx_t;

static void *client_thread(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    ctx->ok    = 0;
    ctx->match = 0;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, ctx->service,
                                             &ctx->config, &session);
    if (err != NIPC_UDS_OK)
        return NULL;

    ctx->ok = 1;

    nipc_header_t hdr = {
        .kind = NIPC_KIND_REQUEST, .code = 1,
        .item_count = 1, .message_id = ctx->message_id,
    };
    nipc_uds_send(&session, &hdr, &ctx->payload_byte, 1);

    uint8_t rbuf[4096];
    nipc_header_t rhdr;
    const void *rp;
    size_t rlen;
    err = nipc_uds_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rp, &rlen);
    if (err == NIPC_UDS_OK && rhdr.message_id == ctx->message_id &&
        rlen == 1 && *(const uint8_t *)rp == ctx->payload_byte)
        ctx->match = 1;

    nipc_uds_close_session(&session);
    return NULL;
}

static void test_multi_client(void)
{
    printf("Test 2: Multi-client concurrent sessions\n");
    const char *svc = "test_multi";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 2,
        .echo_count   = 1,
    };

    pthread_t stid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();

    client_ctx_t c1 = { .service = svc, .config = ccfg,
                         .payload_byte = 0xAA, .message_id = 100 };
    client_ctx_t c2 = { .service = svc, .config = ccfg,
                         .payload_byte = 0xBB, .message_id = 200 };

    pthread_t t1, t2;
    pthread_create(&t1, NULL, client_thread, &c1);
    pthread_create(&t2, NULL, client_thread, &c2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    check("client1 connect", c1.ok);
    check("client1 round-trip", c1.match);
    check("client2 connect", c2.ok);
    check("client2 round-trip", c2.match);

    pthread_join(stid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Pipelining                                                 */
/* ------------------------------------------------------------------ */

static void test_pipelining(void)
{
    printf("Test 3: Pipelining (3 requests, 3 responses)\n");
    const char *svc = "test_pipe";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 3,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send 3 requests */
        for (uint64_t i = 1; i <= 3; i++) {
            uint8_t payload = (uint8_t)i;
            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = i,
            };
            nipc_uds_send(&session, &hdr, &payload, 1);
        }

        /* Receive 3 responses (in order since echo server is in-order) */
        int all_match = 1;
        for (uint64_t i = 1; i <= 3; i++) {
            uint8_t rbuf[4096];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                              &rhdr, &rp, &rlen);
            if (rhdr.message_id != i || rlen != 1 ||
                *(const uint8_t *)rp != (uint8_t)i)
                all_match = 0;
        }
        check("all 3 responses matched by message_id", all_match);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Large message chunking                                     */
/* ------------------------------------------------------------------ */

/* Server thread that handles one client and echoes one large message.
 * Uses a small forced packet_size to trigger chunking. */
static void *chunked_server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err != NIPC_UDS_OK) {
        nipc_uds_close_listener(&listener);
        test_flag_set(&ctx->done);
        return NULL;
    }

    /* Receive chunked message */
    uint8_t buf[256]; /* small, force use of recv_buf */
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;

    err = nipc_uds_receive(&session, buf, sizeof(buf), &hdr,
                            &payload, &payload_len);
    if (err == NIPC_UDS_OK) {
        /* Echo it back */
        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        err = nipc_uds_send(&session, &resp, payload, payload_len);
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_chunking(void)
{
    printf("Test 4: Large message chunking\n");
    const char *svc = "test_chunk";
    cleanup_socket(svc);

    /* Force small packet size to guarantee chunking */
    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = 128;
    scfg.max_request_payload_bytes  = 65536;
    scfg.max_response_payload_bytes = 65536;

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = 1,
    };

    pthread_t tid;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);
    pthread_create(&tid, NULL, chunked_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("chunked server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_request_payload_bytes  = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        check("negotiated packet_size is 128", session.packet_size == 128);

        /* Build a payload larger than 128 - 32 = 96 bytes */
        size_t big_len = 500;
        uint8_t *big = malloc(big_len);
        for (size_t i = 0; i < big_len; i++)
            big[i] = (uint8_t)(i & 0xFF);

        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST, .code = 1,
            .item_count = 1, .message_id = 7,
        };

        err = nipc_uds_send(&session, &hdr, big, big_len);
        check("send chunked message", err == NIPC_UDS_OK);

        /* Receive response (also chunked) */
        uint8_t rbuf[256];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;

        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rpayload, &rpayload_len);
        check("receive chunked response", err == NIPC_UDS_OK);
        check("response message_id", rhdr.message_id == 7);
        check("response payload length", rpayload_len == big_len);
        check("response payload data matches",
              rpayload_len == big_len &&
              memcmp(rpayload, big, big_len) == 0);

        free(big);
        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Handshake failure - bad auth token                         */
/* ------------------------------------------------------------------ */

static void test_bad_auth(void)
{
    printf("Test 5: Handshake failure - bad auth token\n");
    const char *svc = "test_badauth";
    cleanup_socket(svc);

    /* Server with specific auth token */
    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 0,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client with wrong auth token */
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.auth_token = 0xBAD;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect fails with auth error", err == NIPC_UDS_ERR_AUTH_FAILED);

    if (err == NIPC_UDS_OK)
        nipc_uds_close_session(&session);

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Handshake failure - profile mismatch                       */
/* ------------------------------------------------------------------ */

static void test_profile_mismatch(void)
{
    printf("Test 6: Handshake failure - profile mismatch\n");
    const char *svc = "test_badprofile";
    cleanup_socket(svc);

    /* Server only supports SHM (bit 2) */
    nipc_uds_server_config_t scfg = default_server_config();
    scfg.supported_profiles = NIPC_PROFILE_SHM_FUTEX;

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = 0,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client only supports baseline (bit 0) */
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_BASELINE;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect fails with no_profile", err == NIPC_UDS_ERR_NO_PROFILE);

    if (err == NIPC_UDS_OK)
        nipc_uds_close_session(&session);

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* Both peers configured for SHM: only Linux may select it; elsewhere the
 * platform mask leaves baseline on both sides. */
static void test_platform_profile_mask(void)
{
    printf("Test 6b: Platform profile mask\n");
    const char *svc = "test_platprofile";
    cleanup_socket(svc);
#if defined(__linux__) || defined(__FreeBSD__)
    const uint32_t want = NIPC_PROFILE_SHM_HYBRID;
#else
    const uint32_t want = NIPC_PROFILE_BASELINE;
#endif

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    scfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = 0,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID;
    ccfg.preferred_profiles = NIPC_PROFILE_SHM_HYBRID;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("client connect", err == NIPC_UDS_OK);
    if (err == NIPC_UDS_OK) {
        check("selected profile matches the platform", session.selected_profile == want);
        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* An unassigned profile bit offered and preferred by both peers is never
 * selected: they would otherwise run baseline under that label. */
static void test_unassigned_profile_bit(void)
{
    printf("Test 6c: Unassigned profile bit is never selected\n");
    const char *svc = "test_unassigned_profile";
    cleanup_socket(svc);
    const uint32_t unassigned = 0x08u;

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.supported_profiles = NIPC_PROFILE_BASELINE | unassigned;
    scfg.preferred_profiles = unassigned;

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = 0,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.supported_profiles = NIPC_PROFILE_BASELINE | unassigned;
    ccfg.preferred_profiles = unassigned;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("client connect", err == NIPC_UDS_OK);
    if (err == NIPC_UDS_OK) {
        check("baseline selected instead of the unassigned bit",
              session.selected_profile == NIPC_PROFILE_BASELINE);
        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: Handshake failure - request payload proposal over cap      */
/* ------------------------------------------------------------------ */

static void test_request_payload_over_cap(void)
{
    printf("Test 7: Handshake failure - request payload proposal over cap\n");
    const char *svc = "test_req_payload_over_cap";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 0,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = NIPC_MAX_PAYLOAD_CAP + 1u;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect fails with limit exceeded", err == NIPC_UDS_ERR_LIMIT_EXCEEDED);

    if (err == NIPC_UDS_OK)
        nipc_uds_close_session(&session);

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 8: Stale socket recovery                                      */
/* ------------------------------------------------------------------ */

static void test_stale_recovery(void)
{
    printf("Test 8: Stale socket recovery\n");
    const char *svc = "test_stale";
    cleanup_socket(svc);

    /* Create a stale socket file (not backed by a listener) */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);

    int sock = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    /* Close without unlink -> stale socket */
    close(sock);

    /* Verify the socket file exists */
    struct stat st;
    check("stale socket exists", stat(path, &st) == 0);

    /* Now listen should recover it */
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    check("listen recovers stale socket", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&listener);

    cleanup_socket(svc);
}

static void test_stale_recovery_reclaims_regular_file(void)
{
    printf("Test: Stale socket recovery reclaims a foreign regular file\n");
    const char *svc = "test_stale_regular";
    cleanup_socket(svc);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        const char contents[] = "not a socket\n";
        (void)write(fd, contents, sizeof(contents) - 1);
        close(fd);
    }
    check("regular file created at socket path", fd >= 0);

    /* A foreign file squatting on the endpoint path is not a live server —
     * it is silently reclaimed so the service can start. */
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    check("listen reclaims regular file path", err == NIPC_UDS_OK);

    struct stat st;
    check("socket replaces the foreign file",
          stat(path, &st) == 0 && S_ISSOCK(st.st_mode));

    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&listener);

    cleanup_socket(svc);
}

/* Unlinking a directory fails with EISDIR on Linux and EPERM on FreeBSD and
 * macOS; every platform must reclaim an empty one. */
static void test_stale_recovery_reclaims_empty_directory(void)
{
    printf("Test: Stale socket recovery reclaims an empty directory\n");
    const char *svc = "test_stale_dir";
    cleanup_socket(svc);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);
    rmdir(path);
    check("directory created at socket path", mkdir(path, 0700) == 0);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    check("listen reclaims a directory path", err == NIPC_UDS_OK);

    struct stat st;
    check("socket replaces the directory",
          stat(path, &st) == 0 && S_ISSOCK(st.st_mode));

    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&listener);

    rmdir(path);
    cleanup_socket(svc);
}

static void test_listen_socket_owner_only(void)
{
    printf("Test: Listen creates owner-only socket\n");
    const char *svc = "test_socket_owner_only";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    check("listen succeeds", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);

        struct stat st;
        check("socket path can be statted", stat(path, &st) == 0);
        check("socket mode is owner-only", (st.st_mode & 0777) == 0600);

        nipc_uds_close_listener(&listener);
    }

    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 9: Disconnect with in-flight request                          */
/* ------------------------------------------------------------------ */

/* Server that accepts but closes immediately without responding. */
static void *disconnect_server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err == NIPC_UDS_OK) {
        /* Read the request but close without responding */
        uint8_t buf[4096];
        nipc_header_t hdr;
        const void *p;
        size_t plen;
        nipc_uds_receive(&session, buf, sizeof(buf), &hdr, &p, &plen);
        nipc_uds_close_session(&session);
    }

    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_disconnect_inflight(void)
{
    printf("Test 8: Disconnect with in-flight request\n");
    const char *svc = "test_disconn";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 1,
    };

    pthread_t tid;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);
    pthread_create(&tid, NULL, disconnect_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send a request */
        uint8_t payload[] = {0xFF};
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST, .code = 1,
            .item_count = 1, .message_id = 99,
        };
        nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("client tracks first in-flight request", session.inflight_count == 1);
        check("client has spare in-flight capacity", session.inflight_capacity >= 2);
        if (session.inflight_capacity >= 2) {
            session.inflight_ids[session.inflight_count++] = 100;
        }

        /* Try to receive -- server will disconnect */
        uint8_t rbuf[4096];
        nipc_header_t rhdr;
        const void *rp;
        size_t rlen;
        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rp, &rlen);
        check("receive fails on disconnect", err != NIPC_UDS_OK);
        check("disconnect clears all in-flight requests", session.inflight_count == 0);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 9: Batch send/receive                                         */
/* ------------------------------------------------------------------ */

static void test_batch(void)
{
    printf("Test 9: Batch send/receive (3 items)\n");
    const char *svc = "test_batch";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 1,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Build a batch payload using the protocol batch builder */
        uint8_t batch_buf[2048];
        nipc_batch_builder_t builder;
        nipc_batch_builder_init(&builder, batch_buf, sizeof(batch_buf), 3);

        uint8_t item0[] = {0x10, 0x20};
        uint8_t item1[] = {0x30, 0x40, 0x50};
        uint8_t item2[] = {0x60};

        nipc_batch_builder_add(&builder, item0, sizeof(item0));
        nipc_batch_builder_add(&builder, item1, sizeof(item1));
        nipc_batch_builder_add(&builder, item2, sizeof(item2));

        uint32_t batch_count;
        size_t batch_len = nipc_batch_builder_finish(&builder, &batch_count);

        check("batch has 3 items", batch_count == 3);

        /* Send as batch message */
        nipc_header_t hdr = {
            .kind       = NIPC_KIND_REQUEST,
            .code       = NIPC_METHOD_INCREMENT,
            .flags      = NIPC_FLAG_BATCH,
            .item_count = batch_count,
            .message_id = 55,
        };

        err = nipc_uds_send(&session, &hdr, batch_buf, batch_len);
        check("send batch", err == NIPC_UDS_OK);

        /* Receive echoed batch */
        uint8_t rbuf[4096];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;

        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rpayload, &rpayload_len);
        check("receive batch response", err == NIPC_UDS_OK);
        check("batch response message_id", rhdr.message_id == 55);
        check("batch response flags", rhdr.flags & NIPC_FLAG_BATCH);
        check("batch response item_count", rhdr.item_count == 3);

        /* Extract items using protocol layer */
        if (err == NIPC_UDS_OK && rpayload_len == batch_len) {
            const void *ip;
            uint32_t ilen;
            int items_ok = 1;

            if (nipc_batch_item_get(rpayload, rpayload_len, 3, 0, &ip, &ilen) == NIPC_OK) {
                if (ilen != sizeof(item0) || memcmp(ip, item0, ilen) != 0)
                    items_ok = 0;
            } else {
                items_ok = 0;
            }

            if (nipc_batch_item_get(rpayload, rpayload_len, 3, 1, &ip, &ilen) == NIPC_OK) {
                if (ilen != sizeof(item1) || memcmp(ip, item1, ilen) != 0)
                    items_ok = 0;
            } else {
                items_ok = 0;
            }

            if (nipc_batch_item_get(rpayload, rpayload_len, 3, 2, &ip, &ilen) == NIPC_OK) {
                if (ilen != sizeof(item2) || memcmp(ip, item2, ilen) != 0)
                    items_ok = 0;
            } else {
                items_ok = 0;
            }

            check("all batch items match", items_ok);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 10: Invalid service name validation                            */
/* ------------------------------------------------------------------ */

static void test_invalid_service_name(void)
{
    printf("Test 10: Invalid service name validation\n");

    nipc_uds_listener_t listener;
    nipc_uds_server_config_t scfg = default_server_config();

    /* Names with path separators */
    check("reject name with /",
          nipc_uds_listen(TEST_RUN_DIR, "foo/bar", &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    check("reject NULL name",
          nipc_uds_listen(TEST_RUN_DIR, NULL, &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    check("reject name with ..",
          nipc_uds_listen(TEST_RUN_DIR, "..", &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    check("reject name .",
          nipc_uds_listen(TEST_RUN_DIR, ".", &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    check("reject empty name",
          nipc_uds_listen(TEST_RUN_DIR, "", &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    check("reject name with space",
          nipc_uds_listen(TEST_RUN_DIR, "foo bar", &scfg, &listener)
          == NIPC_UDS_ERR_BAD_PARAM);

    /* Connect should also reject */
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    check("connect reject NULL name",
          nipc_uds_connect(TEST_RUN_DIR, NULL, &ccfg, &session)
          == NIPC_UDS_ERR_BAD_PARAM);
    check("connect reject bad name",
          nipc_uds_connect(TEST_RUN_DIR, "../etc", &ccfg, &session)
          == NIPC_UDS_ERR_BAD_PARAM);

    /* Valid names should not fail validation (may fail connect) */
    check("accept valid name (connect may fail)",
          nipc_uds_connect(TEST_RUN_DIR, "valid-name_123.test", &ccfg, &session)
          != NIPC_UDS_ERR_BAD_PARAM);
}

/* ------------------------------------------------------------------ */
/*  Test: Send/receive validation                                      */
/* ------------------------------------------------------------------ */

static void test_send_recv_validation(void)
{
    printf("Test: Send/receive parameter validation\n");

    /* Send with NULL session */
    nipc_header_t hdr = {0};
    uint8_t data[4] = {0};
    check("send null session",
          nipc_uds_send(NULL, &hdr, data, sizeof(data))
              == NIPC_UDS_ERR_BAD_PARAM);

    /* Send with invalid fd */
    nipc_uds_session_t bad_session;
    memset(&bad_session, 0, sizeof(bad_session));
    bad_session.fd = -1;
    check("send bad fd",
          nipc_uds_send(&bad_session, &hdr, data, sizeof(data))
              == NIPC_UDS_ERR_BAD_PARAM);

    /* Receive with NULL session */
    uint8_t buf[256];
    nipc_header_t hdr_out;
    const void *payload;
    size_t payload_len;
    check("recv null session",
          nipc_uds_receive(NULL, buf, sizeof(buf),
                           &hdr_out, &payload, &payload_len)
              == NIPC_UDS_ERR_BAD_PARAM);

    /* Receive with invalid fd */
    check("recv bad fd",
          nipc_uds_receive(&bad_session, buf, sizeof(buf),
                           &hdr_out, &payload, &payload_len)
              == NIPC_UDS_ERR_BAD_PARAM);

    int sv[2] = {-1, -1};
    check("socketpair for zero chunk budget", socketpair(AF_UNIX, TEST_UDS_SOCK_TYPE, 0, sv) == 0);
    if (sv[0] >= 0 && sv[1] >= 0) {
        nipc_uds_session_t tiny_packet_session;
        memset(&tiny_packet_session, 0, sizeof(tiny_packet_session));
        tiny_packet_session.fd = sv[0];
        tiny_packet_session.packet_size = NIPC_HEADER_LEN;
        tiny_packet_session.role = NIPC_UDS_ROLE_SERVER;

        nipc_header_t chunk_hdr = {0};
        chunk_hdr.kind = NIPC_KIND_REQUEST;
        chunk_hdr.code = 1;
        chunk_hdr.message_id = 1;
        chunk_hdr.item_count = 1;
        uint8_t chunk_payload[] = {0xAA};

        check("send rejects zero chunk payload budget",
              nipc_uds_send(&tiny_packet_session, &chunk_hdr,
                            chunk_payload, sizeof(chunk_payload))
                  == NIPC_UDS_ERR_BAD_PARAM);

        tiny_packet_session.packet_size = NIPC_HEADER_LEN - 1;
        check("send rejects underflow chunk payload budget",
              nipc_uds_send(&tiny_packet_session, &chunk_hdr,
                            chunk_payload, sizeof(chunk_payload))
                  == NIPC_UDS_ERR_BAD_PARAM);

        close(sv[0]);
        close(sv[1]);
    } else {
        if (sv[0] >= 0) close(sv[0]);
        if (sv[1] >= 0) close(sv[1]);
    }

    int sv_big[2] = {-1, -1};
    check("socketpair for oversized framed length", socketpair(AF_UNIX, TEST_UDS_SOCK_TYPE, 0, sv_big) == 0);
    if (sv_big[0] >= 0 && sv_big[1] >= 0) {
        nipc_uds_session_t large_session;
        memset(&large_session, 0, sizeof(large_session));
        large_session.fd = sv_big[0];
        large_session.packet_size = NIPC_HEADER_LEN + 1;
        large_session.role = NIPC_UDS_ROLE_SERVER;

        nipc_header_t large_hdr = {0};
        large_hdr.kind = NIPC_KIND_REQUEST;
        large_hdr.code = 1;
        large_hdr.message_id = 2;
        large_hdr.item_count = 1;
        uint8_t one_byte_payload = 0;

        check("send rejects oversized total message length",
              nipc_uds_send(&large_session, &large_hdr,
                            &one_byte_payload, (size_t)UINT32_MAX)
                  == NIPC_UDS_ERR_LIMIT_EXCEEDED);

        close(sv_big[0]);
        close(sv_big[1]);
    } else {
        if (sv_big[0] >= 0) close(sv_big[0]);
        if (sv_big[1] >= 0) close(sv_big[1]);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: Close listener edge cases                                    */
/* ------------------------------------------------------------------ */

static void test_close_listener_edge(void)
{
    printf("Test: Close listener edge cases\n");

    /* Close NULL listener should not crash */
    nipc_uds_close_listener(NULL);
    check("close null listener does not crash", 1);

    /* Close listener with fd=-1 */
    nipc_uds_listener_t listener;
    memset(&listener, 0, sizeof(listener));
    listener.fd = -1;
    listener.path[0] = '\0';
    nipc_uds_close_listener(&listener);
    check("close invalid listener does not crash", 1);
}

/* ------------------------------------------------------------------ */
/*  Test: Close session edge cases                                     */
/* ------------------------------------------------------------------ */

static void test_close_session_edge(void)
{
    printf("Test: Close session edge cases\n");

    /* Close session with fd=-1 should not crash */
    nipc_uds_session_t session;
    memset(&session, 0, sizeof(session));
    session.fd = -1;
    session.recv_buf = NULL;
    nipc_uds_close_session(&session);
    check("close session fd=-1 does not crash", 1);
}

/* ------------------------------------------------------------------ */
/*  Test: Listen path too long                                         */
/* ------------------------------------------------------------------ */

static void test_listen_path_too_long(void)
{
    printf("Test: Listen with path too long\n");

    char long_dir[4096];
    memset(long_dir, 'x', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    check("listen path too long",
          nipc_uds_listen(long_dir, "svc", &scfg, &listener)
              == NIPC_UDS_ERR_PATH_TOO_LONG);
}

/* ------------------------------------------------------------------ */
/*  Test: Connect path too long                                        */
/* ------------------------------------------------------------------ */

static void test_connect_path_too_long(void)
{
    printf("Test: Connect with path too long\n");

    char long_dir[4096];
    memset(long_dir, 'x', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    check("connect path too long",
          nipc_uds_connect(long_dir, "svc", &ccfg, &session)
              == NIPC_UDS_ERR_PATH_TOO_LONG);
}

/* ------------------------------------------------------------------ */
/*  Test: Pipeline 10 requests, verify all matched by message_id       */
/* ------------------------------------------------------------------ */

static void test_pipeline_10(void)
{
    printf("Test: Pipeline 10 requests, verify all matched by message_id\n");
    const char *svc = "test_pipe10";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service      = svc,
        .config       = default_server_config(),
        .accept_count = 1,
        .echo_count   = 10,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send 10 requests before reading any response */
        for (uint64_t i = 1; i <= 10; i++) {
            uint8_t payload[8];
            memcpy(payload, &i, sizeof(i));
            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = i,
            };
            err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
            if (err != NIPC_UDS_OK) {
                check("send all 10", 0);
                break;
            }
        }

        /* Receive 10 responses, verify message_id and payload */
        int all_match = 1;
        for (uint64_t i = 1; i <= 10; i++) {
            uint8_t rbuf[4096];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                    &rhdr, &rp, &rlen);
            if (err != NIPC_UDS_OK) {
                all_match = 0;
                break;
            }
            uint64_t val;
            if (rlen != 8 || rhdr.message_id != i) {
                all_match = 0;
                continue;
            }
            memcpy(&val, rp, 8);
            if (val != i)
                all_match = 0;
        }
        check("all 10 responses matched by message_id and payload", all_match);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: Pipeline 100 requests (stress pipelining)                    */
/* ------------------------------------------------------------------ */

static void test_pipeline_100(void)
{
    printf("Test: Pipeline 100 requests (stress pipelining)\n");
    const char *svc = "test_pipe100";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.max_request_payload_bytes = 65536;
    scfg.max_response_payload_bytes = 65536;

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = 100,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send 100 requests before reading any */
        int send_ok = 1;
        for (uint64_t i = 1; i <= 100; i++) {
            uint8_t payload[8];
            memcpy(payload, &i, sizeof(i));
            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = i,
            };
            if (nipc_uds_send(&session, &hdr, payload, 8) != NIPC_UDS_OK) {
                send_ok = 0;
                break;
            }
        }
        check("sent all 100 requests", send_ok);

        /* Receive 100 responses */
        int recv_ok = 1;
        for (uint64_t i = 1; i <= 100; i++) {
            uint8_t rbuf[4096];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            if (nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                  &rhdr, &rp, &rlen) != NIPC_UDS_OK) {
                recv_ok = 0;
                break;
            }
            uint64_t val;
            if (rlen != 8 || rhdr.message_id != i) {
                recv_ok = 0;
                continue;
            }
            memcpy(&val, rp, 8);
            if (val != i)
                recv_ok = 0;
        }
        check("all 100 responses matched by message_id and payload", recv_ok);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: Pipeline with mixed message sizes                            */
/* ------------------------------------------------------------------ */

static void test_pipeline_mixed_sizes(void)
{
    printf("Test: Pipeline with mixed message sizes (8, 256, 1024 bytes)\n");
    const char *svc = "test_pipemix";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.max_request_payload_bytes = 65536;
    scfg.max_response_payload_bytes = 65536;

    /* 9 messages: 3 sizes x 3 */
    const int count = 9;
    const size_t sizes[] = {8, 256, 1024, 8, 256, 1024, 8, 256, 1024};

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = count,
    };

    pthread_t tid = start_echo_server(&sctx);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send all 9 messages with varying sizes */
        for (int i = 0; i < count; i++) {
            size_t sz = sizes[i];
            uint8_t *payload = malloc(sz);
            for (size_t j = 0; j < sz; j++)
                payload[j] = (uint8_t)((i * 37 + j) & 0xFF);

            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = (uint64_t)(i + 1),
            };
            nipc_uds_send(&session, &hdr, payload, sz);
            free(payload);
        }

        /* Receive all 9 responses */
        int all_ok = 1;
        for (int i = 0; i < count; i++) {
            size_t sz = sizes[i];
            uint8_t rbuf[4096];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            if (nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                  &rhdr, &rp, &rlen) != NIPC_UDS_OK) {
                all_ok = 0;
                break;
            }
            if (rhdr.message_id != (uint64_t)(i + 1) || rlen != sz) {
                all_ok = 0;
                continue;
            }
            /* Verify payload content */
            const uint8_t *rpp = (const uint8_t *)rp;
            for (size_t j = 0; j < sz; j++) {
                if (rpp[j] != (uint8_t)((i * 37 + j) & 0xFF)) {
                    all_ok = 0;
                    break;
                }
            }
        }
        check("all mixed-size responses correct", all_ok);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: Pipeline with chunked messages (> packet_size)               */
/* ------------------------------------------------------------------ */

/* Chunked echo server for pipeline: reads N messages, echoes each. */
static void *chunked_echo_server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err != NIPC_UDS_OK) {
        nipc_uds_close_listener(&listener);
        test_flag_set(&ctx->done);
        return NULL;
    }

    for (int m = 0; m < ctx->echo_count; m++) {
        uint8_t buf[256]; /* small buf, forces recv_buf alloc */
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        err = nipc_uds_receive(&session, buf, sizeof(buf),
                                &hdr, &payload, &payload_len);
        if (err != NIPC_UDS_OK)
            break;

        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        resp.transport_status = NIPC_STATUS_OK;

        err = nipc_uds_send(&session, &resp, payload, payload_len);
        if (err != NIPC_UDS_OK)
            break;
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_pipeline_chunked(void)
{
    printf("Test: Pipeline with chunked messages (> packet_size)\n");
    const char *svc = "test_pipechk";
    cleanup_socket(svc);

    /* Small packet size to force chunking */
    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = 128;
    scfg.max_request_payload_bytes  = 65536;
    scfg.max_response_payload_bytes = 65536;

    const int count = 5;
    const size_t sizes[] = {200, 500, 300, 800, 150};

    server_ctx_t sctx = {
        .service      = svc,
        .config       = scfg,
        .accept_count = 1,
        .echo_count   = count,
    };

    pthread_t tid;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);
    pthread_create(&tid, NULL, chunked_echo_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("chunked pipeline server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_request_payload_bytes  = 65536;
    ccfg.max_response_payload_bytes = 65536;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send all messages (each chunked) */
        for (int i = 0; i < count; i++) {
            size_t sz = sizes[i];
            uint8_t *payload = malloc(sz);
            for (size_t j = 0; j < sz; j++)
                payload[j] = (uint8_t)((i + j) & 0xFF);

            nipc_header_t hdr = {
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = (uint64_t)(i + 1),
            };
            nipc_uds_send(&session, &hdr, payload, sz);
            free(payload);
        }

        /* Receive all responses */
        int all_ok = 1;
        for (int i = 0; i < count; i++) {
            size_t sz = sizes[i];
            uint8_t rbuf[256];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            if (nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                  &rhdr, &rp, &rlen) != NIPC_UDS_OK) {
                all_ok = 0;
                break;
            }
            if (rhdr.message_id != (uint64_t)(i + 1) || rlen != sz) {
                all_ok = 0;
                continue;
            }
            const uint8_t *rpp = (const uint8_t *)rp;
            for (size_t j = 0; j < sz; j++) {
                if (rpp[j] != (uint8_t)((i + j) & 0xFF)) {
                    all_ok = 0;
                    break;
                }
            }
        }
        check("all chunked pipeline responses correct", all_ok);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: Pipelining beyond the socket buffers                         */
/* ------------------------------------------------------------------ */

/* Small buffers make the deadlock reachable on every platform (macOS and
 * FreeBSD 14 default to 8 KiB, FreeBSD 15 to 64 KiB, Linux to about
 * 208 KiB). The client's send timeout
 * only bounds a failing run: a deadlocked send fails instead of hanging. A
 * passing run never waits on it, and the server has none, so a slow server
 * cannot fail a passing run. */
static void shrink_socket_buffers(int fd, bool bound_sends)
{
    int size = 4096;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if (bound_sends) {
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}

#define PIPE_BUF_COUNT 16
#define PIPE_BUF_SIZE  16000
#define PIPE_BUF_PKT   1024
/* Flood mode: small responses keep the client's queue bound small. */
#define PIPE_BUF_FLOOD_RESP 4096

typedef enum {
    SMALL_BUF_ECHO = 1,        /* receive one request, send its response */
    SMALL_BUF_CLOSE_AFTER_ONE, /* receive one request, then close */
    SMALL_BUF_FLOOD,           /* receive one request, then send responses
                                  without reading until the send fails */
} small_buf_mode_t;

typedef struct {
    server_ctx_t     base;
    small_buf_mode_t mode;
} small_buf_ctx_t;

static void *small_buffer_server_thread(void *arg)
{
    small_buf_ctx_t *sb = (small_buf_ctx_t *)arg;
    server_ctx_t *ctx = &sb->base;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err != NIPC_UDS_OK) {
        nipc_uds_close_listener(&listener);
        test_flag_set(&ctx->done);
        return NULL;
    }
    shrink_socket_buffers(session.fd, false);

    int count = sb->mode == SMALL_BUF_ECHO ? ctx->echo_count : 1;
    for (int m = 0; m < count; m++) {
        uint8_t buf[PIPE_BUF_PKT];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        err = nipc_uds_receive(&session, buf, sizeof(buf),
                                &hdr, &payload, &payload_len);
        if (err != NIPC_UDS_OK)
            break;

        if (sb->mode == SMALL_BUF_ECHO) {
            nipc_header_t resp = hdr;
            resp.kind = NIPC_KIND_RESPONSE;
            resp.transport_status = NIPC_STATUS_OK;
            err = nipc_uds_send(&session, &resp, payload, payload_len);
            if (err != NIPC_UDS_OK)
                break;
        }
    }

    if (sb->mode == SMALL_BUF_FLOOD) {
        static uint8_t resp_payload[PIPE_BUF_FLOOD_RESP];
        for (;;) {
            nipc_header_t resp = {
                .kind = NIPC_KIND_RESPONSE, .code = 1,
                .item_count = 1, .message_id = 1,
                .transport_status = NIPC_STATUS_OK,
            };
            if (nipc_uds_send(&session, &resp, resp_payload,
                              sizeof(resp_payload)) != NIPC_UDS_OK)
                break;
        }
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

/* Starts the server in the given mode and connects a client; both ends get
 * small socket buffers. Returns 0 on success. */
static int small_buffer_start(const char *svc, small_buf_mode_t mode,
                              uint32_t max_response, small_buf_ctx_t *sb,
                              pthread_t *tid, nipc_uds_session_t *session)
{
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = PIPE_BUF_PKT;
    scfg.max_request_payload_bytes  = 65536;
    scfg.max_response_payload_bytes = max_response;

    memset(sb, 0, sizeof(*sb));
    sb->base.service = svc;
    sb->base.config = scfg;
    sb->base.accept_count = 1;
    sb->base.echo_count = PIPE_BUF_COUNT;
    sb->mode = mode;

    pthread_create(tid, NULL, small_buffer_server_thread, sb);
    (void)test_flag_wait_any(&sb->base.ready, &sb->base.done);
    check("small-buffer server ready",
          __atomic_load_n(&sb->base.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = PIPE_BUF_PKT;
    ccfg.max_request_payload_bytes  = 65536;
    ccfg.max_response_payload_bytes = max_response;

    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, session);
    check("connect", err == NIPC_UDS_OK);
    if (err != NIPC_UDS_OK)
        return -1;
    shrink_socket_buffers(session->fd, true);
    return 0;
}

static nipc_uds_error_t send_pipelined_request(nipc_uds_session_t *session,
                                               uint8_t *payload, int i)
{
    for (size_t j = 0; j < PIPE_BUF_SIZE; j++)
        payload[j] = (uint8_t)((i * 7 + j) & 0xFF);
    nipc_header_t hdr = {
        .kind = NIPC_KIND_REQUEST, .code = 1,
        .item_count = 1, .message_id = (uint64_t)(i + 1),
    };
    return nipc_uds_send(session, &hdr, payload, PIPE_BUF_SIZE);
}

/* A client that sends every request before reading any response must not
 * deadlock with the server when the bytes in flight exceed the buffers. */
static void test_pipeline_exceeds_socket_buffers(void)
{
    printf("Test: Pipelining beyond the socket buffers completes\n");
    const char *svc = "test_pipebuf";
    small_buf_ctx_t sb;
    pthread_t tid;
    nipc_uds_session_t session;

    if (small_buffer_start(svc, SMALL_BUF_ECHO, 65536, &sb, &tid,
                           &session) == 0) {
        check("no pending data before sending",
              !nipc_uds_session_has_pending(&session));
        uint8_t *payload = malloc(PIPE_BUF_SIZE);
        int sent_ok = 1;
        for (int i = 0; i < PIPE_BUF_COUNT && sent_ok; i++) {
            if (send_pipelined_request(&session, payload, i) != NIPC_UDS_OK)
                sent_ok = 0;
        }
        free(payload);
        check("all pipelined requests sent", sent_ok);
        /* The buffers hold a fraction of the responses, so finishing the
         * sends required reading responses into the queue. */
        check("responses queued during the sends",
              nipc_uds_session_has_pending(&session));

        int all_ok = sent_ok;
        for (int i = 0; i < PIPE_BUF_COUNT && all_ok; i++) {
            uint8_t rbuf[PIPE_BUF_PKT];
            nipc_header_t rhdr;
            const void *rp;
            size_t rlen;
            if (nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                  &rhdr, &rp, &rlen) != NIPC_UDS_OK) {
                all_ok = 0;
                break;
            }
            if (rhdr.message_id != (uint64_t)(i + 1) || rlen != PIPE_BUF_SIZE) {
                all_ok = 0;
                break;
            }
            const uint8_t *rpp = (const uint8_t *)rp;
            for (size_t j = 0; j < PIPE_BUF_SIZE; j++) {
                if (rpp[j] != (uint8_t)((i * 7 + j) & 0xFF)) {
                    all_ok = 0;
                    break;
                }
            }
        }
        check("all pipelined responses correct", all_ok);
        check("pending queue drained", session.pending_tail == 0);
        check("no pending data after receiving",
              !nipc_uds_session_has_pending(&session));

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* A receive into a buffer too small for a header fails without reading a
 * byte, so the message stays queued and the session stays usable. */
static void test_receive_undersized_buffer_keeps_session(void)
{
    printf("Test: Undersized receive buffer keeps the session\n");
    int sv[2] = {-1, -1};
    check("socketpair",
          socketpair(AF_UNIX, TEST_UDS_SOCK_TYPE, 0, sv) == 0);
    if (sv[0] < 0 || sv[1] < 0)
        return;

    nipc_uds_session_t rx, tx;
    memset(&rx, 0, sizeof(rx));
    rx.fd = sv[0];
    rx.role = NIPC_UDS_ROLE_SERVER;
    rx.packet_size = 4096;
    rx.max_request_payload_bytes = 1024;
    rx.max_request_batch_items = 1;
    memset(&tx, 0, sizeof(tx));
    tx.fd = sv[1];
    tx.role = NIPC_UDS_ROLE_SERVER;
    tx.packet_size = 4096;

    nipc_header_t hdr = {
        .kind = NIPC_KIND_REQUEST, .code = 1, .item_count = 1,
        .message_id = 7,
    };
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    check("request sent",
          nipc_uds_send(&tx, &hdr, payload, sizeof(payload)) == NIPC_UDS_OK);

    uint8_t small[NIPC_HEADER_LEN - 1];
    uint8_t rbuf[256];
    nipc_header_t rhdr;
    const void *rp = NULL;
    size_t rlen = 0;
    check("undersized buffer rejected",
          nipc_uds_receive(&rx, small, sizeof(small), &rhdr, &rp, &rlen) ==
              NIPC_UDS_ERR_BAD_PARAM);
    check("request still received",
          nipc_uds_receive_timeout(&rx, rbuf, sizeof(rbuf), &rhdr, &rp, &rlen,
                                   1000, -1) == NIPC_UDS_OK &&
              rhdr.message_id == 7 && rlen == sizeof(payload) &&
              memcmp(rp, payload, sizeof(payload)) == 0);

    free(rx.recv_buf);
    nipc_uds_pending_free(&rx);
    close(sv[0]);
    close(sv[1]);
}

#if !TEST_UDS_STREAM
/* A datagram longer than its header declares is a protocol error. The
 * pending queue, filled while a send waits for buffer space, must keep the
 * whole datagram so its receive rejects it as the direct receive does,
 * instead of storing it cut to packet_size and accepting it. */
static void test_pending_oversized_datagram_rejected(void)
{
    printf("Test: Oversized datagram rejected from the pending queue\n");
    int sv[2] = {-1, -1};
    check("socketpair", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    if (sv[0] < 0 || sv[1] < 0)
        return;

    nipc_uds_session_t s;
    memset(&s, 0, sizeof(s));
    s.fd = sv[0];
    s.role = NIPC_UDS_ROLE_SERVER;
    s.packet_size = 2 * NIPC_HEADER_LEN;
    s.max_request_payload_bytes = 1024;
    s.max_request_batch_items = 1;

    /* The header declares exactly packet_size; 16 more bytes follow. */
    uint8_t dgram[2 * NIPC_HEADER_LEN + 16];
    memset(dgram, 0xAB, sizeof(dgram));
    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN, .kind = NIPC_KIND_REQUEST,
        .code = 1, .item_count = 1, .message_id = 1,
        .payload_len = NIPC_HEADER_LEN,
    };
    nipc_header_encode(&hdr, dgram, NIPC_HEADER_LEN);

    uint8_t rbuf[256];
    nipc_header_t rhdr;
    const void *rp;
    size_t rlen;

    check("direct datagram sent",
          send(sv[1], dgram, sizeof(dgram), 0) == (ssize_t)sizeof(dgram));
    check("direct receive rejects it",
          nipc_uds_receive(&s, rbuf, sizeof(rbuf), &rhdr, &rp, &rlen) ==
              NIPC_UDS_ERR_PROTOCOL);

    check("queued datagram sent",
          send(sv[1], dgram, sizeof(dgram), 0) == (ssize_t)sizeof(dgram));
    bool eof = false;
    check("drain queues it",
          nipc_uds_pending_drain(&s, 4096, &eof) == NIPC_UDS_OK && !eof &&
              nipc_uds_pending_queued(&s));
    check("pending receive rejects it",
          nipc_uds_receive(&s, rbuf, sizeof(rbuf), &rhdr, &rp, &rlen) ==
              NIPC_UDS_ERR_PROTOCOL);
    check("pending queue consumed", !nipc_uds_pending_queued(&s));

    nipc_uds_pending_free(&s);
    close(sv[0]);
    close(sv[1]);
}
#endif

#if TEST_UDS_STREAM
/* Stream baseline: a receive that fails mid-stream marks the receive side failed, and
 * every later receive must fail. Bytes left in the pending queue are part
 * of that abandoned stream: here a forged response for an in-flight id
 * follows an over-limit header, and must not be delivered. */
static void test_stream_failed_receive_drops_pending(void)
{
    printf("Test: Failed stream receive drops pending bytes\n");
    const char *svc = "test_pipestale";
    small_buf_ctx_t sb;
    pthread_t tid;
    nipc_uds_session_t session;

    if (small_buffer_start(svc, SMALL_BUF_CLOSE_AFTER_ONE, 65536, &sb, &tid,
                           &session) == 0) {
        uint8_t *payload = malloc(PIPE_BUF_SIZE);
        check("request 1 sent",
              send_pipelined_request(&session, payload, 0) == NIPC_UDS_OK);
        free(payload);

        nipc_header_t over = {
            .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN, .kind = NIPC_KIND_RESPONSE,
            .item_count = 1, .message_id = 1, .payload_len = 65537,
        };
        nipc_header_t forged = over;
        forged.payload_len = 0;
        session.pending_buf = malloc(2 * NIPC_HEADER_LEN);
        nipc_header_encode(&over, session.pending_buf, NIPC_HEADER_LEN);
        nipc_header_encode(&forged, session.pending_buf + NIPC_HEADER_LEN,
                           NIPC_HEADER_LEN);
        session.pending_cap = 2 * NIPC_HEADER_LEN;
        session.pending_head = 0;
        session.pending_tail = 2 * NIPC_HEADER_LEN;

        uint8_t rbuf[PIPE_BUF_PKT];
        nipc_header_t rhdr;
        const void *rp;
        size_t rlen;
        check("over-limit response rejected",
              nipc_uds_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rp,
                               &rlen) == NIPC_UDS_ERR_LIMIT_EXCEEDED);
        check("later receive fails",
              nipc_uds_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rp,
                               &rlen) != NIPC_UDS_OK);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}
#endif

/* The server closes while the pipelining client waits for buffer space:
 * the send fails instead of waiting forever. */
static void test_pipeline_peer_close_during_send(void)
{
    printf("Test: Peer close during a pipelined send fails the send\n");
    const char *svc = "test_pipeclose";
    small_buf_ctx_t sb;
    pthread_t tid;
    nipc_uds_session_t session;

    if (small_buffer_start(svc, SMALL_BUF_CLOSE_AFTER_ONE, 65536, &sb, &tid,
                           &session) == 0) {
        uint8_t *payload = malloc(PIPE_BUF_SIZE);
        nipc_uds_error_t err = NIPC_UDS_OK;
        for (int i = 0; i < PIPE_BUF_COUNT && err == NIPC_UDS_OK; i++)
            err = send_pipelined_request(&session, payload, i);
        free(payload);
        check("pipelined send fails with SEND", err == NIPC_UDS_ERR_SEND);
        check("in-flight set cleared", session.inflight_count == 0);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* A server that sends more than the in-flight responses can occupy fails
 * the client session instead of growing its queue without limit. */
static void test_pipeline_pending_bound(void)
{
    printf("Test: Inbound bytes beyond the in-flight responses fail the session\n");
    const char *svc = "test_pipebound";
    small_buf_ctx_t sb;
    pthread_t tid;
    nipc_uds_session_t session;

    if (small_buffer_start(svc, SMALL_BUF_FLOOD, PIPE_BUF_FLOOD_RESP, &sb,
                           &tid, &session) == 0) {
        uint8_t *payload = malloc(PIPE_BUF_SIZE);
        nipc_uds_error_t err = NIPC_UDS_OK;
        for (int i = 0; i < PIPE_BUF_COUNT && err == NIPC_UDS_OK; i++)
            err = send_pipelined_request(&session, payload, i);
        free(payload);
        check("pipelined send fails with PROTOCOL",
              err == NIPC_UDS_ERR_PROTOCOL);
        check("in-flight set cleared", session.inflight_count == 0);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: handshake with wrong message kind from mock client        */
/* ------------------------------------------------------------------ */

static void *wrong_kind_server(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    nipc_uds_server_config_t scfg = ctx->config;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                             &scfg, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 1, &session);
    /* We expect this to fail because the client sends wrong kind */
    if (err == NIPC_UDS_OK)
        nipc_uds_close_session(&session);

    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_handshake_wrong_kind(void)
{
    printf("Test: Handshake with wrong message kind\n");
    const char *svc = "test_wrong_kind";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service = svc,
        .config  = default_server_config(),
    };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, wrong_kind_server, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect raw socket and send a RESPONSE instead of HELLO */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    check("raw connect", rc == 0);

    if (rc == 0) {
        /* Send a RESPONSE header where server expects HELLO */
        nipc_header_t bad_hdr = {
            .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_RESPONSE, /* wrong kind */
            .code = 99,
        };
        uint8_t buf[32];
        nipc_header_encode(&bad_hdr, buf, sizeof(buf));
        send(fd, buf, 32, MSG_NOSIGNAL);
        /* Server should reject this handshake */
    }
    close(fd);

    pthread_join(tid, NULL);
    check("server finished", __atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) == 1);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: inflight_add duplicate, inflight_remove not-found         */
/* ------------------------------------------------------------------ */

static void test_inflight_duplicate(void)
{
    printf("Test: inflight duplicate message_id\n");
    const char *svc = "test_infldup";
    cleanup_socket(svc);

    /* Start a server to accept one client */
    server_ctx_t sctx = {
        .service = svc,
        .config  = default_server_config(),
        .accept_count = 1,
        .echo_count = 2,
    };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, echo_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Connect client */
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send a request with message_id=100 */
        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = 1;
        hdr.message_id = 100;
        hdr.item_count = 1;
        uint8_t payload[] = {0xAA};
        err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("first send ok", err == NIPC_UDS_OK);

        /* Send another request with same message_id=100 (duplicate) */
        err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("duplicate message_id rejected",
              err == NIPC_UDS_ERR_DUPLICATE_MSG_ID);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: directional request/response negotiation                 */
/* ------------------------------------------------------------------ */

static void *oversized_server(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                           &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 1, &session);
    if (err == NIPC_UDS_OK) {
        /* Read one request, then send a large response that fits the server-advertised limit. */
        uint8_t buf[4096];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                    &hdr, &payload, &payload_len);
        if (rerr == NIPC_UDS_OK) {
            /* Build oversized response: use server's direct send */
            nipc_header_t resp = {0};
            resp.kind = NIPC_KIND_RESPONSE;
            resp.code = hdr.code;
            resp.message_id = hdr.message_id;
            resp.item_count = 1;
            resp.transport_status = NIPC_STATUS_OK;
            uint8_t big[2048];
            memset(big, 0xBB, sizeof(big));
            nipc_uds_send(&session, &resp, big, sizeof(big));
        }
        nipc_uds_close_session(&session);
    }
    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_directional_limit_negotiation(void)
{
    printf("Test: Directional request/response negotiation\n");
    const char *svc = "test_exceed_limit";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service = svc,
        .config  = default_server_config(),
    };
    sctx.config.max_response_payload_bytes = 65536;
    sctx.config.max_response_batch_items = 32;
    sctx.config.max_request_payload_bytes = 65536;
    sctx.config.max_request_batch_items = 8;
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, oversized_server, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client requests large request capacity but advertises small response capacity. */
    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.max_request_payload_bytes = 65536;
    ccfg.max_request_batch_items = 16;
    ccfg.max_response_payload_bytes = 128;
    ccfg.max_response_batch_items = 16;
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        check("request payload echoes client proposal",
              session.max_request_payload_bytes == 65536);
        check("request batch echoes client proposal",
              session.max_request_batch_items == 16);
        check("response payload uses server final value",
              session.max_response_payload_bytes == 65536);
        check("response batch stays symmetric with request batch",
              session.max_response_batch_items == 16);
        check("server returns non-zero session_id",
              session.session_id != 0);

        /* Send a request */
        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = 1;
        hdr.message_id = 1;
        hdr.item_count = 1;
        uint8_t payload[] = {0xAA};
        nipc_uds_send(&session, &hdr, payload, sizeof(payload));

        /* Receive - should fail because server sends > negotiated limit */
        uint8_t buf[4096];
        nipc_header_t resp_hdr;
        const void *rpay;
        size_t rpay_len;
        nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                 &resp_hdr, &rpay, &rpay_len);
        check("large server-driven response accepted", rerr == NIPC_UDS_OK);
        if (rerr == NIPC_UDS_OK) {
            check("response payload length", rpay_len == 2048);
            check("response payload byte", ((const uint8_t *)rpay)[0] == 0xBB);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_payload_exceeds_limit(void)
{
    printf("Test: Receive rejects response payload over limit\n");
    const char *svc = "test_payload_over_limit";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.max_response_payload_bytes = 4096;

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = scfg,
        .mode = RAW_RESP_PAYLOAD_EXCEEDED,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7003,
        };
        uint8_t payload[] = { 0xAE };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("response payload over limit rejected", rerr == NIPC_UDS_ERR_LIMIT_EXCEEDED);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: receive with unknown response message_id                  */
/* ------------------------------------------------------------------ */

static void *wrong_msgid_server(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    nipc_uds_server_config_t scfg = ctx->config;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                             &scfg, &listener);
    if (err != NIPC_UDS_OK) {
        test_flag_set(&ctx->done);
        return NULL;
    }
    test_flag_set(&ctx->ready);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 1, &session);
    if (err == NIPC_UDS_OK) {
        uint8_t buf[4096];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;
        nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                    &hdr, &payload, &payload_len);
        if (rerr == NIPC_UDS_OK) {
            /* Send response with WRONG message_id */
            nipc_header_t resp = {0};
            resp.kind = NIPC_KIND_RESPONSE;
            resp.code = hdr.code;
            resp.message_id = hdr.message_id + 999; /* wrong! */
            resp.item_count = 1;
            resp.transport_status = NIPC_STATUS_OK;
            uint8_t rpay[] = {0xBB};
            nipc_uds_send(&session, &resp, rpay, sizeof(rpay));
        }
        nipc_uds_close_session(&session);
    }
    nipc_uds_close_listener(&listener);
    test_flag_set(&ctx->done);
    return NULL;
}

static void test_receive_unknown_message_id(void)
{
    printf("Test: Receive with unknown response message_id\n");
    const char *svc = "test_unknown_mid";
    cleanup_socket(svc);

    server_ctx_t sctx = {
        .service = svc,
        .config  = default_server_config(),
    };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, wrong_msgid_server, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = 1;
        hdr.message_id = 42;
        hdr.item_count = 1;
        uint8_t payload[] = {0xAA};
        nipc_uds_send(&session, &hdr, payload, sizeof(payload));

        uint8_t buf[4096];
        nipc_header_t resp_hdr;
        const void *rpay;
        size_t rpay_len;
        nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                    &resp_hdr, &rpay, &rpay_len);
        check("unknown message_id rejected",
              rerr == NIPC_UDS_ERR_UNKNOWN_MSG_ID);

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: stale detection of live server (ADDR_IN_USE)              */
/* ------------------------------------------------------------------ */

static void test_stale_live_server(void)
{
    printf("Test: Stale detection of live server (ADDR_IN_USE)\n");
    const char *svc = "test_stale_live";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener1;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener1);
    check("first listen ok", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Try to listen again on same path - should detect live server */
        nipc_uds_listener_t listener2;
        err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener2);
        check("second listen ADDR_IN_USE", err == NIPC_UDS_ERR_ADDR_IN_USE);

        nipc_uds_close_listener(&listener1);
    }
    cleanup_socket(svc);
}

/* A server between bind() and listen() refuses connects just like a dead
 * server's leftover socket file. A second server starting in that window
 * must not take the endpoint over: it fails with ADDR_IN_USE, and the
 * first server stays reachable. */
typedef struct {
    const char *service;
    int bound;
    int release;
    int listened;
    nipc_uds_error_t err;
    nipc_uds_listener_t listener;
} held_listen_t;

static void hold_after_bind(void *user)
{
    held_listen_t *held = (held_listen_t *)user;
    test_flag_set(&held->bound);
    (void)test_flag_wait(&held->release);
}

static void *held_listen_thread(void *arg)
{
    held_listen_t *held = (held_listen_t *)arg;
    nipc_uds_server_config_t scfg = default_server_config();
    held->err = nipc_uds_listen(TEST_RUN_DIR, held->service, &scfg,
                                &held->listener);
    test_flag_set(&held->listened);
    return NULL;
}

static void test_listen_excludes_starting_server(void)
{
    printf("Test: a server between bind and listen keeps its endpoint\n");
    const char *svc = "test_listen_window";
    cleanup_socket(svc);

    held_listen_t held = {.service = svc};
    nipc_uds_set_listen_hook_for_tests(hold_after_bind, &held);
    pthread_t tid;
    pthread_create(&tid, NULL, held_listen_thread, &held);
    check("first server bound", test_flag_wait(&held.bound));
    nipc_uds_set_listen_hook_for_tests(NULL, NULL);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t second;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &second);
    check("second server gets ADDR_IN_USE", err == NIPC_UDS_ERR_ADDR_IN_USE);
    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&second);

    test_flag_set(&held.release);
    check("first server listens", test_flag_wait(&held.listened));
    pthread_join(tid, NULL);
    check("first listen ok", held.err == NIPC_UDS_OK);

    if (held.err == NIPC_UDS_OK) {
        /* A connect to the endpoint must reach the first server. */
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s.sock",
                 TEST_RUN_DIR, svc);
        int cfd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
        check("connect to endpoint",
              cfd >= 0 && connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        struct pollfd pfd = {.fd = held.listener.fd, .events = POLLIN};
        check("first server receives the connection",
              poll(&pfd, 1, (int)TEST_WAIT_LIMIT_MS) == 1);
        if (cfd >= 0)
            close(cfd);
        nipc_uds_close_listener(&held.listener);
    }

    /* After the owner closed, the endpoint can be taken again. */
    err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &second);
    check("endpoint free after close", err == NIPC_UDS_OK);
    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&second);
    cleanup_socket(svc);
}

static mode_t lock_file_mode(const char *svc)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.lock", TEST_RUN_DIR, svc);
    struct stat st;
    return stat(path, &st) == 0 ? (st.st_mode & 0777) : 0;
}

/* Under umask, the endpoint lock is created, contended and reclaimed. */
static void check_lock_under_umask(const char *svc, mode_t mask)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.lock", TEST_RUN_DIR, svc);
    unlink(path);
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t first, second;
    mode_t old_mask = umask(mask);
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &first);
    nipc_uds_error_t busy = NIPC_UDS_ERR_SOCKET, again = NIPC_UDS_ERR_SOCKET;
    if (err == NIPC_UDS_OK) {
        busy = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &second);
        if (busy == NIPC_UDS_OK)
            nipc_uds_close_listener(&second);
        nipc_uds_close_listener(&first);
        again = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &second);
        if (again == NIPC_UDS_OK)
            nipc_uds_close_listener(&second);
    }
    umask(old_mask);
    printf("  umask %04o: listen=%d second=%d restart=%d lock mode=%04o\n",
           (unsigned)mask, err, busy, again, (unsigned)lock_file_mode(svc));
    check("listen creates the lock", err == NIPC_UDS_OK);
    check("a second server gets ADDR_IN_USE", busy == NIPC_UDS_ERR_ADDR_IN_USE);
    check("the endpoint restarts", again == NIPC_UDS_OK);
    check("the lock file is 0600", lock_file_mode(svc) == 0600);
    cleanup_socket(svc);
}

/* A leftover lock file of the given mode must not block a start. */
static void check_lock_leftover_mode(const char *svc, mode_t mode)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.lock", TEST_RUN_DIR, svc);
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check("leftover lock created", fd >= 0 && fchmod(fd, mode) == 0);
    if (fd >= 0)
        close(fd);
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    printf("  leftover %04o: listen=%d lock mode=%04o\n", (unsigned)mode, err,
           (unsigned)lock_file_mode(svc));
    check("listen over the leftover lock", err == NIPC_UDS_OK);
    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&listener);
    check("the lock file is 0600 again", lock_file_mode(svc) == 0600);
    cleanup_socket(svc);
}

static void test_listen_lock_restrictive_umask(void)
{
    printf("Test: endpoint lock under a umask without owner read or write\n");
    check_lock_under_umask("test_lock_umask", 0477);
    check_lock_under_umask("test_lock_umask", 0277);
    check_lock_leftover_mode("test_lock_leftover", 0200);
    check_lock_leftover_mode("test_lock_leftover", 0400);
    check_lock_leftover_mode("test_lock_leftover", 0000);
}

/* A child forked by another thread shares the lock's open file description
 * until it execs. Closing the listener must still free the endpoint. */
static void test_listen_close_releases_lock_held_by_child(void)
{
    printf("Test: close frees the endpoint while a forked child holds the lock fd\n");
    const char *svc = "test_listen_child_lock";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t first;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &first);
    check("first listen ok", err == NIPC_UDS_OK);
    if (err != NIPC_UDS_OK)
        return;

    int gate[2];
    check("pipe", pipe(gate) == 0);
    pid_t child = fork();
    if (child == 0) {
        /* Keep every inherited fd until the parent closes the pipe. */
        char c;
        close(gate[1]);
        while (read(gate[0], &c, 1) < 0 && errno == EINTR)
            ;
        _exit(0);
    }
    check("child forked", child > 0);
    close(gate[0]);

    nipc_uds_close_listener(&first);
    nipc_uds_listener_t second;
    err = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &second);
    check("endpoint free while the child holds the lock fd", err == NIPC_UDS_OK);
    if (err == NIPC_UDS_OK)
        nipc_uds_close_listener(&second);

    close(gate[1]);
    if (child > 0)
        waitpid(child, NULL, 0);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: listen on invalid path (bind failure)                     */
/* ------------------------------------------------------------------ */

/* An empty run_dir or a NULL config/out is a caller error, not a path. */
static void test_listen_connect_bad_params(void)
{
    printf("Test: empty run_dir and NULL parameters are rejected\n");
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_listener_t listener;
    nipc_uds_session_t session;

    check("listen with empty run_dir",
          nipc_uds_listen("", "test_bad_params", &scfg, &listener) == NIPC_UDS_ERR_BAD_PARAM);
    check("listen with NULL run_dir",
          nipc_uds_listen(NULL, "test_bad_params", &scfg, &listener) == NIPC_UDS_ERR_BAD_PARAM);
    check("listen with NULL config",
          nipc_uds_listen(TEST_RUN_DIR, "test_bad_params", NULL, &listener) == NIPC_UDS_ERR_BAD_PARAM);
    check("listen with NULL out",
          nipc_uds_listen(TEST_RUN_DIR, "test_bad_params", &scfg, NULL) == NIPC_UDS_ERR_BAD_PARAM);
    check("connect with empty run_dir",
          nipc_uds_connect("", "test_bad_params", &ccfg, &session) == NIPC_UDS_ERR_BAD_PARAM);
    check("connect with NULL config",
          nipc_uds_connect(TEST_RUN_DIR, "test_bad_params", NULL, &session) == NIPC_UDS_ERR_BAD_PARAM);
    check("connect with NULL out",
          nipc_uds_connect(TEST_RUN_DIR, "test_bad_params", &ccfg, NULL) == NIPC_UDS_ERR_BAD_PARAM);
}

static void test_listen_bind_failure(void)
{
    printf("Test: Listen on invalid path (bind failure)\n");

    /* Use a non-existent parent directory */
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen("/tmp/nonexistent_dir_99999",
                                             "svc", &scfg, &listener);
    check("bind failure", err == NIPC_UDS_ERR_SOCKET);
}

/* ------------------------------------------------------------------ */
/*  Coverage: batch validation with corrupt directory (via echo server) */
/* ------------------------------------------------------------------ */

static void test_batch_corrupt_dir(void)
{
    printf("Test: Batch validation with corrupt directory\n");
    const char *svc = "test_batch_corrupt";
    cleanup_socket(svc);

    /* Use echo server that echoes any message back */
    server_ctx_t sctx = {
        .service = svc,
        .config  = default_server_config(),
        .accept_count = 1,
        .echo_count = 1,
    };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, echo_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        /* Send a batch REQUEST with corrupt directory. The echo server
         * will send it back as a RESPONSE. The client's receive path
         * should reject the corrupt batch directory. */
        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = 1;
        hdr.message_id = 1;
        hdr.flags = NIPC_FLAG_BATCH;
        hdr.item_count = 2;

        /* Build corrupt batch payload: 2 dir entries with unaligned offset */
        uint8_t payload[32];
        memset(payload, 0, sizeof(payload));
        uint32_t off = 3, len = 4; /* unaligned offset */
        memcpy(payload, &off, 4);
        memcpy(payload + 4, &len, 4);
        off = 0; len = 4;
        memcpy(payload + 8, &off, 4);
        memcpy(payload + 12, &len, 4);

        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send batch request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            /* The echo server sends it back as RESPONSE with same flags.
             * The client receive should validate the batch and reject. */
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *rpay;
            size_t rpay_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                        &resp_hdr, &rpay, &rpay_len);
            check("corrupt batch dir rejected",
                  rerr == NIPC_UDS_ERR_PROTOCOL || rerr == NIPC_UDS_ERR_RECV);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: malformed client HELLO_ACK handling                       */
/* ------------------------------------------------------------------ */

static void test_connect_malformed_hello_ack(void)
{
    printf("Test: Connect with malformed HELLO_ACK variants\n");

    typedef struct {
        const char *service;
        malformed_ack_mode_t mode;
        nipc_uds_error_t expected_err;
        const char *label;
    } malformed_ack_case_t;

    const malformed_ack_case_t cases[] = {
        { "test_ack_short", MALFORMED_ACK_SHORT, NIPC_UDS_ERR_PROTOCOL,
          "short HELLO_ACK rejected" },
        { "test_ack_bad_version", MALFORMED_ACK_BAD_VERSION, NIPC_UDS_ERR_INCOMPATIBLE,
          "bad-version HELLO_ACK rejected as incompatible" },
        { "test_ack_kind", MALFORMED_ACK_WRONG_KIND, NIPC_UDS_ERR_PROTOCOL,
          "wrong-kind HELLO_ACK rejected" },
        { "test_ack_status", MALFORMED_ACK_BAD_STATUS, NIPC_UDS_ERR_HANDSHAKE,
          "bad-status HELLO_ACK rejected" },
        { "test_ack_incompatible_status", MALFORMED_ACK_INCOMPATIBLE_STATUS,
          NIPC_UDS_ERR_INCOMPATIBLE,
          "incompatible-status HELLO_ACK rejected as incompatible" },
        { "test_ack_layout", MALFORMED_ACK_BAD_LAYOUT, NIPC_UDS_ERR_INCOMPATIBLE,
          "bad-layout HELLO_ACK rejected as incompatible" },
        { "test_ack_payload", MALFORMED_ACK_BAD_PAYLOAD, NIPC_UDS_ERR_PROTOCOL,
          "truncated HELLO_ACK payload rejected" },
        { "test_ack_pkt_above", MALFORMED_ACK_PACKET_ABOVE_PROPOSAL, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK packet size above the client proposal rejected" },
        { "test_ack_sel_unoffered", MALFORMED_ACK_SELECTED_NOT_OFFERED, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK selecting a profile the client did not offer rejected" },
        { "test_ack_sel_none", MALFORMED_ACK_SELECTED_NONE, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK selecting no profile rejected" },
        { "test_ack_sel_two", MALFORMED_ACK_SELECTED_TWO, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK selecting two profiles rejected" },
#if !TEST_UDS_STREAM
        /* A stream has no packet boundary: bytes after the payload are
         * read as the next packet. */
        { "test_ack_trailing", MALFORMED_ACK_TRAILING_BYTES, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK packet with bytes after its payload rejected" },
#endif
        { "test_ack_long", MALFORMED_ACK_LONG_PAYLOAD, NIPC_UDS_ERR_PROTOCOL,
          "HELLO_ACK declaring a 64-byte payload rejected" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const malformed_ack_case_t *tc = &cases[i];
        cleanup_socket(tc->service);

        malformed_ack_server_ctx_t sctx = {
            .service = tc->service,
            .mode = tc->mode,
            .ready = 0,
            .done = 0,
        };

        pthread_t tid;
        pthread_create(&tid, NULL, malformed_ack_server_thread, &sctx);

        (void)test_flag_wait_any(&sctx.ready, &sctx.done);
        check("malformed-ack server ready",
              __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

        nipc_uds_client_config_t ccfg = default_client_config();
        nipc_uds_session_t session;
        nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, tc->service, &ccfg, &session);
        check(tc->label, err == tc->expected_err);
        if (err == NIPC_UDS_OK)
            nipc_uds_close_session(&session);

        pthread_join(tid, NULL);
        cleanup_socket(tc->service);
    }
}

/* ------------------------------------------------------------------ */
/*  Coverage: malformed client HELLO payload on server accept          */
/* ------------------------------------------------------------------ */

/* Sends pkt as a raw client's HELLO and returns the server's accept result. */
static nipc_uds_error_t accept_raw_hello(const char *svc, const void *pkt,
                                         size_t len)
{
    cleanup_socket(svc);

    accept_result_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .accept_err = NIPC_UDS_OK,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, accept_result_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("accept-result server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    char path[256];
    build_socket_path(path, sizeof(path), svc);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    check("raw client socket created", fd >= 0);

    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        check("raw client connect", rc == 0);
        if (rc == 0)
            send(fd, pkt, len, MSG_NOSIGNAL);
        close(fd);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
    return sctx.accept_err;
}

static void test_accept_malformed_hello_payload(void)
{
    printf("Test: Accept rejects malformed HELLO payload\n");

    nipc_header_t hdr = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_CONTROL,
        .code = NIPC_CODE_HELLO,
        .payload_len = 4,
        .item_count = 1,
    };
    uint8_t pkt[NIPC_HEADER_LEN + NIPC_HELLO_WIRE_SIZE + 16];
    memset(pkt, 0, sizeof(pkt));
    nipc_header_encode(&hdr, pkt, sizeof(pkt));
    check("malformed HELLO payload rejected",
          accept_raw_hello("test_bad_hello_payload", pkt,
                           NIPC_HEADER_LEN + 4) == NIPC_UDS_ERR_PROTOCOL);

    /* A valid HELLO followed by 16 more bytes in the same packet, then one
     * whose header declares those 16 bytes too. */
    nipc_hello_t hello = {
        .layout_version = 1,
        .supported_profiles = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 1024,
        .max_request_batch_items = 1,
        .max_response_payload_bytes = 1024,
        .max_response_batch_items = 1,
        .packet_size = 4096,
    };
    memset(pkt, 0, sizeof(pkt));
    hdr.payload_len = NIPC_HELLO_WIRE_SIZE;
    nipc_header_encode(&hdr, pkt, sizeof(pkt));
    nipc_hello_encode(&hello, pkt + NIPC_HEADER_LEN, NIPC_HELLO_WIRE_SIZE);
#if !TEST_UDS_STREAM
    /* SEQPACKET only: on a stream the extra bytes are the next packet. */
    check("HELLO packet with bytes after its payload rejected",
          accept_raw_hello("test_hello_trailing", pkt, sizeof(pkt)) ==
              NIPC_UDS_ERR_PROTOCOL);
#endif

    hdr.payload_len = NIPC_HELLO_WIRE_SIZE + 16;
    nipc_header_encode(&hdr, pkt, sizeof(pkt));
    check("HELLO declaring a 60-byte payload rejected",
          accept_raw_hello("test_hello_long", pkt, sizeof(pkt)) ==
              NIPC_UDS_ERR_PROTOCOL);
}

static void test_accept_incompatible_hello_version(void)
{
    printf("Test: Accept rejects incompatible HELLO header version\n");
    const char *svc = "test_bad_hello_version";
    cleanup_socket(svc);

    accept_result_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .accept_err = NIPC_UDS_OK,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, accept_result_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("accept-result server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    char path[256];
    build_socket_path(path, sizeof(path), svc);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    check("raw client socket created", fd >= 0);

    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        check("raw client connect", rc == 0);

        if (rc == 0) {
            uint8_t pkt[NIPC_HEADER_LEN + NIPC_HELLO_WIRE_SIZE] = {0};
            uint8_t payload[NIPC_HELLO_WIRE_SIZE] = {0};
            nipc_hello_t hello = {
                .layout_version = 1,
                .supported_profiles = NIPC_PROFILE_BASELINE,
                .max_request_payload_bytes = 4096,
                .max_request_batch_items = 16,
                .max_response_payload_bytes = 4096,
                .max_response_batch_items = 16,
                .auth_token = AUTH_TOKEN,
                .packet_size = 65536,
            };
            nipc_header_t hdr = {
                .magic = NIPC_MAGIC_MSG,
                .version = NIPC_VERSION + 1,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_CONTROL,
                .code = NIPC_CODE_HELLO,
                .payload_len = NIPC_HELLO_WIRE_SIZE,
                .item_count = 1,
            };
            nipc_hello_encode(&hello, payload, sizeof(payload));
            nipc_header_encode(&hdr, pkt, sizeof(pkt));
            memcpy(pkt + NIPC_HEADER_LEN, payload, sizeof(payload));
            send(fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
        }
        close(fd);
    }

    pthread_join(tid, NULL);
    check("incompatible HELLO version rejected",
          sctx.accept_err == NIPC_UDS_ERR_INCOMPATIBLE);
    cleanup_socket(svc);
}

static void test_accept_incompatible_hello_layout(void)
{
    printf("Test: Accept rejects incompatible HELLO payload layout\n");
    const char *svc = "test_bad_hello_layout";
    cleanup_socket(svc);

    accept_result_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .accept_err = NIPC_UDS_OK,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, accept_result_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("accept-result server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    char path[256];
    build_socket_path(path, sizeof(path), svc);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    check("raw client socket created", fd >= 0);

    if (fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        check("raw client connect", rc == 0);

        if (rc == 0) {
            uint8_t pkt[NIPC_HEADER_LEN + NIPC_HELLO_WIRE_SIZE] = {0};
            uint8_t payload[NIPC_HELLO_WIRE_SIZE] = {0};
            nipc_hello_t hello = {
                .layout_version = 2,
                .supported_profiles = NIPC_PROFILE_BASELINE,
                .max_request_payload_bytes = 4096,
                .max_request_batch_items = 16,
                .max_response_payload_bytes = 4096,
                .max_response_batch_items = 16,
                .auth_token = AUTH_TOKEN,
                .packet_size = 65536,
            };
            nipc_header_t hdr = {
                .magic = NIPC_MAGIC_MSG,
                .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_CONTROL,
                .code = NIPC_CODE_HELLO,
                .payload_len = NIPC_HELLO_WIRE_SIZE,
                .item_count = 1,
            };
            nipc_hello_encode(&hello, payload, sizeof(payload));
            nipc_header_encode(&hdr, pkt, sizeof(pkt));
            memcpy(pkt + NIPC_HEADER_LEN, payload, sizeof(payload));
            send(fd, pkt, sizeof(pkt), MSG_NOSIGNAL);
        }
        close(fd);
    }

    pthread_join(tid, NULL);
    check("incompatible HELLO layout rejected",
          sctx.accept_err == NIPC_UDS_ERR_INCOMPATIBLE);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: receive protocol validation from malformed responses     */
/* ------------------------------------------------------------------ */

static void test_receive_short_packet_protocol(void)
{
    printf("Test: Receive rejects short response packet\n");
    const char *svc = "test_short_resp";
    cleanup_socket(svc);

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .mode = RAW_RESP_SHORT_PACKET,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = 1,
            .item_count = 1,
            .message_id = 7001,
        };
        uint8_t payload[] = { 0xAA };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("short response packet rejected", rerr == NIPC_UDS_ERR_PROTOCOL);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_trailing_packet_protocol(void)
{
    printf("Test: Receive rejects response packet with trailing bytes\n");
    const char *svc = "test_trailing_resp";
    cleanup_socket(svc);

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .mode = RAW_RESP_TRAILING_PACKET,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response trailing server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect trailing response test", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = 1,
            .item_count = 1,
            .message_id = 7002,
        };
        uint8_t payload[] = { 0xAA };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send trailing response request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
#if TEST_UDS_STREAM
            /* A stream has no packet end to check: the message completes
             * and the trailing byte is rejected as the next packet, a
             * truncated header once the server closes. */
            check("trailing response message delivered",
                  rerr == NIPC_UDS_OK && resp_payload_len == 1 &&
                      ((const uint8_t *)resp_payload)[0] == 0xBE);
            rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                    &resp_hdr, &resp_payload, &resp_payload_len);
#endif
            check("trailing response packet rejected", rerr == NIPC_UDS_ERR_PROTOCOL);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_batch_dir_too_short(void)
{
    printf("Test: Receive rejects too-short batch directory\n");
    const char *svc = "test_batch_dir_short";
    cleanup_socket(svc);

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .mode = RAW_RESP_BAD_BATCH_DIR_SHORT,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7002,
        };
        uint8_t payload[] = { 0xAB };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("short batch directory rejected", rerr == NIPC_UDS_ERR_PROTOCOL);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_short_continuation_packet(void)
{
    printf("Test: Receive rejects short continuation packet\n");
    const char *svc = "test_short_chunk";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = 128;
    scfg.max_response_payload_bytes = 4096;

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = scfg,
        .mode = RAW_RESP_SHORT_CONTINUATION,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_response_payload_bytes = 4096;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        check("negotiated packet_size is 128", session.packet_size == 128);

        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7003,
        };
        uint8_t payload[] = { 0xAC };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[128];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("short continuation rejected", rerr == NIPC_UDS_ERR_CHUNK);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_item_count_exceeds_limit(void)
{
    printf("Test: Receive rejects response item_count over limit\n");
    const char *svc = "test_resp_count_limit";
    cleanup_socket(svc);

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = default_server_config(),
        .mode = RAW_RESP_BATCH_COUNT_EXCEEDED,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7004,
        };
        uint8_t payload[] = { 0xAD };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[4096];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("response item_count over limit rejected", rerr == NIPC_UDS_ERR_LIMIT_EXCEEDED);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_bad_continuation_header(void)
{
    printf("Test: Receive rejects bad continuation header\n");
    const char *svc = "test_bad_chunk_hdr";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = 128;
    scfg.max_response_payload_bytes = 4096;

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = scfg,
        .mode = RAW_RESP_BAD_CONTINUATION_HEADER,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_response_payload_bytes = 4096;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7005,
        };
        uint8_t payload[] = { 0xAE };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[128];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("bad continuation header rejected", rerr == NIPC_UDS_ERR_CHUNK);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

static void test_receive_missing_continuation(void)
{
    printf("Test: Receive rejects missing continuation packet\n");
    const char *svc = "test_missing_chunk";
    cleanup_socket(svc);

    nipc_uds_server_config_t scfg = default_server_config();
    scfg.packet_size = 128;
    scfg.max_response_payload_bytes = 4096;

    raw_response_server_ctx_t sctx = {
        .service = svc,
        .config = scfg,
        .mode = RAW_RESP_MISSING_CONTINUATION,
        .ready = 0,
        .done = 0,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, raw_response_server_thread, &sctx);

    (void)test_flag_wait_any(&sctx.ready, &sctx.done);
    check("raw-response server ready",
          __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_uds_client_config_t ccfg = default_client_config();
    ccfg.packet_size = 128;
    ccfg.max_response_payload_bytes = 4096;

    nipc_uds_session_t session;
    nipc_uds_error_t err = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect", err == NIPC_UDS_OK);

    if (err == NIPC_UDS_OK) {
        nipc_header_t hdr = {
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 7006,
        };
        uint8_t payload[] = { 0xAF };
        nipc_uds_error_t serr = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        check("send request ok", serr == NIPC_UDS_OK);

        if (serr == NIPC_UDS_OK) {
            uint8_t buf[128];
            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_payload_len;
            nipc_uds_error_t rerr = nipc_uds_receive(&session, buf, sizeof(buf),
                                                     &resp_hdr, &resp_payload, &resp_payload_len);
            check("missing continuation rejected", rerr == NIPC_UDS_ERR_RECV);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    cleanup_socket(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: close_session(NULL)                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Stream-socket packet I/O (macOS and FreeBSD baseline; tested everywhere) */
/* ------------------------------------------------------------------ */

static size_t encode_test_packet(uint8_t *dst, uint32_t magic,
                                 uint32_t declared, const void *body,
                                 size_t body_len)
{
    memset(dst, 0, NIPC_HEADER_LEN);
    if (magic == NIPC_MAGIC_MSG) {
        nipc_header_t hdr = {
            .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN, .kind = NIPC_KIND_REQUEST,
            .code = 1, .item_count = 1, .message_id = 9,
            .payload_len = declared,
        };
        nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    } else if (magic == NIPC_MAGIC_CHUNK) {
        nipc_chunk_header_t chk = {
            .magic = NIPC_MAGIC_CHUNK, .version = NIPC_VERSION,
            .message_id = 9, .chunk_index = 1, .chunk_count = 2,
            .chunk_payload_len = declared,
        };
        nipc_chunk_header_encode(&chk, dst, NIPC_HEADER_LEN);
    } else {
        memcpy(dst, &magic, sizeof(magic));
    }
    if (body_len)
        memcpy(dst + NIPC_HEADER_LEN, body, body_len);
    return NIPC_HEADER_LEN + body_len;
}

static void test_stream_body_len(void)
{
    printf("Test: Stream packet length comes from its header\n");
    uint8_t pkt[NIPC_HEADER_LEN];

    encode_test_packet(pkt, NIPC_MAGIC_MSG, 100, NULL, 0);
    check("outer header: whole payload when it fits the packet",
          nipc_uds_stream_body_len(pkt, 1024, 1024) == 100);
    check("outer header: first chunk fills the packet",
          nipc_uds_stream_body_len(pkt, 1024, 48) == 16);
    check("outer header in a handshake: capped by the buffer",
          nipc_uds_stream_body_len(pkt, 80, 0) == 48);
    check("outer header: bytes past the buffer are not read",
          nipc_uds_stream_body_len(pkt, 64, 1024) == 32);

    encode_test_packet(pkt, NIPC_MAGIC_CHUNK, 7, NULL, 0);
    check("continuation header: chunk_payload_len",
          nipc_uds_stream_body_len(pkt, 1024, 48) == 7);

    encode_test_packet(pkt, 0x12345678u, 0, NULL, 0);
    check("unknown magic: header only",
          nipc_uds_stream_body_len(pkt, 1024, 1024) == 0);
}

static void test_stream_packet_io(void)
{
    printf("Test: Stream packet send and receive\n");
    int sv[2];
    check("stream socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint8_t out[NIPC_HEADER_LEN + 16];
    uint8_t in[NIPC_HEADER_LEN + 16];
    size_t len = 0;

    /* Two packets back to back come out one at a time. */
    size_t n1 = encode_test_packet(out, NIPC_MAGIC_MSG, 5, "hello", 5);
    check("send outer packet",
          nipc_uds_stream_send_iov(sv[0], out, NIPC_HEADER_LEN,
                                   out + NIPC_HEADER_LEN, 5) == NIPC_UDS_OK);
    uint8_t chunk[NIPC_HEADER_LEN + 3];
    size_t n2 = encode_test_packet(chunk, NIPC_MAGIC_CHUNK, 3, "abc", 3);
    check("send continuation packet",
          nipc_uds_stream_send_iov(sv[0], chunk, n2, NULL, 0) == NIPC_UDS_OK);

    check("receive outer packet",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_OK && len == n1 && memcmp(in, out, n1) == 0);
    check("receive continuation packet",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_OK && len == n2 && memcmp(in, chunk, n2) == 0);

    /* A deadline with nothing queued is a timeout, not a failure. */
    nipc_uds_wait_t wait = nipc_uds_wait_init(1, -1);
    check("idle stream times out",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, &wait, &len) ==
              NIPC_UDS_ERR_TIMEOUT);

    /* EOF inside a packet is a truncated packet. */
    n1 = encode_test_packet(out, NIPC_MAGIC_MSG, 5, "he", 2);
    check("send truncated packet",
          nipc_uds_stream_send_iov(sv[0], out, n1, NULL, 0) == NIPC_UDS_OK);
    shutdown(sv[0], SHUT_WR);
    check("EOF inside the body is a protocol error",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_ERR_PROTOCOL);
    close(sv[0]);
    close(sv[1]);

    check("stream socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    check("send partial header",
          nipc_uds_stream_send_iov(sv[0], out, 10, NULL, 0) == NIPC_UDS_OK);
    shutdown(sv[0], SHUT_WR);
    check("EOF inside the header is a protocol error",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_ERR_PROTOCOL);
    close(sv[0]);
    close(sv[1]);

    check("stream socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[0]);
    check("EOF between packets is a disconnect",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_ERR_RECV);
    close(sv[1]);
}

#ifdef NIPC_TEST_EINTR_WRAP
/* A stream moves any part of the data per call and a signal may interrupt
 * any call: one-byte transfers and EINTR must still yield whole packets. */
static void test_stream_short_io(void)
{
    printf("Test: Stream packets survive one-byte transfers and EINTR\n");
    int sv[2];
    check("stream socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    uint8_t out[NIPC_HEADER_LEN + 5];
    uint8_t in[NIPC_HEADER_LEN + 5];
    size_t n = encode_test_packet(out, NIPC_MAGIC_MSG, 5, "hello", 5);

    /* 36 one-byte writes leave one byte for the last call; reads of more
     * than one byte are cut to one, 31 in the header and 4 in the body. */
    uds_short_io_set(0, n - 1);
    uds_eintr_wrap_set(0, 2, 0);
    check("send in one-byte writes",
          nipc_uds_stream_send_iov(sv[0], out, NIPC_HEADER_LEN,
                                   out + NIPC_HEADER_LEN, 5) == NIPC_UDS_OK);

    uds_short_io_set(n - 2, 0);
    uds_eintr_wrap_set(0, 0, 2);
    size_t len = 0;
    check("receive in one-byte reads",
          nipc_uds_stream_recv_packet(sv[1], in, sizeof(in), 64, NULL, &len) ==
              NIPC_UDS_OK && len == n && memcmp(in, out, n) == 0);
    check("every injection was hit", uds_eintr_wrap_pending() == 0);
    uds_short_io_set(0, 0);
    uds_eintr_wrap_set(0, 0, 0);
    close(sv[0]);
    close(sv[1]);
}
#endif

#if TEST_UDS_STREAM
static void stream_server_session(nipc_uds_session_t *s, int fd)
{
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->role = NIPC_UDS_ROLE_SERVER;
    s->packet_size = 1024;
    s->max_request_payload_bytes = 1024;
    s->max_request_batch_items = 1;
    s->max_response_payload_bytes = 1024;
    s->max_response_batch_items = 1;
}

/* A receive that consumed part of a message cannot leave the session usable:
 * the rest is still queued and would be read as a header. */
static void test_stream_partial_receive_ends_reads(void)
{
    printf("Test: Stream receive after a partial message fails\n");
    int sv[2];
    check("stream socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    nipc_uds_session_t session;
    stream_server_session(&session, sv[1]);

    uint8_t buf[2048];
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;
    check("timeout before the first byte",
          nipc_uds_receive_timeout(&session, buf, sizeof(buf), &hdr, &payload,
                                   &payload_len, 1, -1) == NIPC_UDS_ERR_TIMEOUT);

    uint8_t pkt[NIPC_HEADER_LEN + 5];
    size_t n = encode_test_packet(pkt, NIPC_MAGIC_MSG, 5, "hello", 5);
    check("send a whole request",
          nipc_uds_stream_send_iov(sv[0], pkt, n, NULL, 0) == NIPC_UDS_OK);
    check("the session still receives after a clean timeout",
          nipc_uds_receive(&session, buf, sizeof(buf), &hdr, &payload,
                           &payload_len) == NIPC_UDS_OK && payload_len == 5);

    check("send part of a request",
          nipc_uds_stream_send_iov(sv[0], pkt, 10, NULL, 0) == NIPC_UDS_OK);
    check("timeout inside a message",
          nipc_uds_receive_timeout(&session, buf, sizeof(buf), &hdr, &payload,
                                   &payload_len, 1, -1) == NIPC_UDS_ERR_TIMEOUT);
    check("send the rest of the request",
          nipc_uds_stream_send_iov(sv[0], pkt + 10, n - 10, NULL, 0) == NIPC_UDS_OK);
    check("later receives fail",
          nipc_uds_receive(&session, buf, sizeof(buf), &hdr, &payload,
                           &payload_len) == NIPC_UDS_ERR_RECV);

    /* Sends always write whole messages, so they stay valid. */
    nipc_header_t resp = {.kind = NIPC_KIND_RESPONSE, .code = 1,
                          .item_count = 1, .message_id = 9};
    check("sends still work",
          nipc_uds_send(&session, &resp, "ok", 2) == NIPC_UDS_OK);
    size_t len = 0;
    check("the peer gets the whole response",
          nipc_uds_stream_recv_packet(sv[0], buf, sizeof(buf), 1024, NULL, &len) ==
              NIPC_UDS_OK && len == NIPC_HEADER_LEN + 2);

    close(sv[0]);
    nipc_uds_close_session(&session);
}
#endif

#if defined(__APPLE__)
static void bind_dir_path(char *dst, size_t len, const char *svc)
{
    snprintf(dst, len, "%s/%s.d", TEST_RUN_DIR, svc);
}

static bool listen_and_close(const char *svc, nipc_uds_error_t *err_out,
                             mode_t *mode_out)
{
    nipc_uds_server_config_t scfg = default_server_config();
    nipc_uds_listener_t listener;
    *err_out = nipc_uds_listen(TEST_RUN_DIR, svc, &scfg, &listener);
    if (*err_out != NIPC_UDS_OK)
        return false;
    char path[256];
    build_socket_path(path, sizeof(path), svc);
    struct stat st;
    *mode_out = stat(path, &st) == 0 ? (st.st_mode & 0777) : 0;
    nipc_uds_close_listener(&listener);
    return true;
}

/* macOS binds in {service}.d/ and renames the socket into place. */
static void test_listen_bind_dir(void)
{
    printf("Test: macOS listen binds through a private directory\n");
    const char *svc = "test_bind_dir";
    char dir[256], inner[300];
    bind_dir_path(dir, sizeof(dir), svc);
    snprintf(inner, sizeof(inner), "%s/s", dir);
    cleanup_socket(svc);

    nipc_uds_error_t err;
    mode_t mode = 0;
    struct stat st;
    check("listen", listen_and_close(svc, &err, &mode));
    check("socket is owner-only", mode == 0600);
    check("bind directory removed", lstat(dir, &st) < 0 && errno == ENOENT);

    /* A crashed server's leftover directory is reclaimed. */
    check("leftover dir", mkdir(dir, 0700) == 0);
    int fd = open(inner, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    check("leftover socket file", fd >= 0);
    if (fd >= 0)
        close(fd);
    check("listen over a leftover directory", listen_and_close(svc, &err, &mode));
    check("leftover directory removed", lstat(dir, &st) < 0 && errno == ENOENT);

    /* A non-directory at the name is junk, as at the socket name. */
    fd = open(dir, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    check("leftover file", fd >= 0);
    if (fd >= 0)
        close(fd);
    check("listen over a leftover file", listen_and_close(svc, &err, &mode));
    check("leftover file removed", lstat(dir, &st) < 0 && errno == ENOENT);

    /* Foreign content is never deleted: listen fails instead. */
    char other[300];
    snprintf(other, sizeof(other), "%s/other", dir);
    check("foreign dir", mkdir(dir, 0700) == 0);
    fd = open(other, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    check("foreign file", fd >= 0);
    if (fd >= 0)
        close(fd);
    check("listen refuses a directory with foreign content",
          !listen_and_close(svc, &err, &mode) && err == NIPC_UDS_ERR_SOCKET);
    check("foreign content kept", stat(other, &st) == 0);
    unlink(other);

    /* A directory where the socket goes is foreign content too. */
    char sub[300];
    snprintf(sub, sizeof(sub), "%s/s", dir);
    check("foreign subdirectory", mkdir(sub, 0700) == 0);
    check("listen refuses a directory at the socket name",
          !listen_and_close(svc, &err, &mode) && err == NIPC_UDS_ERR_SOCKET);
    check("foreign subdirectory kept", stat(sub, &st) == 0 && S_ISDIR(st.st_mode));
    rmdir(sub);
    rmdir(dir);

    /* The umask still narrows the mode, as with fchmod() on Linux. */
    mode_t old_mask = umask(0277);
    bool ok = listen_and_close(svc, &err, &mode);
    umask(old_mask);
    check("listen under umask 0277", ok);
    check("socket mode narrowed by the umask", mode == 0400);
    cleanup_socket(svc);

    /* A umask without owner read must not stop the bind directory from
     * being opened. */
    old_mask = umask(0477);
    ok = listen_and_close(svc, &err, &mode);
    umask(old_mask);
    check("listen under umask 0477", ok);
    check("socket mode narrowed by umask 0477", mode == 0200);
    cleanup_socket(svc);
}
#endif

static void test_close_session_null(void)
{
    printf("Test: close_session(NULL)\n");
    nipc_uds_close_session(NULL);
    check("close_session(NULL) does not crash", 1);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Ignore SIGPIPE so broken pipes return errors instead of signals */
    signal(SIGPIPE, SIG_IGN);

    ensure_run_dir();

    /* Line-buffer stdout for test visibility */
    setbuf(stdout, NULL);

    printf("=== L1 POSIX UDS SEQPACKET Transport Tests ===\n\n");

    test_ping_pong();              printf("\n");
#ifdef NIPC_TEST_EINTR_WRAP
    test_eintr_retried();          printf("\n");
    test_listen_keeps_process_umask(); printf("\n");
    test_stream_short_io();        printf("\n");
#endif
    test_stream_body_len();        printf("\n");
    test_stream_packet_io();       printf("\n");
#if TEST_UDS_STREAM
    test_stream_partial_receive_ends_reads(); printf("\n");
#endif
#if defined(__APPLE__)
    test_listen_bind_dir();        printf("\n");
#endif
    test_multi_client();           printf("\n");
    test_pipelining();             printf("\n");
    test_chunking();               printf("\n");
    test_bad_auth();               printf("\n");
    test_profile_mismatch();       printf("\n");
    test_platform_profile_mask();  printf("\n");
    test_unassigned_profile_bit(); printf("\n");
    test_request_payload_over_cap(); printf("\n");
    test_stale_recovery();         printf("\n");
    test_stale_recovery_reclaims_regular_file(); printf("\n");
    test_stale_recovery_reclaims_empty_directory(); printf("\n");
    test_listen_socket_owner_only(); printf("\n");
    test_disconnect_inflight();    printf("\n");
    test_batch();                  printf("\n");
    test_invalid_service_name();   printf("\n");
    test_send_recv_validation();   printf("\n");
    test_close_listener_edge();    printf("\n");
    test_close_session_edge();     printf("\n");
    test_listen_path_too_long();   printf("\n");
    test_connect_path_too_long();  printf("\n");
    test_pipeline_10();            printf("\n");
    test_pipeline_100();           printf("\n");
    test_pipeline_mixed_sizes();   printf("\n");
    test_pipeline_chunked();       printf("\n");
    test_pipeline_exceeds_socket_buffers(); printf("\n");
    test_pipeline_peer_close_during_send(); printf("\n");
    test_pipeline_pending_bound(); printf("\n");
    test_receive_undersized_buffer_keeps_session(); printf("\n");
#if TEST_UDS_STREAM
    test_stream_failed_receive_drops_pending(); printf("\n");
#else
    test_pending_oversized_datagram_rejected(); printf("\n");
#endif

    /* Coverage gap tests */
    test_handshake_wrong_kind();   printf("\n");
    test_inflight_duplicate();     printf("\n");
    test_directional_limit_negotiation();  printf("\n");
    test_receive_payload_exceeds_limit(); printf("\n");
    test_receive_unknown_message_id(); printf("\n");
    test_stale_live_server();      printf("\n");
    test_listen_excludes_starting_server(); printf("\n");
    test_listen_close_releases_lock_held_by_child(); printf("\n");
    test_listen_lock_restrictive_umask(); printf("\n");
    test_listen_connect_bad_params(); printf("\n");
    test_listen_bind_failure();    printf("\n");
    test_batch_corrupt_dir();      printf("\n");
    test_connect_malformed_hello_ack(); printf("\n");
    test_accept_malformed_hello_payload(); printf("\n");
    test_accept_incompatible_hello_version(); printf("\n");
    test_accept_incompatible_hello_layout(); printf("\n");
    test_receive_short_packet_protocol(); printf("\n");
    test_receive_trailing_packet_protocol(); printf("\n");
    test_receive_batch_dir_too_short(); printf("\n");
    test_receive_short_continuation_packet(); printf("\n");
    test_receive_item_count_exceeds_limit(); printf("\n");
    test_receive_bad_continuation_header(); printf("\n");
    test_receive_missing_continuation(); printf("\n");
    test_close_session_null();     printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
