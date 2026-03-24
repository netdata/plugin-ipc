/*
 * test_win_shm.c - Integration tests for L1 Windows SHM transport.
 *
 * Uses threads for server/client. Prints PASS/FAIL for each test.
 * Returns 0 on all-pass.
 */

#ifdef _WIN32

#include "netipc/netipc_win_shm.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static volatile LONG g_test_counter = 0;

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define TEST_RUN_DIR "C:\\Temp\\nipc_win_shm_test"

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

static void unique_service(char *buf, size_t len)
{
    LONG n = InterlockedIncrement(&g_test_counter);
    snprintf(buf, len, "wshm_%ld_%lu", (long)n, (unsigned long)GetCurrentProcessId());
}

static void fill_service_name(char *buf, size_t len, size_t count)
{
    if (count + 1 > len)
        count = len - 1;
    memset(buf, 'a', count);
    buf[count] = '\0';
}

static void fill_long_run_dir(char *buf, size_t len)
{
    if (len < 3)
        return;

    buf[0] = 'C';
    buf[1] = ':';
    for (size_t i = 2; i + 1 < len; i++)
        buf[i] = 'x';
    buf[len - 1] = '\0';
}

/* Build a complete message (outer header + payload) for SHM. */
static size_t build_message(uint8_t *dst, size_t dst_size,
                             uint16_t kind, uint16_t code,
                             uint64_t message_id,
                             const void *payload, size_t payload_len)
{
    if (dst_size < NIPC_HEADER_LEN + payload_len)
        return 0;

    nipc_header_t hdr = {
        .magic       = NIPC_MAGIC_MSG,
        .version     = NIPC_VERSION,
        .header_len  = NIPC_HEADER_LEN,
        .kind        = kind,
        .code        = code,
        .flags       = 0,
        .item_count  = 1,
        .message_id  = message_id,
        .payload_len = (uint32_t)payload_len,
        .transport_status = NIPC_STATUS_OK,
    };

    nipc_header_encode(&hdr, dst, NIPC_HEADER_LEN);
    if (payload_len > 0)
        memcpy(dst + NIPC_HEADER_LEN, payload, payload_len);

    return NIPC_HEADER_LEN + payload_len;
}

/* ------------------------------------------------------------------ */
/*  Test: header layout assertions                                     */
/* ------------------------------------------------------------------ */

static void test_header_layout(void)
{
    printf("--- Header layout ---\n");
    check("sizeof(header) == 128",
          sizeof(nipc_win_shm_region_header_t) == 128);
    check("offsetof(spin_tries) == 32",
          offsetof(nipc_win_shm_region_header_t, spin_tries) == 32);
    check("offsetof(req_len) == 36",
          offsetof(nipc_win_shm_region_header_t, req_len) == 36);
    check("offsetof(resp_len) == 40",
          offsetof(nipc_win_shm_region_header_t, resp_len) == 40);
    check("offsetof(req_client_closed) == 44",
          offsetof(nipc_win_shm_region_header_t, req_client_closed) == 44);
    check("offsetof(req_server_waiting) == 48",
          offsetof(nipc_win_shm_region_header_t, req_server_waiting) == 48);
    check("offsetof(resp_server_closed) == 52",
          offsetof(nipc_win_shm_region_header_t, resp_server_closed) == 52);
    check("offsetof(resp_client_waiting) == 56",
          offsetof(nipc_win_shm_region_header_t, resp_client_waiting) == 56);
    check("offsetof(req_seq) == 64",
          offsetof(nipc_win_shm_region_header_t, req_seq) == 64);
    check("offsetof(resp_seq) == 72",
          offsetof(nipc_win_shm_region_header_t, resp_seq) == 72);
}

/* ------------------------------------------------------------------ */
/*  Test: basic roundtrip                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int result;
} server_args_t;

static DWORD WINAPI server_thread(LPVOID arg)
{
    server_args_t *sa = (server_args_t *)arg;
    sa->result = 1;

    nipc_win_shm_ctx_t ctx;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, sa->service, AUTH_TOKEN, 1,
        NIPC_WIN_SHM_PROFILE_HYBRID, 65536, 65536, &ctx);
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "server: create failed: %d\n", err);
        return 1;
    }

    /* Receive one message */
    uint8_t msg[65536];
    size_t msg_len;
    err = nipc_win_shm_receive(&ctx, msg, sizeof(msg), &msg_len, 10000);
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "server: receive failed: %d\n", err);
        nipc_win_shm_destroy(&ctx);
        return 1;
    }

    /* Echo as response */
    if (msg_len >= NIPC_HEADER_LEN) {
        nipc_header_t hdr;
        nipc_header_decode(msg, msg_len, &hdr);
        hdr.kind = NIPC_KIND_RESPONSE;
        hdr.transport_status = NIPC_STATUS_OK;

        size_t payload_len = msg_len - NIPC_HEADER_LEN;
        uint8_t resp[65536];
        size_t resp_len = build_message(resp, sizeof(resp),
                                         NIPC_KIND_RESPONSE, hdr.code,
                                         hdr.message_id,
                                         msg + NIPC_HEADER_LEN, payload_len);

        err = nipc_win_shm_send(&ctx, resp, resp_len);
        if (err != NIPC_WIN_SHM_OK) {
            fprintf(stderr, "server: send failed: %d\n", err);
            nipc_win_shm_destroy(&ctx);
            return 1;
        }
    }

    nipc_win_shm_destroy(&ctx);
    sa->result = 0;
    return 0;
}

static void test_basic_roundtrip(void)
{
    printf("--- Basic roundtrip (HYBRID) ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    server_args_t sa = { .service = service };
    HANDLE thread = CreateThread(NULL, 0, server_thread, &sa, 0, NULL);
    check("server thread created", thread != NULL);
    if (!thread) return;

    /* Give server time to create mapping */
    Sleep(100);

    /* Client side */
    nipc_win_shm_ctx_t client;
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
    for (int i = 0; i < 100; i++) {
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                          1, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        if (err == NIPC_WIN_SHM_OK) break;
        Sleep(10);
    }
    check("client attach", err == NIPC_WIN_SHM_OK);

    if (err == NIPC_WIN_SHM_OK) {
        /* Build and send request */
        uint8_t payload[256];
        for (int i = 0; i < 256; i++) payload[i] = (uint8_t)(i & 0xFF);

        uint8_t msg[512];
        size_t msg_len = build_message(msg, sizeof(msg),
                                        NIPC_KIND_REQUEST, NIPC_METHOD_INCREMENT,
                                        42, payload, sizeof(payload));
        err = nipc_win_shm_send(&client, msg, msg_len);
        check("client send", err == NIPC_WIN_SHM_OK);

        /* Receive response */
        uint8_t resp[65536];
        size_t resp_len;
        err = nipc_win_shm_receive(&client, resp, sizeof(resp), &resp_len, 10000);
        check("client receive", err == NIPC_WIN_SHM_OK);

        if (err == NIPC_WIN_SHM_OK) {
            nipc_header_t rhdr;
            nipc_header_decode(resp, resp_len, &rhdr);
            check("response kind", rhdr.kind == NIPC_KIND_RESPONSE);
            check("response message_id", rhdr.message_id == 42);
            check("response payload len", resp_len == NIPC_HEADER_LEN + 256);
            check("response payload data",
                  memcmp(resp + NIPC_HEADER_LEN, payload, 256) == 0);
        }

        nipc_win_shm_close(&client);
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    check("server completed ok", sa.result == 0);
}

/* ------------------------------------------------------------------ */
/*  Test: multiple roundtrips                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    int count;
    int result;
} multi_server_args_t;

static DWORD WINAPI multi_server_thread(LPVOID arg)
{
    multi_server_args_t *sa = (multi_server_args_t *)arg;
    sa->result = 1;

    nipc_win_shm_ctx_t ctx;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, sa->service, AUTH_TOKEN, 1,
        NIPC_WIN_SHM_PROFILE_HYBRID, 65536, 65536, &ctx);
    if (err != NIPC_WIN_SHM_OK) return 1;

    uint8_t msg[65536];
    uint8_t resp[65536];

    for (int i = 0; i < sa->count; i++) {
        size_t msg_len;
        err = nipc_win_shm_receive(&ctx, msg, sizeof(msg), &msg_len, 10000);
        if (err != NIPC_WIN_SHM_OK) {
            nipc_win_shm_destroy(&ctx);
            return 1;
        }

        nipc_header_t hdr;
        nipc_header_decode(msg, msg_len, &hdr);
        size_t payload_len = msg_len - NIPC_HEADER_LEN;
        size_t resp_len = build_message(resp, sizeof(resp),
                                         NIPC_KIND_RESPONSE, hdr.code,
                                         hdr.message_id,
                                         msg + NIPC_HEADER_LEN, payload_len);
        err = nipc_win_shm_send(&ctx, resp, resp_len);
        if (err != NIPC_WIN_SHM_OK) {
            nipc_win_shm_destroy(&ctx);
            return 1;
        }
    }

    nipc_win_shm_destroy(&ctx);
    sa->result = 0;
    return 0;
}

static void test_multiple_roundtrips(void)
{
    printf("--- Multiple roundtrips (10x) ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    multi_server_args_t sa = { .service = service, .count = 10 };
    HANDLE thread = CreateThread(NULL, 0, multi_server_thread, &sa, 0, NULL);
    if (!thread) { g_fail++; return; }

    Sleep(100);

    nipc_win_shm_ctx_t client;
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
    for (int i = 0; i < 100; i++) {
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                          1, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        if (err == NIPC_WIN_SHM_OK) break;
        Sleep(10);
    }

    int all_ok = (err == NIPC_WIN_SHM_OK);

    if (all_ok) {
        uint8_t msg[512];
        uint8_t resp[65536];
        for (int i = 0; i < 10 && all_ok; i++) {
            uint8_t payload = (uint8_t)i;
            size_t msg_len = build_message(msg, sizeof(msg),
                                            NIPC_KIND_REQUEST, 1,
                                            (uint64_t)(i + 1),
                                            &payload, 1);
            err = nipc_win_shm_send(&client, msg, msg_len);
            if (err != NIPC_WIN_SHM_OK) { all_ok = 0; break; }

            size_t resp_len;
            err = nipc_win_shm_receive(&client, resp, sizeof(resp),
                                        &resp_len, 10000);
            if (err != NIPC_WIN_SHM_OK) { all_ok = 0; break; }

            nipc_header_t rhdr;
            nipc_header_decode(resp, resp_len, &rhdr);
            if (rhdr.message_id != (uint64_t)(i + 1)) { all_ok = 0; break; }
            if (resp[NIPC_HEADER_LEN] != (uint8_t)i) { all_ok = 0; break; }
        }
        nipc_win_shm_close(&client);
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    check("10 roundtrips completed", all_ok && sa.result == 0);
}

/* ------------------------------------------------------------------ */
/*  Test: BUSYWAIT profile paths                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI busywait_server_thread(LPVOID arg)
{
    server_args_t *sa = (server_args_t *)arg;
    sa->result = 1;

    nipc_win_shm_ctx_t ctx;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, sa->service, AUTH_TOKEN, 2,
        NIPC_WIN_SHM_PROFILE_BUSYWAIT, 65536, 65536, &ctx);
    if (err != NIPC_WIN_SHM_OK)
        return 1;

    ctx.spin_tries = 0; /* force the busywait receive path */

    uint8_t msg[65536];
    size_t msg_len;
    err = nipc_win_shm_receive(&ctx, msg, sizeof(msg), &msg_len, 10000);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&ctx);
        return 1;
    }

    nipc_header_t hdr;
    nipc_header_decode(msg, msg_len, &hdr);
    size_t payload_len = msg_len - NIPC_HEADER_LEN;

    uint8_t resp[65536];
    size_t resp_len = build_message(resp, sizeof(resp),
                                     NIPC_KIND_RESPONSE, hdr.code,
                                     hdr.message_id,
                                     msg + NIPC_HEADER_LEN, payload_len);
    err = nipc_win_shm_send(&ctx, resp, resp_len);

    nipc_win_shm_destroy(&ctx);
    sa->result = (err == NIPC_WIN_SHM_OK) ? 0 : 1;
    return (err == NIPC_WIN_SHM_OK) ? 0 : 1;
}

static void test_busywait_roundtrip(void)
{
    printf("--- Basic roundtrip (BUSYWAIT) ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    server_args_t sa = { .service = service };
    HANDLE thread = CreateThread(NULL, 0, busywait_server_thread, &sa, 0, NULL);
    check("busywait server thread created", thread != NULL);
    if (!thread)
        return;

    Sleep(100);

    nipc_win_shm_ctx_t client;
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
    for (int i = 0; i < 100; i++) {
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                          2, NIPC_WIN_SHM_PROFILE_BUSYWAIT, &client);
        if (err == NIPC_WIN_SHM_OK)
            break;
        Sleep(10);
    }
    check("busywait client attach", err == NIPC_WIN_SHM_OK);

    if (err == NIPC_WIN_SHM_OK) {
        client.spin_tries = 0; /* force the busywait receive path */

        uint8_t payload[256];
        for (int i = 0; i < 256; i++)
            payload[i] = (uint8_t)(255 - i);

        uint8_t msg[512];
        size_t msg_len = build_message(msg, sizeof(msg),
                                        NIPC_KIND_REQUEST, NIPC_METHOD_INCREMENT,
                                        99, payload, sizeof(payload));

        err = nipc_win_shm_send(&client, msg, msg_len);
        check("busywait client send", err == NIPC_WIN_SHM_OK);

        uint8_t resp[65536];
        size_t resp_len;
        err = nipc_win_shm_receive(&client, resp, sizeof(resp), &resp_len, 10000);
        check("busywait client receive", err == NIPC_WIN_SHM_OK);

        if (err == NIPC_WIN_SHM_OK) {
            nipc_header_t rhdr;
            nipc_header_decode(resp, resp_len, &rhdr);
            check("busywait response kind", rhdr.kind == NIPC_KIND_RESPONSE);
            check("busywait response message_id", rhdr.message_id == 99);
            check("busywait payload echo",
                  resp_len == NIPC_HEADER_LEN + sizeof(payload) &&
                  memcmp(resp + NIPC_HEADER_LEN, payload, sizeof(payload)) == 0);
        }

        nipc_win_shm_close(&client);
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    check("busywait server completed ok", sa.result == 0);
}

typedef struct {
    const char *service;
    int result;
} small_recv_args_t;

static DWORD WINAPI small_recv_server_thread(LPVOID arg)
{
    small_recv_args_t *sa = (small_recv_args_t *)arg;
    sa->result = 1;

    nipc_win_shm_ctx_t ctx;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, sa->service, AUTH_TOKEN, 3,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx);
    if (err != NIPC_WIN_SHM_OK)
        return 1;

    uint8_t msg[32];
    size_t msg_len;
    err = nipc_win_shm_receive(&ctx, msg, sizeof(msg), &msg_len, 10000);
    sa->result = (err == NIPC_WIN_SHM_ERR_MSG_TOO_LARGE) ? 0 : 1;

    nipc_win_shm_destroy(&ctx);
    return sa->result;
}

static void test_receive_msg_too_large(void)
{
    printf("--- Receive into small buffer -> MSG_TOO_LARGE ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    small_recv_args_t sa = { .service = service };
    HANDLE thread = CreateThread(NULL, 0, small_recv_server_thread, &sa, 0, NULL);
    check("small-recv server thread created", thread != NULL);
    if (!thread)
        return;

    Sleep(100);

    nipc_win_shm_ctx_t client;
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
    for (int i = 0; i < 100; i++) {
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                          3, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        if (err == NIPC_WIN_SHM_OK)
            break;
        Sleep(10);
    }
    check("small-recv client attach", err == NIPC_WIN_SHM_OK);

    if (err == NIPC_WIN_SHM_OK) {
        uint8_t big_msg[256];
        memset(big_msg, 0xAA, sizeof(big_msg));
        nipc_header_t hdr = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = NIPC_KIND_REQUEST,
            .code = NIPC_METHOD_INCREMENT,
            .item_count = 1,
            .message_id = 1,
            .payload_len = sizeof(big_msg) - NIPC_HEADER_LEN,
            .transport_status = NIPC_STATUS_OK,
        };
        nipc_header_encode(&hdr, big_msg, NIPC_HEADER_LEN);

        err = nipc_win_shm_send(&client, big_msg, sizeof(big_msg));
        check("small-recv client send", err == NIPC_WIN_SHM_OK);
        nipc_win_shm_close(&client);
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    check("server got MSG_TOO_LARGE", sa.result == 0);
}

static void test_send_too_large(void)
{
    printf("--- Send too large ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 4,
        NIPC_WIN_SHM_PROFILE_HYBRID, 128, 128, &server);
    check("too-large server create", err == NIPC_WIN_SHM_OK);

    if (err == NIPC_WIN_SHM_OK) {
        nipc_win_shm_ctx_t client;
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                          4, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        check("too-large client attach", err == NIPC_WIN_SHM_OK);

        if (err == NIPC_WIN_SHM_OK) {
            uint8_t msg[256];
            memset(msg, 0x55, sizeof(msg));
            err = nipc_win_shm_send(&client, msg, sizeof(msg));
            check("send too large rejected", err == NIPC_WIN_SHM_ERR_MSG_TOO_LARGE);
            nipc_win_shm_close(&client);
        }

        nipc_win_shm_destroy(&server);
    }
}

static void test_send_receive_validation(void)
{
    printf("--- Send/receive bad-parameter validation ---\n");

    uint8_t buf[16] = {0};
    size_t msg_len = 0;
    nipc_win_shm_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    check("send null ctx rejected",
          nipc_win_shm_send(NULL, buf, sizeof(buf)) == NIPC_WIN_SHM_ERR_BAD_PARAM);
    check("send null base rejected",
          nipc_win_shm_send(&ctx, buf, sizeof(buf)) == NIPC_WIN_SHM_ERR_BAD_PARAM);

    ctx.base = &ctx;
    check("send null msg rejected",
          nipc_win_shm_send(&ctx, NULL, sizeof(buf)) == NIPC_WIN_SHM_ERR_BAD_PARAM);
    check("send zero len rejected",
          nipc_win_shm_send(&ctx, buf, 0) == NIPC_WIN_SHM_ERR_BAD_PARAM);

    ctx.base = NULL;
    check("receive null ctx rejected",
          nipc_win_shm_receive(NULL, buf, sizeof(buf), &msg_len, 1)
              == NIPC_WIN_SHM_ERR_BAD_PARAM);
    check("receive null base rejected",
          nipc_win_shm_receive(&ctx, buf, sizeof(buf), &msg_len, 1)
              == NIPC_WIN_SHM_ERR_BAD_PARAM);

    ctx.base = &ctx;
    check("receive null buf rejected",
          nipc_win_shm_receive(&ctx, NULL, sizeof(buf), &msg_len, 1)
              == NIPC_WIN_SHM_ERR_BAD_PARAM);
    check("receive zero buf size rejected",
          nipc_win_shm_receive(&ctx, buf, 0, &msg_len, 1)
              == NIPC_WIN_SHM_ERR_BAD_PARAM);
    check("receive null len out rejected",
          nipc_win_shm_receive(&ctx, buf, sizeof(buf), NULL, 1)
              == NIPC_WIN_SHM_ERR_BAD_PARAM);
}

static void test_null_close_destroy(void)
{
    printf("--- NULL close/destroy ---\n");
    nipc_win_shm_close(NULL);
    nipc_win_shm_destroy(NULL);
    check("NULL close/destroy no-op", 1);
}

/* ------------------------------------------------------------------ */
/*  Test: service name validation                                      */
/* ------------------------------------------------------------------ */

static void test_service_name_validation(void)
{
    printf("--- Service name validation ---\n");

    nipc_win_shm_ctx_t ctx;
    char long_run_dir[520];
    char mapping_overflow_service[208];
    char hybrid_event_overflow_service[202];

    fill_long_run_dir(long_run_dir, sizeof(long_run_dir));
    fill_service_name(mapping_overflow_service, sizeof(mapping_overflow_service), 200);
    fill_service_name(hybrid_event_overflow_service,
                      sizeof(hybrid_event_overflow_service), 195);

    check("null run dir rejected",
          nipc_win_shm_server_create(NULL, "good-name", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("null service name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, NULL, AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("empty name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("dot name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, ".", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("slash in name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "bad/name", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("space in name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "bad name", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("invalid profile rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "good-name", AUTH_TOKEN, 1,
              0xFF, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("run dir overflow rejected",
          nipc_win_shm_server_create(long_run_dir, "good-name", AUTH_TOKEN, 1,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("mapping name overflow rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, mapping_overflow_service,
              AUTH_TOKEN, 1, NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("hybrid event name overflow rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, hybrid_event_overflow_service,
              AUTH_TOKEN, 1, NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("busywait long name still accepted",
          nipc_win_shm_server_create(TEST_RUN_DIR, hybrid_event_overflow_service,
              AUTH_TOKEN, 1, NIPC_WIN_SHM_PROFILE_BUSYWAIT, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_OK);
    nipc_win_shm_destroy(&ctx);
}

static void test_client_attach_validation(void)
{
    printf("--- Client attach validation / header rejection ---\n");

    char service[64];
    char long_run_dir[520];
    char long_service[208];
    nipc_win_shm_ctx_t temp_ctx;
    unique_service(service, sizeof(service));
    fill_long_run_dir(long_run_dir, sizeof(long_run_dir));
    fill_service_name(long_service, sizeof(long_service), 200);

    check("server create null ctx rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, NULL)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach null ctx rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, NULL)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach null run dir rejected",
          nipc_win_shm_client_attach(NULL, service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach null service rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, NULL, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach empty service rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, "", AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach invalid profile rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
              0xFF, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach run dir overflow rejected",
          nipc_win_shm_client_attach(long_run_dir, service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("client attach mapping name overflow rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, long_service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 5,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("validation server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_region_header_t *hdr =
        (nipc_win_shm_region_header_t *)server.base;
    const uint32_t saved_magic = hdr->magic;
    const uint16_t saved_version = hdr->version;
    const uint16_t saved_header_len = hdr->header_len;
    const uint32_t saved_profile = hdr->profile;

    hdr->magic = 0;
    MemoryBarrier();
    {
        nipc_win_shm_ctx_t client;
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        check("client attach rejects bad magic",
              err == NIPC_WIN_SHM_ERR_BAD_MAGIC);
    }

    hdr->magic = saved_magic;
    hdr->version = 0;
    MemoryBarrier();
    {
        nipc_win_shm_ctx_t client;
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        check("client attach rejects bad version",
              err == NIPC_WIN_SHM_ERR_BAD_VERSION);
    }

    hdr->version = saved_version;
    hdr->header_len = 0;
    MemoryBarrier();
    {
        nipc_win_shm_ctx_t client;
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        check("client attach rejects bad header len",
              err == NIPC_WIN_SHM_ERR_BAD_HEADER);
    }

    hdr->header_len = saved_header_len;
    hdr->profile = NIPC_WIN_SHM_PROFILE_BUSYWAIT;
    MemoryBarrier();
    {
        nipc_win_shm_ctx_t client;
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN, 5,
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
        check("client attach rejects mismatched profile",
              err == NIPC_WIN_SHM_ERR_BAD_PROFILE);
    }

    hdr->profile = saved_profile;
    MemoryBarrier();
    nipc_win_shm_destroy(&server);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== Windows SHM Transport Tests ===\n\n");

    /* Ensure test directory exists */
    CreateDirectoryA(TEST_RUN_DIR, NULL);

    test_header_layout();
    test_service_name_validation();
    test_client_attach_validation();
    test_basic_roundtrip();
    test_multiple_roundtrips();
    test_busywait_roundtrip();
    test_receive_msg_too_large();
    test_send_too_large();
    test_send_receive_validation();
    test_null_close_destroy();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

#else /* !_WIN32 */

#include <stdio.h>
int main(void)
{
    printf("Windows SHM tests not supported on this platform\n");
    return 1;
}

#endif /* _WIN32 */
