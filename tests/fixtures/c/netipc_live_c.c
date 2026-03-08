#include <netipc/netipc_named_pipe.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
    if (!samples || samples->used == 0u) {
        return 0.0;
    }
    if (pct <= 0.0) {
        return counter_to_micros(samples->values[0], freq);
    }
    if (pct >= 100.0) {
        return counter_to_micros(samples->values[samples->used - 1u], freq);
    }

    size_t index = (size_t)((pct / 100.0) * (double)(samples->used - 1u));
    return counter_to_micros(samples->values[index], freq);
}

static const char *profile_label(uint32_t profile) {
    switch (profile) {
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

    uint64_t handled = 0u;
    while (max_requests == 0u || handled < max_requests) {
        uint64_t request_id = 0u;
        struct netipc_increment_request request;
        struct netipc_increment_response response;

        if (netipc_named_pipe_server_receive_increment(server, &request_id, &request, 0u) != 0) {
            if (errno == EPIPE) {
                netipc_named_pipe_server_destroy(server);
                return 0;
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

    netipc_named_pipe_server_destroy(server);
    return 0;
}

static int client_bench(const char *run_dir,
                        const char *service,
                        int32_t duration_sec,
                        int32_t target_rps) {
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
    uint64_t end_ticks = start_ticks + (uint64_t)duration_sec * (uint64_t)freq.QuadPart;
    double cpu_start = self_cpu_seconds();

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

    while (performance_counter() < end_ticks) {
        if (interval_ticks != 0u) {
            sleep_until(next_send_ticks, freq);
            next_send_ticks += interval_ticks;
        }

        struct netipc_increment_request request = {.value = counter};
        struct netipc_increment_response response;
        uint64_t send_start_ticks = performance_counter();

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
        if (latency_samples_push(&samples, performance_counter() - send_start_ticks) != 0) {
            fprintf(stderr, "failed to record latency sample\n");
            free(samples.values);
            netipc_named_pipe_client_destroy(client);
            return 1;
        }
    }

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
