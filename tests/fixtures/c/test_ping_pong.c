/*
 * test_ping_pong.c - L2 typed ping-pong tests for INCREMENT and STRING_REVERSE.
 *
 * Proves the architecture: L1 transport is generic, Codec is per-method,
 * L2 composes them. Both fixed-size (INCREMENT) and variable-length
 * (STRING_REVERSE) payloads are exercised over the same connection.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR  "/tmp/nipc_pingpong_test"
#define SERVICE_NAME  "ping-pong"
#define AUTH_TOKEN    0x1234567890ABCDEFull
#define RESPONSE_BUF  4096

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

static void cleanup_socket(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, SERVICE_NAME);
    unlink(path);
}

/* ------------------------------------------------------------------ */
/*  Multi-method server handler                                        */
/* ------------------------------------------------------------------ */

/* ---- Typed business-logic handlers (never touch wire format) ---- */

static bool on_increment(void *user, uint64_t request, uint64_t *response)
{
    (void)user;
    *response = request + 1;
    return true;
}

static bool on_string_reverse(void *user,
                                const char *request_str, uint32_t request_str_len,
                                char *response_str, uint32_t response_capacity,
                                uint32_t *response_str_len)
{
    (void)user;
    if (request_str_len > response_capacity)
        return false;

    for (uint32_t i = 0; i < request_str_len; i++)
        response_str[i] = request_str[request_str_len - 1 - i];

    *response_str_len = request_str_len;
    return true;
}

/* ---- Thin dispatcher: routes method_code to typed helpers ---- */

static bool multi_method_handler(
    void *user,
    uint16_t method_code,
    const uint8_t *request_payload, size_t request_len,
    uint8_t *response_buf, size_t response_buf_size,
    size_t *response_len_out)
{
    switch (method_code) {
    case NIPC_METHOD_INCREMENT:
        return nipc_dispatch_increment(request_payload, request_len,
                                        response_buf, response_buf_size,
                                        response_len_out, on_increment, user);
    case NIPC_METHOD_STRING_REVERSE:
        return nipc_dispatch_string_reverse(request_payload, request_len,
                                             response_buf, response_buf_size,
                                             response_len_out, on_string_reverse, user);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/*  Server thread                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    nipc_managed_server_t server;
    bool started;
} server_ctx_t;

static void *server_thread(void *arg)
{
    server_ctx_t *sctx = (server_ctx_t *)arg;
    nipc_server_run(&sctx->server);
    return NULL;
}

static bool start_server(server_ctx_t *sctx)
{
    cleanup_socket();

    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = RESPONSE_BUF,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_error_t err = nipc_server_init(&sctx->server,
                                          TEST_RUN_DIR, SERVICE_NAME,
                                          &scfg, 4, RESPONSE_BUF,
                                          multi_method_handler, NULL);
    if (err != NIPC_OK)
        return false;

    pthread_t tid;
    if (pthread_create(&tid, NULL, server_thread, sctx) != 0)
        return false;
    pthread_detach(tid);

    usleep(50000); /* 50ms for server to start */
    sctx->started = true;
    return true;
}

static void stop_server(server_ctx_t *sctx)
{
    if (sctx->started) {
        nipc_server_drain(&sctx->server, 2000);
        nipc_server_destroy(&sctx->server);
        sctx->started = false;
    }
    cleanup_socket();
}

/* ------------------------------------------------------------------ */
/*  Test: INCREMENT ping-pong                                          */
/* ------------------------------------------------------------------ */

static void test_increment_ping_pong(void)
{
    printf("\nTest: INCREMENT ping-pong (10 rounds)\n");

    server_ctx_t sctx = {0};
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = RESPONSE_BUF,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    uint8_t resp_buf[RESPONSE_BUF];
    uint64_t value = 0;
    int responses_received = 0;
    bool all_ok = true;

    for (int round = 0; round < 10; round++) {
        uint64_t sent = value;
        uint64_t result;
        nipc_error_t err = nipc_client_call_increment(
            &client, sent, resp_buf, sizeof(resp_buf), &result);

        if (err != NIPC_OK) {
            printf("  FAIL: round %d: call error %d\n", round, err);
            all_ok = false;
            break;
        }
        responses_received++;

        if (result != sent + 1) {
            printf("  FAIL: round %d: sent %lu, expected %lu, got %lu\n",
                   round, (unsigned long)sent,
                   (unsigned long)(sent + 1), (unsigned long)result);
            all_ok = false;
            break;
        }
        value = result; /* feed the response back as next request */
    }

    check("each response == request + 1", all_ok);
    check("responses received == 10", responses_received == 10);
    check("final value == 10", value == 10);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Test: STRING_REVERSE ping-pong                                     */
/* ------------------------------------------------------------------ */

static void test_string_reverse_ping_pong(void)
{
    printf("\nTest: STRING_REVERSE ping-pong (alphabet)\n");

    server_ctx_t sctx = {0};
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = RESPONSE_BUF,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    uint8_t resp_buf[RESPONSE_BUF];
    char current[64];
    strncpy(current, "abcdefghijklmnopqrstuvwxyz", sizeof(current));
    current[sizeof(current) - 1] = '\0';

    bool all_ok = true;
    int responses_received = 0;

    for (int round = 0; round < 6; round++) {
        uint32_t sent_len = (uint32_t)strlen(current);

        nipc_string_reverse_view_t view;
        nipc_error_t err = nipc_client_call_string_reverse(
            &client, current, sent_len,
            resp_buf, sizeof(resp_buf), &view);

        if (err != NIPC_OK) {
            printf("  FAIL: round %d: call error %d\n", round, err);
            all_ok = false;
            break;
        }
        responses_received++;

        /* Verify response length matches request length */
        if (view.str_len != sent_len) {
            printf("  FAIL: round %d: response len %u != request len %u\n",
                   round, view.str_len, sent_len);
            all_ok = false;
            break;
        }

        /* Verify response is the reverse of the request we sent */
        bool is_reverse = true;
        for (uint32_t i = 0; i < sent_len; i++) {
            if (view.str[i] != current[sent_len - 1 - i]) {
                printf("  FAIL: round %d: pos %u: expected '%c', got '%c'\n",
                       round, i, current[sent_len - 1 - i], view.str[i]);
                is_reverse = false;
                break;
            }
        }
        if (!is_reverse) {
            all_ok = false;
            break;
        }

        /* Feed response back as next request */
        memcpy(current, view.str, view.str_len);
        current[view.str_len] = '\0';
    }

    check("every response is the reverse of its request", all_ok);
    check("responses received == 6", responses_received == 6);
    /* After even number of reversals, should be back to original */
    check("6 reversals returns to original",
          strcmp(current, "abcdefghijklmnopqrstuvwxyz") == 0);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Test: mixed methods on same connection                             */
/* ------------------------------------------------------------------ */

static void test_mixed_methods(void)
{
    printf("\nTest: mixed INCREMENT + STRING_REVERSE on same connection\n");

    server_ctx_t sctx = {0};
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = RESPONSE_BUF,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    uint8_t resp_buf[RESPONSE_BUF];

    /* Interleave: increment, reverse, increment, reverse */
    uint64_t inc_val;
    nipc_error_t err;

    err = nipc_client_call_increment(&client, 100, resp_buf, sizeof(resp_buf), &inc_val);
    check("increment 100 -> 101", err == NIPC_OK && inc_val == 101);

    nipc_string_reverse_view_t sv;
    err = nipc_client_call_string_reverse(&client, "hello", 5,
                                           resp_buf, sizeof(resp_buf), &sv);
    check("reverse 'hello' -> 'olleh'",
          err == NIPC_OK && sv.str_len == 5 && memcmp(sv.str, "olleh", 5) == 0);

    err = nipc_client_call_increment(&client, inc_val, resp_buf, sizeof(resp_buf), &inc_val);
    check("increment 101 -> 102", err == NIPC_OK && inc_val == 102);

    err = nipc_client_call_string_reverse(&client, "world", 5,
                                           resp_buf, sizeof(resp_buf), &sv);
    check("reverse 'world' -> 'dlrow'",
          err == NIPC_OK && sv.str_len == 5 && memcmp(sv.str, "dlrow", 5) == 0);

    err = nipc_client_call_increment(&client, inc_val, resp_buf, sizeof(resp_buf), &inc_val);
    check("increment 102 -> 103", err == NIPC_OK && inc_val == 103);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Test: empty string                                                 */
/* ------------------------------------------------------------------ */

static void test_empty_string(void)
{
    printf("\nTest: STRING_REVERSE with empty string\n");

    server_ctx_t sctx = {0};
    check("server started", start_server(&sctx));

    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = RESPONSE_BUF,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, SERVICE_NAME, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));

    uint8_t resp_buf[RESPONSE_BUF];
    nipc_string_reverse_view_t sv;
    nipc_error_t err = nipc_client_call_string_reverse(
        &client, "", 0, resp_buf, sizeof(resp_buf), &sv);

    check("empty string: call ok", err == NIPC_OK);
    check("empty string: result len == 0", err == NIPC_OK && sv.str_len == 0);

    nipc_client_close(&client);
    stop_server(&sctx);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== L2 Ping-Pong Tests (INCREMENT + STRING_REVERSE) ===\n");

    ensure_run_dir();

    test_increment_ping_pong();
    test_string_reverse_ping_pong();
    test_mixed_methods();
    test_empty_string();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
