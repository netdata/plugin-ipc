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
        TEST_RUN_DIR, sa->service, AUTH_TOKEN,
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
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
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
        TEST_RUN_DIR, sa->service, AUTH_TOKEN,
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
                                          NIPC_WIN_SHM_PROFILE_HYBRID, &client);
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
/*  Test: service name validation                                      */
/* ------------------------------------------------------------------ */

static void test_service_name_validation(void)
{
    printf("--- Service name validation ---\n");

    nipc_win_shm_ctx_t ctx;

    check("empty name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "", AUTH_TOKEN,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("dot name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, ".", AUTH_TOKEN,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("slash in name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "bad/name", AUTH_TOKEN,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("space in name rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "bad name", AUTH_TOKEN,
              NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);

    check("invalid profile rejected",
          nipc_win_shm_server_create(TEST_RUN_DIR, "good-name", AUTH_TOKEN,
              0xFF, 4096, 4096, &ctx)
          == NIPC_WIN_SHM_ERR_BAD_PARAM);
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
    test_basic_roundtrip();
    test_multiple_roundtrips();

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
