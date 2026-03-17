/*
 * test_shm.c - Integration tests for L1 POSIX SHM transport.
 *
 * Tests the SHM data plane directly (server create, client attach,
 * send/receive round-trip) and the negotiated upgrade path via UDS.
 *
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_shm.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR  "/tmp/nipc_shm_test"
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

/* Clean up leftover SHM files for a service (all session IDs 0..9). */
static void cleanup_shm(const char *service)
{
    char path[256];
    for (uint64_t sid = 0; sid < 10; sid++) {
        snprintf(path, sizeof(path), "%s/%s-%016" PRIx64 ".ipcshm",
                 TEST_RUN_DIR, service, sid);
        unlink(path);
    }
}

static void cleanup_socket(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
}

static void cleanup_all(const char *service)
{
    cleanup_shm(service);
    cleanup_socket(service);
}

/* ------------------------------------------------------------------ */
/*  Test 1: Direct SHM round-trip                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int ready;
    int done;
    int echo_ok;
} shm_server_ctx_t;

static void *shm_echo_server_thread(void *arg)
{
    shm_server_ctx_t *ctx = (shm_server_ctx_t *)arg;
    ctx->echo_ok = 0;

    nipc_shm_ctx_t shm;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, ctx->service, 1, 4096, 4096, &shm);
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "shm server create failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    /* Receive a request. */
    uint8_t msg[65536];
    size_t msg_len;
    err = nipc_shm_receive(&shm, msg, sizeof(msg), &msg_len, 5000);
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "shm server receive failed: %d\n", err);
        nipc_shm_destroy(&shm);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    /*
     * Build response: decode the outer header from the request,
     * flip kind to RESPONSE, and send back the same payload.
     */
    if (msg_len >= NIPC_HEADER_LEN) {
        nipc_header_t hdr;
        nipc_header_decode(msg, msg_len, &hdr);
        hdr.kind = NIPC_KIND_RESPONSE;
        hdr.transport_status = NIPC_STATUS_OK;

        /* Build complete response message (header + payload). */
        size_t payload_len = msg_len - NIPC_HEADER_LEN;
        size_t resp_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *resp_buf = malloc(resp_len);
        if (resp_buf) {
            nipc_header_encode(&hdr, resp_buf, NIPC_HEADER_LEN);
            if (payload_len > 0)
                memcpy(resp_buf + NIPC_HEADER_LEN,
                       msg + NIPC_HEADER_LEN,
                       payload_len);

            err = nipc_shm_send(&shm, resp_buf, resp_len);
            ctx->echo_ok = (err == NIPC_SHM_OK) ? 1 : 0;
            free(resp_buf);
        }
    }

    nipc_shm_destroy(&shm);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_direct_roundtrip(void)
{
    printf("Test 1: Direct SHM round-trip\n");
    const char *svc = "shm_rt";
    cleanup_shm(svc);

    shm_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, shm_echo_server_thread, &sctx);

    /* Wait for server to be ready. */
    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server created SHM region", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client attaches. */
    nipc_shm_ctx_t client;
    nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
    check("client attach", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Build a request message (header + 4-byte payload). */
        uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
        nipc_header_t hdr = {
            .magic       = NIPC_MAGIC_MSG,
            .version     = NIPC_VERSION,
            .header_len  = NIPC_HEADER_LEN,
            .kind        = NIPC_KIND_REQUEST,
            .code        = NIPC_METHOD_INCREMENT,
            .flags       = 0,
            .item_count  = 1,
            .message_id  = 42,
            .payload_len = sizeof(payload),
            .transport_status = NIPC_STATUS_OK,
        };

        uint8_t msg_buf[NIPC_HEADER_LEN + sizeof(payload)];
        nipc_header_encode(&hdr, msg_buf, NIPC_HEADER_LEN);
        memcpy(msg_buf + NIPC_HEADER_LEN, payload, sizeof(payload));

        err = nipc_shm_send(&client, msg_buf, sizeof(msg_buf));
        check("client send request", err == NIPC_SHM_OK);

        /* Receive response. */
        uint8_t resp[65536];
        size_t resp_len;
        err = nipc_shm_receive(&client, resp, sizeof(resp), &resp_len, 5000);
        check("client receive response", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            check("response length",
                  resp_len == NIPC_HEADER_LEN + sizeof(payload));

            nipc_header_t rhdr;
            nipc_header_decode(resp, resp_len, &rhdr);
            check("response kind", rhdr.kind == NIPC_KIND_RESPONSE);
            check("response message_id", rhdr.message_id == 42);
            check("response payload matches",
                  resp_len >= NIPC_HEADER_LEN + sizeof(payload) &&
                  memcmp(resp + NIPC_HEADER_LEN,
                         payload, sizeof(payload)) == 0);
        }

        nipc_shm_close(&client);
    }

    pthread_join(tid, NULL);
    check("server echo succeeded", sctx.echo_ok);
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Negotiated SHM via UDS handshake                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int ready;
    int done;
    int shm_created;
    int echo_ok;
} hybrid_server_ctx_t;

static void *hybrid_server_thread(void *arg)
{
    hybrid_server_ctx_t *ctx = (hybrid_server_ctx_t *)arg;
    ctx->shm_created = 0;
    ctx->echo_ok = 0;

    /* Listen on UDS with SHM_HYBRID support. */
    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 4,
    };

    nipc_uds_listener_t listener;
    nipc_uds_error_t uerr = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                             &scfg, &listener);
    if (uerr != NIPC_UDS_OK) {
        fprintf(stderr, "hybrid server listen failed: %d\n", uerr);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    nipc_uds_session_t session;
    uerr = nipc_uds_accept(&listener, 1, &session);
    if (uerr != NIPC_UDS_OK) {
        nipc_uds_close_listener(&listener);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    /* If SHM_HYBRID was negotiated, create SHM region. */
    nipc_shm_ctx_t shm;
    int use_shm = 0;

    if (session.selected_profile == NIPC_PROFILE_SHM_HYBRID) {
        nipc_shm_error_t serr = nipc_shm_server_create(
            TEST_RUN_DIR, ctx->service, session.session_id,
            4096, 4096, &shm);
        if (serr == NIPC_SHM_OK) {
            ctx->shm_created = 1;
            use_shm = 1;
        }
    }

    if (use_shm) {
        /* Echo via SHM. */
        uint8_t msg[65536];
        size_t msg_len;
        nipc_shm_error_t serr = nipc_shm_receive(&shm, msg, sizeof(msg),
                                                    &msg_len, 5000);
        if (serr == NIPC_SHM_OK && msg_len >= NIPC_HEADER_LEN) {
            nipc_header_t hdr;
            nipc_header_decode(msg, msg_len, &hdr);
            hdr.kind = NIPC_KIND_RESPONSE;

            size_t payload_len = msg_len - NIPC_HEADER_LEN;
            size_t resp_len = NIPC_HEADER_LEN + payload_len;
            uint8_t *resp_buf = malloc(resp_len);
            if (resp_buf) {
                nipc_header_encode(&hdr, resp_buf, NIPC_HEADER_LEN);
                if (payload_len > 0)
                    memcpy(resp_buf + NIPC_HEADER_LEN,
                           msg + NIPC_HEADER_LEN,
                           payload_len);
                serr = nipc_shm_send(&shm, resp_buf, resp_len);
                ctx->echo_ok = (serr == NIPC_SHM_OK) ? 1 : 0;
                free(resp_buf);
            }
        }
        nipc_shm_destroy(&shm);
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_negotiated_shm(void)
{
    printf("Test 2: Negotiated SHM via UDS handshake\n");
    const char *svc = "shm_nego";
    cleanup_all(svc);

    hybrid_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, hybrid_server_thread, &sctx);

    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client connects via UDS with SHM_HYBRID support. */
    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
    };

    nipc_uds_session_t session;
    nipc_uds_error_t uerr = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("UDS connect", uerr == NIPC_UDS_OK);

    if (uerr == NIPC_UDS_OK) {
        check("selected profile is SHM_HYBRID",
              session.selected_profile == NIPC_PROFILE_SHM_HYBRID);

        /* Attach to SHM (with retry -- server creates region after
         * the UDS handshake, so the file may not exist yet or the
         * header may not be written yet). */
        nipc_shm_ctx_t client_shm;
        nipc_shm_error_t serr = NIPC_SHM_ERR_NOT_READY;
        for (int i = 0; i < 500; i++) {
            serr = nipc_shm_client_attach(TEST_RUN_DIR, svc,
                                         session.session_id, &client_shm);
            if (serr == NIPC_SHM_OK)
                break;
            /* Retryable: file doesn't exist, header not ready, or
             * header not yet written (magic is 0 after ftruncate). */
            if (serr == NIPC_SHM_ERR_NOT_READY ||
                serr == NIPC_SHM_ERR_OPEN ||
                serr == NIPC_SHM_ERR_BAD_MAGIC)
                usleep(10000);
            else
                break; /* unexpected error */
        }
        check("client SHM attach", serr == NIPC_SHM_OK);

        if (serr == NIPC_SHM_OK) {
            /* Send request via SHM. */
            uint8_t payload[] = {0xDE, 0xAD};
            nipc_header_t hdr = {
                .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = 77,
                .payload_len = sizeof(payload),
            };

            uint8_t msg_buf[NIPC_HEADER_LEN + sizeof(payload)];
            nipc_header_encode(&hdr, msg_buf, NIPC_HEADER_LEN);
            memcpy(msg_buf + NIPC_HEADER_LEN, payload, sizeof(payload));

            serr = nipc_shm_send(&client_shm, msg_buf, sizeof(msg_buf));
            check("SHM send", serr == NIPC_SHM_OK);

            /* Receive response via SHM. */
            uint8_t resp[65536];
            size_t resp_len;
            serr = nipc_shm_receive(&client_shm, resp, sizeof(resp),
                                      &resp_len, 5000);
            check("SHM receive", serr == NIPC_SHM_OK);

            if (serr == NIPC_SHM_OK) {
                nipc_header_t rhdr;
                nipc_header_decode(resp, resp_len, &rhdr);
                check("response kind", rhdr.kind == NIPC_KIND_RESPONSE);
                check("response message_id", rhdr.message_id == 77);
            }

            nipc_shm_close(&client_shm);
        }

        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    check("server created SHM region", sctx.shm_created);
    check("server echo via SHM succeeded", sctx.echo_ok);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Profile fallback (SHM not mutually supported)              */
/* ------------------------------------------------------------------ */

static void *baseline_only_server_thread(void *arg)
{
    hybrid_server_ctx_t *ctx = (hybrid_server_ctx_t *)arg;
    ctx->shm_created = 0;
    ctx->echo_ok = 0;

    /* Server supports only baseline. */
    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .backlog                   = 4,
    };

    nipc_uds_listener_t listener;
    nipc_uds_error_t uerr = nipc_uds_listen(TEST_RUN_DIR, ctx->service,
                                             &scfg, &listener);
    if (uerr != NIPC_UDS_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    nipc_uds_session_t session;
    uerr = nipc_uds_accept(&listener, 1, &session);
    if (uerr == NIPC_UDS_OK) {
        /* Just accept + close, no echo needed for this test. */
        ctx->echo_ok = (session.selected_profile == NIPC_PROFILE_BASELINE);
        nipc_uds_close_session(&session);
    }

    nipc_uds_close_listener(&listener);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_profile_fallback(void)
{
    printf("Test 3: Profile fallback (only one side supports SHM)\n");
    const char *svc = "shm_fallback";
    cleanup_all(svc);

    hybrid_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, baseline_only_server_thread, &sctx);

    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* Client supports both, but server only supports baseline. */
    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_uds_session_t session;
    nipc_uds_error_t uerr = nipc_uds_connect(TEST_RUN_DIR, svc, &ccfg, &session);
    check("connect succeeds", uerr == NIPC_UDS_OK);

    if (uerr == NIPC_UDS_OK) {
        check("falls back to baseline",
              session.selected_profile == NIPC_PROFILE_BASELINE);
        nipc_uds_close_session(&session);
    }

    pthread_join(tid, NULL);
    check("server confirms baseline", sctx.echo_ok);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: SHM disconnect detection (client closes, server detects)   */
/* ------------------------------------------------------------------ */

static void test_disconnect_detection(void)
{
    printf("Test 4: SHM owner alive check\n");
    const char *svc = "shm_disc";
    cleanup_shm(svc);

    /* Create a server SHM region in this process. */
    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 4096, 4096, &server);
    check("server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Client attaches. */
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("client attach", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            /* Owner (this process) is alive. */
            check("owner alive from client", nipc_shm_owner_alive(&client));
            nipc_shm_close(&client);
        }

        nipc_shm_destroy(&server);
    }

    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Stale SHM region recovery                                  */
/* ------------------------------------------------------------------ */

static void test_stale_shm_recovery(void)
{
    printf("Test 5: Stale SHM region recovery\n");
    const char *svc = "shm_stale";
    cleanup_shm(svc);

    /* Create a region, then close the fd but leave the file.
     * Corrupt the owner_pid to simulate a dead process. */
    nipc_shm_ctx_t first;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &first);
    check("first create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Set owner_pid to a definitely-dead PID. */
        nipc_shm_region_header_t *hdr =
            (nipc_shm_region_header_t *)first.base;
        hdr->owner_pid = 99999; /* very unlikely to be alive */

        /* Don't destroy (unlink) -- just unmap and close. */
        nipc_shm_close(&first);

        /* Now creating again should succeed (stale recovery). */
        nipc_shm_ctx_t second;
        err = nipc_shm_server_create(
            TEST_RUN_DIR, svc, 1, 2048, 2048, &second);
        check("stale recovery create succeeds", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            check("new region has correct capacity",
                  second.request_capacity >= 2048);
            nipc_shm_destroy(&second);
        }
    }

    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Large message via SHM                                      */
/* ------------------------------------------------------------------ */

static void *large_msg_server_thread(void *arg)
{
    shm_server_ctx_t *ctx = (shm_server_ctx_t *)arg;
    ctx->echo_ok = 0;

    nipc_shm_ctx_t shm;
    /* 64 KiB areas. */
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, ctx->service, 1, 65536, 65536, &shm);
    if (err != NIPC_SHM_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    uint8_t *msg = malloc(65536);
    size_t msg_len;
    err = nipc_shm_receive(&shm, msg, 65536, &msg_len, 5000);
    if (err == NIPC_SHM_OK && msg_len >= NIPC_HEADER_LEN) {
        nipc_header_t hdr;
        nipc_header_decode(msg, msg_len, &hdr);
        hdr.kind = NIPC_KIND_RESPONSE;

        size_t payload_len = msg_len - NIPC_HEADER_LEN;
        size_t resp_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *resp_buf = malloc(resp_len);
        if (resp_buf) {
            nipc_header_encode(&hdr, resp_buf, NIPC_HEADER_LEN);
            if (payload_len > 0)
                memcpy(resp_buf + NIPC_HEADER_LEN,
                       msg + NIPC_HEADER_LEN,
                       payload_len);
            err = nipc_shm_send(&shm, resp_buf, resp_len);
            ctx->echo_ok = (err == NIPC_SHM_OK) ? 1 : 0;
            free(resp_buf);
        }
    }
    free(msg);

    nipc_shm_destroy(&shm);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_large_message(void)
{
    printf("Test 6: Large message via SHM\n");
    const char *svc = "shm_large";
    cleanup_shm(svc);

    shm_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, large_msg_server_thread, &sctx);

    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_shm_ctx_t client;
    nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
    check("client attach", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Build a large request: header + 60000 bytes of payload. */
        size_t payload_len = 60000;
        size_t msg_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *msg = malloc(msg_len);
        check("alloc large message", msg != NULL);

        if (msg) {
            nipc_header_t hdr = {
                .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = 999,
                .payload_len = (uint32_t)payload_len,
            };
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);

            /* Fill payload with a pattern. */
            for (size_t i = 0; i < payload_len; i++)
                msg[NIPC_HEADER_LEN + i] = (uint8_t)(i & 0xFF);

            err = nipc_shm_send(&client, msg, msg_len);
            check("send large message", err == NIPC_SHM_OK);

            /* Receive response. */
            uint8_t *resp = malloc(65536);
            size_t resp_len;
            err = nipc_shm_receive(&client, resp, 65536, &resp_len, 5000);
            check("receive large response", err == NIPC_SHM_OK);

            if (err == NIPC_SHM_OK) {
                check("response length matches",
                      resp_len == msg_len);

                /* Verify payload pattern. */
                int payload_ok = 1;
                if (resp_len >= NIPC_HEADER_LEN + payload_len) {
                    const uint8_t *rp = resp + NIPC_HEADER_LEN;
                    for (size_t i = 0; i < payload_len; i++) {
                        if (rp[i] != (uint8_t)(i & 0xFF)) {
                            payload_ok = 0;
                            break;
                        }
                    }
                } else {
                    payload_ok = 0;
                }
                check("response payload pattern matches", payload_ok);
            }

            free(resp);
            free(msg);
        }

        nipc_shm_close(&client);
    }

    pthread_join(tid, NULL);
    check("server echo succeeded", sctx.echo_ok);
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: Message too large for capacity                             */
/* ------------------------------------------------------------------ */

static void test_msg_too_large(void)
{
    printf("Test 7: Message too large for SHM capacity\n");
    const char *svc = "shm_toolarge";
    cleanup_shm(svc);

    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 128, 128, &server);
    check("server create with small capacity", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("client attach", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            /* Try to send a message larger than request capacity. */
            size_t too_big = 256;
            uint8_t *big = calloc(1, too_big);
            err = nipc_shm_send(&client, big, too_big);
            check("send too-large fails", err == NIPC_SHM_ERR_MSG_TOO_LARGE);
            free(big);

            nipc_shm_close(&client);
        }

        nipc_shm_destroy(&server);
    }

    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 8: SHM addr-in-use (live server blocks second create)         */
/* ------------------------------------------------------------------ */

static void test_addr_in_use(void)
{
    printf("Test 8: SHM addr-in-use (live server)\n");
    const char *svc = "shm_inuse";
    cleanup_shm(svc);

    nipc_shm_ctx_t first;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &first);
    check("first server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Second create should fail: our PID owns the region. */
        nipc_shm_ctx_t second;
        err = nipc_shm_server_create(
            TEST_RUN_DIR, svc, 1, 1024, 1024, &second);
        check("second create fails with ADDR_IN_USE",
              err == NIPC_SHM_ERR_ADDR_IN_USE);

        if (err == NIPC_SHM_OK)
            nipc_shm_destroy(&second);

        nipc_shm_destroy(&first);
    }

    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 9: Multiple round-trips on same SHM region                    */
/* ------------------------------------------------------------------ */

static void *multi_rt_server_thread(void *arg)
{
    shm_server_ctx_t *ctx = (shm_server_ctx_t *)arg;
    ctx->echo_ok = 0;

    nipc_shm_ctx_t shm;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, ctx->service, 1, 4096, 4096, &shm);
    if (err != NIPC_SHM_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    int all_ok = 1;
    uint8_t recv_msg[65536];
    for (int i = 0; i < 10; i++) {
        size_t msg_len;
        err = nipc_shm_receive(&shm, recv_msg, sizeof(recv_msg),
                                 &msg_len, 5000);
        if (err != NIPC_SHM_OK) { all_ok = 0; break; }

        if (msg_len >= NIPC_HEADER_LEN) {
            nipc_header_t hdr;
            nipc_header_decode(recv_msg, msg_len, &hdr);
            hdr.kind = NIPC_KIND_RESPONSE;

            size_t plen = msg_len - NIPC_HEADER_LEN;
            size_t rlen = NIPC_HEADER_LEN + plen;
            uint8_t *rbuf = malloc(rlen);
            if (rbuf) {
                nipc_header_encode(&hdr, rbuf, NIPC_HEADER_LEN);
                if (plen > 0)
                    memcpy(rbuf + NIPC_HEADER_LEN,
                           recv_msg + NIPC_HEADER_LEN, plen);
                err = nipc_shm_send(&shm, rbuf, rlen);
                if (err != NIPC_SHM_OK) all_ok = 0;
                free(rbuf);
            } else {
                all_ok = 0;
            }
        }
    }

    ctx->echo_ok = all_ok;
    nipc_shm_destroy(&shm);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_multiple_roundtrips(void)
{
    printf("Test 9: Multiple round-trips on same SHM region\n");
    const char *svc = "shm_multi_rt";
    cleanup_shm(svc);

    shm_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, multi_rt_server_thread, &sctx);

    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_shm_ctx_t client;
    nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
    check("client attach", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        int all_ok = 1;
        uint8_t resp[65536];
        for (int i = 0; i < 10; i++) {
            uint8_t payload = (uint8_t)i;
            nipc_header_t hdr = {
                .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
                .header_len = NIPC_HEADER_LEN,
                .kind = NIPC_KIND_REQUEST, .code = 1,
                .item_count = 1, .message_id = (uint64_t)(i + 1),
                .payload_len = 1,
            };

            uint8_t msg[NIPC_HEADER_LEN + 1];
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
            msg[NIPC_HEADER_LEN] = payload;

            err = nipc_shm_send(&client, msg, sizeof(msg));
            if (err != NIPC_SHM_OK) { all_ok = 0; break; }

            size_t resp_len;
            err = nipc_shm_receive(&client, resp, sizeof(resp),
                                     &resp_len, 5000);
            if (err != NIPC_SHM_OK) { all_ok = 0; break; }

            if (resp_len < NIPC_HEADER_LEN + 1) { all_ok = 0; break; }

            nipc_header_t rhdr;
            nipc_header_decode(resp, resp_len, &rhdr);
            if (rhdr.kind != NIPC_KIND_RESPONSE ||
                rhdr.message_id != (uint64_t)(i + 1))
                all_ok = 0;

            if (resp[NIPC_HEADER_LEN] != payload)
                all_ok = 0;
        }
        check("all 10 round-trips correct", all_ok);

        nipc_shm_close(&client);
    }

    pthread_join(tid, NULL);
    check("server echoed all 10", sctx.echo_ok);
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Test: SHM validation error paths                                   */
/* ------------------------------------------------------------------ */

static void test_server_create_validation(void)
{
    printf("Test: Server create validation errors\n");

    nipc_shm_ctx_t ctx;

    /* NULL run_dir */
    check("null run_dir",
          nipc_shm_server_create(NULL, "svc", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* NULL service_name */
    check("null service_name",
          nipc_shm_server_create(TEST_RUN_DIR, NULL, 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Invalid service name (bad chars) */
    check("bad service name",
          nipc_shm_server_create(TEST_RUN_DIR, "bad/name", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Empty service name */
    check("empty service name",
          nipc_shm_server_create(TEST_RUN_DIR, "", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Dot service name */
    check("dot service name",
          nipc_shm_server_create(TEST_RUN_DIR, ".", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Dotdot service name */
    check("dotdot service name",
          nipc_shm_server_create(TEST_RUN_DIR, "..", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Path too long */
    char long_dir[4096];
    memset(long_dir, 'a', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';
    check("path too long",
          nipc_shm_server_create(long_dir, "svc", 1, 4096, 4096, &ctx)
              == NIPC_SHM_ERR_PATH_TOO_LONG);
}

static void test_client_attach_validation(void)
{
    printf("Test: Client attach validation errors\n");

    nipc_shm_ctx_t ctx;

    /* NULL run_dir */
    check("null run_dir",
          nipc_shm_client_attach(NULL, "svc", 1, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* NULL service_name */
    check("null service_name",
          nipc_shm_client_attach(TEST_RUN_DIR, NULL, 1, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Bad service name */
    check("bad service name",
          nipc_shm_client_attach(TEST_RUN_DIR, "bad/name", 1, &ctx)
              == NIPC_SHM_ERR_BAD_PARAM);

    /* Path too long */
    char long_dir[4096];
    memset(long_dir, 'a', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';
    check("path too long",
          nipc_shm_client_attach(long_dir, "svc", 1, &ctx)
              == NIPC_SHM_ERR_PATH_TOO_LONG);

    /* Non-existent file */
    check("non-existent file",
          nipc_shm_client_attach(TEST_RUN_DIR, "does_not_exist_12345", 1, &ctx)
              == NIPC_SHM_ERR_OPEN);
}

static void test_shm_close_null(void)
{
    printf("Test: SHM close with NULL/invalid\n");

    /* Close with fd=-1 should not crash */
    nipc_shm_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;
    ctx.base = NULL;
    ctx.region_size = 0;
    nipc_shm_close(&ctx);
    check("close null/empty does not crash", 1);
}

static void test_shm_send_bad_param(void)
{
    printf("Test: SHM send with bad params\n");

    nipc_shm_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;

    uint8_t msg[32];
    /* Sending on a non-initialized context */
    nipc_shm_error_t err = nipc_shm_send(&ctx, msg, sizeof(msg));
    /* Should fail -- map is NULL so any access would segfault,
     * but the function should handle it. Let's see if it validates. */
    check("send on null ctx returns error or does not crash",
          err != NIPC_SHM_OK);
}

static void test_shm_bad_magic_file(void)
{
    printf("Test: Client attach to file with bad magic\n");
    const char *svc = "shm_bad_magic";
    cleanup_shm(svc);

    /* Create a file with wrong magic (use per-session path format) */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s-%016" PRIx64 ".ipcshm",
             TEST_RUN_DIR, svc, (uint64_t)1);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        /* Write enough for stat check but with bad magic */
        uint8_t zeros[4096];
        memset(zeros, 0, sizeof(zeros));
        /* Write a bad magic at the start */
        uint32_t bad_magic = 0xDEADBEEF;
        memcpy(zeros, &bad_magic, 4);
        write(fd, zeros, sizeof(zeros));
        close(fd);

        nipc_shm_ctx_t ctx;
        nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &ctx);
        check("bad magic rejected",
              err == NIPC_SHM_ERR_BAD_MAGIC || err == NIPC_SHM_ERR_NOT_READY);

        unlink(path);
    } else {
        check("could not create bad magic test file", 0);
    }
}

static void test_shm_bad_version_file(void)
{
    printf("Test: Client attach to file with bad version\n");
    const char *svc = "shm_bad_ver";
    cleanup_shm(svc);

    /* Create a valid SHM region, then corrupt the version */
    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 4096, 4096, &server);
    if (err == NIPC_SHM_OK) {
        /* Corrupt version field: offset 4 in the header */
        nipc_shm_region_header_t *hdr =
            (nipc_shm_region_header_t *)server.base;
        hdr->version = 999;

        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("bad version rejected", err == NIPC_SHM_ERR_BAD_VERSION);

        /* Restore version before destroy */
        hdr->version = NIPC_SHM_REGION_VERSION;
        nipc_shm_destroy(&server);
    } else {
        check("could not create server for bad version test", 0);
    }
    cleanup_shm(svc);
}

static void test_shm_truncated_file(void)
{
    printf("Test: Client attach to truncated file\n");
    const char *svc = "shm_truncated";
    cleanup_shm(svc);

    /* Create a file that's too small to be a valid SHM region */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s-%016" PRIx64 ".ipcshm",
             TEST_RUN_DIR, svc, (uint64_t)1);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        /* Write only 10 bytes -- way too small for header check */
        uint8_t data[10] = {0};
        write(fd, data, sizeof(data));
        close(fd);

        nipc_shm_ctx_t ctx;
        nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &ctx);
        check("truncated file rejected",
              err != NIPC_SHM_OK);

        unlink(path);
    } else {
        check("could not create truncated test file", 0);
    }
}

static void test_shm_partial_header_not_ready(void)
{
    printf("Test: Client attach retries partial header instead of accepting it\n");
    const char *svc = "shm_partial_header";
    cleanup_shm(svc);

    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 4096, 4096, &server);
    check("server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        nipc_shm_region_header_t *hdr =
            (nipc_shm_region_header_t *)server.base;
        uint32_t saved_req_off = hdr->request_offset;
        uint32_t saved_req_cap = hdr->request_capacity;
        uint32_t saved_resp_off = hdr->response_offset;
        uint32_t saved_resp_cap = hdr->response_capacity;

        hdr->request_offset = 0;
        hdr->request_capacity = 0;
        hdr->response_offset = 0;
        hdr->response_capacity = 0;

        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("partial header treated as not ready",
              err == NIPC_SHM_ERR_NOT_READY);

        hdr->request_offset = saved_req_off;
        hdr->request_capacity = saved_req_cap;
        hdr->response_offset = saved_resp_off;
        hdr->response_capacity = saved_resp_cap;
        nipc_shm_destroy(&server);
    }

    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: nipc_shm_destroy(NULL), nipc_shm_close(NULL),             */
/*  nipc_shm_receive(NULL, ...)                                         */
/* ------------------------------------------------------------------ */

static void test_null_params(void)
{
    printf("Test: NULL parameter handling\n");

    nipc_shm_destroy(NULL);
    check("destroy(NULL) does not crash", 1);

    nipc_shm_close(NULL);
    check("close(NULL) does not crash", 1);

    uint8_t buf[64];
    size_t msg_len;
    nipc_shm_error_t err = nipc_shm_receive(NULL, buf, sizeof(buf),
                                              &msg_len, 1000);
    check("receive(NULL) returns error", err == NIPC_SHM_ERR_BAD_PARAM);
}

/* ------------------------------------------------------------------ */
/*  Coverage: pid_alive with pid=0                                      */
/* ------------------------------------------------------------------ */

static void test_pid_zero(void)
{
    printf("Test: owner_alive with dead PID via pid=0 in header\n");
    const char *svc = "shm_pid0";
    cleanup_shm(svc);

    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
    check("server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("client attach", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            /* Corrupt owner_pid to 0 */
            nipc_shm_region_header_t *hdr =
                (nipc_shm_region_header_t *)client.base;
            int32_t saved_pid = hdr->owner_pid;
            hdr->owner_pid = 0;

            check("owner_alive with pid=0 returns false",
                  !nipc_shm_owner_alive(&client));

            /* Restore */
            hdr->owner_pid = saved_pid;
            nipc_shm_close(&client);
        }
        nipc_shm_destroy(&server);
    }
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: owner_alive with dead PID, generation mismatch            */
/* ------------------------------------------------------------------ */

static void test_owner_dead_pid(void)
{
    printf("Test: owner_alive with dead PID\n");
    const char *svc = "shm_deadpid";
    cleanup_shm(svc);

    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
    check("server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("client attach", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            /* Set owner_pid to a definitely-dead PID */
            nipc_shm_region_header_t *hdr =
                (nipc_shm_region_header_t *)client.base;
            hdr->owner_pid = 99999;

            check("owner_alive with dead pid returns false",
                  !nipc_shm_owner_alive(&client));

            /* Restore for cleanup */
            hdr->owner_pid = (int32_t)getpid();
            nipc_shm_close(&client);
        }
        nipc_shm_destroy(&server);
    }
    cleanup_shm(svc);
}

static void test_owner_generation_mismatch(void)
{
    printf("Test: owner_alive with generation mismatch\n");
    const char *svc = "shm_gen_mm";
    cleanup_shm(svc);

    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
    check("server create", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("client attach", err == NIPC_SHM_OK);

        if (err == NIPC_SHM_OK) {
            /* Corrupt generation in header to mismatch client's cached value */
            nipc_shm_region_header_t *hdr =
                (nipc_shm_region_header_t *)client.base;
            uint32_t saved_gen = hdr->owner_generation;
            hdr->owner_generation = saved_gen + 1; /* mismatch */

            check("owner_alive with gen mismatch returns false",
                  !nipc_shm_owner_alive(&client));

            /* Restore */
            hdr->owner_generation = saved_gen;
            nipc_shm_close(&client);
        }
        nipc_shm_destroy(&server);
    }
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: cleanup_stale with NULL params, invalid service, no dir   */
/* ------------------------------------------------------------------ */

static void test_cleanup_stale_params(void)
{
    printf("Test: cleanup_stale parameter validation\n");

    /* NULL params - should not crash */
    nipc_shm_cleanup_stale(NULL, "svc");
    check("cleanup_stale(NULL, svc) ok", 1);

    nipc_shm_cleanup_stale(TEST_RUN_DIR, NULL);
    check("cleanup_stale(dir, NULL) ok", 1);

    nipc_shm_cleanup_stale(TEST_RUN_DIR, "bad/name");
    check("cleanup_stale invalid service ok", 1);

    nipc_shm_cleanup_stale("/tmp/nonexistent_shm_dir_99999", "svc");
    check("cleanup_stale nonexistent dir ok", 1);
}

/* ------------------------------------------------------------------ */
/*  Coverage: cleanup_stale with actual stale files                     */
/* ------------------------------------------------------------------ */

static void test_cleanup_stale_with_files(void)
{
    printf("Test: cleanup_stale with actual stale files\n");
    const char *svc = "shm_stale_cleanup";
    cleanup_shm(svc);

    /* Create a region, corrupt the PID to simulate dead owner, close but
     * leave the file. Then run cleanup_stale. */
    nipc_shm_ctx_t server;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
    check("create for cleanup test", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Set owner to dead PID */
        nipc_shm_region_header_t *hdr =
            (nipc_shm_region_header_t *)server.base;
        hdr->owner_pid = 99999;

        /* Close without unlink (just close fd + munmap) */
        nipc_shm_close(&server);

        /* Now run cleanup_stale - should find and unlink the stale file */
        nipc_shm_cleanup_stale(TEST_RUN_DIR, svc);

        /* Try to attach - should fail because file was cleaned up */
        nipc_shm_ctx_t client;
        err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
        check("stale file cleaned up", err != NIPC_SHM_OK);
    }
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: SHM msg_too_large on receive (caller buf too small)       */
/* ------------------------------------------------------------------ */

static void *small_recv_server_thread(void *arg)
{
    shm_server_ctx_t *ctx = (shm_server_ctx_t *)arg;
    ctx->echo_ok = 0;

    nipc_shm_ctx_t shm;
    nipc_shm_error_t err = nipc_shm_server_create(
        TEST_RUN_DIR, ctx->service, 1, 4096, 4096, &shm);
    if (err != NIPC_SHM_OK) {
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);

    /* Receive into a small buffer to trigger MSG_TOO_LARGE */
    uint8_t msg[32]; /* very small */
    size_t msg_len;
    err = nipc_shm_receive(&shm, msg, sizeof(msg), &msg_len, 5000);
    /* The message is larger than our buffer */
    ctx->echo_ok = (err == NIPC_SHM_ERR_MSG_TOO_LARGE) ? 1 : 0;

    nipc_shm_destroy(&shm);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_shm_receive_msg_too_large(void)
{
    printf("Test: SHM receive msg_too_large (caller buffer too small)\n");
    const char *svc = "shm_recv_large";
    cleanup_shm(svc);

    shm_server_ctx_t sctx = { .service = svc };
    __atomic_store_n(&sctx.ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx.done, 0, __ATOMIC_RELAXED);

    pthread_t tid;
    pthread_create(&tid, NULL, small_recv_server_thread, &sctx);

    int retries = 0;
    while (!__atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&sctx.done, __ATOMIC_ACQUIRE) && retries < 2000) {
        usleep(500);
        retries++;
    }
    check("server ready", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    nipc_shm_ctx_t client;
    nipc_shm_error_t err = nipc_shm_client_attach(TEST_RUN_DIR, svc, 1, &client);
    check("client attach", err == NIPC_SHM_OK);

    if (err == NIPC_SHM_OK) {
        /* Send a message larger than server's recv buffer (32 bytes) */
        uint8_t big_msg[256];
        memset(big_msg, 0xAA, sizeof(big_msg));
        /* Still need a valid header for semantics */
        nipc_header_t hdr = {
            .magic = NIPC_MAGIC_MSG, .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_REQUEST, .code = 1,
            .item_count = 1, .message_id = 1,
            .payload_len = sizeof(big_msg) - NIPC_HEADER_LEN,
        };
        nipc_header_encode(&hdr, big_msg, NIPC_HEADER_LEN);

        err = nipc_shm_send(&client, big_msg, sizeof(big_msg));
        check("client send large msg", err == NIPC_SHM_OK);

        nipc_shm_close(&client);
    }

    pthread_join(tid, NULL);
    check("server got MSG_TOO_LARGE", sctx.echo_ok);
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Coverage: stale file scenarios: undersized file, bad magic          */
/* ------------------------------------------------------------------ */

static void test_stale_undersized_file(void)
{
    printf("Test: Stale SHM undersized file\n");
    const char *svc = "shm_stale_small";
    cleanup_shm(svc);

    /* Create an undersized SHM file (< 64 bytes) */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s-%016" PRIx64 ".ipcshm",
             TEST_RUN_DIR, svc, (uint64_t)1);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        uint8_t small[10] = {0};
        write(fd, small, sizeof(small));
        close(fd);

        /* Server create should succeed by removing the stale undersized file */
        nipc_shm_ctx_t server;
        nipc_shm_error_t err = nipc_shm_server_create(
            TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
        check("create after undersized stale", err == NIPC_SHM_OK);
        if (err == NIPC_SHM_OK)
            nipc_shm_destroy(&server);
    } else {
        check("could not create undersized test file", 0);
    }
    cleanup_shm(svc);
}

static void test_stale_bad_magic_file(void)
{
    printf("Test: Stale SHM bad magic file\n");
    const char *svc = "shm_stale_bm";
    cleanup_shm(svc);

    /* Create an SHM file with bad magic but correct size */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s-%016" PRIx64 ".ipcshm",
             TEST_RUN_DIR, svc, (uint64_t)1);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        uint8_t data[256];
        memset(data, 0, sizeof(data));
        uint32_t bad_magic = 0xDEADBEEF;
        memcpy(data, &bad_magic, 4);
        write(fd, data, sizeof(data));
        close(fd);

        /* Server create should succeed by removing the stale bad-magic file */
        nipc_shm_ctx_t server;
        nipc_shm_error_t err = nipc_shm_server_create(
            TEST_RUN_DIR, svc, 1, 1024, 1024, &server);
        check("create after bad magic stale", err == NIPC_SHM_OK);
        if (err == NIPC_SHM_OK)
            nipc_shm_destroy(&server);
    } else {
        check("could not create bad magic test file", 0);
    }
    cleanup_shm(svc);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    ensure_run_dir();
    setbuf(stdout, NULL);

    printf("=== L1 POSIX SHM Transport Tests ===\n\n");

    test_direct_roundtrip();           printf("\n");
    test_negotiated_shm();             printf("\n");
    test_profile_fallback();           printf("\n");
    test_disconnect_detection();       printf("\n");
    test_stale_shm_recovery();         printf("\n");
    test_large_message();              printf("\n");
    test_msg_too_large();              printf("\n");
    test_addr_in_use();                printf("\n");
    test_multiple_roundtrips();        printf("\n");
    test_server_create_validation();   printf("\n");
    test_client_attach_validation();   printf("\n");
    test_shm_close_null();             printf("\n");
    test_shm_send_bad_param();         printf("\n");
    test_shm_bad_magic_file();         printf("\n");
    test_shm_bad_version_file();       printf("\n");
    test_shm_truncated_file();         printf("\n");
    test_shm_partial_header_not_ready(); printf("\n");

    /* Coverage gap tests */
    test_null_params();                printf("\n");
    test_pid_zero();                   printf("\n");
    test_owner_dead_pid();             printf("\n");
    test_owner_generation_mismatch();  printf("\n");
    test_cleanup_stale_params();       printf("\n");
    test_cleanup_stale_with_files();   printf("\n");
    test_shm_receive_msg_too_large();  printf("\n");
    test_stale_undersized_file();      printf("\n");
    test_stale_bad_magic_file();       printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
