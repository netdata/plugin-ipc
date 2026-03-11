#include <netipc/netipc_named_pipe.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

/* ------------------------------------------------------------------ */
/* RDTSC-based timing: avoids ~4.5μs QPC overhead under Hyper-V       */
/* ------------------------------------------------------------------ */

static inline uint64_t rdtsc_now(void) {
    __asm__ volatile("lfence" ::: "memory");
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static double g_tsc_ns_per_tick = 0.0;

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void calibrate_tsc(void) {
    LARGE_INTEGER freq, qpc0, qpc1;
    QueryPerformanceFrequency(&freq);

    /* Robust multi-pass calibration.
     * Under Hyper-V, QPC can occasionally return wildly wrong deltas.
     * We take 5 samples of 200ms each, discard outliers, and use the median. */

    /* Warmup pass - let turbo boost settle */
    rdtsc_now();
    Sleep(200);

    #define CAL_PASSES 5
    double samples[CAL_PASSES];
    for (int i = 0; i < CAL_PASSES; i++) {
        uint64_t tsc0 = rdtsc_now();
        QueryPerformanceCounter(&qpc0);
        Sleep(200);
        uint64_t tsc1 = rdtsc_now();
        QueryPerformanceCounter(&qpc1);

        double wall_sec = (double)(qpc1.QuadPart - qpc0.QuadPart) / (double)freq.QuadPart;
        if (wall_sec < 0.05) {
            /* QPC returned garbage - retry this sample */
            i--;
            continue;
        }
        samples[i] = (double)(tsc1 - tsc0) / wall_sec;
    }

    /* Sort and take the median */
    qsort(samples, CAL_PASSES, sizeof(double), compare_double);
    double tsc_hz = samples[CAL_PASSES / 2];

    /* Sanity check: expect 1-10 GHz range */
    if (tsc_hz < 1e9 || tsc_hz > 10e9) {
        fprintf(stderr, "TSC calibration suspect: %.2f GHz, retrying with long window\n", tsc_hz / 1e9);
        /* Fallback: single long 1-second measurement */
        uint64_t tsc0 = rdtsc_now();
        QueryPerformanceCounter(&qpc0);
        Sleep(1000);
        uint64_t tsc1 = rdtsc_now();
        QueryPerformanceCounter(&qpc1);
        double wall_sec = (double)(qpc1.QuadPart - qpc0.QuadPart) / (double)freq.QuadPart;
        tsc_hz = (double)(tsc1 - tsc0) / wall_sec;
    }

    g_tsc_ns_per_tick = 1e9 / tsc_hz;
    fprintf(stderr, "TSC calibrated: %.2f GHz (%.3f ns/tick)\n", tsc_hz / 1e9, g_tsc_ns_per_tick);
    #undef CAL_PASSES
}

static double tsc_ticks_to_micros(uint64_t ticks) {
    return (double)ticks * g_tsc_ns_per_tick / 1000.0;
}

static void apply_cpu_affinity(void) {
    const char *value = getenv("NETIPC_CPU_AFFINITY");
    if (!value || value[0] == '\0') {
        return;
    }

    char *end = NULL;
    unsigned long long mask = strtoull(value, &end, 0);
    if (!end || *end != '\0' || mask == 0u) {
        fprintf(stderr, "warning: invalid NETIPC_CPU_AFFINITY=%s\n", value);
        return;
    }

    DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)mask);
    if (result == 0) {
        fprintf(stderr, "warning: SetThreadAffinityMask(0x%llx) failed: %lu\n",
                mask, GetLastError());
    }

    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
}

struct latency_samples {
    uint64_t *values;
    size_t used;
    size_t capacity;
};

static void usage(const char *argv0) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s server-once <run_dir> <service>\n", argv0);
    fprintf(stderr, "  %s client-once <run_dir> <service> <value>\n", argv0);
    fprintf(stderr, "  %s server-loop <run_dir> <service> <max_requests|0>\n", argv0);
    fprintf(stderr, "  %s client-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0);
}

static uint64_t parse_u64(const char *s) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "invalid u64: %s\n", s);
        exit(2);
    }
    return (uint64_t)v;
}

static int32_t parse_i32(const char *s) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "invalid i32: %s\n", s);
        exit(2);
    }
    return (int32_t)v;
}

static uint32_t parse_u32(const char *s) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "invalid u32: %s\n", s);
        exit(2);
    }
    return (uint32_t)v;
}

static uint32_t parse_env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    return parse_u32(value);
}

static uint64_t parse_env_u64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    return parse_u64(value);
}

static LARGE_INTEGER performance_frequency(void) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

static uint64_t performance_counter(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

static double counter_to_seconds(uint64_t ticks, LARGE_INTEGER freq) {
    if (freq.QuadPart <= 0) {
        return 0.0;
    }
    return (double)ticks / (double)freq.QuadPart;
}

static double counter_to_micros(uint64_t ticks, LARGE_INTEGER freq) {
    return counter_to_seconds(ticks, freq) * 1e6;
}

static double self_cpu_seconds(void) {
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    ULARGE_INTEGER kernel;
    ULARGE_INTEGER user;

    if (!GetProcessTimes(GetCurrentProcess(),
                         &create_time,
                         &exit_time,
                         &kernel_time,
                         &user_time)) {
        return 0.0;
    }

    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;

    return (double)(kernel.QuadPart + user.QuadPart) / 10000000.0;
}

static void sleep_until(uint64_t target_ticks, LARGE_INTEGER freq) {
    for (;;) {
        uint64_t now_ticks = performance_counter();
        if (now_ticks >= target_ticks) {
            return;
        }

        uint64_t remaining_ticks = target_ticks - now_ticks;
        double remaining_ns =
            ((double)remaining_ticks * 1000000000.0) / (double)freq.QuadPart;

        if (remaining_ns > 2000000.0) {
            Sleep(1u);
        } else if (remaining_ns > 200000.0) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}

static int latency_samples_push(struct latency_samples *samples, uint64_t value) {
    if (!samples) {
        return -1;
    }

    if (samples->used == samples->capacity) {
        size_t next_capacity = samples->capacity == 0u ? 4096u : samples->capacity * 2u;
        uint64_t *next_values =
            (uint64_t *)realloc(samples->values, next_capacity * sizeof(uint64_t));
        if (!next_values) {
            return -1;
        }
        samples->values = next_values;
        samples->capacity = next_capacity;
    }

    samples->values[samples->used++] = value;
    return 0;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t lhs = *(const uint64_t *)a;
    uint64_t rhs = *(const uint64_t *)b;
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

static double percentile_micros(const struct latency_samples *samples,
                                LARGE_INTEGER freq,
                                double pct) {
    (void)freq; /* latency samples now use RDTSC ticks */
    if (!samples || samples->used == 0u) {
        return 0.0;
    }
    if (pct <= 0.0) {
        return tsc_ticks_to_micros(samples->values[0]);
    }
    if (pct >= 100.0) {
        return tsc_ticks_to_micros(samples->values[samples->used - 1u]);
    }

    size_t index = (size_t)((pct / 100.0) * (double)(samples->used - 1u));
    return tsc_ticks_to_micros(samples->values[index]);
}

static const char *profile_label(uint32_t profile) {
    switch (profile) {
        case NETIPC_PROFILE_SHM_WAITADDR:
            return "c-shm-waitaddr";
        case NETIPC_PROFILE_SHM_BUSYWAIT:
            return "c-shm-busywait";
        case NETIPC_PROFILE_SHM_HYBRID:
            return "c-shm-hybrid";
        case NETIPC_PROFILE_NAMED_PIPE:
            return "c-npipe";
        default:
            return "c-win-unknown";
    }
}

static struct netipc_named_pipe_config pipe_config(const char *run_dir, const char *service) {
    struct netipc_named_pipe_config config = {
        .run_dir = run_dir,
        .service_name = service,
        .supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", NETIPC_PROFILE_NAMED_PIPE),
        .preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", NETIPC_PROFILE_NAMED_PIPE),
        .auth_token = parse_env_u64("NETIPC_AUTH_TOKEN", 0u),
        .shm_spin_tries = parse_env_u32("NETIPC_SHM_SPIN_TRIES", NETIPC_SHM_HYBRID_DEFAULT_SPIN_TRIES),
    };
    return config;
}

static int protocol_error(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
}

static int server_once(const char *run_dir, const char *service) {
    struct netipc_named_pipe_config config = pipe_config(run_dir, service);
    netipc_named_pipe_server_t *server = NULL;
    uint64_t request_id = 0u;
    struct netipc_increment_request request;

    if (netipc_named_pipe_server_create(&config, &server) != 0) {
        perror("netipc_named_pipe_server_create");
        return 1;
    }

    if (netipc_named_pipe_server_accept(server, 10000u) != 0) {
        perror("netipc_named_pipe_server_accept");
        netipc_named_pipe_server_destroy(server);
        return 1;
    }

    if (netipc_named_pipe_server_receive_increment(server, &request_id, &request, 10000u) != 0) {
        perror("netipc_named_pipe_server_receive_increment");
        netipc_named_pipe_server_destroy(server);
        return 1;
    }

    struct netipc_increment_response response = {
        .status = NETIPC_STATUS_OK,
        .value = request.value + 1u,
    };
    if (netipc_named_pipe_server_send_increment(server, request_id, &response, 10000u) != 0) {
        perror("netipc_named_pipe_server_send_increment");
        netipc_named_pipe_server_destroy(server);
        return 1;
    }

    printf("C-WIN-SERVER request_id=%" PRIu64 " value=%" PRIu64
           " response=%" PRIu64 " profile=%u\n",
           request_id,
           request.value,
           response.value,
           netipc_named_pipe_server_negotiated_profile(server));

    netipc_named_pipe_server_destroy(server);
    return 0;
}

static int client_once(const char *run_dir, const char *service, uint64_t value) {
    struct netipc_named_pipe_config config = pipe_config(run_dir, service);
    netipc_named_pipe_client_t *client = NULL;
    struct netipc_increment_response response;

    if (netipc_named_pipe_client_create(&config, &client, 10000u) != 0) {
        perror("netipc_named_pipe_client_create");
        return 1;
    }

    struct netipc_increment_request request = {.value = value};
    if (netipc_named_pipe_client_call_increment(client, &request, &response, 10000u) != 0) {
        perror("netipc_named_pipe_client_call_increment");
        netipc_named_pipe_client_destroy(client);
        return 1;
    }

    if (response.status != NETIPC_STATUS_OK) {
        fprintf(stderr, "server returned status=%d\n", response.status);
        netipc_named_pipe_client_destroy(client);
        return 1;
    }
    if (response.value != value + 1u) {
        fprintf(stderr,
                "unexpected response value: got=%" PRIu64 " expected=%" PRIu64 "\n",
                response.value,
                value + 1u);
        netipc_named_pipe_client_destroy(client);
        return 1;
    }

    printf("C-WIN-CLIENT request=%" PRIu64 " response=%" PRIu64 " profile=%u\n",
           value,
           response.value,
           netipc_named_pipe_client_negotiated_profile(client));

    netipc_named_pipe_client_destroy(client);
    return 0;
}

static int server_loop(const char *run_dir, const char *service, uint64_t max_requests) {
    apply_cpu_affinity();
    struct netipc_named_pipe_config config = pipe_config(run_dir, service);
    netipc_named_pipe_server_t *server = NULL;

    if (netipc_named_pipe_server_create(&config, &server) != 0) {
        perror("netipc_named_pipe_server_create");
        return 1;
    }
    if (netipc_named_pipe_server_accept(server, 10000u) != 0) {
        perror("netipc_named_pipe_server_accept");
        netipc_named_pipe_server_destroy(server);
        return 1;
    }

    /* Begin CPU measurement from the moment the benchmark loop starts. */
    LARGE_INTEGER sfreq = performance_frequency();
    uint64_t s_start_ticks = performance_counter();
    double s_cpu_start = self_cpu_seconds();

    uint64_t handled = 0u;
    while (max_requests == 0u || handled < max_requests) {
        uint64_t request_id = 0u;
        struct netipc_increment_request request;
        struct netipc_increment_response response;

        if (netipc_named_pipe_server_receive_increment(server, &request_id, &request, 0u) != 0) {
            if (errno == EPIPE) {
                break;
            }
            perror("netipc_named_pipe_server_receive_increment");
            netipc_named_pipe_server_destroy(server);
            return 1;
        }

        response.status = NETIPC_STATUS_OK;
        response.value = request.value + 1u;
        if (netipc_named_pipe_server_send_increment(server, request_id, &response, 0u) != 0) {
            perror("netipc_named_pipe_server_send_increment");
            netipc_named_pipe_server_destroy(server);
            return 1;
        }
        handled++;
    }

    /* Report server CPU utilization so the client can pick it up. */
    double s_elapsed = counter_to_seconds(performance_counter() - s_start_ticks, sfreq);
    double s_cpu = s_elapsed <= 0.0 ? 0.0 : (self_cpu_seconds() - s_cpu_start) / s_elapsed;
    fprintf(stderr, "SERVER_CPU_CORES=%.3f\n", s_cpu);

    netipc_named_pipe_server_destroy(server);
    return 0;
}

static int client_bench(const char *run_dir,
                        const char *service,
                        int32_t duration_sec,
                        int32_t target_rps) {
    calibrate_tsc();
    apply_cpu_affinity();
    timeBeginPeriod(1u);
    if (duration_sec <= 0) {
        return protocol_error("duration_sec must be > 0");
    }
    if (target_rps < 0) {
        return protocol_error("target_rps must be >= 0");
    }

    struct netipc_named_pipe_config config = pipe_config(run_dir, service);
    netipc_named_pipe_client_t *client = NULL;
    if (netipc_named_pipe_client_create(&config, &client, 10000u) != 0) {
        perror("netipc_named_pipe_client_create");
        return 1;
    }

    LARGE_INTEGER freq = performance_frequency();
    uint64_t start_ticks = performance_counter();
    double cpu_start = self_cpu_seconds();

    /* Use QPC for loop termination (checked every N iterations to amortize ~4.5us cost).
     * RDTSC is used only for per-iteration latency measurement.
     * QPC is safe across core migrations; RDTSC alone is not. */
    uint64_t qpc_end = start_ticks + (uint64_t)((double)duration_sec * (double)freq.QuadPart);
    #define DEADLINE_CHECK_MASK 1023u

    /* Maximum plausible latency in TSC ticks (~100ms at TSC freq).
     * RDTSC can produce huge deltas on core migration; discard those. */
    double tsc_hz = 1e9 / g_tsc_ns_per_tick;
    uint64_t max_sane_tsc_delta = (uint64_t)(0.1 * tsc_hz);

    struct latency_samples samples = {0};
    uint64_t counter = 1u;
    uint64_t requests = 0u;
    uint64_t responses = 0u;
    uint64_t mismatches = 0u;
    uint64_t interval_ticks = 0u;
    uint64_t next_send_ticks = start_ticks;

    if (target_rps > 0) {
        interval_ticks = (uint64_t)((double)freq.QuadPart / (double)target_rps);
        if (interval_ticks == 0u) {
            interval_ticks = 1u;
        }
    }

    for (;;) {
        /* Check QPC deadline every 1024 iterations to amortize the ~4.5us cost */
        if ((requests & DEADLINE_CHECK_MASK) == 0u && requests > 0u) {
            if (performance_counter() >= qpc_end) {
                break;
            }
        }

        if (interval_ticks != 0u) {
            sleep_until(next_send_ticks, freq);
            next_send_ticks += interval_ticks;
        }

        struct netipc_increment_request request = {.value = counter};
        struct netipc_increment_response response;
        uint64_t send_start_tsc = rdtsc_now();

        requests++;
        if (netipc_named_pipe_client_call_increment(client, &request, &response, 0u) != 0) {
            perror("netipc_named_pipe_client_call_increment");
            free(samples.values);
            netipc_named_pipe_client_destroy(client);
            return 1;
        }

        if (response.status != NETIPC_STATUS_OK || response.value != counter + 1u) {
            mismatches++;
        }

        counter = response.value;
        responses++;

        uint64_t tsc_delta = rdtsc_now() - send_start_tsc;
        /* Discard insane deltas caused by cross-core TSC offset differences */
        if (tsc_delta < max_sane_tsc_delta) {
            if (latency_samples_push(&samples, tsc_delta) != 0) {
                fprintf(stderr, "failed to record latency sample\n");
                free(samples.values);
                netipc_named_pipe_client_destroy(client);
                return 1;
            }
        }
    }
    #undef DEADLINE_CHECK_MASK

    uint64_t elapsed_ticks = performance_counter() - start_ticks;
    double elapsed_sec = counter_to_seconds(elapsed_ticks, freq);
    double cpu_cores = elapsed_sec <= 0.0 ? 0.0 : (self_cpu_seconds() - cpu_start) / elapsed_sec;
    double throughput = elapsed_sec <= 0.0 ? 0.0 : (double)responses / elapsed_sec;
    const char *mode_label = profile_label(netipc_named_pipe_client_negotiated_profile(client));

    qsort(samples.values, samples.used, sizeof(uint64_t), compare_u64);
    double p50 = percentile_micros(&samples, freq, 50.0);
    double p95 = percentile_micros(&samples, freq, 95.0);
    double p99 = percentile_micros(&samples, freq, 99.0);

    printf("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,"
           "p50_us,p95_us,p99_us,client_cpu_cores\n");
    printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.3f\n",
           mode_label,
           duration_sec,
           target_rps,
           requests,
           responses,
           mismatches,
           throughput,
           p50,
           p95,
           p99,
           cpu_cores);

    free(samples.values);
    netipc_named_pipe_client_destroy(client);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "server-once") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }
        return server_once(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "client-once") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }
        return client_once(argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "server-loop") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }
        return server_loop(argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "client-bench") == 0) {
        if (argc != 6) {
            usage(argv[0]);
            return 2;
        }
        return client_bench(argv[2], argv[3], parse_i32(argv[4]), parse_i32(argv[5]));
    }

    usage(argv[0]);
    return 2;
}
