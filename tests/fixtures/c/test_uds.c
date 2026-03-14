/*
 * test_uds.c - Integration tests for L1 POSIX UDS SEQPACKET transport.
 *
 * Forks server processes and exercises the transport API directly.
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_uds.h"
#include "netipc/netipc_protocol.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
    volatile int             ready;        /* set to 1 when listening */
    volatile int             done;         /* set to 1 when finished */
} server_ctx_t;

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
        ctx->done = 1;
        return NULL;
    }

    ctx->ready = 1;

    for (int c = 0; c < ctx->accept_count; c++) {
        nipc_uds_session_t session;
        err = nipc_uds_accept(&listener, &session);
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
    ctx->done = 1;
    return NULL;
}

/* Start echo server in a thread. Waits until it's ready. */
static pthread_t start_echo_server(server_ctx_t *ctx)
{
    pthread_t tid;
    ctx->ready = 0;
    ctx->done = 0;
    pthread_create(&tid, NULL, echo_server_thread, ctx);

    /* Wait for the server to be ready */
    int retries = 0;
    while (!ctx->ready && !ctx->done && retries < 1000) {
        usleep(1000);
        retries++;
    }
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
    check("server ready", sctx.ready == 1);

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
    check("server ready", sctx.ready == 1);

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
    check("server ready", sctx.ready == 1);

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
        ctx->done = 1;
        return NULL;
    }
    ctx->ready = 1;

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, &session);
    if (err != NIPC_UDS_OK) {
        nipc_uds_close_listener(&listener);
        ctx->done = 1;
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
    ctx->done = 1;
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
    sctx.ready = 0;
    sctx.done = 0;
    pthread_create(&tid, NULL, chunked_server_thread, &sctx);

    int retries = 0;
    while (!sctx.ready && !sctx.done && retries < 1000) {
        usleep(1000);
        retries++;
    }
    check("chunked server ready", sctx.ready == 1);

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
    check("server ready", sctx.ready == 1);

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
    check("server ready", sctx.ready == 1);

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

/* ------------------------------------------------------------------ */
/*  Test 7: Stale socket recovery                                      */
/* ------------------------------------------------------------------ */

static void test_stale_recovery(void)
{
    printf("Test 7: Stale socket recovery\n");
    const char *svc = "test_stale";
    cleanup_socket(svc);

    /* Create a stale socket file (not backed by a listener) */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, svc);

    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
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

/* ------------------------------------------------------------------ */
/*  Test 8: Disconnect with in-flight request                          */
/* ------------------------------------------------------------------ */

/* Server that accepts but closes immediately without responding. */
static void *disconnect_server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    nipc_uds_listener_t listener;
    nipc_uds_error_t err = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                            &ctx->config, &listener);
    if (err != NIPC_UDS_OK) {
        ctx->done = 1;
        return NULL;
    }
    ctx->ready = 1;

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, &session);
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
    ctx->done = 1;
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
    sctx.ready = 0;
    sctx.done = 0;
    pthread_create(&tid, NULL, disconnect_server_thread, &sctx);

    int retries = 0;
    while (!sctx.ready && !sctx.done && retries < 1000) {
        usleep(1000);
        retries++;
    }
    check("server ready", sctx.ready == 1);

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

        /* Try to receive -- server will disconnect */
        uint8_t rbuf[4096];
        nipc_header_t rhdr;
        const void *rp;
        size_t rlen;
        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf),
                                &rhdr, &rp, &rlen);
        check("receive fails on disconnect", err != NIPC_UDS_OK);

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
    check("server ready", sctx.ready == 1);

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
    check("connect reject bad name",
          nipc_uds_connect(TEST_RUN_DIR, "../etc", &ccfg, &session)
          == NIPC_UDS_ERR_BAD_PARAM);

    /* Valid names should not fail validation (may fail connect) */
    check("accept valid name (connect may fail)",
          nipc_uds_connect(TEST_RUN_DIR, "valid-name_123.test", &ccfg, &session)
          != NIPC_UDS_ERR_BAD_PARAM);
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

    test_ping_pong();           printf("\n");
    test_multi_client();        printf("\n");
    test_pipelining();          printf("\n");
    test_chunking();            printf("\n");
    test_bad_auth();            printf("\n");
    test_profile_mismatch();    printf("\n");
    test_stale_recovery();      printf("\n");
    test_disconnect_inflight(); printf("\n");
    test_batch();               printf("\n");
    test_invalid_service_name(); printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
