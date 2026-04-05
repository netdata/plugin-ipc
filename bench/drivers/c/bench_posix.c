/*
 * bench_posix.c - POSIX benchmark driver for netipc.
 *
 * Exercises the public L1/L2/L3 API surface. Measures throughput,
 * latency (p50/p95/p99), and CPU for ping-pong, snapshot, and lookup
 * scenarios.
 *
 * Subcommands:
 *   uds-ping-pong-server   <run_dir> <service> [duration_sec]
 *   uds-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>
 *   shm-ping-pong-server   <run_dir> <service> [duration_sec]
 *   shm-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-server         <run_dir> <service> [duration_sec]
 *   snapshot-client         <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-shm-server     <run_dir> <service> [duration_sec]
 *   snapshot-shm-client     <run_dir> <service> <duration_sec> <target_rps>
 *   lookup-bench            <duration_sec>
 *
 * target_rps=0 means maximum throughput (no rate limiting).
 *
 * Output (client): one CSV line per run:
 *   scenario,client,server,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct
 */

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define AUTH_TOKEN          0xBE4C400000C0FFEEull
#define RESPONSE_BUF_SIZE  65536
#define MAX_LATENCY_SAMPLES (10 * 1000 * 1000) /* 10M samples max */
#define DEFAULT_DURATION   30 /* seconds */

/* Profiles for SHM vs baseline */
#define BENCH_PROFILE_UDS  NIPC_PROFILE_BASELINE
#define BENCH_PROFILE_SHM  (NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_HYBRID)

/* ------------------------------------------------------------------ */
/*  Timing helpers                                                     */
/* ------------------------------------------------------------------ */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/*  Latency recording                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *samples;  /* latency in nanoseconds */
    size_t    count;
    size_t    capacity;
} latency_recorder_t;

static void latency_init(latency_recorder_t *lr, size_t cap)
{
    if (cap > MAX_LATENCY_SAMPLES)
        cap = MAX_LATENCY_SAMPLES;
    lr->samples = malloc(cap * sizeof(uint64_t));
    lr->count = 0;
    lr->capacity = cap;
}

static inline void latency_record(latency_recorder_t *lr, uint64_t ns)
{
    if (lr->count < lr->capacity)
        lr->samples[lr->count++] = ns;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static uint64_t latency_percentile(latency_recorder_t *lr, double pct)
{
    if (lr->count == 0)
        return 0;
    qsort(lr->samples, lr->count, sizeof(uint64_t), cmp_u64);
    size_t idx = (size_t)(pct / 100.0 * (double)(lr->count - 1));
    if (idx >= lr->count)
        idx = lr->count - 1;
    return lr->samples[idx];
}

static void latency_free(latency_recorder_t *lr)
{
    free(lr->samples);
    lr->samples = NULL;
    lr->count = 0;
}

/* ------------------------------------------------------------------ */
/*  Rate limiter (adaptive sleep, no busy-wait)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t target_interval_ns; /* 0 = no limit */
    uint64_t next_send_ns;
} rate_limiter_t;

static void rate_limiter_init(rate_limiter_t *rl, uint64_t target_rps)
{
    if (target_rps == 0) {
        rl->target_interval_ns = 0;
    } else {
        rl->target_interval_ns = 1000000000ull / target_rps;
    }
    rl->next_send_ns = now_ns();
}

static void rate_limiter_wait(rate_limiter_t *rl)
{
    if (rl->target_interval_ns == 0)
        return;

    uint64_t current = now_ns();
    if (current < rl->next_send_ns) {
        uint64_t wait_ns = rl->next_send_ns - current;
        struct timespec ts;
        ts.tv_sec = (time_t)(wait_ns / 1000000000ull);
        ts.tv_nsec = (long)(wait_ns % 1000000000ull);
        nanosleep(&ts, NULL);
    }
    rl->next_send_ns += rl->target_interval_ns;
}

/* ------------------------------------------------------------------ */
/*  Ping-pong handler (INCREMENT method)                               */
/*                                                                     */
/*  Request payload: 8 bytes (uint64_t counter LE)                     */
/*  Response payload: 8 bytes (counter + 1 LE)                         */
/* ------------------------------------------------------------------ */

static nipc_error_t ping_pong_handler(void *user,
                                      const nipc_header_t *request_hdr,
                                      const uint8_t *request_payload,
                                      size_t request_len,
                                      uint8_t *response_buf,
                                      size_t response_buf_size,
                                      size_t *response_len_out)
{
    (void)user;

    if (!request_hdr || request_hdr->code != NIPC_METHOD_INCREMENT)
        return NIPC_ERR_BAD_LAYOUT;

    if ((request_hdr->flags & NIPC_FLAG_BATCH) && request_hdr->item_count >= 1) {
        nipc_batch_builder_t builder;
        nipc_batch_builder_init(&builder, response_buf, response_buf_size,
                                request_hdr->item_count);

        for (uint32_t i = 0; i < request_hdr->item_count; i++) {
            const void *item_ptr;
            uint32_t item_len;
            uint64_t counter;
            uint8_t item[NIPC_INCREMENT_PAYLOAD_SIZE];
            nipc_error_t err = nipc_batch_item_get(request_payload, request_len,
                                                   request_hdr->item_count, i,
                                                   &item_ptr, &item_len);
            if (err != NIPC_OK)
                return err;
            err = nipc_increment_decode(item_ptr, item_len, &counter);
            if (err != NIPC_OK)
                return err;
            nipc_increment_encode(counter + 1, item, sizeof(item));
            err = nipc_batch_builder_add(&builder, item, sizeof(item));
            if (err != NIPC_OK)
                return err;
        }

        uint32_t out_count = 0;
        *response_len_out = nipc_batch_builder_finish(&builder, &out_count);
        return (out_count == request_hdr->item_count) ? NIPC_OK : NIPC_ERR_BAD_LAYOUT;
    }

    if (response_buf_size < NIPC_INCREMENT_PAYLOAD_SIZE)
        return NIPC_ERR_OVERFLOW;

    uint64_t counter;
    nipc_error_t err = nipc_increment_decode(request_payload, request_len, &counter);
    if (err != NIPC_OK)
        return err;

    nipc_increment_encode(counter + 1, response_buf, NIPC_INCREMENT_PAYLOAD_SIZE);
    *response_len_out = NIPC_INCREMENT_PAYLOAD_SIZE;
    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Snapshot handler (16 cgroup items)                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t hash;
    uint32_t options;
    uint32_t enabled;
    uint32_t name_len;
    uint32_t path_len;
    char name[32];
    char path[96];
} snapshot_template_item_t;

static snapshot_template_item_t g_snapshot_template[16];
static pthread_once_t g_snapshot_template_once = PTHREAD_ONCE_INIT;

static void snapshot_template_init(void)
{
    for (int i = 0; i < 16; i++) {
        snapshot_template_item_t *item = &g_snapshot_template[i];

        item->hash = (uint32_t)(1000 + i);
        item->options = 0;
        item->enabled = (uint32_t)(i % 2);
        item->name_len = (uint32_t)snprintf(
            item->name, sizeof(item->name), "cgroup-%d", i);
        item->path_len = (uint32_t)snprintf(
            item->path, sizeof(item->path), "/sys/fs/cgroup/bench/cg-%d", i);
    }
}

static nipc_error_t snapshot_handler(void *user,
                                     const nipc_header_t *request_hdr,
                                     const uint8_t *request_payload,
                                     size_t request_len,
                                     uint8_t *response_buf,
                                     size_t response_buf_size,
                                     size_t *response_len_out)
{
    (void)user;

    if (!request_hdr || request_hdr->code != NIPC_METHOD_CGROUPS_SNAPSHOT)
        return NIPC_ERR_BAD_LAYOUT;
    if ((request_hdr->flags & NIPC_FLAG_BATCH) || request_hdr->item_count != 1)
        return NIPC_ERR_BAD_LAYOUT;

    nipc_cgroups_req_t req;
    if (nipc_cgroups_req_decode(request_payload, request_len, &req) != NIPC_OK)
        return NIPC_ERR_BAD_LAYOUT;

    pthread_once(&g_snapshot_template_once, snapshot_template_init);

    static uint64_t gen = 0;
    gen++;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               16, 1, gen);

    for (int i = 0; i < 16; i++) {
        const snapshot_template_item_t *item = &g_snapshot_template[i];

        nipc_error_t err = nipc_cgroups_builder_add(
            &builder,
            item->hash,
            item->options,
            item->enabled,
            item->name, item->name_len,
            item->path, item->path_len);

        if (err != NIPC_OK)
            return err;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

/* ------------------------------------------------------------------ */
/*  Server main loop (runs until duration expires or signal)            */
/* ------------------------------------------------------------------ */

/* Global server pointer for signal-driven shutdown */
static nipc_managed_server_t *g_server = NULL;

static void sighandler(int sig)
{
    (void)sig;
    if (g_server)
        nipc_server_stop(g_server);
}

/* Timer thread: stops server after duration_sec */
static void *timer_thread(void *arg)
{
    int duration_sec = *(int *)arg;
    sleep(duration_sec + 3);
    if (g_server)
        nipc_server_stop(g_server);
    return NULL;
}

static int run_server(const char *run_dir, const char *service,
                      uint32_t profiles, int duration_sec,
                      uint16_t expected_method_code,
                      nipc_server_handler_fn handler)
{
    nipc_uds_server_config_t scfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 4,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init(&server, run_dir, service, &scfg,
                                        1, expected_method_code,
                                        handler, NULL);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return 1;
    }

    g_server = &server;

    /* Print READY, then print CPU when done */
    printf("READY\n");
    fflush(stdout);

    uint64_t cpu_start = cpu_ns();

    /* Signal-driven shutdown */
    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);

    /* Timer-driven shutdown after duration */
    pthread_t timer_tid = 0;
    if (duration_sec > 0) {
        pthread_create(&timer_tid, NULL, timer_thread, &duration_sec);
    }

    /* Blocking: runs until nipc_server_stop() is called */
    nipc_server_run(&server);

    if (timer_tid)
        pthread_join(timer_tid, NULL);

    uint64_t cpu_end = cpu_ns();
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;

    /* Print server CPU for the harness to parse */
    printf("SERVER_CPU_SEC=%.6f\n", cpu_sec);
    fflush(stdout);

    g_server = NULL;
    nipc_server_destroy(&server);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Batch server (same handler, higher batch limits)                   */
/* ------------------------------------------------------------------ */

#define BENCH_MAX_BATCH_ITEMS 1000
/* Max batch payload: 1000 items × (8-byte dir entry + 8-byte aligned payload) + alignment.
 * Server uses resp_buf_size/2 for builder and /2 for item scratch.
 * Must be large enough that half exceeds worst-case batch output. */
#define BENCH_BATCH_BUF_SIZE  (BENCH_MAX_BATCH_ITEMS * 48 + 4096)

static int run_server_batch(const char *run_dir, const char *service,
                            uint32_t profiles, int duration_sec,
                            uint16_t expected_method_code,
                            nipc_server_handler_fn handler)
{
    nipc_uds_server_config_t scfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_request_batch_items   = BENCH_MAX_BATCH_ITEMS,
        .max_response_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_response_batch_items  = BENCH_MAX_BATCH_ITEMS,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
        .backlog                   = 4,
    };

    nipc_managed_server_t server;
    nipc_error_t err = nipc_server_init(&server, run_dir, service, &scfg,
                                        4, expected_method_code,
                                        handler, NULL);
    if (err != NIPC_OK) {
        fprintf(stderr, "batch server init failed: %d\n", err);
        return 1;
    }

    g_server = &server;

    printf("READY\n");
    fflush(stdout);

    uint64_t cpu_start = cpu_ns();

    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);

    pthread_t timer_tid = 0;
    if (duration_sec > 0) {
        pthread_create(&timer_tid, NULL, timer_thread, &duration_sec);
    }

    nipc_server_run(&server);

    if (timer_tid)
        pthread_join(timer_tid, NULL);

    uint64_t cpu_end = cpu_ns();
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;

    printf("SERVER_CPU_SEC=%.6f\n", cpu_sec);
    fflush(stdout);

    g_server = NULL;
    nipc_server_destroy(&server);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Batch ping-pong client (random 2-1000 items per batch)             */
/* ------------------------------------------------------------------ */

static uint32_t bench_rand_state = 12345;

static uint32_t bench_rand(void)
{
    bench_rand_state ^= bench_rand_state << 13;
    bench_rand_state ^= bench_rand_state >> 17;
    bench_rand_state ^= bench_rand_state << 5;
    return bench_rand_state;
}

static int run_batch_ping_pong_client(const char *run_dir, const char *service,
                                       uint32_t profiles, int duration_sec,
                                       uint64_t target_rps,
                                       const char *scenario, const char *lang)
{
    nipc_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_request_batch_items   = BENCH_MAX_BATCH_ITEMS,
        .max_response_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_response_batch_items  = BENCH_MAX_BATCH_ITEMS,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);

    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(10000);
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "batch client: not ready after retries\n");
        return 1;
    }

    latency_recorder_t lr;
    size_t est_samples = (target_rps == 0) ? 2000000 :
                         (size_t)(target_rps * (uint64_t)duration_sec);
    latency_init(&lr, est_samples);

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t counter = 0;
    uint64_t total_items = 0;
    uint64_t errors = 0;

    /* Pre-allocate buffers for batch building */
    uint8_t *req_buf = malloc(BENCH_BATCH_BUF_SIZE);
    uint8_t *resp_buf = malloc(BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN);
    uint64_t *expected = malloc(BENCH_MAX_BATCH_ITEMS * sizeof(uint64_t));
    if (!req_buf || !resp_buf || !expected) {
        fprintf(stderr, "batch client: malloc failed\n");
        free(req_buf); free(resp_buf); free(expected);
        return 1;
    }

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
        rate_limiter_wait(&rl);

        /* Random batch size 2-1000. item_count==1 is normalized to the
         * single-item increment path by the server, so it is not a real
         * batch round trip. */
        uint32_t batch_size = (bench_rand() % (BENCH_MAX_BATCH_ITEMS - 1)) + 2;

        /* Build batch request */
        nipc_batch_builder_t bb;
        nipc_batch_builder_init(&bb, req_buf, BENCH_BATCH_BUF_SIZE, batch_size);

        for (uint32_t i = 0; i < batch_size; i++) {
            uint8_t item[NIPC_INCREMENT_PAYLOAD_SIZE];
            uint64_t val = counter + i;
            nipc_increment_encode(val, item, sizeof(item));
            expected[i] = val + 1;

            nipc_error_t berr = nipc_batch_builder_add(&bb, item, sizeof(item));
            if (berr != NIPC_OK) {
                errors++;
                break;
            }
        }

        uint32_t out_count;
        size_t req_len = nipc_batch_builder_finish(&bb, &out_count);

        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_INCREMENT;
        hdr.flags = NIPC_FLAG_BATCH;
        hdr.item_count = batch_size;
        hdr.message_id = counter + 1;
        hdr.transport_status = NIPC_STATUS_OK;

        uint64_t t0 = now_ns();

        /* Send + receive via L1 transport.
         * Any error after send desynchronizes the stream — break out
         * of the benchmark loop (reconnection is not worth it for a bench). */
        nipc_uds_error_t uerr;
        nipc_header_t resp_hdr;
        const void *resp_payload;
        size_t resp_len;
        int fatal = 0;

        if (client.shm) {
            /* SHM path: manual header+payload assembly */
            size_t msg_len = NIPC_HEADER_LEN + req_len;
            uint8_t *msg = malloc(msg_len);
            if (!msg) { errors++; break; }

            hdr.magic = NIPC_MAGIC_MSG;
            hdr.version = NIPC_VERSION;
            hdr.header_len = NIPC_HEADER_LEN;
            hdr.payload_len = (uint32_t)req_len;
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
            memcpy(msg + NIPC_HEADER_LEN, req_buf, req_len);

            nipc_shm_error_t serr = nipc_shm_send(client.shm, msg, msg_len);
            free(msg);
            if (serr != NIPC_SHM_OK) { errors++; break; }

            size_t shm_resp_len;
            serr = nipc_shm_receive(client.shm, resp_buf,
                                    BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                                    &shm_resp_len, 30000);
            if (serr != NIPC_SHM_OK) { errors++; fatal = 1; break; }

            if (shm_resp_len < NIPC_HEADER_LEN) { errors++; fatal = 1; break; }
            nipc_header_decode(resp_buf, NIPC_HEADER_LEN, &resp_hdr);
            resp_payload = resp_buf + NIPC_HEADER_LEN;
            resp_len = shm_resp_len - NIPC_HEADER_LEN;
        } else {
            /* UDS path */
            uerr = nipc_uds_send(&client.session, &hdr, req_buf, req_len);
            if (uerr != NIPC_UDS_OK) { errors++; break; }

            uerr = nipc_uds_receive(&client.session, resp_buf,
                                    BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                                    &resp_hdr, &resp_payload, &resp_len);
            if (uerr != NIPC_UDS_OK) { errors++; fatal = 1; break; }
        }

        /* Validate response — any mismatch is a protocol desync, stop */
        if (resp_hdr.kind != NIPC_KIND_RESPONSE ||
            resp_hdr.code != NIPC_METHOD_INCREMENT ||
            resp_hdr.transport_status != NIPC_STATUS_OK ||
            resp_hdr.item_count != batch_size) {
            fprintf(stderr, "batch: response mismatch kind=%u code=%u status=%u items=%u (expected %u)\n",
                    resp_hdr.kind, resp_hdr.code, resp_hdr.transport_status,
                    resp_hdr.item_count, batch_size);
            errors++;
            fatal = 1;
            break;
        }

        /* Verify each item */
        for (uint32_t i = 0; i < batch_size; i++) {
            const void *item_ptr;
            uint32_t item_len;
            nipc_error_t gerr = nipc_batch_item_get(resp_payload, resp_len,
                                                      batch_size, i,
                                                      &item_ptr, &item_len);
            if (gerr != NIPC_OK) {
                fprintf(stderr, "batch: item_get failed at %u/%u\n", i, batch_size);
                errors++;
                fatal = 1;
                break;
            }

            uint64_t resp_val;
            if (nipc_increment_decode(item_ptr, item_len, &resp_val) != NIPC_OK) {
                fprintf(stderr, "batch: decode failed at %u/%u\n", i, batch_size);
                errors++;
                fatal = 1;
                break;
            }
            if (resp_val != expected[i]) {
                fprintf(stderr, "batch: value mismatch at %u/%u: expected %lu got %lu\n",
                        i, batch_size, (unsigned long)expected[i], (unsigned long)resp_val);
                errors++;
                fatal = 1;
                break;
            }
        }

        if (fatal)
            break;

        {
            uint64_t t1 = now_ns();
            latency_record(&lr, t1 - t0);
        }

        counter += batch_size;
        total_items += batch_size;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)total_items / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
    uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
    uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

    printf("%s,%s,%s,%.0f,%lu,%lu,%lu,%.1f,0.0,%.1f\n",
           scenario, lang, lang,
           throughput,
           (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors > 0)
        fprintf(stderr, "batch client: %lu errors out of %lu items\n",
                (unsigned long)errors, (unsigned long)total_items);

    free(req_buf);
    free(resp_buf);
    free(expected);
    latency_free(&lr);
    nipc_client_close(&client);
    return (errors > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Ping-pong client                                                   */
/* ------------------------------------------------------------------ */

static int run_ping_pong_client(const char *run_dir, const char *service,
                                 uint32_t profiles, int duration_sec,
                                 uint64_t target_rps,
                                 const char *scenario, const char *lang)
{
    nipc_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);

    /* Connect with retry */
    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(10000); /* 10ms */
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "client: not ready after retries\n");
        return 1;
    }

    latency_recorder_t lr;
    size_t est_samples = (target_rps == 0) ? 5000000 :
                         (size_t)(target_rps * (uint64_t)duration_sec);
    latency_init(&lr, est_samples);

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t counter = 0;
    uint64_t requests = 0;
    uint64_t errors = 0;

    nipc_shm_ctx_t *shm = client.shm;

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
        rate_limiter_wait(&rl);

        /* Build INCREMENT request */
        uint8_t req_payload[8];
        memcpy(req_payload, &counter, 8);

        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_INCREMENT;
        hdr.flags = 0;
        hdr.item_count = 1;
        hdr.message_id = counter + 1;
        hdr.transport_status = NIPC_STATUS_OK;
        hdr.payload_len = 8;

        /* Send + receive via L1 transport since L2 doesn't expose INCREMENT */
        uint64_t t0 = now_ns();

        if (shm) {
            /* SHM path */
            size_t msg_len = NIPC_HEADER_LEN + 8;
            uint8_t msg[NIPC_HEADER_LEN + 8];

            hdr.magic = NIPC_MAGIC_MSG;
            hdr.version = NIPC_VERSION;
            hdr.header_len = NIPC_HEADER_LEN;
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
            memcpy(msg + NIPC_HEADER_LEN, req_payload, 8);

            nipc_shm_error_t serr = nipc_shm_send(shm, msg, msg_len);
            if (serr != NIPC_SHM_OK) {
                errors++;
                continue;
            }

            uint8_t resp_msg[NIPC_HEADER_LEN + 64];
            size_t resp_len;
            serr = nipc_shm_receive(shm, resp_msg, sizeof(resp_msg),
                                      &resp_len, 30000);
            if (serr != NIPC_SHM_OK) {
                errors++;
                continue;
            }

            if (resp_len >= NIPC_HEADER_LEN + 8) {
                uint64_t resp_val;
                memcpy(&resp_val, resp_msg + NIPC_HEADER_LEN, 8);
                if (resp_val != counter + 1) {
                    fprintf(stderr, "counter chain broken: expected %lu, got %lu\n",
                            (unsigned long)(counter + 1), (unsigned long)resp_val);
                    errors++;
                }
            }
        } else {
            /* UDS path */
            nipc_uds_error_t uerr = nipc_uds_send(&client.session, &hdr,
                                                     req_payload, 8);
            if (uerr != NIPC_UDS_OK) {
                errors++;
                continue;
            }

            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_len;
            uint8_t recv_buf[256];

            uerr = nipc_uds_receive(&client.session, recv_buf, sizeof(recv_buf),
                                     &resp_hdr, &resp_payload, &resp_len);
            if (uerr != NIPC_UDS_OK) {
                errors++;
                continue;
            }

            if (resp_len >= 8) {
                uint64_t resp_val;
                memcpy(&resp_val, resp_payload, 8);
                if (resp_val != counter + 1) {
                    fprintf(stderr, "counter chain broken: expected %lu, got %lu\n",
                            (unsigned long)(counter + 1), (unsigned long)resp_val);
                    errors++;
                }
            }
        }

        uint64_t t1 = now_ns();
        latency_record(&lr, t1 - t0);

        counter++;
        requests++;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)requests / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    uint64_t p50 = latency_percentile(&lr, 50.0) / 1000; /* ns -> us */
    uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
    uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

    /* Output CSV line (server CPU filled later by the harness) */
    printf("%s,c,%s,%.0f,%lu,%lu,%lu,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors > 0)
        fprintf(stderr, "client: %lu errors\n", (unsigned long)errors);

    latency_free(&lr);
    nipc_client_close(&client);
    return (errors > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Snapshot client (L2 typed call)                                     */
/* ------------------------------------------------------------------ */

static int run_snapshot_client(const char *run_dir, const char *service,
                                uint32_t profiles, int duration_sec,
                                uint64_t target_rps,
                                const char *scenario, const char *lang)
{
    nipc_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_client_ctx_t client;
    nipc_client_init(&client, run_dir, service, &ccfg);

    /* Connect with retry */
    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        usleep(10000);
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "client: not ready after retries\n");
        return 1;
    }

    latency_recorder_t lr;
    size_t est_samples = (target_rps == 0) ? 5000000 :
                         (size_t)(target_rps * (uint64_t)duration_sec);
    latency_init(&lr, est_samples);

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t requests = 0;
    uint64_t errors = 0;
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
        rate_limiter_wait(&rl);

        uint64_t t0 = now_ns();

        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

        uint64_t t1 = now_ns();

        if (err != NIPC_OK) {
            errors++;
            /* Try reconnect */
            nipc_client_refresh(&client);
            continue;
        }

        /* Verify item count */
        if (view.item_count != 16) {
            fprintf(stderr, "snapshot: expected 16 items, got %u\n",
                    view.item_count);
            errors++;
        }

        latency_record(&lr, t1 - t0);
        requests++;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)requests / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
    uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
    uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

    printf("%s,c,%s,%.0f,%lu,%lu,%lu,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors > 0)
        fprintf(stderr, "client: %lu errors\n", (unsigned long)errors);

    latency_free(&lr);
    nipc_client_close(&client);
    return (errors > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Lookup benchmark (L3 cache, no transport)                          */
/* ------------------------------------------------------------------ */

static int run_lookup_bench(int duration_sec)
{
    /* Build a synthetic cache with 16 items */
    nipc_cgroups_cache_item_t items[16];
    for (int i = 0; i < 16; i++) {
        items[i].hash = (uint32_t)(1000 + i);
        items[i].options = 0;
        items[i].enabled = (uint32_t)(i % 2);
        char name[64], path[128];
        snprintf(name, sizeof(name), "cgroup-%d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/bench/cg-%d", i);
        items[i].name = strdup(name);
        items[i].path = strdup(path);
    }

    nipc_cgroups_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.items = items;
    cache.item_count = 16;
    cache.populated = true;
    cache.systemd_enabled = 1;
    cache.generation = 1;

    uint64_t lookups = 0;
    uint64_t hits = 0;

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
        /* Cycle through all 16 items */
        for (int i = 0; i < 16; i++) {
            const nipc_cgroups_cache_item_t *found =
                nipc_cgroups_cache_lookup(&cache, items[i].hash, items[i].name);
            if (found)
                hits++;
            lookups++;
        }
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)lookups / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    printf("lookup,c,c,%.0f,0,0,0,%.1f,0.0,%.1f\n",
           throughput, cpu_pct, cpu_pct);
    fflush(stdout);

    if (hits != lookups) {
        fprintf(stderr, "lookup: missed %lu/%lu\n",
                (unsigned long)(lookups - hits), (unsigned long)lookups);
    }

    /* Cleanup */
    for (int i = 0; i < 16; i++) {
        free(items[i].name);
        free(items[i].path);
    }

    return (hits == lookups) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Usage and main                                                     */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s uds-ping-pong-server   <run_dir> <service> [duration_sec]\n"
        "  %s uds-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s shm-ping-pong-server   <run_dir> <service> [duration_sec]\n"
        "  %s shm-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-server         <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-client         <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-shm-server     <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-shm-client     <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s uds-pipeline-client    <run_dir> <service> <duration_sec> <target_rps> <depth>\n"
        "  %s lookup-bench            <duration_sec>\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    /* Server subcommands: <run_dir> <service> [duration_sec] */
    if (strcmp(cmd, "uds-ping-pong-server") == 0 ||
        strcmp(cmd, "shm-ping-pong-server") == 0 ||
        strcmp(cmd, "snapshot-server") == 0 ||
        strcmp(cmd, "snapshot-shm-server") == 0) {

        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = (argc >= 5) ? atoi(argv[4]) : DEFAULT_DURATION;

        mkdir(run_dir, 0700);

        uint32_t profiles;
        uint16_t expected_method_code;
        nipc_server_handler_fn handler;

        if (strcmp(cmd, "uds-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_UDS;
            expected_method_code = NIPC_METHOD_INCREMENT;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "shm-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_SHM;
            expected_method_code = NIPC_METHOD_INCREMENT;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "snapshot-server") == 0) {
            profiles = BENCH_PROFILE_UDS;
            expected_method_code = NIPC_METHOD_CGROUPS_SNAPSHOT;
            handler = snapshot_handler;
        } else { /* snapshot-shm-server */
            profiles = BENCH_PROFILE_SHM;
            expected_method_code = NIPC_METHOD_CGROUPS_SNAPSHOT;
            handler = snapshot_handler;
        }

        return run_server(run_dir, service, profiles, duration,
                          expected_method_code, handler);
    }

    /* Client subcommands: <run_dir> <service> <duration_sec> <target_rps> */
    if (strcmp(cmd, "uds-ping-pong-client") == 0 ||
        strcmp(cmd, "shm-ping-pong-client") == 0) {

        if (argc < 6) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = atoi(argv[4]);
        uint64_t target_rps = (uint64_t)strtoull(argv[5], NULL, 10);

        uint32_t profiles;
        const char *scenario;

        if (strcmp(cmd, "uds-ping-pong-client") == 0) {
            profiles = BENCH_PROFILE_UDS;
            scenario = "uds-ping-pong";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "shm-ping-pong";
        }

        /* Server lang is passed via argv[3] service name suffix, or
         * the harness fills it in. We output 'c' as server placeholder. */
        return run_ping_pong_client(run_dir, service, profiles,
                                     duration, target_rps,
                                     scenario, "c");
    }

    if (strcmp(cmd, "snapshot-client") == 0 ||
        strcmp(cmd, "snapshot-shm-client") == 0) {

        if (argc < 6) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = atoi(argv[4]);
        uint64_t target_rps = (uint64_t)strtoull(argv[5], NULL, 10);

        uint32_t profiles;
        const char *scenario;

        if (strcmp(cmd, "snapshot-client") == 0) {
            profiles = BENCH_PROFILE_UDS;
            scenario = "snapshot-baseline";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "snapshot-shm";
        }

        return run_snapshot_client(run_dir, service, profiles,
                                    duration, target_rps,
                                    scenario, "c");
    }

    /* Batch server subcommands */
    if (strcmp(cmd, "uds-batch-ping-pong-server") == 0 ||
        strcmp(cmd, "shm-batch-ping-pong-server") == 0) {

        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = (argc >= 5) ? atoi(argv[4]) : DEFAULT_DURATION;

        mkdir(run_dir, 0700);

        uint32_t profiles;
        if (strcmp(cmd, "uds-batch-ping-pong-server") == 0)
            profiles = BENCH_PROFILE_UDS;
        else
            profiles = BENCH_PROFILE_SHM;

        return run_server_batch(run_dir, service, profiles, duration,
                                NIPC_METHOD_INCREMENT,
                                ping_pong_handler);
    }

    /* Batch client subcommands */
    if (strcmp(cmd, "uds-batch-ping-pong-client") == 0 ||
        strcmp(cmd, "shm-batch-ping-pong-client") == 0) {

        if (argc < 6) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = atoi(argv[4]);
        uint64_t target_rps = (uint64_t)strtoull(argv[5], NULL, 10);

        uint32_t profiles;
        const char *scenario;

        if (strcmp(cmd, "uds-batch-ping-pong-client") == 0) {
            profiles = BENCH_PROFILE_UDS;
            scenario = "uds-batch-ping-pong";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "shm-batch-ping-pong";
        }

        return run_batch_ping_pong_client(run_dir, service, profiles,
                                           duration, target_rps,
                                           scenario, "c");
    }

    /* Pipeline+batch client */
    if (strcmp(cmd, "uds-pipeline-batch-client") == 0) {
        if (argc < 7) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = atoi(argv[4]);
        uint64_t target_rps = (uint64_t)strtoull(argv[5], NULL, 10);
        int depth = atoi(argv[6]);
        if (depth < 1) depth = 1;

        /* Pipeline+batch: each pipelined message is a random 2-1000 item
         * batch. item_count==1 is normalized to the single-item path.
         * Use the batch server (higher limits). The pipeline client
         * sends `depth` batch messages, receives `depth` batch responses. */
        nipc_client_config_t ccfg = {
            .supported_profiles        = BENCH_PROFILE_UDS,
            .preferred_profiles        = BENCH_PROFILE_UDS,
            .max_request_payload_bytes = BENCH_BATCH_BUF_SIZE,
            .max_request_batch_items   = BENCH_MAX_BATCH_ITEMS,
            .max_response_payload_bytes = BENCH_BATCH_BUF_SIZE,
            .max_response_batch_items  = BENCH_MAX_BATCH_ITEMS,
            .auth_token                = AUTH_TOKEN,
        };

        nipc_client_ctx_t client;
        nipc_client_init(&client, run_dir, service, &ccfg);

        for (int i = 0; i < 200; i++) {
            nipc_client_refresh(&client);
            if (nipc_client_ready(&client))
                break;
            usleep(10000);
        }

        if (!nipc_client_ready(&client)) {
            fprintf(stderr, "pipeline-batch client: not ready\n");
            return 1;
        }

        latency_recorder_t lr;
        latency_init(&lr, 2000000);
        rate_limiter_t rl;
        rate_limiter_init(&rl, target_rps);

        uint64_t counter = 0;
        uint64_t total_items = 0;
        uint64_t errors = 0;

        uint8_t *req_bufs[128];
        size_t req_lens[128];
        uint32_t batch_sizes[128];
        nipc_header_t hdrs[128];

        for (int i = 0; i < depth && i < 128; i++)
            req_bufs[i] = malloc(BENCH_BATCH_BUF_SIZE);

        uint64_t cpu_start = cpu_ns();
        uint64_t wall_start = now_ns();
        uint64_t wall_end = wall_start + (uint64_t)duration * 1000000000ull;

        while (now_ns() < wall_end) {
            rate_limiter_wait(&rl);
            uint64_t t0 = now_ns();

            /* Build and send `depth` batch requests */
            int send_ok = 1;
            for (int d = 0; d < depth; d++) {
                uint32_t bs = (bench_rand() % (BENCH_MAX_BATCH_ITEMS - 1)) + 2;
                batch_sizes[d] = bs;

                nipc_batch_builder_t bb;
                nipc_batch_builder_init(&bb, req_bufs[d], BENCH_BATCH_BUF_SIZE, bs);

                for (uint32_t i = 0; i < bs; i++) {
                    uint8_t item[NIPC_INCREMENT_PAYLOAD_SIZE];
                    nipc_increment_encode(counter + i, item, sizeof(item));
                    nipc_batch_builder_add(&bb, item, sizeof(item));
                }

                uint32_t out_count;
                req_lens[d] = nipc_batch_builder_finish(&bb, &out_count);

                hdrs[d] = (nipc_header_t){0};
                hdrs[d].kind = NIPC_KIND_REQUEST;
                hdrs[d].code = NIPC_METHOD_INCREMENT;
                hdrs[d].flags = NIPC_FLAG_BATCH;
                hdrs[d].item_count = bs;
                hdrs[d].message_id = counter + 1 + (uint64_t)d;
                hdrs[d].transport_status = NIPC_STATUS_OK;

                nipc_uds_error_t uerr = nipc_uds_send(&client.session,
                    &hdrs[d], req_bufs[d], req_lens[d]);
                if (uerr != NIPC_UDS_OK) { send_ok = 0; errors++; break; }

                counter += bs;
            }

            if (!send_ok) continue;

            /* Receive `depth` batch responses */
            for (int d = 0; d < depth; d++) {
                nipc_header_t resp_hdr;
                const void *resp_payload;
                size_t resp_len;
                uint8_t *recv_buf = req_bufs[d]; /* reuse buffer */

                nipc_uds_error_t uerr = nipc_uds_receive(&client.session,
                    recv_buf, BENCH_BATCH_BUF_SIZE,
                    &resp_hdr, &resp_payload, &resp_len);
                if (uerr != NIPC_UDS_OK) { errors++; break; }

                total_items += batch_sizes[d];
            }

            uint64_t t1 = now_ns();
            latency_record(&lr, t1 - t0);
        }

        uint64_t cpu_end = cpu_ns();
        uint64_t wall_actual = now_ns() - wall_start;
        double wall_sec = (double)wall_actual / 1e9;
        double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
        double throughput = (double)total_items / wall_sec;
        double cpu_pct = (cpu_sec / wall_sec) * 100.0;

        uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
        uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
        uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

        char scenario[64];
        snprintf(scenario, sizeof(scenario), "uds-pipeline-batch-d%d", depth);
        printf("%s,c,c,%.0f,%lu,%lu,%lu,%.1f,0.0,%.1f\n",
               scenario, throughput,
               (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
               cpu_pct, cpu_pct);
        fflush(stdout);

        for (int i = 0; i < depth && i < 128; i++)
            free(req_bufs[i]);
        latency_free(&lr);
        nipc_client_close(&client);
        return (errors > 0) ? 1 : 0;
    }

    if (strcmp(cmd, "lookup-bench") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        int duration = atoi(argv[2]);
        return run_lookup_bench(duration);
    }

    /* Pipeline client: <run_dir> <service> <duration_sec> <target_rps> <depth> */
    if (strcmp(cmd, "uds-pipeline-client") == 0) {
        if (argc < 7) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = atoi(argv[4]);
        uint64_t target_rps = (uint64_t)strtoull(argv[5], NULL, 10);
        int depth = atoi(argv[6]);

        if (depth < 1)
            depth = 1;

        nipc_client_config_t ccfg = {
            .supported_profiles        = BENCH_PROFILE_UDS,
            .preferred_profiles        = BENCH_PROFILE_UDS,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items   = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 1,
            .auth_token                = AUTH_TOKEN,
        };

        nipc_client_ctx_t client;
        nipc_client_init(&client, run_dir, service, &ccfg);

        /* Connect with retry */
        for (int i = 0; i < 200; i++) {
            nipc_client_refresh(&client);
            if (nipc_client_ready(&client))
                break;
            usleep(10000);
        }

        if (!nipc_client_ready(&client)) {
            fprintf(stderr, "pipeline client: not ready after retries\n");
            return 1;
        }

        latency_recorder_t lr;
        size_t est_samples = (target_rps == 0) ? 5000000 :
                             (size_t)(target_rps * (uint64_t)duration);
        latency_init(&lr, est_samples);

        rate_limiter_t rl;
        rate_limiter_init(&rl, target_rps);

        uint64_t counter = 0;
        uint64_t requests = 0;
        uint64_t errors = 0;

        uint64_t cpu_start = cpu_ns();
        uint64_t wall_start = now_ns();
        uint64_t wall_end = wall_start + (uint64_t)duration * 1000000000ull;

        while (now_ns() < wall_end) {
            rate_limiter_wait(&rl);

            uint64_t t0 = now_ns();

            /* Send `depth` requests */
            int send_ok = 1;
            for (int d = 0; d < depth; d++) {
                uint64_t val = counter + (uint64_t)d;
                uint8_t req_payload[8];
                memcpy(req_payload, &val, 8);

                nipc_header_t hdr = {0};
                hdr.kind = NIPC_KIND_REQUEST;
                hdr.code = NIPC_METHOD_INCREMENT;
                hdr.item_count = 1;
                hdr.message_id = val + 1;
                hdr.transport_status = NIPC_STATUS_OK;
                hdr.payload_len = 8;

                nipc_uds_error_t uerr = nipc_uds_send(&client.session, &hdr,
                                                         req_payload, 8);
                if (uerr != NIPC_UDS_OK) {
                    send_ok = 0;
                    errors++;
                    break;
                }
            }

            if (!send_ok)
                continue;

            /* Receive `depth` responses */
            int recv_ok = 1;
            for (int d = 0; d < depth; d++) {
                nipc_header_t resp_hdr;
                const void *resp_payload;
                size_t resp_len;
                uint8_t recv_buf[256];

                nipc_uds_error_t uerr = nipc_uds_receive(&client.session,
                    recv_buf, sizeof(recv_buf),
                    &resp_hdr, &resp_payload, &resp_len);
                if (uerr != NIPC_UDS_OK) {
                    recv_ok = 0;
                    errors++;
                    break;
                }

                if (resp_len >= 8) {
                    uint64_t resp_val;
                    memcpy(&resp_val, resp_payload, 8);
                    uint64_t expected = counter + (uint64_t)d + 1;
                    if (resp_val != expected) {
                        fprintf(stderr, "pipeline chain broken at depth %d: expected %lu, got %lu\n",
                                d, (unsigned long)expected, (unsigned long)resp_val);
                        errors++;
                    }
                }
            }

            uint64_t t1 = now_ns();
            /* Record latency for the full batch round-trip */
            latency_record(&lr, t1 - t0);

            counter += (uint64_t)depth;
            requests += (uint64_t)depth;
        }

        uint64_t cpu_end = cpu_ns();
        uint64_t wall_actual = now_ns() - wall_start;

        double wall_sec = (double)wall_actual / 1e9;
        double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
        double throughput = (double)requests / wall_sec;
        double cpu_pct = (cpu_sec / wall_sec) * 100.0;

        uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
        uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
        uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

        char scenario[64];
        snprintf(scenario, sizeof(scenario), "uds-pipeline-d%d", depth);

        printf("%s,c,c,%.0f,%lu,%lu,%lu,%.1f,0.0,%.1f\n",
               scenario,
               throughput,
               (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
               cpu_pct, cpu_pct);
        fflush(stdout);

        if (errors > 0)
            fprintf(stderr, "pipeline client: %lu errors\n", (unsigned long)errors);

        latency_free(&lr);
        nipc_client_close(&client);
        return (errors > 0) ? 1 : 0;
    }

    fprintf(stderr, "Unknown subcommand: %s\n", cmd);
    usage(argv[0]);
    return 1;
}
