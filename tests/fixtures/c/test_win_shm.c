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

static nipc_win_shm_error_t attach_client_retry(const char *service,
                                                uint64_t session_id,
                                                uint32_t profile,
                                                nipc_win_shm_ctx_t *ctx)
{
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;

    for (int i = 0; i < 100; i++) {
        err = nipc_win_shm_client_attach(TEST_RUN_DIR, service, AUTH_TOKEN,
                                         session_id, profile, ctx);
        if (err == NIPC_WIN_SHM_OK)
            break;
        Sleep(10);
    }

    return err;
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

static uint64_t compute_shm_hash_for_test(const char *run_dir,
                                          const char *service_name)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s\n%s\n%llu",
                     run_dir, service_name,
                     (unsigned long long)AUTH_TOKEN);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return 0;

    return nipc_fnv1a_64(buf, (size_t)n);
}

static int build_object_name_for_test(wchar_t *dst, size_t dst_chars,
                                      uint64_t hash,
                                      const char *service_name,
                                      uint32_t profile,
                                      uint64_t session_id,
                                      const char *suffix)
{
    char narrow[NIPC_WIN_SHM_MAX_NAME];
    int n = snprintf(narrow, sizeof(narrow),
                     "Local\\netipc-%016llx-%s-p%u-s%016llx-%s",
                     (unsigned long long)hash, service_name,
                     (unsigned)profile,
                     (unsigned long long)session_id, suffix);
    if (n < 0 || (size_t)n >= sizeof(narrow))
        return -1;

    if ((size_t)(n + 1) > dst_chars)
        return -1;

    for (int i = 0; i <= n; i++)
        dst[i] = (wchar_t)(unsigned char)narrow[i];

    return 0;
}

static int setup_manual_hybrid_mapping_for_test(const char *service_name,
                                                uint64_t session_id,
                                                HANDLE *mapping_out,
                                                void **base_out,
                                                wchar_t *req_event_name,
                                                size_t req_event_chars,
                                                wchar_t *resp_event_name,
                                                size_t resp_event_chars)
{
    const size_t region_size = 128u + 4096u + 4096u;
    uint64_t hash = compute_shm_hash_for_test(TEST_RUN_DIR, service_name);
    wchar_t mapping_name[NIPC_WIN_SHM_MAX_NAME];
    HANDLE mapping = NULL;
    void *base = NULL;
    int ok = hash != 0 &&
        build_object_name_for_test(mapping_name, NIPC_WIN_SHM_MAX_NAME,
                                   hash, service_name,
                                   NIPC_WIN_SHM_PROFILE_HYBRID,
                                   session_id, "mapping") == 0 &&
        build_object_name_for_test(req_event_name, req_event_chars,
                                   hash, service_name,
                                   NIPC_WIN_SHM_PROFILE_HYBRID,
                                   session_id, "req_event") == 0 &&
        build_object_name_for_test(resp_event_name, resp_event_chars,
                                   hash, service_name,
                                   NIPC_WIN_SHM_PROFILE_HYBRID,
                                   session_id, "resp_event") == 0;

    if (ok) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                     PAGE_READWRITE,
                                     (DWORD)(region_size >> 32),
                                     (DWORD)(region_size & 0xFFFFFFFF),
                                     mapping_name);
        ok = mapping != NULL;
    }

    if (ok) {
        base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, region_size);
        ok = base != NULL;
    }

    if (ok) {
        nipc_win_shm_region_header_t *hdr =
            (nipc_win_shm_region_header_t *)base;
        memset(base, 0, region_size);
        hdr->magic = NIPC_WIN_SHM_MAGIC;
        hdr->version = NIPC_WIN_SHM_VERSION;
        hdr->header_len = NIPC_WIN_SHM_HEADER_LEN;
        hdr->profile = NIPC_WIN_SHM_PROFILE_HYBRID;
        hdr->request_offset = 128;
        hdr->request_capacity = 4096;
        hdr->response_offset = 128 + 4096;
        hdr->response_capacity = 4096;
        hdr->spin_tries = NIPC_WIN_SHM_DEFAULT_SPIN;
        MemoryBarrier();
    }

    if (!ok) {
        if (base)
            UnmapViewOfFile(base);
        if (mapping)
            CloseHandle(mapping);
        return 0;
    }

    *mapping_out = mapping;
    *base_out = base;
    return 1;
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

static void test_cleanup_stale_noop(void)
{
    printf("--- cleanup_stale() no-op ---\n");
    nipc_win_shm_cleanup_stale(TEST_RUN_DIR, "noop-service");
    nipc_win_shm_cleanup_stale(NULL, NULL);
    check("cleanup_stale no-op", 1);
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
    char missing_service[64];
    char long_run_dir[520];
    char long_service[208];
    char hybrid_event_overflow_service[202];
    nipc_win_shm_ctx_t temp_ctx;
    const uint64_t overflow_session_id = 55;
    unique_service(service, sizeof(service));
    unique_service(missing_service, sizeof(missing_service));
    fill_long_run_dir(long_run_dir, sizeof(long_run_dir));
    fill_service_name(long_service, sizeof(long_service), 200);
    fill_service_name(hybrid_event_overflow_service,
                      sizeof(hybrid_event_overflow_service), 195);

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

    check("client attach missing mapping rejected",
          nipc_win_shm_client_attach(TEST_RUN_DIR, missing_service, AUTH_TOKEN, 5,
              NIPC_WIN_SHM_PROFILE_HYBRID, &temp_ctx)
          == NIPC_WIN_SHM_ERR_OPEN_MAPPING);

    {
        uint64_t hash = compute_shm_hash_for_test(TEST_RUN_DIR,
                                                  hybrid_event_overflow_service);
        wchar_t mapping_name[NIPC_WIN_SHM_MAX_NAME];
        HANDLE mapping = NULL;
        void *base = NULL;
        const size_t region_size = 128u + 4096u + 4096u;
        int setup_ok = hash != 0 &&
            build_object_name_for_test(mapping_name, NIPC_WIN_SHM_MAX_NAME,
                                       hash, hybrid_event_overflow_service,
                                       NIPC_WIN_SHM_PROFILE_HYBRID,
                                       overflow_session_id, "mapping") == 0;

        if (setup_ok) {
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                         PAGE_READWRITE,
                                         (DWORD)(region_size >> 32),
                                         (DWORD)(region_size & 0xFFFFFFFF),
                                         mapping_name);
            setup_ok = mapping != NULL;
        }

        if (setup_ok) {
            base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, region_size);
            setup_ok = base != NULL;
        }

        check("manual hybrid mapping setup for attach overflow", setup_ok);
        if (setup_ok) {
            nipc_win_shm_region_header_t *hdr =
                (nipc_win_shm_region_header_t *)base;
            memset(base, 0, region_size);
            hdr->magic = NIPC_WIN_SHM_MAGIC;
            hdr->version = NIPC_WIN_SHM_VERSION;
            hdr->header_len = NIPC_WIN_SHM_HEADER_LEN;
            hdr->profile = NIPC_WIN_SHM_PROFILE_HYBRID;
            hdr->request_offset = 128;
            hdr->request_capacity = 4096;
            hdr->response_offset = 128 + 4096;
            hdr->response_capacity = 4096;
            hdr->spin_tries = NIPC_WIN_SHM_DEFAULT_SPIN;
            MemoryBarrier();

            check("client attach hybrid event name overflow rejected",
                  nipc_win_shm_client_attach(TEST_RUN_DIR,
                      hybrid_event_overflow_service, AUTH_TOKEN,
                      overflow_session_id, NIPC_WIN_SHM_PROFILE_HYBRID,
                      &temp_ctx) == NIPC_WIN_SHM_ERR_BAD_PARAM);
        }

        if (base)
            UnmapViewOfFile(base);
        if (mapping)
            CloseHandle(mapping);
    }

    {
        char eventless_service[64];
        wchar_t req_event_name[NIPC_WIN_SHM_MAX_NAME];
        wchar_t resp_event_name[NIPC_WIN_SHM_MAX_NAME];
        HANDLE mapping = NULL;
        void *base = NULL;
        HANDLE req_event = NULL;
        const uint64_t session_id = 56;

        unique_service(eventless_service, sizeof(eventless_service));
        check("manual hybrid mapping setup without events",
              setup_manual_hybrid_mapping_for_test(eventless_service, session_id,
                  &mapping, &base,
                  req_event_name, NIPC_WIN_SHM_MAX_NAME,
                  resp_event_name, NIPC_WIN_SHM_MAX_NAME));

        if (mapping && base) {
            check("client attach rejects missing hybrid request event",
                  nipc_win_shm_client_attach(TEST_RUN_DIR, eventless_service,
                      AUTH_TOKEN, session_id, NIPC_WIN_SHM_PROFILE_HYBRID,
                      &temp_ctx) == NIPC_WIN_SHM_ERR_OPEN_EVENT);

            req_event = CreateEventW(NULL, FALSE, FALSE, req_event_name);
            check("manual hybrid request event created", req_event != NULL);
            if (req_event) {
                check("client attach rejects missing hybrid response event",
                      nipc_win_shm_client_attach(TEST_RUN_DIR, eventless_service,
                          AUTH_TOKEN, session_id, NIPC_WIN_SHM_PROFILE_HYBRID,
                          &temp_ctx) == NIPC_WIN_SHM_ERR_OPEN_EVENT);
            }
        }

        if (req_event)
            CloseHandle(req_event);
        if (base)
            UnmapViewOfFile(base);
        if (mapping)
            CloseHandle(mapping);
    }

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

static void test_server_create_rejects_existing_objects(void)
{
    printf("--- Server create rejects existing kernel objects ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t first = {0};
    nipc_win_shm_ctx_t second = {0};
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 77,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &first);
    check("initial server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 77,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &second);
    check("duplicate server create gets ADDR_IN_USE",
          err == NIPC_WIN_SHM_ERR_ADDR_IN_USE);

    nipc_win_shm_destroy(&first);
}

static void test_hybrid_receive_timeout_and_disconnect(void)
{
    printf("--- HYBRID receive timeout / disconnect ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 6,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("hybrid timeout server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 6, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
    check("hybrid timeout client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 1);
    check("hybrid receive timeout", err == NIPC_WIN_SHM_ERR_TIMEOUT);
    check("hybrid timeout keeps resp seq", client.local_resp_seq == 0);

    nipc_win_shm_destroy(&server);

    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 1000);
    check("hybrid receive disconnected after server destroy",
          err == NIPC_WIN_SHM_ERR_DISCONNECTED);
    check("hybrid disconnect advances resp seq", client.local_resp_seq == 1);

    nipc_win_shm_close(&client);
}

typedef struct {
    nipc_win_shm_ctx_t *ctx;
    DWORD delay_ms;
    uint16_t code;
    uint64_t message_id;
    uint8_t payload[16];
    size_t payload_len;
    int result;
} delayed_send_args_t;

static DWORD WINAPI delayed_send_thread(LPVOID arg)
{
    delayed_send_args_t *sa = (delayed_send_args_t *)arg;
    sa->result = 1;

    Sleep(sa->delay_ms);

    uint8_t msg[128];
    size_t msg_len = build_message(msg, sizeof(msg),
                                   NIPC_KIND_RESPONSE, sa->code,
                                   sa->message_id,
                                   sa->payload, sa->payload_len);
    nipc_win_shm_error_t err = nipc_win_shm_send(sa->ctx, msg, msg_len);
    sa->result = (err == NIPC_WIN_SHM_OK) ? 0 : 1;
    return (err == NIPC_WIN_SHM_OK) ? 0 : 1;
}

static void test_hybrid_receive_zero_timeout_waits_for_data(void)
{
    printf("--- HYBRID receive zero-timeout waits for data ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 16,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("hybrid zero-timeout server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 16, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
    check("hybrid zero-timeout client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    client.spin_tries = 0;

    delayed_send_args_t sa = {
        .ctx = &server,
        .delay_ms = 25,
        .code = NIPC_METHOD_INCREMENT,
        .message_id = 777,
        .payload = { 0x11, 0x22, 0x33, 0x44 },
        .payload_len = 4,
        .result = 1,
    };
    HANDLE thread = CreateThread(NULL, 0, delayed_send_thread, &sa, 0, NULL);
    check("hybrid zero-timeout sender thread created", thread != NULL);
    if (!thread) {
        nipc_win_shm_close(&client);
        nipc_win_shm_destroy(&server);
        return;
    }

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 0);
    check("hybrid zero-timeout receive returns data", err == NIPC_WIN_SHM_OK);
    check("hybrid zero-timeout advances resp seq", client.local_resp_seq == 1);

    if (err == NIPC_WIN_SHM_OK) {
        nipc_header_t hdr;
        nipc_header_decode(buf, msg_len, &hdr);
        check("hybrid zero-timeout response kind", hdr.kind == NIPC_KIND_RESPONSE);
        check("hybrid zero-timeout response code", hdr.code == NIPC_METHOD_INCREMENT);
        check("hybrid zero-timeout response message_id", hdr.message_id == 777);
        check("hybrid zero-timeout payload echo",
              msg_len == NIPC_HEADER_LEN + sa.payload_len &&
              memcmp(buf + NIPC_HEADER_LEN, sa.payload, sa.payload_len) == 0);
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    check("hybrid zero-timeout sender completed", sa.result == 0);

    nipc_win_shm_close(&client);
    nipc_win_shm_destroy(&server);
}

static void test_hybrid_receive_recheck_observes_ready_data(void)
{
    printf("--- HYBRID receive recheck sees ready data ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 17,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("hybrid recheck server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 17, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
    check("hybrid recheck client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    client.spin_tries = 0;

    nipc_win_shm_region_header_t *hdr =
        (nipc_win_shm_region_header_t *)client.base;
    uint8_t *resp_area = (uint8_t *)client.base + client.response_offset;
    uint8_t msg[128];
    uint8_t payload[4] = { 0x51, 0x52, 0x53, 0x54 };
    size_t wire_len = build_message(msg, sizeof(msg),
                                    NIPC_KIND_RESPONSE, NIPC_METHOD_INCREMENT,
                                    778, payload, sizeof(payload));

    memcpy(resp_area, msg, wire_len);
    InterlockedExchange(&hdr->resp_len, (LONG)wire_len);
    InterlockedExchange64((volatile LONG64 *)&hdr->resp_seq, 1);
    MemoryBarrier();

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 1000);
    check("hybrid recheck receive returns data", err == NIPC_WIN_SHM_OK);
    check("hybrid recheck advances resp seq", client.local_resp_seq == 1);
    check("hybrid recheck clears waiting flag", hdr->resp_client_waiting == 0);

    if (err == NIPC_WIN_SHM_OK) {
        nipc_header_t out;
        nipc_header_decode(buf, msg_len, &out);
        check("hybrid recheck response kind", out.kind == NIPC_KIND_RESPONSE);
        check("hybrid recheck response code", out.code == NIPC_METHOD_INCREMENT);
        check("hybrid recheck response message_id", out.message_id == 778);
        check("hybrid recheck payload echo",
              msg_len == NIPC_HEADER_LEN + sizeof(payload) &&
              memcmp(buf + NIPC_HEADER_LEN, payload, sizeof(payload)) == 0);
    }

    nipc_win_shm_close(&client);
    nipc_win_shm_destroy(&server);
}

static void test_busywait_receive_timeout_and_disconnect(void)
{
    printf("--- BUSYWAIT receive timeout / disconnect ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 7,
        NIPC_WIN_SHM_PROFILE_BUSYWAIT, 4096, 4096, &server);
    check("busywait timeout server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 7, NIPC_WIN_SHM_PROFILE_BUSYWAIT, &client);
    check("busywait timeout client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    client.spin_tries = 0;

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 1);
    check("busywait receive timeout", err == NIPC_WIN_SHM_ERR_TIMEOUT);
    check("busywait timeout keeps resp seq", client.local_resp_seq == 0);

    nipc_win_shm_destroy(&server);

    err = nipc_win_shm_receive(&client, buf, sizeof(buf), &msg_len, 1000);
    check("busywait receive disconnected after server destroy",
          err == NIPC_WIN_SHM_ERR_DISCONNECTED);
    check("busywait disconnect advances resp seq", client.local_resp_seq == 1);

    nipc_win_shm_close(&client);
}

static void test_hybrid_server_receive_disconnect_advances_req_seq(void)
{
    printf("--- HYBRID server receive disconnect advances req seq ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 18,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("hybrid server-disconnect server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 18, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
    check("hybrid server-disconnect client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    server.spin_tries = 0;

    nipc_win_shm_close(&client);

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&server, buf, sizeof(buf), &msg_len, 1000);
    check("hybrid server receive disconnected after client close",
          err == NIPC_WIN_SHM_ERR_DISCONNECTED);
    check("hybrid server disconnect advances req seq", server.local_req_seq == 1);

    nipc_win_shm_destroy(&server);
}

static void test_busywait_server_receive_disconnect_advances_req_seq(void)
{
    printf("--- BUSYWAIT server receive disconnect advances req seq ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 17,
        NIPC_WIN_SHM_PROFILE_BUSYWAIT, 4096, 4096, &server);
    check("busywait server-disconnect server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 17, NIPC_WIN_SHM_PROFILE_BUSYWAIT, &client);
    check("busywait server-disconnect client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    server.spin_tries = 0;

    nipc_win_shm_close(&client);

    uint8_t buf[64];
    size_t msg_len = 0;
    err = nipc_win_shm_receive(&server, buf, sizeof(buf), &msg_len, 1000);
    check("busywait server receive disconnected after client close",
          err == NIPC_WIN_SHM_ERR_DISCONNECTED);
    check("busywait server disconnect advances req seq", server.local_req_seq == 1);

    nipc_win_shm_destroy(&server);
}

static void test_client_receive_msg_too_large_response(void)
{
    printf("--- Client receive large response -> MSG_TOO_LARGE ---\n");

    char service[64];
    unique_service(service, sizeof(service));

    nipc_win_shm_ctx_t server;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        TEST_RUN_DIR, service, AUTH_TOKEN, 8,
        NIPC_WIN_SHM_PROFILE_HYBRID, 4096, 4096, &server);
    check("large-response server create", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK)
        return;

    nipc_win_shm_ctx_t client;
    err = attach_client_retry(service, 8, NIPC_WIN_SHM_PROFILE_HYBRID, &client);
    check("large-response client attach", err == NIPC_WIN_SHM_OK);
    if (err != NIPC_WIN_SHM_OK) {
        nipc_win_shm_destroy(&server);
        return;
    }

    uint8_t req_payload[1] = { 0x42 };
    uint8_t req_msg[128];
    size_t req_len = build_message(req_msg, sizeof(req_msg),
                                   NIPC_KIND_REQUEST, NIPC_METHOD_INCREMENT,
                                   123, req_payload, sizeof(req_payload));
    err = nipc_win_shm_send(&client, req_msg, req_len);
    check("large-response client send", err == NIPC_WIN_SHM_OK);

    uint8_t server_req[128];
    size_t server_req_len = 0;
    err = nipc_win_shm_receive(&server, server_req, sizeof(server_req),
                               &server_req_len, 10000);
    check("large-response server receive", err == NIPC_WIN_SHM_OK);

    if (err == NIPC_WIN_SHM_OK) {
        uint8_t resp_payload[256];
        memset(resp_payload, 0xAB, sizeof(resp_payload));
        uint8_t resp_msg[512];
        size_t resp_len = build_message(resp_msg, sizeof(resp_msg),
                                        NIPC_KIND_RESPONSE, NIPC_METHOD_INCREMENT,
                                        123, resp_payload, sizeof(resp_payload));
        err = nipc_win_shm_send(&server, resp_msg, resp_len);
        check("large-response server send", err == NIPC_WIN_SHM_OK);
    }

    uint8_t small_resp[32];
    size_t small_resp_len = 0;
    err = nipc_win_shm_receive(&client, small_resp, sizeof(small_resp),
                               &small_resp_len, 10000);
    check("large-response client gets MSG_TOO_LARGE",
          err == NIPC_WIN_SHM_ERR_MSG_TOO_LARGE);
    check("large-response reports actual message len",
          err == NIPC_WIN_SHM_ERR_MSG_TOO_LARGE &&
          small_resp_len == NIPC_HEADER_LEN + 256);
    check("large-response advances client resp seq", client.local_resp_seq == 1);

    nipc_win_shm_close(&client);
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
    test_server_create_rejects_existing_objects();
    test_client_attach_validation();
    test_hybrid_receive_timeout_and_disconnect();
    test_hybrid_receive_zero_timeout_waits_for_data();
    test_hybrid_receive_recheck_observes_ready_data();
    test_hybrid_server_receive_disconnect_advances_req_seq();
    test_busywait_receive_timeout_and_disconnect();
    test_busywait_server_receive_disconnect_advances_req_seq();
    test_basic_roundtrip();
    test_multiple_roundtrips();
    test_busywait_roundtrip();
    test_receive_msg_too_large();
    test_client_receive_msg_too_large_response();
    test_send_too_large();
    test_send_receive_validation();
    test_null_close_destroy();
    test_cleanup_stale_noop();

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
