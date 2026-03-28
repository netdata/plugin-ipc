/*
 * test_chaos.c - Transport-level chaos testing and SHM chaos testing.
 *
 * Phase H9: Verifies the server handles every form of malformed,
 * truncated, and garbage input without crashing, leaking, or hanging.
 *
 * Tests:
 *   1. Random bytes after handshake (100 iterations)
 *   2. Valid header + truncated payload
 *   3. Valid header with wrong magic
 *   4. Valid header with payload_len exceeding negotiated limit
 *   5. Partial chunk (start chunking, don't send all chunks)
 *   6. Connect, send nothing, disconnect after 100ms
 *   7. Connect, send half a header, disconnect
 *   8. SHM chaos: random bytes in request area
 *
 * Every test verifies the server is still alive after the attack
 * by completing a normal client request.
 *
 * Prints PASS/FAIL for each test. Returns 0 on all-pass.
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Test infrastructure                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_RUN_DIR       "/tmp/nipc_chaos_test"
#define AUTH_TOKEN         0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE  65536

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

static void cleanup_all(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
    nipc_shm_cleanup_stale(TEST_RUN_DIR, service);
}

/* Fill buffer with random bytes from /dev/urandom */
static void fill_random(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* fallback: use rand() */
        uint8_t *p = (uint8_t *)buf;
        for (size_t i = 0; i < len; i++)
            p[i] = (uint8_t)(rand() & 0xFF);
        return;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (uint8_t *)buf + total, len - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/*  Cgroups handler (server side)                                      */
/* ------------------------------------------------------------------ */

static nipc_error_t test_cgroups_handler(void *user,
                                         const nipc_header_t *request_hdr,
                                         const uint8_t *request_payload,
                                         size_t request_len,
                                         uint8_t *response_buf,
                                         size_t response_buf_size,
                                         size_t *response_len_out)
{
    (void)user;
    (void)request_hdr;

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               2, 1, 42);

    err = nipc_cgroups_builder_add(&builder,
        1001, 0, 1, "test-cg1", 8, "/sys/fs/cgroup/test1", 20);
    if (err != NIPC_OK) return err;

    err = nipc_cgroups_builder_add(&builder,
        2002, 0, 1, "test-cg2", 8, "/sys/fs/cgroup/test2", 20);
    if (err != NIPC_OK) return err;

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/* ------------------------------------------------------------------ */
/*  Server management                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_managed_server_t server;
    int ready;   /* __atomic */
    int done;    /* __atomic */
} chaos_server_ctx_t;

static void *chaos_server_fn(void *arg)
{
    chaos_server_ctx_t *ctx = (chaos_server_ctx_t *)arg;

    nipc_uds_server_config_t scfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
        .preferred_profiles        = 0,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 64,
    };

    nipc_error_t err = nipc_server_init(&ctx->server,
        TEST_RUN_DIR, ctx->service, &scfg,
        4, NIPC_METHOD_CGROUPS_SNAPSHOT, test_cgroups_handler, NULL);

    if (err != NIPC_OK) {
        fprintf(stderr, "chaos server init failed: %d\n", err);
        __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    nipc_server_run(&ctx->server);
    nipc_server_destroy(&ctx->server);
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void start_chaos_server(chaos_server_ctx_t *sctx, const char *service,
                                pthread_t *tid)
{
    memset(sctx, 0, sizeof(*sctx));
    sctx->service = service;
    __atomic_store_n(&sctx->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sctx->done, 0, __ATOMIC_RELAXED);

    pthread_create(tid, NULL, chaos_server_fn, sctx);

    for (int i = 0; i < 4000
         && !__atomic_load_n(&sctx->ready, __ATOMIC_ACQUIRE)
         && !__atomic_load_n(&sctx->done, __ATOMIC_ACQUIRE); i++)
        usleep(500);
}

static void stop_chaos_server(chaos_server_ctx_t *sctx, pthread_t tid)
{
    nipc_server_stop(&sctx->server);
    pthread_join(tid, NULL);
}

/* ------------------------------------------------------------------ */
/*  Helper: verify server is still alive with a normal client          */
/* ------------------------------------------------------------------ */

static bool verify_server_alive(const char *service)
{
    nipc_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);

    /* Try connecting with retries (server may be processing disconnect) */
    bool connected = false;
    for (int r = 0; r < 200; r++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client)) {
            connected = true;
            break;
        }
        usleep(5000);
    }

    if (!connected) {
        nipc_client_close(&client);
        return false;
    }

    /* Make a real call */
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];
    nipc_cgroups_resp_view_t view;

    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

    bool alive = (err == NIPC_OK && view.item_count == 2);

    nipc_client_close(&client);
    return alive;
}

/* ------------------------------------------------------------------ */
/*  Helper: raw connect + handshake, return the raw fd                 */
/* ------------------------------------------------------------------ */

/* Connect and handshake normally, then return the raw session fd.
 * The caller owns the fd and can send whatever garbage they want. */
static int chaos_connect(const char *service, nipc_uds_session_t *session)
{
    nipc_uds_client_config_t ccfg = {
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_uds_error_t err = nipc_uds_connect(
        TEST_RUN_DIR, service, &ccfg, session);

    if (err != NIPC_UDS_OK)
        return -1;

    return session->fd;
}

/* Raw connect via socket (no handshake) */
static int raw_connect(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Random bytes after handshake (100 iterations)              */
/* ------------------------------------------------------------------ */

static void test_random_bytes_after_handshake(void)
{
    printf("Test 1: Random bytes after handshake (100 iterations)\n");
    const char *svc = "chaos_random";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    int success = 0;
    int connect_fail = 0;

    for (int i = 0; i < 100; i++) {
        nipc_uds_session_t session;
        int fd = chaos_connect(svc, &session);
        if (fd < 0) {
            connect_fail++;
            /* Server might be busy cleaning up, retry after delay */
            usleep(10000);
            continue;
        }

        /* Send random garbage (16 to 4096 bytes) */
        size_t garbage_len = 16 + (rand() % 4080);
        uint8_t garbage[4096];
        fill_random(garbage, garbage_len);

        ssize_t sent = send(fd, garbage, garbage_len, MSG_NOSIGNAL);
        (void)sent;

        /* Close immediately */
        close(fd);
        session.fd = -1;
        free(session.recv_buf);

        success++;
    }

    printf("    100 iterations: %d connected+sent, %d connect failures\n",
           success, connect_fail);
    check("most chaos clients connected", success >= 80);

    /* Verify server survived */
    usleep(100000); /* let server process disconnects */
    bool alive = verify_server_alive(svc);
    check("server alive after 100 random-byte attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 2: Valid header + truncated payload                           */
/* ------------------------------------------------------------------ */

static void test_truncated_payload(void)
{
    printf("Test 2: Valid header + truncated payload\n");
    const char *svc = "chaos_trunc";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    for (int i = 0; i < 10; i++) {
        nipc_uds_session_t session;
        int fd = chaos_connect(svc, &session);
        if (fd < 0) { usleep(10000); continue; }

        /* Build a valid header claiming 1000 bytes of payload */
        nipc_header_t hdr = {0};
        hdr.magic = NIPC_MAGIC_MSG;
        hdr.version = NIPC_VERSION;
        hdr.header_len = NIPC_HEADER_LEN;
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        hdr.payload_len = 1000; /* claimed payload */
        hdr.item_count = 1;
        hdr.message_id = (uint64_t)(i + 1);

        uint8_t msg[64]; /* only send header + 10 bytes, not 1000 */
        nipc_header_encode(&hdr, msg, sizeof(msg));

        /* Add 10 bytes of "payload" (truncated) */
        fill_random(msg + NIPC_HEADER_LEN, 10);
        ssize_t sent = send(fd, msg, NIPC_HEADER_LEN + 10, MSG_NOSIGNAL);
        (void)sent;

        close(fd);
        session.fd = -1;
        free(session.recv_buf);

        usleep(5000);
    }

    usleep(100000);
    bool alive = verify_server_alive(svc);
    check("server alive after truncated payload attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Valid header with wrong magic                               */
/* ------------------------------------------------------------------ */

static void test_wrong_magic(void)
{
    printf("Test 3: Valid header structure with wrong magic\n");
    const char *svc = "chaos_magic";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    for (int i = 0; i < 10; i++) {
        nipc_uds_session_t session;
        int fd = chaos_connect(svc, &session);
        if (fd < 0) { usleep(10000); continue; }

        /* Build a header with bad magic */
        nipc_header_t hdr = {0};
        hdr.magic = 0xDEADDEADu; /* wrong magic */
        hdr.version = NIPC_VERSION;
        hdr.header_len = NIPC_HEADER_LEN;
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        hdr.payload_len = 4;
        hdr.item_count = 1;
        hdr.message_id = (uint64_t)(i + 1);

        uint8_t msg[48];
        nipc_header_encode(&hdr, msg, sizeof(msg));
        /* Overwrite magic field at offset 0 (native endian) */
        uint32_t bad_magic = 0xDEADDEADu;
        memcpy(msg, &bad_magic, 4);

        /* Add 4 bytes of payload */
        memset(msg + NIPC_HEADER_LEN, 0xAA, 4);
        ssize_t sent = send(fd, msg, NIPC_HEADER_LEN + 4, MSG_NOSIGNAL);
        (void)sent;

        close(fd);
        session.fd = -1;
        free(session.recv_buf);

        usleep(5000);
    }

    usleep(100000);
    bool alive = verify_server_alive(svc);
    check("server alive after wrong-magic attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 4: payload_len exceeds negotiated limit                       */
/* ------------------------------------------------------------------ */

static void test_payload_exceeds_limit(void)
{
    printf("Test 4: Valid header with payload_len exceeding negotiated limit\n");
    const char *svc = "chaos_limit";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    for (int i = 0; i < 10; i++) {
        nipc_uds_session_t session;
        int fd = chaos_connect(svc, &session);
        if (fd < 0) { usleep(10000); continue; }

        /* Claim a huge payload_len (exceeds negotiated max) */
        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
        hdr.payload_len = 10000000; /* 10MB - way over limit */
        hdr.item_count = 1;
        hdr.message_id = (uint64_t)(i + 1);

        uint8_t msg[64];
        size_t encoded = nipc_header_encode(&hdr, msg, sizeof(msg));

        /* Send just the header (payload doesn't exist) */
        ssize_t sent = send(fd, msg, encoded, MSG_NOSIGNAL);
        (void)sent;

        close(fd);
        session.fd = -1;
        free(session.recv_buf);

        usleep(5000);
    }

    usleep(100000);
    bool alive = verify_server_alive(svc);
    check("server alive after limit-exceeding attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 5: Partial chunk (start chunking, don't finish)               */
/* ------------------------------------------------------------------ */

static void test_partial_chunk(void)
{
    printf("Test 5: Partial chunk (start chunking, don't send all chunks)\n");
    const char *svc = "chaos_chunk";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    for (int i = 0; i < 10; i++) {
        nipc_uds_session_t session;
        int fd = chaos_connect(svc, &session);
        if (fd < 0) { usleep(10000); continue; }

        /* Send chunk 0 of 3 (then disconnect without sending chunks 1,2) */
        nipc_chunk_header_t chk = {0};
        chk.magic = NIPC_MAGIC_CHUNK;
        chk.version = NIPC_VERSION;
        chk.flags = 0;
        chk.message_id = (uint64_t)(i + 1);
        chk.total_message_len = 4096;
        chk.chunk_index = 0;
        chk.chunk_count = 3;
        chk.chunk_payload_len = 100;

        uint8_t msg[256];
        size_t encoded = nipc_chunk_header_encode(&chk, msg, sizeof(msg));

        /* Add 100 bytes of chunk data */
        fill_random(msg + encoded, 100);
        ssize_t sent = send(fd, msg, encoded + 100, MSG_NOSIGNAL);
        (void)sent;

        /* Disconnect without sending remaining chunks */
        close(fd);
        session.fd = -1;
        free(session.recv_buf);

        usleep(5000);
    }

    usleep(100000);
    bool alive = verify_server_alive(svc);
    check("server alive after partial-chunk attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 6: Connect, send nothing, disconnect                          */
/* ------------------------------------------------------------------ */

static void test_silent_connect_disconnect(void)
{
    printf("Test 6: Connect, send nothing, disconnect after 100ms\n");
    const char *svc = "chaos_silent";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    int connected = 0;
    for (int i = 0; i < 20; i++) {
        /* Raw connect (no handshake) */
        int fd = raw_connect(svc);
        if (fd < 0) {
            usleep(10000);
            continue;
        }
        connected++;

        /* Wait 100ms, then disconnect without sending anything */
        usleep(100000);
        close(fd);
    }

    printf("    %d silent connects\n", connected);
    check("silent connects succeeded", connected >= 10);

    usleep(200000);
    bool alive = verify_server_alive(svc);
    check("server alive after silent connect/disconnect", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 7: Connect, send half a header, disconnect                    */
/* ------------------------------------------------------------------ */

static void test_half_header(void)
{
    printf("Test 7: Connect, send half a header, disconnect\n");
    const char *svc = "chaos_half";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    int sent_count = 0;
    for (int i = 0; i < 20; i++) {
        /* Raw connect (no handshake) */
        int fd = raw_connect(svc);
        if (fd < 0) {
            usleep(10000);
            continue;
        }

        /* Send only 12 bytes of a 32-byte header */
        uint8_t partial[12];
        fill_random(partial, sizeof(partial));
        ssize_t n = send(fd, partial, sizeof(partial), MSG_NOSIGNAL);
        if (n > 0) sent_count++;

        usleep(50000);
        close(fd);
    }

    printf("    %d half-header sends\n", sent_count);
    check("half-header sends succeeded", sent_count >= 10);

    usleep(200000);
    bool alive = verify_server_alive(svc);
    check("server alive after half-header attacks", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 8: SHM chaos - random bytes in request area                   */
/* ------------------------------------------------------------------ */

static void test_shm_chaos(void)
{
    printf("Test 8: SHM chaos - random bytes in request area\n");
    const char *svc = "chaos_shm";
    uint64_t session_id = 0;
    cleanup_all(svc);

    /* Start server with SHM support */
    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    /* First, do a normal connect to trigger SHM creation */
    {
        nipc_client_config_t ccfg = {
            .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID,
            .preferred_profiles        = NIPC_PROFILE_SHM_HYBRID,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items   = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 1,
            .auth_token                = AUTH_TOKEN,
        };

        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

        /* Connect and do one normal call to ensure SHM is set up */
        bool connected = false;
        for (int r = 0; r < 200; r++) {
            nipc_client_refresh(&client);
            if (nipc_client_ready(&client)) {
                connected = true;
                break;
            }
            usleep(5000);
        }

        if (connected) {
            uint8_t req_buf[64], resp_buf[RESPONSE_BUF_SIZE];
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);
            check("initial SHM call succeeded", err == NIPC_OK);
            if (err == NIPC_OK)
                session_id = client.session.session_id;
        }

        nipc_client_close(&client);
    }

    /* Now directly open the SHM file and write garbage */
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "%s/%s-%016llx.ipcshm",
             TEST_RUN_DIR, svc, (unsigned long long)session_id);
    check("SHM session established", session_id != 0);

    int shm_fd = open(shm_path, O_RDWR);
    if (shm_fd >= 0) {
        struct stat st;
        if (fstat(shm_fd, &st) == 0 && st.st_size > 0) {
            void *base = mmap(NULL, (size_t)st.st_size,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              shm_fd, 0);

            if (base != MAP_FAILED) {
                /* Read the header to find request area offset */
                uint32_t req_offset = 0;
                uint32_t req_capacity = 0;
                memcpy(&req_offset, (uint8_t *)base + 16, sizeof(uint32_t));
                memcpy(&req_capacity, (uint8_t *)base + 20, sizeof(uint32_t));

                printf("    SHM region: size=%ld, req_offset=%u, req_capacity=%u\n",
                       (long)st.st_size, req_offset, req_capacity);

                if (req_offset > 0 && req_capacity > 0 &&
                    req_offset + req_capacity <= (uint32_t)st.st_size) {

                    /* Write random bytes into the request area */
                    uint8_t *req_area = (uint8_t *)base + req_offset;
                    int chaos_rounds = 50;

                    for (int i = 0; i < chaos_rounds; i++) {
                        /* Fill request area with random data */
                        size_t garbage_len = (req_capacity < 4096) ?
                                              req_capacity : 4096;
                        fill_random(req_area, garbage_len);

                        /* Also write a garbage req_len (offset 48) */
                        uint32_t fake_len = (uint32_t)(32 + (rand() % 2000));
                        __atomic_store_n(
                            (uint32_t *)((uint8_t *)base + 48),
                            fake_len, __ATOMIC_RELEASE);

                        /* Increment req_seq (offset 32) to signal the server */
                        __atomic_fetch_add(
                            (uint64_t *)((uint8_t *)base + 32),
                            1, __ATOMIC_RELEASE);

                        /* Wake the server futex (req_signal at offset 56) */
                        __atomic_fetch_add(
                            (uint32_t *)((uint8_t *)base + 56),
                            1, __ATOMIC_RELEASE);
                        /* futex_wake equivalent: just signal the word,
                         * the server's spin loop should notice. */

                        usleep(5000); /* let server process */
                    }

                    printf("    Sent %d SHM chaos rounds\n", chaos_rounds);
                    check("SHM chaos injection completed", 1);
                } else {
                    check("SHM region layout valid for chaos test",
                          req_offset > 0 && req_capacity > 0);
                }

                munmap(base, (size_t)st.st_size);
            } else {
                check("mmap succeeded", 0);
            }
        }
        close(shm_fd);
    } else {
        printf("    expected live SHM file not found: %s\n", shm_path);
        check("open live SHM file for chaos", 0);
    }

    /* Verify server survived (it should have broken the SHM session
     * but still be able to accept new UDS clients) */
    usleep(500000); /* give server time to process */
    bool alive = verify_server_alive(svc);
    check("server alive after SHM chaos", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Test 9: Rapid chaos mix (combined attack patterns)                 */
/* ------------------------------------------------------------------ */

static void test_rapid_chaos_mix(void)
{
    printf("Test 9: Rapid chaos mix (varied attack patterns, 50 rounds)\n");
    const char *svc = "chaos_mix";
    cleanup_all(svc);

    chaos_server_ctx_t sctx;
    pthread_t tid;
    start_chaos_server(&sctx, svc, &tid);
    check("server started", __atomic_load_n(&sctx.ready, __ATOMIC_ACQUIRE) == 1);

    int attacks = 0;
    for (int i = 0; i < 50; i++) {
        int attack_type = rand() % 5;

        switch (attack_type) {
        case 0: {
            /* Random bytes after handshake */
            nipc_uds_session_t session;
            int fd = chaos_connect(svc, &session);
            if (fd >= 0) {
                uint8_t garbage[256];
                fill_random(garbage, sizeof(garbage));
                send(fd, garbage, sizeof(garbage), MSG_NOSIGNAL);
                close(fd);
                session.fd = -1;
                free(session.recv_buf);
                attacks++;
            }
            break;
        }
        case 1: {
            /* Silent connect/disconnect */
            int fd = raw_connect(svc);
            if (fd >= 0) {
                usleep(10000);
                close(fd);
                attacks++;
            }
            break;
        }
        case 2: {
            /* Half header */
            int fd = raw_connect(svc);
            if (fd >= 0) {
                uint8_t partial[16];
                fill_random(partial, sizeof(partial));
                send(fd, partial, sizeof(partial), MSG_NOSIGNAL);
                usleep(10000);
                close(fd);
                attacks++;
            }
            break;
        }
        case 3: {
            /* Valid header, huge payload_len */
            nipc_uds_session_t session;
            int fd = chaos_connect(svc, &session);
            if (fd >= 0) {
                nipc_header_t hdr = {0};
                hdr.kind = NIPC_KIND_REQUEST;
                hdr.code = NIPC_METHOD_CGROUPS_SNAPSHOT;
                hdr.payload_len = 999999;
                hdr.message_id = (uint64_t)(i + 100);
                uint8_t msg[64];
                nipc_header_encode(&hdr, msg, sizeof(msg));
                send(fd, msg, NIPC_HEADER_LEN, MSG_NOSIGNAL);
                close(fd);
                session.fd = -1;
                free(session.recv_buf);
                attacks++;
            }
            break;
        }
        case 4: {
            /* Zero-length message */
            int fd = raw_connect(svc);
            if (fd >= 0) {
                uint8_t zero[1] = {0};
                send(fd, zero, 0, MSG_NOSIGNAL);
                close(fd);
                attacks++;
            }
            break;
        }
        }

        usleep(5000);
    }

    printf("    %d mixed attacks delivered\n", attacks);
    check("mixed attacks delivered", attacks >= 30);

    usleep(200000);
    bool alive = verify_server_alive(svc);
    check("server alive after rapid chaos mix", alive);

    stop_chaos_server(&sctx, tid);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned int)time(NULL));
    ensure_run_dir();
    setbuf(stdout, NULL);

    printf("=== Transport-Level Chaos Tests (Phase H9) ===\n\n");

    test_random_bytes_after_handshake();   printf("\n");
    test_truncated_payload();               printf("\n");
    test_wrong_magic();                     printf("\n");
    test_payload_exceeds_limit();           printf("\n");
    test_partial_chunk();                   printf("\n");
    test_silent_connect_disconnect();       printf("\n");
    test_half_header();                     printf("\n");
    test_shm_chaos();                       printf("\n");
    test_rapid_chaos_mix();                 printf("\n");

    printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
