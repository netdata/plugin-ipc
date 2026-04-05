/*
 * bench_windows.c - Windows benchmark driver for netipc.
 *
 * Exercises Named Pipe and Win SHM transports. Measures throughput,
 * latency (p50/p95/p99), and CPU for ping-pong, snapshot, and lookup
 * scenarios.
 *
 * Subcommands:
 *   np-ping-pong-server           <run_dir> <service> [duration_sec]
 *   np-ping-pong-client           <run_dir> <service> <duration_sec> <target_rps>
 *   shm-ping-pong-server          <run_dir> <service> [duration_sec]
 *   shm-ping-pong-client          <run_dir> <service> <duration_sec> <target_rps>
 *   np-batch-ping-pong-server     <run_dir> <service> [duration_sec]
 *   np-batch-ping-pong-client     <run_dir> <service> <duration_sec> <target_rps>
 *   shm-batch-ping-pong-server    <run_dir> <service> [duration_sec]
 *   shm-batch-ping-pong-client    <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-server                <run_dir> <service> [duration_sec]
 *   snapshot-client                <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-shm-server            <run_dir> <service> [duration_sec]
 *   snapshot-shm-client            <run_dir> <service> <duration_sec> <target_rps>
 *   np-pipeline-client             <run_dir> <service> <duration_sec> <target_rps> <depth>
 *   np-pipeline-batch-client       <run_dir> <service> <duration_sec> <target_rps> <depth>
 *   lookup-bench                   <duration_sec>
 *
 * target_rps=0 means maximum throughput (no rate limiting).
 *
 * Output (client): one CSV line per run:
 *   scenario,client,server,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct
 */

#if defined(_WIN32) || defined(__MSYS__)

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_win_shm.h"

#include <math.h>
#include <io.h>
#include <process.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef __MSYS__
#include <unistd.h>
#ifndef _dup
#define _dup dup
#endif
#ifndef _dup2
#define _dup2 dup2
#endif
#ifndef _close
#define _close close
#endif
#ifndef _fileno
#define _fileno fileno
#endif
#ifndef _spawnv
#define _spawnv spawnv
#endif
#ifndef _P_WAIT
#define _P_WAIT P_WAIT
#endif
#ifndef _strdup
#define _strdup strdup
#endif
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define AUTH_TOKEN          0xBE4C400000C0FFEEull
#define RESPONSE_BUF_SIZE  65536
#define MAX_LATENCY_SAMPLES (500 * 1000)
#define LATENCY_SAMPLE_STRIDE 64ULL
#define LATENCY_SAMPLE_SLACK  4096ULL
#define DEFAULT_DURATION   30
#define CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS 5000ULL

/* Profiles */
#define BENCH_PROFILE_NP   NIPC_PROFILE_BASELINE
#define BENCH_PROFILE_SHM  (NIPC_PROFILE_BASELINE | NIPC_WIN_SHM_PROFILE_HYBRID)

/* Batch scenario limits (mirrors POSIX driver) */
#define BENCH_MAX_BATCH_ITEMS  1000
#define BENCH_BATCH_BUF_SIZE   (BENCH_MAX_BATCH_ITEMS * 48 + 4096)

/* ------------------------------------------------------------------ */
/*  Timing helpers (Windows)                                           */
/* ------------------------------------------------------------------ */

static LARGE_INTEGER qpc_freq;

static inline uint64_t now_ns(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (qpc_freq.QuadPart <= 0 || counter.QuadPart <= 0)
        return 0;

    uint64_t c = (uint64_t)counter.QuadPart;
    uint64_t f = (uint64_t)qpc_freq.QuadPart;
    uint64_t secs = c / f;
    uint64_t rem  = c % f;

    return secs * 1000000000ULL + (rem * 1000000000ULL) / f;
}

static inline uint64_t cpu_ns(void)
{
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    /* FILETIME is in 100ns intervals */
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (k + u) * 100;
}

static void elevate_bench_priority(void)
{
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        fprintf(stderr, "warn: SetPriorityClass(HIGH_PRIORITY_CLASS) failed: %lu\n",
                (unsigned long)GetLastError());

    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
        fprintf(stderr, "warn: SetThreadPriority(THREAD_PRIORITY_HIGHEST) failed: %lu\n",
                (unsigned long)GetLastError());
}

static int bench_affinity_slot(const char *cmd)
{
    char *slot_env = getenv("NIPC_BENCH_AFFINITY_SLOT");
    if (slot_env && *slot_env) {
        char *end = NULL;
        long parsed = strtol(slot_env, &end, 10);
        if (end && *end == '\0' && parsed >= 0)
            return (int)parsed;
    }

    if (cmd && strcmp(cmd, "lookup-bench") == 0)
        return 0;
    if (cmd && strstr(cmd, "client") != NULL)
        return 1;
    return 0;
}

static DWORD_PTR select_bench_affinity_mask(DWORD_PTR process_mask, int slot)
{
    if (!process_mask)
        return 0;
    if (slot < 0)
        slot = 0;

    DWORD_PTR available[sizeof(DWORD_PTR) * 8];
    size_t count = 0;
    for (DWORD_PTR bit = 1; bit; bit <<= 1) {
        if (process_mask & bit)
            available[count++] = bit;
    }

    if (!count)
        return 0;
    if ((size_t)slot >= count)
        slot = (int)count - 1;

    return available[count - 1 - (size_t)slot];
}

static void pin_bench_affinity(const char *cmd)
{
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        fprintf(stderr, "warn: GetProcessAffinityMask failed: %lu\n",
                (unsigned long)GetLastError());
        return;
    }

    if (!process_mask)
        return;

    DWORD_PTR pin_mask = select_bench_affinity_mask(process_mask,
                                                    bench_affinity_slot(cmd));
    if (!pin_mask)
        return;

    if (!SetProcessAffinityMask(GetCurrentProcess(), pin_mask))
        fprintf(stderr, "warn: SetProcessAffinityMask failed: %lu\n",
                (unsigned long)GetLastError());

    if (!SetThreadAffinityMask(GetCurrentThread(), pin_mask))
        fprintf(stderr, "warn: SetThreadAffinityMask failed: %lu\n",
                (unsigned long)GetLastError());
}

/* ------------------------------------------------------------------ */
/*  Latency recording                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *samples;
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

static size_t latency_sparse_cap(uint64_t target_rps, uint32_t duration_sec)
{
    if (target_rps == 0)
        return MAX_LATENCY_SAMPLES;

    uint64_t estimated = (target_rps * (uint64_t)duration_sec + LATENCY_SAMPLE_STRIDE - 1ULL) /
                         LATENCY_SAMPLE_STRIDE;
    estimated += LATENCY_SAMPLE_SLACK;
    if (estimated > MAX_LATENCY_SAMPLES)
        estimated = MAX_LATENCY_SAMPLES;
    return (size_t)estimated;
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

static inline double latency_us(uint64_t ns)
{
    return (double)ns / 1000.0;
}

static void latency_free(latency_recorder_t *lr)
{
    free(lr->samples);
    lr->samples = NULL;
    lr->count = 0;
}

static int attach_bench_shm(const char *run_dir, const char *service,
                            const nipc_np_session_t *session,
                            nipc_win_shm_ctx_t **shm_out)
{
    if (!(session->selected_profile & NIPC_WIN_SHM_PROFILE_HYBRID)) {
        fprintf(stderr, "client: expected Windows SHM profile, negotiated %u\n",
                session->selected_profile);
        return -1;
    }

    nipc_win_shm_ctx_t *shm = calloc(1, sizeof(*shm));
    if (!shm) {
        fprintf(stderr, "client: SHM context alloc failed\n");
        return -1;
    }

    ULONGLONG deadline = GetTickCount64() + CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS;
    nipc_win_shm_error_t last_err = NIPC_WIN_SHM_ERR_BAD_PARAM;

    while (1) {
        nipc_win_shm_error_t serr = nipc_win_shm_client_attach(
            run_dir, service, AUTH_TOKEN,
            session->session_id,
            session->selected_profile,
            shm);
        if (serr == NIPC_WIN_SHM_OK) {
            *shm_out = shm;
            return 0;
        }

        last_err = serr;
        if (GetTickCount64() >= deadline)
            break;

        Sleep(5);
    }

    fprintf(stderr, "client: Windows SHM attach failed after %llums (err=%d)\n",
            (unsigned long long)CLIENT_SHM_ATTACH_RETRY_TIMEOUT_MS,
            (int)last_err);
    free(shm);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Rate limiter                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t target_interval_ns;
    uint64_t next_send_ns;
} rate_limiter_t;

static void rate_limiter_init(rate_limiter_t *rl, uint64_t target_rps)
{
    rl->target_interval_ns = (target_rps == 0) ? 0 : (1000000000ull / target_rps);
    rl->next_send_ns = now_ns();
}

static void rate_limiter_wait(rate_limiter_t *rl)
{
    if (rl->target_interval_ns == 0)
        return;

    uint64_t current = now_ns();
    while (current < rl->next_send_ns) {
        uint64_t wait_ns = rl->next_send_ns - current;

        if (wait_ns >= 2000000ULL) {
            DWORD sleep_ms = (DWORD)(wait_ns / 1000000ULL);
            if (sleep_ms > 1)
                Sleep(sleep_ms - 1);
            else
                SwitchToThread();
        } else if (wait_ns >= 100000ULL) {
            SwitchToThread();
        } else {
            current = now_ns();
            while (current < rl->next_send_ns)
                current = now_ns();
            break;
        }

        current = now_ns();
    }

    rl->next_send_ns += rl->target_interval_ns;
}

/* ------------------------------------------------------------------ */
/*  Ping-pong handler (INCREMENT method)                               */
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
static INIT_ONCE g_snapshot_template_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK snapshot_template_init(PINIT_ONCE init_once, PVOID parameter, PVOID *context)
{
    (void)init_once;
    (void)parameter;
    (void)context;

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

    return TRUE;
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

    if (!InitOnceExecuteOnce(&g_snapshot_template_once,
                             snapshot_template_init, NULL, NULL))
        return NIPC_ERR_HANDLER_FAILED;

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
/*  Server main loop                                                   */
/* ------------------------------------------------------------------ */

static nipc_managed_server_t *g_server = NULL;

static BOOL WINAPI console_handler(DWORD type)
{
    (void)type;
    if (g_server)
        nipc_server_stop(g_server);
    return TRUE;
}

static DWORD WINAPI timer_thread(LPVOID arg)
{
    int duration_sec = *(int *)arg;
    Sleep((duration_sec + 3) * 1000);
    if (g_server)
        nipc_server_stop(g_server);
    return 0;
}

typedef struct {
    nipc_managed_server_t *server;
} bench_server_thread_ctx_t;

static DWORD WINAPI bench_server_thread(LPVOID arg)
{
    bench_server_thread_ctx_t *ctx = (bench_server_thread_ctx_t *)arg;
    nipc_server_run(ctx->server);
    return 0;
}

static int warm_server_from_env(const char *run_dir, const char *service)
{
    char *warmup_bin = getenv("NIPC_BENCH_SERVER_WARMUP_BIN");
    char *warmup_subcmd = getenv("NIPC_BENCH_SERVER_WARMUP_SUBCMD");
    char *warmup_duration = getenv("NIPC_BENCH_SERVER_WARMUP_DURATION_SEC");
    char *warmup_target = getenv("NIPC_BENCH_SERVER_WARMUP_TARGET_RPS");
    char *warmup_depth = getenv("NIPC_BENCH_SERVER_WARMUP_DEPTH");

    if (!warmup_bin || !*warmup_bin ||
        !warmup_subcmd || !*warmup_subcmd ||
        !warmup_duration || !*warmup_duration)
        return 0;

    const char *argv[8];
    int argc = 0;
    argv[argc++] = warmup_bin;
    argv[argc++] = warmup_subcmd;
    argv[argc++] = run_dir;
    argv[argc++] = service;
    argv[argc++] = warmup_duration;
    if (warmup_target && *warmup_target)
        argv[argc++] = warmup_target;
    if (warmup_depth && *warmup_depth)
        argv[argc++] = warmup_depth;
    argv[argc] = NULL;

    FILE *nul = fopen("NUL", "w");
    if (!nul) {
        fprintf(stderr, "server warmup: failed to open NUL: %lu\n",
                (unsigned long)GetLastError());
        return -1;
    }

    int saved_stdout = _dup(_fileno(stdout));
    if (saved_stdout < 0) {
        fprintf(stderr, "server warmup: failed to dup stdout\n");
        fclose(nul);
        return -1;
    }

    fflush(stdout);
    if (_dup2(_fileno(nul), _fileno(stdout)) < 0) {
        fprintf(stderr, "server warmup: failed to redirect stdout\n");
        _close(saved_stdout);
        fclose(nul);
        return -1;
    }

    intptr_t rc = _spawnv(_P_WAIT, warmup_bin, argv);

    fflush(stdout);
    (void)_dup2(saved_stdout, _fileno(stdout));
    _close(saved_stdout);
    fclose(nul);

    if (rc == -1) {
        fprintf(stderr, "server warmup: spawn failed for %s\n", warmup_bin);
        return -1;
    }
    if (rc != 0) {
        fprintf(stderr, "server warmup client failed with status %ld\n", (long)rc);
        return -1;
    }

    return 0;
}

static int run_server(const char *run_dir, const char *service,
                      uint32_t profiles, int duration_sec,
                      uint16_t expected_method_code,
                      nipc_server_handler_fn handler)
{
    nipc_np_server_config_t scfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
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
    SetConsoleCtrlHandler(console_handler, TRUE);

    bench_server_thread_ctx_t thread_ctx = {
        .server = &server,
    };
    HANDLE server_thread = CreateThread(NULL, 0, bench_server_thread, &thread_ctx, 0, NULL);
    if (!server_thread) {
        fprintf(stderr, "server thread create failed: %lu\n", (unsigned long)GetLastError());
        g_server = NULL;
        nipc_server_destroy(&server);
        return 1;
    }

    if (warm_server_from_env(run_dir, service) != 0) {
        nipc_server_stop(&server);
        WaitForSingleObject(server_thread, INFINITE);
        CloseHandle(server_thread);
        g_server = NULL;
        nipc_server_destroy(&server);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    uint64_t cpu_start = cpu_ns();

    HANDLE timer = NULL;
    if (duration_sec > 0) {
        timer = CreateThread(NULL, 0, timer_thread, &duration_sec, 0, NULL);
    }

    WaitForSingleObject(server_thread, INFINITE);
    CloseHandle(server_thread);

    if (timer) {
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }

    uint64_t cpu_end = cpu_ns();
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;

    printf("SERVER_CPU_SEC=%.6f\n", cpu_sec);
    fflush(stdout);

    g_server = NULL;
    nipc_server_destroy(&server);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Batch server (same handler, higher batch limits)                   */
/* ------------------------------------------------------------------ */

static int run_server_batch(const char *run_dir, const char *service,
                            uint32_t profiles, int duration_sec,
                            uint16_t expected_method_code,
                            nipc_server_handler_fn handler)
{
    nipc_np_server_config_t scfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_request_batch_items   = BENCH_MAX_BATCH_ITEMS,
        .max_response_payload_bytes = BENCH_BATCH_BUF_SIZE,
        .max_response_batch_items  = BENCH_MAX_BATCH_ITEMS,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
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
    SetConsoleCtrlHandler(console_handler, TRUE);

    bench_server_thread_ctx_t thread_ctx = {
        .server = &server,
    };
    HANDLE server_thread = CreateThread(NULL, 0, bench_server_thread, &thread_ctx, 0, NULL);
    if (!server_thread) {
        fprintf(stderr, "batch server thread create failed: %lu\n",
                (unsigned long)GetLastError());
        g_server = NULL;
        nipc_server_destroy(&server);
        return 1;
    }

    if (warm_server_from_env(run_dir, service) != 0) {
        nipc_server_stop(&server);
        WaitForSingleObject(server_thread, INFINITE);
        CloseHandle(server_thread);
        g_server = NULL;
        nipc_server_destroy(&server);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    uint64_t cpu_start = cpu_ns();

    HANDLE timer = NULL;
    if (duration_sec > 0) {
        timer = CreateThread(NULL, 0, timer_thread, &duration_sec, 0, NULL);
    }

    WaitForSingleObject(server_thread, INFINITE);
    CloseHandle(server_thread);

    if (timer) {
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }

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
        Sleep(10);
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "batch client: not ready after retries\n");
        return 1;
    }

    latency_recorder_t lr;
    latency_init(&lr, latency_sparse_cap(target_rps, (uint32_t)duration_sec));

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t counter = 0;
    uint64_t total_items = 0;
    uint64_t errors = 0;

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
    ULONGLONG btick_start = GetTickCount64();
    ULONGLONG btick_deadline = btick_start + (ULONGLONG)duration_sec * 1000;

    while (GetTickCount64() < btick_deadline) {
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

        uint64_t t0 = (total_items & 63) == 0 ? now_ns() : 0;

        nipc_header_t resp_hdr;
        const void *resp_payload;
        size_t resp_len;
        int fatal = 0;

        if (client.shm) {
            /* Win SHM path. Reuse resp_buf as a temporary contiguous
             * header+payload buffer so large batch payloads do not overflow
             * a fixed-size stack allocation. */
            uint8_t *msg = resp_buf;
            size_t msg_len = NIPC_HEADER_LEN + req_len;

            hdr.magic = NIPC_MAGIC_MSG;
            hdr.version = NIPC_VERSION;
            hdr.header_len = NIPC_HEADER_LEN;
            hdr.payload_len = (uint32_t)req_len;
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
            memcpy(msg + NIPC_HEADER_LEN, req_buf, req_len);

            nipc_win_shm_error_t serr = nipc_win_shm_send(client.shm, msg, msg_len);
            if (serr != NIPC_WIN_SHM_OK) { errors++; break; }

            size_t shm_resp_len;
            serr = nipc_win_shm_receive(client.shm, resp_buf,
                                        BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                                        &shm_resp_len, 30000);
            if (serr != NIPC_WIN_SHM_OK) { errors++; fatal = 1; break; }

            if (shm_resp_len < NIPC_HEADER_LEN) { errors++; fatal = 1; break; }
            nipc_header_decode(resp_buf, NIPC_HEADER_LEN, &resp_hdr);
            resp_payload = resp_buf + NIPC_HEADER_LEN;
            resp_len = shm_resp_len - NIPC_HEADER_LEN;
        } else {
            /* Named Pipe path */
            nipc_np_error_t uerr = nipc_np_send(&client.session, &hdr,
                                                   req_buf, req_len);
            if (uerr != NIPC_NP_OK) { errors++; break; }

            uerr = nipc_np_receive(&client.session, resp_buf,
                                   BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                                   &resp_hdr, &resp_payload, &resp_len);
            if (uerr != NIPC_NP_OK) { errors++; fatal = 1; break; }
        }

        /* Validate response */
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
                fprintf(stderr, "batch: value mismatch at %u/%u: expected %llu got %llu\n",
                        i, batch_size,
                        (unsigned long long)expected[i],
                        (unsigned long long)resp_val);
                errors++;
                fatal = 1;
                break;
            }
        }

        if (fatal)
            break;

        if (t0 != 0) {
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

    double p50 = latency_us(latency_percentile(&lr, 50.0));
    double p95 = latency_us(latency_percentile(&lr, 95.0));
    double p99 = latency_us(latency_percentile(&lr, 99.0));

    printf("%s,%s,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
           scenario, lang, lang,
           throughput,
           p50, p95, p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors > 0)
        fprintf(stderr, "batch client: %llu errors out of %llu items\n",
                (unsigned long long)errors, (unsigned long long)total_items);

    free(req_buf);
    free(resp_buf);
    free(expected);
    latency_free(&lr);
    nipc_client_close(&client);
    return (errors > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Ping-pong client (Named Pipe or SHM)                               */
/* ------------------------------------------------------------------ */

static int warm_ping_pong_client(nipc_np_session_t *session,
                                 nipc_win_shm_ctx_t *shm)
{
    uint64_t counter = 0;
    uint8_t req_payload[8];
    memcpy(req_payload, &counter, sizeof(req_payload));

    nipc_header_t hdr = {0};
    hdr.kind = NIPC_KIND_REQUEST;
    hdr.code = NIPC_METHOD_INCREMENT;
    hdr.item_count = 1;
    hdr.message_id = 1;
    hdr.transport_status = NIPC_STATUS_OK;
    hdr.payload_len = sizeof(req_payload);

    if (shm) {
        uint8_t msg[NIPC_HEADER_LEN + sizeof(req_payload)];
        size_t msg_len = sizeof(msg);
        hdr.magic = NIPC_MAGIC_MSG;
        hdr.version = NIPC_VERSION;
        hdr.header_len = NIPC_HEADER_LEN;
        nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
        memcpy(msg + NIPC_HEADER_LEN, req_payload, sizeof(req_payload));

        nipc_win_shm_error_t serr = nipc_win_shm_send(shm, msg, msg_len);
        if (serr != NIPC_WIN_SHM_OK) {
            fprintf(stderr, "client warmup: send failed: %d\n", (int)serr);
            return -1;
        }

        uint8_t resp_msg[NIPC_HEADER_LEN + 64];
        size_t resp_len = 0;
        serr = nipc_win_shm_receive(shm, resp_msg, sizeof(resp_msg),
                                    &resp_len, 30000);
        if (serr != NIPC_WIN_SHM_OK) {
            fprintf(stderr, "client warmup: receive failed: %d\n", (int)serr);
            return -1;
        }
        if (resp_len < NIPC_HEADER_LEN + sizeof(uint64_t)) {
            fprintf(stderr, "client warmup: short SHM response: %zu bytes\n",
                    resp_len);
            return -1;
        }

        uint64_t resp_val = 0;
        memcpy(&resp_val, resp_msg + NIPC_HEADER_LEN, sizeof(resp_val));
        if (resp_val != counter + 1) {
            fprintf(stderr,
                    "client warmup: counter mismatch: expected %llu, got %llu\n",
                    (unsigned long long)(counter + 1),
                    (unsigned long long)resp_val);
            return -1;
        }
        return 0;
    }

    nipc_np_error_t uerr = nipc_np_send(session, &hdr, req_payload,
                                        sizeof(req_payload));
    if (uerr != NIPC_NP_OK) {
        fprintf(stderr, "client warmup: send failed: %d\n", (int)uerr);
        return -1;
    }

    nipc_header_t resp_hdr;
    const void *resp_payload;
    size_t resp_len;
    uint8_t recv_buf[256];
    uerr = nipc_np_receive(session, recv_buf, sizeof(recv_buf),
                           &resp_hdr, &resp_payload, &resp_len);
    if (uerr != NIPC_NP_OK) {
        fprintf(stderr, "client warmup: receive failed: %d\n", (int)uerr);
        return -1;
    }
    if (resp_len < sizeof(uint64_t)) {
        fprintf(stderr, "client warmup: short NP response: %zu bytes\n", resp_len);
        return -1;
    }

    uint64_t resp_val = 0;
    memcpy(&resp_val, resp_payload, sizeof(resp_val));
    if (resp_val != counter + 1) {
        fprintf(stderr,
                "client warmup: counter mismatch: expected %llu, got %llu\n",
                (unsigned long long)(counter + 1),
                (unsigned long long)resp_val);
        return -1;
    }

    return 0;
}

static int run_ping_pong_client(const char *run_dir, const char *service,
                                 uint32_t profiles, int duration_sec,
                                 uint64_t target_rps,
                                 const char *scenario, const char *lang)
{
    /* Direct L1 Named Pipe connection with retry */
        nipc_np_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
    };

    nipc_np_session_t session;
    memset(&session, 0, sizeof(session));
    session.pipe = INVALID_HANDLE_VALUE;

    int connected = 0;
    for (int i = 0; i < 200; i++) {
        nipc_np_error_t nerr = nipc_np_connect(run_dir, service, &ccfg, &session);
        if (nerr == NIPC_NP_OK) {
            connected = 1;
            break;
        }
        Sleep(10);
    }

    if (!connected) {
        fprintf(stderr, "client: connect failed after retries\n");
        return 1;
    }

    nipc_win_shm_ctx_t *shm = NULL;
    if (profiles & (NIPC_WIN_SHM_PROFILE_HYBRID | NIPC_WIN_SHM_PROFILE_BUSYWAIT)) {
        if (attach_bench_shm(run_dir, service, &session, &shm) != 0) {
            nipc_np_close_session(&session);
            return 1;
        }
    }

    if (warm_ping_pong_client(&session, shm) != 0) {
        if (shm) {
            nipc_win_shm_close(shm);
            free(shm);
        }
        nipc_np_close_session(&session);
        return 1;
    }

    latency_recorder_t lr;
    latency_init(&lr, latency_sparse_cap(target_rps, (uint32_t)duration_sec));

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t counter = 0;
    uint64_t requests = 0;
    uint64_t errors = 0;

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    /* Use GetTickCount64 for loop condition — cheap (~1ms resolution).
     * QPC is expensive on Windows (esp. under Hyper-V) and was capping
     * throughput at ~70k req/s when called 3x per iteration. */
    ULONGLONG tick_start = GetTickCount64();
    ULONGLONG tick_deadline = tick_start + (ULONGLONG)duration_sec * 1000;

    while (GetTickCount64() < tick_deadline) {
        rate_limiter_wait(&rl);

        uint8_t req_payload[8];
        memcpy(req_payload, &counter, 8);

        nipc_header_t hdr = {0};
        hdr.kind = NIPC_KIND_REQUEST;
        hdr.code = NIPC_METHOD_INCREMENT;
        hdr.item_count = 1;
        hdr.message_id = counter + 1;
        hdr.transport_status = NIPC_STATUS_OK;
        hdr.payload_len = 8;

        /* Sample latency with QPC only every 64th request to avoid
         * QPC overhead dominating the benchmark. */
        uint64_t t0 = (requests & 63) == 0 ? now_ns() : 0;

        if (shm) {
            /* Win SHM path */
            size_t msg_len = NIPC_HEADER_LEN + 8;
            uint8_t msg[NIPC_HEADER_LEN + 8];

            hdr.magic = NIPC_MAGIC_MSG;
            hdr.version = NIPC_VERSION;
            hdr.header_len = NIPC_HEADER_LEN;
            nipc_header_encode(&hdr, msg, NIPC_HEADER_LEN);
            memcpy(msg + NIPC_HEADER_LEN, req_payload, 8);

            nipc_win_shm_error_t serr = nipc_win_shm_send(shm, msg, msg_len);
            if (serr != NIPC_WIN_SHM_OK) {
                errors++;
                continue;
            }

            uint8_t resp_msg[NIPC_HEADER_LEN + 64];
            size_t resp_len;
            serr = nipc_win_shm_receive(shm, resp_msg, sizeof(resp_msg),
                                          &resp_len, 30000);
            if (serr != NIPC_WIN_SHM_OK) {
                errors++;
                continue;
            }

            if (resp_len >= NIPC_HEADER_LEN + 8) {
                uint64_t resp_val;
                memcpy(&resp_val, resp_msg + NIPC_HEADER_LEN, 8);
                if (resp_val != counter + 1) {
                    fprintf(stderr, "counter chain broken: expected %llu, got %llu\n",
                            (unsigned long long)(counter + 1), (unsigned long long)resp_val);
                    errors++;
                }
            }
        } else {
            /* Named Pipe path */
            nipc_np_error_t uerr = nipc_np_send(&session, &hdr, req_payload, 8);
            if (uerr != NIPC_NP_OK) {
                errors++;
                continue;
            }

            nipc_header_t resp_hdr;
            const void *resp_payload;
            size_t resp_len;
            uint8_t recv_buf[256];

            uerr = nipc_np_receive(&session, recv_buf, sizeof(recv_buf),
                                    &resp_hdr, &resp_payload, &resp_len);
            if (uerr != NIPC_NP_OK) {
                errors++;
                continue;
            }

            if (resp_len >= 8) {
                uint64_t resp_val;
                memcpy(&resp_val, resp_payload, 8);
                if (resp_val != counter + 1) {
                    fprintf(stderr, "counter chain broken: expected %llu, got %llu\n",
                            (unsigned long long)(counter + 1), (unsigned long long)resp_val);
                    errors++;
                }
            }
        }

        if (t0 != 0) {
            uint64_t t1 = now_ns();
            latency_record(&lr, t1 - t0);
        }

        counter++;
        requests++;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)requests / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    double p50 = latency_us(latency_percentile(&lr, 50.0));
    double p95 = latency_us(latency_percentile(&lr, 95.0));
    double p99 = latency_us(latency_percentile(&lr, 99.0));

    printf("%s,c,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           p50, p95, p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors > 0)
        fprintf(stderr, "client: %llu errors\n", (unsigned long long)errors);

    latency_free(&lr);

    if (shm) {
        nipc_win_shm_close(shm);
        free(shm);
    }
    nipc_np_close_session(&session);

    return (errors > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Snapshot client                                                    */
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

    for (int i = 0; i < 200; i++) {
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client))
            break;
        Sleep(10);
    }

    if (!nipc_client_ready(&client)) {
        fprintf(stderr, "client: not ready after retries\n");
        return 1;
    }

    latency_recorder_t lr;
    latency_init(&lr, latency_sparse_cap(target_rps, (uint32_t)duration_sec));

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t requests_cnt = 0;
    uint64_t errors_cnt = 0;
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    ULONGLONG tick_deadline = GetTickCount64() + (ULONGLONG)duration_sec * 1000;

    while (GetTickCount64() < tick_deadline) {
        rate_limiter_wait(&rl);

        uint64_t t0 = (requests_cnt & 63) == 0 ? now_ns() : 0;

        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

        if (err != NIPC_OK) {
            errors_cnt++;
            nipc_client_refresh(&client);
            continue;
        }

        if (view.item_count != 16) {
            fprintf(stderr, "snapshot: expected 16 items, got %u\n",
                    view.item_count);
            errors_cnt++;
        }

        if (t0 != 0) {
            uint64_t t1 = now_ns();
            latency_record(&lr, t1 - t0);
        }
        requests_cnt++;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)requests_cnt / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    double p50 = latency_us(latency_percentile(&lr, 50.0));
    double p95 = latency_us(latency_percentile(&lr, 95.0));
    double p99 = latency_us(latency_percentile(&lr, 99.0));

    printf("%s,c,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           p50, p95, p99,
           cpu_pct, cpu_pct);
    fflush(stdout);

    if (errors_cnt > 0)
        fprintf(stderr, "client: %llu errors\n", (unsigned long long)errors_cnt);

    latency_free(&lr);
    nipc_client_close(&client);
    return (errors_cnt > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Lookup benchmark                                                   */
/* ------------------------------------------------------------------ */

static int run_lookup_bench(int duration_sec)
{
    nipc_cgroups_cache_item_t items[16];
    for (int i = 0; i < 16; i++) {
        items[i].hash = (uint32_t)(1000 + i);
        items[i].options = 0;
        items[i].enabled = (uint32_t)(i % 2);
        char name[64], path[128];
        snprintf(name, sizeof(name), "cgroup-%d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/bench/cg-%d", i);
        items[i].name = _strdup(name);
        items[i].path = _strdup(path);
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
    ULONGLONG tick_deadline = GetTickCount64() + (ULONGLONG)duration_sec * 1000;

    while (GetTickCount64() < tick_deadline) {
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
        fprintf(stderr, "lookup: missed %llu/%llu\n",
                (unsigned long long)(lookups - hits), (unsigned long long)lookups);
    }

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
        "  %s np-ping-pong-server         <run_dir> <service> [duration_sec]\n"
        "  %s np-ping-pong-client         <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s shm-ping-pong-server        <run_dir> <service> [duration_sec]\n"
        "  %s shm-ping-pong-client        <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s np-batch-ping-pong-server   <run_dir> <service> [duration_sec]\n"
        "  %s np-batch-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s shm-batch-ping-pong-server  <run_dir> <service> [duration_sec]\n"
        "  %s shm-batch-ping-pong-client  <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-server              <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-client              <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-shm-server          <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-shm-client          <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s np-pipeline-client           <run_dir> <service> <duration_sec> <target_rps> <depth>\n"
        "  %s np-pipeline-batch-client     <run_dir> <service> <duration_sec> <target_rps> <depth>\n"
        "  %s lookup-bench                 <duration_sec>\n",
        prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    QueryPerformanceFrequency(&qpc_freq);
    elevate_bench_priority();

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    pin_bench_affinity(cmd);

    /* Single-item server subcommands */
    if (strcmp(cmd, "np-ping-pong-server") == 0 ||
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

        CreateDirectoryA(run_dir, NULL);

        uint32_t profiles;
        uint16_t expected_method_code;
        nipc_server_handler_fn handler;

        if (strcmp(cmd, "np-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_NP;
            expected_method_code = NIPC_METHOD_INCREMENT;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "shm-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_SHM;
            expected_method_code = NIPC_METHOD_INCREMENT;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "snapshot-server") == 0) {
            profiles = BENCH_PROFILE_NP;
            expected_method_code = NIPC_METHOD_CGROUPS_SNAPSHOT;
            handler = snapshot_handler;
        } else {
            profiles = BENCH_PROFILE_SHM;
            expected_method_code = NIPC_METHOD_CGROUPS_SNAPSHOT;
            handler = snapshot_handler;
        }

        return run_server(run_dir, service, profiles, duration,
                          expected_method_code, handler);
    }

    /* Batch server subcommands */
    if (strcmp(cmd, "np-batch-ping-pong-server") == 0 ||
        strcmp(cmd, "shm-batch-ping-pong-server") == 0) {

        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }

        const char *run_dir = argv[2];
        const char *service = argv[3];
        int duration = (argc >= 5) ? atoi(argv[4]) : DEFAULT_DURATION;

        CreateDirectoryA(run_dir, NULL);

        uint32_t profiles;
        if (strcmp(cmd, "np-batch-ping-pong-server") == 0)
            profiles = BENCH_PROFILE_NP;
        else
            profiles = BENCH_PROFILE_SHM;

        return run_server_batch(run_dir, service, profiles, duration,
                                NIPC_METHOD_INCREMENT,
                                ping_pong_handler);
    }

    /* Ping-pong client */
    if (strcmp(cmd, "np-ping-pong-client") == 0 ||
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

        if (strcmp(cmd, "np-ping-pong-client") == 0) {
            profiles = BENCH_PROFILE_NP;
            scenario = "np-ping-pong";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "shm-ping-pong";
        }

        return run_ping_pong_client(run_dir, service, profiles,
                                     duration, target_rps,
                                     scenario, "c");
    }

    /* Batch client subcommands */
    if (strcmp(cmd, "np-batch-ping-pong-client") == 0 ||
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

        if (strcmp(cmd, "np-batch-ping-pong-client") == 0) {
            profiles = BENCH_PROFILE_NP;
            scenario = "np-batch-ping-pong";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "shm-batch-ping-pong";
        }

        return run_batch_ping_pong_client(run_dir, service, profiles,
                                           duration, target_rps,
                                           scenario, "c");
    }

    /* Snapshot client */
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
            profiles = BENCH_PROFILE_NP;
            scenario = "snapshot-baseline";
        } else {
            profiles = BENCH_PROFILE_SHM;
            scenario = "snapshot-shm";
        }

        return run_snapshot_client(run_dir, service, profiles,
                                    duration, target_rps,
                                    scenario, "c");
    }

    /* Pipeline client: send depth requests, receive depth responses */
    if (strcmp(cmd, "np-pipeline-client") == 0) {
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
        if (depth > 128) depth = 128;

        nipc_np_client_config_t ccfg = {
            .supported_profiles        = BENCH_PROFILE_NP,
            .preferred_profiles        = BENCH_PROFILE_NP,
            .max_request_payload_bytes = 4096,
            .max_request_batch_items   = 1,
            .max_response_payload_bytes = RESPONSE_BUF_SIZE,
            .max_response_batch_items  = 1,
            .auth_token                = AUTH_TOKEN,
        };

        nipc_np_session_t session;
        memset(&session, 0, sizeof(session));
        session.pipe = INVALID_HANDLE_VALUE;

        int connected = 0;
        for (int i = 0; i < 200; i++) {
            nipc_np_error_t nerr = nipc_np_connect(run_dir, service, &ccfg, &session);
            if (nerr == NIPC_NP_OK) {
                connected = 1;
                break;
            }
            Sleep(10);
        }
        if (!connected) {
            fprintf(stderr, "pipeline client: connect failed after retries\n");
            return 1;
        }

        latency_recorder_t lr;
        latency_init(&lr, latency_sparse_cap(target_rps, (uint32_t)duration));

        rate_limiter_t rl;
        rate_limiter_init(&rl, target_rps);

        uint64_t counter = 0;
        uint64_t requests = 0;
        uint64_t errors = 0;

        uint64_t cpu_start = cpu_ns();
        uint64_t wall_start = now_ns();
        ULONGLONG tick_deadline = GetTickCount64() + (ULONGLONG)duration * 1000;

        while (GetTickCount64() < tick_deadline) {
            rate_limiter_wait(&rl);
            uint64_t t0 = (requests & 63) == 0 ? now_ns() : 0;

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

                nipc_np_error_t uerr = nipc_np_send(&session, &hdr,
                                                       req_payload, 8);
                if (uerr != NIPC_NP_OK) {
                    send_ok = 0;
                    errors++;
                    break;
                }
            }

            if (!send_ok)
                continue;

            /* Receive `depth` responses */
            for (int d = 0; d < depth; d++) {
                nipc_header_t resp_hdr;
                const void *resp_payload;
                size_t resp_len;
                uint8_t recv_buf[256];

                nipc_np_error_t uerr = nipc_np_receive(&session,
                    recv_buf, sizeof(recv_buf),
                    &resp_hdr, &resp_payload, &resp_len);
                if (uerr != NIPC_NP_OK) {
                    errors++;
                    break;
                }

                if (resp_len >= 8) {
                    uint64_t resp_val;
                    memcpy(&resp_val, resp_payload, 8);
                    uint64_t expected = counter + (uint64_t)d + 1;
                    if (resp_val != expected) {
                        fprintf(stderr, "pipeline chain broken at depth %d: expected %llu, got %llu\n",
                                d, (unsigned long long)expected, (unsigned long long)resp_val);
                        errors++;
                    }
                }
            }

            if (t0 != 0) {
                uint64_t t1 = now_ns();
                latency_record(&lr, t1 - t0);
            }

            counter += (uint64_t)depth;
            requests += (uint64_t)depth;
        }

        uint64_t cpu_end = cpu_ns();
        uint64_t wall_actual = now_ns() - wall_start;

        double wall_sec = (double)wall_actual / 1e9;
        double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
        double throughput = (double)requests / wall_sec;
        double cpu_pct = (cpu_sec / wall_sec) * 100.0;

        double p50 = latency_us(latency_percentile(&lr, 50.0));
        double p95 = latency_us(latency_percentile(&lr, 95.0));
        double p99 = latency_us(latency_percentile(&lr, 99.0));

        char scenario[64];
        snprintf(scenario, sizeof(scenario), "np-pipeline-d%d", depth);

        printf("%s,c,c,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
               scenario,
               throughput,
               p50, p95, p99,
               cpu_pct, cpu_pct);
        fflush(stdout);

        if (errors > 0)
            fprintf(stderr, "pipeline client: %llu errors\n", (unsigned long long)errors);

        latency_free(&lr);
        nipc_np_close_session(&session);
        return (errors > 0) ? 1 : 0;
    }

    /* Pipeline+batch client: each pipelined message is a random 2-1000 item
     * batch. item_count==1 is normalized to the single-item path. */
    if (strcmp(cmd, "np-pipeline-batch-client") == 0) {
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

        nipc_np_client_config_t ccfg = {
            .supported_profiles        = BENCH_PROFILE_NP,
            .preferred_profiles        = BENCH_PROFILE_NP,
            .max_request_payload_bytes = BENCH_BATCH_BUF_SIZE,
            .max_request_batch_items   = BENCH_MAX_BATCH_ITEMS,
            .max_response_payload_bytes = BENCH_BATCH_BUF_SIZE,
            .max_response_batch_items  = BENCH_MAX_BATCH_ITEMS,
            .auth_token                = AUTH_TOKEN,
        };

        nipc_np_session_t session;
        memset(&session, 0, sizeof(session));
        session.pipe = INVALID_HANDLE_VALUE;

        int connected = 0;
        for (int i = 0; i < 200; i++) {
            nipc_np_error_t nerr = nipc_np_connect(run_dir, service, &ccfg, &session);
            if (nerr == NIPC_NP_OK) {
                connected = 1;
                break;
            }
            Sleep(10);
        }
        if (!connected) {
            fprintf(stderr, "pipeline-batch client: connect failed after retries\n");
            return 1;
        }

        latency_recorder_t lr;
        latency_init(&lr, latency_sparse_cap(target_rps, (uint32_t)duration));
        rate_limiter_t rl;
        rate_limiter_init(&rl, target_rps);

        uint64_t counter = 0;
        uint64_t total_items = 0;
        uint64_t errors = 0;

        uint8_t *req_bufs[128];
        uint8_t *recv_buf = NULL;
        size_t req_lens[128];
        size_t slot_costs[128];
        uint32_t batch_sizes[128];
        uint64_t slot_msg_ids[128];
        bool slot_active[128];
        nipc_header_t hdrs[128];

        memset(req_bufs, 0, sizeof(req_bufs));
        memset(slot_active, 0, sizeof(slot_active));
        for (int i = 0; i < depth && i < 128; i++)
            req_bufs[i] = malloc(BENCH_BATCH_BUF_SIZE);
        recv_buf = malloc(BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN);

        if (!recv_buf) {
            fprintf(stderr, "pipeline-batch client: recv buffer alloc failed\n");
            for (int i = 0; i < depth && i < 128; i++)
                free(req_bufs[i]);
            nipc_np_close_session(&session);
            return 1;
        }

        uint64_t cpu_start = cpu_ns();
        uint64_t wall_start = now_ns();
        ULONGLONG tick_deadline = GetTickCount64() + (ULONGLONG)duration * 1000;
        size_t inflight_budget = (size_t)session.packet_size * 2;

        while (GetTickCount64() < tick_deadline) {
            rate_limiter_wait(&rl);
            uint64_t t0 = (total_items & 63) == 0 ? now_ns() : 0;

            /* Keep the logical pipeline depth, but drain responses once the
             * estimated request+response bytes in flight get too large for a
             * blocking named pipe session. */
            memset(slot_active, 0, sizeof(slot_active));
            int sent = 0;
            int received = 0;
            int outstanding = 0;
            size_t inflight_bytes = 0;
            int cycle_ok = 1;

            while (received < depth) {
                if (sent < depth) {
                    int slot = sent;
                    uint32_t bs = (bench_rand() % (BENCH_MAX_BATCH_ITEMS - 1)) + 2;
                    batch_sizes[slot] = bs;

                    nipc_batch_builder_t bb;
                    nipc_batch_builder_init(&bb, req_bufs[slot], BENCH_BATCH_BUF_SIZE, bs);

                    for (uint32_t i = 0; i < bs; i++) {
                        uint8_t item[NIPC_INCREMENT_PAYLOAD_SIZE];
                        nipc_increment_encode(counter + i, item, sizeof(item));
                        nipc_batch_builder_add(&bb, item, sizeof(item));
                    }

                    uint32_t out_count;
                    req_lens[slot] = nipc_batch_builder_finish(&bb, &out_count);

                    hdrs[slot] = (nipc_header_t){0};
                    hdrs[slot].kind = NIPC_KIND_REQUEST;
                    hdrs[slot].code = NIPC_METHOD_INCREMENT;
                    hdrs[slot].flags = NIPC_FLAG_BATCH;
                    hdrs[slot].item_count = bs;
                    hdrs[slot].message_id = counter + 1 + (uint64_t)slot;
                    hdrs[slot].transport_status = NIPC_STATUS_OK;

                    slot_msg_ids[slot] = hdrs[slot].message_id;
                    slot_costs[slot] = (NIPC_HEADER_LEN + req_lens[slot]) * 2;

                    while (outstanding > 0 &&
                           inflight_bytes + slot_costs[slot] > inflight_budget) {
                        nipc_header_t resp_hdr;
                        const void *resp_payload;
                        size_t resp_len;
                        nipc_np_error_t uerr = nipc_np_receive(&session,
                            recv_buf, BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                            &resp_hdr, &resp_payload, &resp_len);
                        if (uerr != NIPC_NP_OK) {
                            errors++;
                            cycle_ok = 0;
                            break;
                        }

                        int matched = -1;
                        for (int i = 0; i < depth; i++) {
                            if (slot_active[i] && slot_msg_ids[i] == resp_hdr.message_id) {
                                matched = i;
                                break;
                            }
                        }
                        if (matched < 0) {
                            fprintf(stderr, "pipeline-batch client: unknown response id %llu\n",
                                    (unsigned long long)resp_hdr.message_id);
                            errors++;
                            cycle_ok = 0;
                            break;
                        }

                        inflight_bytes -= slot_costs[matched];
                        total_items += batch_sizes[matched];
                        slot_active[matched] = false;
                        outstanding--;
                        received++;
                    }
                    if (!cycle_ok)
                        break;

                    nipc_np_error_t uerr = nipc_np_send(&session,
                        &hdrs[slot], req_bufs[slot], req_lens[slot]);
                    if (uerr != NIPC_NP_OK) {
                        errors++;
                        cycle_ok = 0;
                        break;
                    }

                    slot_active[slot] = true;
                    inflight_bytes += slot_costs[slot];
                    outstanding++;
                    counter += bs;
                    sent++;
                    continue;
                }

                nipc_header_t resp_hdr;
                const void *resp_payload;
                size_t resp_len;
                nipc_np_error_t uerr = nipc_np_receive(&session,
                    recv_buf, BENCH_BATCH_BUF_SIZE + NIPC_HEADER_LEN,
                    &resp_hdr, &resp_payload, &resp_len);
                if (uerr != NIPC_NP_OK) {
                    errors++;
                    cycle_ok = 0;
                    break;
                }

                int matched = -1;
                for (int i = 0; i < depth; i++) {
                    if (slot_active[i] && slot_msg_ids[i] == resp_hdr.message_id) {
                        matched = i;
                        break;
                    }
                }
                if (matched < 0) {
                    fprintf(stderr, "pipeline-batch client: unknown response id %llu\n",
                            (unsigned long long)resp_hdr.message_id);
                    errors++;
                    cycle_ok = 0;
                    break;
                }

                inflight_bytes -= slot_costs[matched];
                total_items += batch_sizes[matched];
                slot_active[matched] = false;
                outstanding--;
                received++;
            }

            if (!cycle_ok)
                break;

            if (t0 != 0) {
                uint64_t t1 = now_ns();
                latency_record(&lr, t1 - t0);
            }
        }

        uint64_t cpu_end = cpu_ns();
        uint64_t wall_actual = now_ns() - wall_start;
        double wall_sec = (double)wall_actual / 1e9;
        double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
        double throughput = (double)total_items / wall_sec;
        double cpu_pct = (cpu_sec / wall_sec) * 100.0;

        double p50 = latency_us(latency_percentile(&lr, 50.0));
        double p95 = latency_us(latency_percentile(&lr, 95.0));
        double p99 = latency_us(latency_percentile(&lr, 99.0));

        char scenario[64];
        snprintf(scenario, sizeof(scenario), "np-pipeline-batch-d%d", depth);
        printf("%s,c,c,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
               scenario, throughput,
               p50, p95, p99,
               cpu_pct, cpu_pct);
        fflush(stdout);

        free(recv_buf);
        for (int i = 0; i < depth && i < 128; i++)
            free(req_bufs[i]);
        latency_free(&lr);
        nipc_np_close_session(&session);
        return (errors > 0) ? 1 : 0;
    }

    if (strcmp(cmd, "lookup-bench") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        return run_lookup_bench(atoi(argv[2]));
    }

    fprintf(stderr, "Unknown subcommand: %s\n", cmd);
    usage(argv[0]);
    return 1;
}

#else /* !_WIN32 */

#include <stdio.h>
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fprintf(stderr, "Windows benchmark driver: not supported on this platform\n");
    return 1;
}

#endif /* _WIN32 || __MSYS__ */
