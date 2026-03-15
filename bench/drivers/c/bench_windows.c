/*
 * bench_windows.c - Windows benchmark driver for netipc.
 *
 * Exercises Named Pipe and Win SHM transports. Measures throughput,
 * latency (p50/p95/p99), and CPU for ping-pong, snapshot, and lookup
 * scenarios.
 *
 * Subcommands:
 *   np-ping-pong-server     <run_dir> <service> [duration_sec]
 *   np-ping-pong-client     <run_dir> <service> <duration_sec> <target_rps>
 *   shm-ping-pong-server    <run_dir> <service> [duration_sec]
 *   shm-ping-pong-client    <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-server          <run_dir> <service> [duration_sec]
 *   snapshot-client          <run_dir> <service> <duration_sec> <target_rps>
 *   snapshot-shm-server      <run_dir> <service> [duration_sec]
 *   snapshot-shm-client      <run_dir> <service> <duration_sec> <target_rps>
 *   lookup-bench             <duration_sec>
 *
 * target_rps=0 means maximum throughput (no rate limiting).
 *
 * Output (client): one CSV line per run:
 *   scenario,client,server,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct
 */

#ifdef _WIN32

#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_win_shm.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define AUTH_TOKEN          0xBE4C400000C0FFEEull
#define RESPONSE_BUF_SIZE  65536
#define MAX_LATENCY_SAMPLES (10 * 1000 * 1000)
#define DEFAULT_DURATION   30

/* Profiles */
#define BENCH_PROFILE_NP   NIPC_PROFILE_BASELINE
#define BENCH_PROFILE_SHM  (NIPC_PROFILE_BASELINE | NIPC_WIN_SHM_PROFILE_HYBRID)

/* ------------------------------------------------------------------ */
/*  Timing helpers (Windows)                                           */
/* ------------------------------------------------------------------ */

static LARGE_INTEGER qpc_freq;

static inline uint64_t now_ns(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / qpc_freq.QuadPart);
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
    if (current < rl->next_send_ns) {
        uint64_t wait_us = (rl->next_send_ns - current) / 1000;
        if (wait_us > 0)
            Sleep((DWORD)((wait_us + 999) / 1000));
    }
    rl->next_send_ns += rl->target_interval_ns;
}

/* ------------------------------------------------------------------ */
/*  Ping-pong handler (INCREMENT method)                               */
/* ------------------------------------------------------------------ */

static bool ping_pong_handler(void *user,
                               uint16_t method_code,
                               const uint8_t *request_payload,
                               size_t request_len,
                               uint8_t *response_buf,
                               size_t response_buf_size,
                               size_t *response_len_out)
{
    (void)user;

    if (method_code != NIPC_METHOD_INCREMENT)
        return false;
    if (request_len < 8)
        return false;
    if (response_buf_size < 8)
        return false;

    uint64_t counter;
    memcpy(&counter, request_payload, 8);
    counter++;
    memcpy(response_buf, &counter, 8);
    *response_len_out = 8;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Snapshot handler (16 cgroup items)                                  */
/* ------------------------------------------------------------------ */

static bool snapshot_handler(void *user,
                              uint16_t method_code,
                              const uint8_t *request_payload,
                              size_t request_len,
                              uint8_t *response_buf,
                              size_t response_buf_size,
                              size_t *response_len_out)
{
    (void)user;

    if (method_code != NIPC_METHOD_CGROUPS_SNAPSHOT)
        return false;

    nipc_cgroups_req_t req;
    if (nipc_cgroups_req_decode(request_payload, request_len, &req) != NIPC_OK)
        return false;

    static uint64_t gen = 0;
    gen++;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size,
                               16, 1, gen);

    for (int i = 0; i < 16; i++) {
        char name[64], path[128];
        snprintf(name, sizeof(name), "cgroup-%d", i);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/bench/cg-%d", i);

        nipc_error_t err = nipc_cgroups_builder_add(
            &builder,
            (uint32_t)(1000 + i),
            0,
            (uint32_t)(i % 2),
            name, (uint32_t)strlen(name),
            path, (uint32_t)strlen(path));

        if (err != NIPC_OK)
            return false;
    }

    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return true;
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

static int run_server(const char *run_dir, const char *service,
                      uint32_t profiles, int duration_sec,
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
                                          1, RESPONSE_BUF_SIZE,
                                          handler, NULL);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return 1;
    }

    g_server = &server;

    printf("READY\n");
    fflush(stdout);

    uint64_t cpu_start = cpu_ns();

    SetConsoleCtrlHandler(console_handler, TRUE);

    HANDLE timer = NULL;
    if (duration_sec > 0) {
        timer = CreateThread(NULL, 0, timer_thread, &duration_sec, 0, NULL);
    }

    nipc_server_run(&server);

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
/*  Ping-pong client (Named Pipe or SHM)                               */
/* ------------------------------------------------------------------ */

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
        .packet_size               = 0,
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

    /* SHM upgrade if negotiated */
    nipc_win_shm_ctx_t *shm = NULL;
    if (session.selected_profile & NIPC_WIN_SHM_PROFILE_HYBRID) {
        shm = calloc(1, sizeof(nipc_win_shm_ctx_t));
        int attached = 0;
        for (int i = 0; i < 200; i++) {
            nipc_win_shm_error_t serr = nipc_win_shm_client_attach(
                run_dir, service, AUTH_TOKEN,
                NIPC_WIN_SHM_PROFILE_HYBRID, shm);
            if (serr == NIPC_WIN_SHM_OK) {
                attached = 1;
                break;
            }
            Sleep(5);
        }
        if (!attached) {
            free(shm);
            shm = NULL;
        }
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

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
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

        uint64_t t0 = now_ns();

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

    uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
    uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
    uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

    printf("%s,c,%s,%.0f,%llu,%llu,%llu,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           (unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99,
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
    nipc_np_client_config_t ccfg = {
        .supported_profiles        = profiles,
        .preferred_profiles        = profiles,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .max_response_batch_items  = 1,
        .auth_token                = AUTH_TOKEN,
        .packet_size               = 0,
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
    size_t est_samples = (target_rps == 0) ? 5000000 :
                         (size_t)(target_rps * (uint64_t)duration_sec);
    latency_init(&lr, est_samples);

    rate_limiter_t rl;
    rate_limiter_init(&rl, target_rps);

    uint64_t requests_cnt = 0;
    uint64_t errors_cnt = 0;
    uint8_t req_buf[64];
    uint8_t resp_buf[RESPONSE_BUF_SIZE];

    uint64_t cpu_start = cpu_ns();
    uint64_t wall_start = now_ns();
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
        rate_limiter_wait(&rl);

        uint64_t t0 = now_ns();

        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot(
            &client, req_buf, resp_buf, sizeof(resp_buf), &view);

        uint64_t t1 = now_ns();

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

        latency_record(&lr, t1 - t0);
        requests_cnt++;
    }

    uint64_t cpu_end = cpu_ns();
    uint64_t wall_actual = now_ns() - wall_start;

    double wall_sec = (double)wall_actual / 1e9;
    double cpu_sec = (double)(cpu_end - cpu_start) / 1e9;
    double throughput = (double)requests_cnt / wall_sec;
    double cpu_pct = (cpu_sec / wall_sec) * 100.0;

    uint64_t p50 = latency_percentile(&lr, 50.0) / 1000;
    uint64_t p95 = latency_percentile(&lr, 95.0) / 1000;
    uint64_t p99 = latency_percentile(&lr, 99.0) / 1000;

    printf("%s,c,%s,%.0f,%llu,%llu,%llu,%.1f,0.0,%.1f\n",
           scenario, lang,
           throughput,
           (unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99,
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
    uint64_t wall_end = wall_start + (uint64_t)duration_sec * 1000000000ull;

    while (now_ns() < wall_end) {
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
        "  %s np-ping-pong-server    <run_dir> <service> [duration_sec]\n"
        "  %s np-ping-pong-client    <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s shm-ping-pong-server   <run_dir> <service> [duration_sec]\n"
        "  %s shm-ping-pong-client   <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-server         <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-client         <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s snapshot-shm-server     <run_dir> <service> [duration_sec]\n"
        "  %s snapshot-shm-client     <run_dir> <service> <duration_sec> <target_rps>\n"
        "  %s lookup-bench            <duration_sec>\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    QueryPerformanceFrequency(&qpc_freq);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    /* Server subcommands */
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
        nipc_server_handler_fn handler;

        if (strcmp(cmd, "np-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_NP;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "shm-ping-pong-server") == 0) {
            profiles = BENCH_PROFILE_SHM;
            handler = ping_pong_handler;
        } else if (strcmp(cmd, "snapshot-server") == 0) {
            profiles = BENCH_PROFILE_NP;
            handler = snapshot_handler;
        } else {
            profiles = BENCH_PROFILE_SHM;
            handler = snapshot_handler;
        }

        return run_server(run_dir, service, profiles, duration, handler);
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

#endif /* _WIN32 */
