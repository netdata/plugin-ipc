#include <netipc/netipc_shm_hybrid.h>
#include <netipc/netipc_uds_seqpacket.h>

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct latency_samples {
    uint64_t *values;
    size_t used;
    size_t capacity;
};

struct bench_result {
    int32_t duration_sec;
    int32_t target_rps;
    uint64_t requests;
    uint64_t responses;
    uint64_t mismatches;
    double elapsed_sec;
    double throughput_rps;
    double p50_us;
    double p95_us;
    double p99_us;
    double client_cpu_cores;
};

struct uds_runtime_options {
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
};

struct transport_ops {
    const char *label;
    const char *server_tag;
    const char *client_tag;
    bool has_profile;
    int (*server_open)(const char *run_dir, const char *service, void **out_server);
    int (*server_receive_increment)(void *server,
                                    uint64_t *request_id,
                                    struct netipc_increment_request *request,
                                    uint32_t timeout_ms);
    int (*server_send_increment)(void *server,
                                 uint64_t request_id,
                                 const struct netipc_increment_response *response,
                                 uint32_t timeout_ms);
    uint32_t (*server_negotiated_profile)(const void *server);
    void (*server_destroy)(void *server);
    int (*client_open)(const char *run_dir, const char *service, void **out_client, uint32_t timeout_ms);
    int (*client_call_increment)(void *client,
                                 const struct netipc_increment_request *request,
                                 struct netipc_increment_response *response,
                                 uint32_t timeout_ms);
    uint32_t (*client_negotiated_profile)(const void *client);
    void (*client_destroy)(void *client);
};

static volatile sig_atomic_t g_bench_stop_requested = 0;
static struct uds_runtime_options g_uds_runtime_options = {
    .supported_profiles = NETIPC_PROFILE_UDS_SEQPACKET,
    .preferred_profiles = NETIPC_PROFILE_UDS_SEQPACKET,
    .auth_token = 0u,
};

static void usage(const char *argv0) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr,
            "  %s uds-server-once <run_dir> <service> [iterations|1] [supported_profiles] "
            "[preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-client-once <run_dir> <service> <value> [iterations|1] "
            "[supported_profiles] [preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-client-loop <run_dir> <service> <value> <iterations> "
            "[supported_profiles] [preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-server-loop <run_dir> <service> <max_requests|0> [supported_profiles] "
            "[preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-server-bench <run_dir> <service> <max_requests|0> [supported_profiles] "
            "[preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-client-bench <run_dir> <service> <duration_sec> <target_rps> "
            "[supported_profiles] [preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s uds-bench <run_dir> <service> <duration_sec> <target_rps> [supported_profiles] "
            "[preferred_profiles] [auth_token]\n",
            argv0);
    fprintf(stderr, "  %s shm-server-once <run_dir> <service>\n", argv0);
    fprintf(stderr, "  %s shm-client-once <run_dir> <service> <value>\n", argv0);
    fprintf(stderr, "  %s shm-server-loop <run_dir> <service> <max_requests|0>\n", argv0);
    fprintf(stderr, "  %s shm-server-bench <run_dir> <service> <max_requests|0>\n", argv0);
    fprintf(stderr, "  %s shm-client-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0);
    fprintf(stderr, "  %s shm-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0);
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

static uint32_t parse_u32(const char *s) {
    uint64_t v = parse_u64(s);
    if (v > UINT32_MAX) {
        fprintf(stderr, "value out of range for u32: %s\n", s);
        exit(2);
    }
    return (uint32_t)v;
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

static int protocol_error(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ns_to_seconds(uint64_t ns) {
    return (double)ns / 1e9;
}

static double ns_to_micros(uint64_t ns) {
    return (double)ns / 1e3;
}

static double self_cpu_seconds(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }

    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
           (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

static void reset_uds_runtime_options(void) {
    g_uds_runtime_options.supported_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    g_uds_runtime_options.preferred_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    g_uds_runtime_options.auth_token = 0u;
}

static int configure_uds_runtime_options(int argc,
                                         char **argv,
                                         int start_index,
                                         bool allow_iteration_placeholder) {
    int remaining = argc - start_index;

    reset_uds_runtime_options();
    if (remaining == 0) {
        return 0;
    }

    if (allow_iteration_placeholder && (remaining == 1 || remaining == 4)) {
        uint64_t iterations = parse_u64(argv[start_index]);
        if (iterations != 1u) {
            fprintf(stderr, "one-shot UDS commands only support iterations=1, got=%" PRIu64 "\n", iterations);
            return 1;
        }
        start_index++;
        remaining--;
        if (remaining == 0) {
            return 0;
        }
    }

    if (remaining != 3) {
        fprintf(stderr,
                "expected either no UDS overrides or supported_profiles preferred_profiles auth_token\n");
        return 1;
    }

    g_uds_runtime_options.supported_profiles = parse_u32(argv[start_index]);
    g_uds_runtime_options.preferred_profiles = parse_u32(argv[start_index + 1]);
    g_uds_runtime_options.auth_token = parse_u64(argv[start_index + 2]);
    return 0;
}

static void benchmark_stop_handler(int signum) {
    (void)signum;
    g_bench_stop_requested = 1;
}

static bool install_benchmark_stop_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = benchmark_stop_handler;
    sigemptyset(&action.sa_mask);

    return sigaction(SIGTERM, &action, NULL) == 0 && sigaction(SIGINT, &action, NULL) == 0;
}

static void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    sched_yield();
#endif
}

static void sleep_until_ns(uint64_t target_ns) {
    for (;;) {
        uint64_t now_ns = monotonic_ns();
        if (now_ns >= target_ns) {
            return;
        }

        uint64_t remaining_ns = target_ns - now_ns;
        if (remaining_ns > 2000000ull) {
            struct timespec req = {
                .tv_sec = 0,
                .tv_nsec = 1000000L,
            };
            nanosleep(&req, NULL);
        } else if (remaining_ns > 200000ull) {
            sched_yield();
        } else {
            cpu_relax();
        }
    }
}

static int latency_samples_push(struct latency_samples *samples, uint64_t value) {
    if (!samples) {
        return -1;
    }

    if (samples->used == samples->capacity) {
        size_t next_capacity = samples->capacity == 0u ? 4096u : samples->capacity * 2u;
        uint64_t *next_values = realloc(samples->values, next_capacity * sizeof(uint64_t));
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

static double percentile_micros(const struct latency_samples *samples, double pct) {
    if (!samples || samples->used == 0u) {
        return 0.0;
    }
    if (pct <= 0.0) {
        return ns_to_micros(samples->values[0]);
    }
    if (pct >= 100.0) {
        return ns_to_micros(samples->values[samples->used - 1u]);
    }

    size_t index = (size_t)((pct / 100.0) * (double)(samples->used - 1u));
    return ns_to_micros(samples->values[index]);
}

static struct netipc_uds_seqpacket_config uds_config(const char *run_dir, const char *service) {
    struct netipc_uds_seqpacket_config config = {
        .run_dir = run_dir,
        .service_name = service,
        .file_mode = 0600,
        .supported_profiles = g_uds_runtime_options.supported_profiles,
        .preferred_profiles = g_uds_runtime_options.preferred_profiles,
        .auth_token = g_uds_runtime_options.auth_token,
    };
    return config;
}

static struct netipc_shm_config shm_config(const char *run_dir, const char *service) {
    struct netipc_shm_config config = {
        .run_dir = run_dir,
        .service_name = service,
        .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
        .file_mode = 0600,
    };
    return config;
}

static bool build_bench_endpoint_path(const struct transport_ops *ops,
                                      const char *run_dir,
                                      const char *service,
                                      char *out_path,
                                      size_t out_size) {
    const char *suffix = ops && ops->has_profile ? ".sock" : ".ipcshm";
    int n = snprintf(out_path, out_size, "%s/%s%s", run_dir, service, suffix);
    return n > 0 && (size_t)n < out_size;
}

static bool wait_for_endpoint_path(const struct transport_ops *ops,
                                   const char *run_dir,
                                   const char *service,
                                   uint32_t timeout_ms) {
    char path[512];
    if (!build_bench_endpoint_path(ops, run_dir, service, path, sizeof(path))) {
        return false;
    }

    uint64_t start_ns = monotonic_ns();
    for (;;) {
        if (access(path, F_OK) == 0) {
            return true;
        }

        uint64_t now_ns = monotonic_ns();
        if (now_ns == 0u || now_ns - start_ns >= (uint64_t)timeout_ms * 1000000ull) {
            return false;
        }

        struct timespec req = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };
        nanosleep(&req, NULL);
    }
}

static int uds_server_open(const char *run_dir, const char *service, void **out_server) {
    netipc_uds_seqpacket_server_t *server = NULL;
    struct netipc_uds_seqpacket_config config = uds_config(run_dir, service);

    if (netipc_uds_seqpacket_server_create(&config, &server) != 0) {
        return -1;
    }
    if (netipc_uds_seqpacket_server_accept(server, 10000u) != 0) {
        int saved = errno;
        netipc_uds_seqpacket_server_destroy(server);
        errno = saved;
        return -1;
    }

    *out_server = server;
    return 0;
}

static int uds_server_receive_increment_wrapper(void *server,
                                                uint64_t *request_id,
                                                struct netipc_increment_request *request,
                                                uint32_t timeout_ms) {
    return netipc_uds_seqpacket_server_receive_increment(server, request_id, request, timeout_ms);
}

static int uds_server_send_increment_wrapper(void *server,
                                             uint64_t request_id,
                                             const struct netipc_increment_response *response,
                                             uint32_t timeout_ms) {
    return netipc_uds_seqpacket_server_send_increment(server, request_id, response, timeout_ms);
}

static uint32_t uds_server_negotiated_profile_wrapper(const void *server) {
    return netipc_uds_seqpacket_server_negotiated_profile(server);
}

static void uds_server_destroy_wrapper(void *server) {
    netipc_uds_seqpacket_server_destroy(server);
}

static int uds_client_open(const char *run_dir, const char *service, void **out_client, uint32_t timeout_ms) {
    netipc_uds_seqpacket_client_t *client = NULL;
    struct netipc_uds_seqpacket_config config = uds_config(run_dir, service);
    if (netipc_uds_seqpacket_client_create(&config, &client, timeout_ms) != 0) {
        return -1;
    }
    *out_client = client;
    return 0;
}

static int uds_client_call_increment_wrapper(void *client,
                                             const struct netipc_increment_request *request,
                                             struct netipc_increment_response *response,
                                             uint32_t timeout_ms) {
    return netipc_uds_seqpacket_client_call_increment(client, request, response, timeout_ms);
}

static uint32_t uds_client_negotiated_profile_wrapper(const void *client) {
    return netipc_uds_seqpacket_client_negotiated_profile(client);
}

static void uds_client_destroy_wrapper(void *client) {
    netipc_uds_seqpacket_client_destroy(client);
}

static int shm_server_open(const char *run_dir, const char *service, void **out_server) {
    netipc_shm_server_t *server = NULL;
    struct netipc_shm_config config = shm_config(run_dir, service);
    if (netipc_shm_server_create(&config, &server) != 0) {
        return -1;
    }
    *out_server = server;
    return 0;
}

static int shm_server_receive_increment_wrapper(void *server,
                                                uint64_t *request_id,
                                                struct netipc_increment_request *request,
                                                uint32_t timeout_ms) {
    return netipc_shm_server_receive_increment(server, request_id, request, timeout_ms);
}

static int shm_server_send_increment_wrapper(void *server,
                                             uint64_t request_id,
                                             const struct netipc_increment_response *response,
                                             uint32_t timeout_ms) {
    (void)timeout_ms;
    return netipc_shm_server_send_increment(server, request_id, response);
}

static void shm_server_destroy_wrapper(void *server) {
    netipc_shm_server_destroy(server);
}

static int shm_client_open(const char *run_dir, const char *service, void **out_client, uint32_t timeout_ms) {
    (void)timeout_ms;
    netipc_shm_client_t *client = NULL;
    struct netipc_shm_config config = shm_config(run_dir, service);
    if (netipc_shm_client_create(&config, &client) != 0) {
        return -1;
    }
    *out_client = client;
    return 0;
}

static int shm_client_call_increment_wrapper(void *client,
                                             const struct netipc_increment_request *request,
                                             struct netipc_increment_response *response,
                                             uint32_t timeout_ms) {
    return netipc_shm_client_call_increment(client, request, response, timeout_ms);
}

static void shm_client_destroy_wrapper(void *client) {
    netipc_shm_client_destroy(client);
}

static const struct transport_ops uds_ops = {
    .label = "c-uds",
    .server_tag = "C-UDS-SERVER",
    .client_tag = "C-UDS-CLIENT",
    .has_profile = true,
    .server_open = uds_server_open,
    .server_receive_increment = uds_server_receive_increment_wrapper,
    .server_send_increment = uds_server_send_increment_wrapper,
    .server_negotiated_profile = uds_server_negotiated_profile_wrapper,
    .server_destroy = uds_server_destroy_wrapper,
    .client_open = uds_client_open,
    .client_call_increment = uds_client_call_increment_wrapper,
    .client_negotiated_profile = uds_client_negotiated_profile_wrapper,
    .client_destroy = uds_client_destroy_wrapper,
};

static const struct transport_ops shm_ops = {
    .label = "c-shm-hybrid",
    .server_tag = "C-SHM-SERVER",
    .client_tag = "C-SHM-CLIENT",
    .has_profile = false,
    .server_open = shm_server_open,
    .server_receive_increment = shm_server_receive_increment_wrapper,
    .server_send_increment = shm_server_send_increment_wrapper,
    .server_negotiated_profile = NULL,
    .server_destroy = shm_server_destroy_wrapper,
    .client_open = shm_client_open,
    .client_call_increment = shm_client_call_increment_wrapper,
    .client_negotiated_profile = NULL,
    .client_destroy = shm_client_destroy_wrapper,
};

static bool is_disconnect_errno(int errnum) {
    return errnum == ECONNRESET || errnum == EPIPE || errnum == ENOTCONN;
}

static void print_bench_output_header(void) {
    printf("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,"
           "p50_us,p95_us,p99_us,client_cpu_cores,server_cpu_cores,total_cpu_cores\n");
}

static void print_bench_output_row(const char *label, const struct bench_result *result, double server_cpu_cores) {
    double total_cpu_cores = result->client_cpu_cores + server_cpu_cores;
    printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f\n",
           label,
           result->duration_sec,
           result->target_rps,
           result->requests,
           result->responses,
           result->mismatches,
           result->throughput_rps,
           result->p50_us,
           result->p95_us,
           result->p99_us,
           result->client_cpu_cores,
           server_cpu_cores,
           total_cpu_cores);
}

static void print_server_bench_output_row(const char *label,
                                          uint64_t handled_requests,
                                          double elapsed_sec,
                                          double server_cpu_cores) {
    printf("%s-server,%" PRIu64 ",%.6f,%.3f\n", label, handled_requests, elapsed_sec, server_cpu_cores);
}

static int run_server_once(const struct transport_ops *ops, const char *run_dir, const char *service) {
    void *server = NULL;
    uint64_t request_id = 0u;
    struct netipc_increment_request request;

    if (ops->server_open(run_dir, service, &server) != 0) {
        perror("server_open");
        return 1;
    }

    if (ops->server_receive_increment(server, &request_id, &request, 10000u) != 0) {
        perror("server_receive_increment");
        ops->server_destroy(server);
        return 1;
    }

    struct netipc_increment_response response = {
        .status = NETIPC_STATUS_OK,
        .value = request.value + 1u,
    };
    if (ops->server_send_increment(server, request_id, &response, 10000u) != 0) {
        perror("server_send_increment");
        ops->server_destroy(server);
        return 1;
    }

    if (ops->has_profile && ops->server_negotiated_profile) {
        printf("%s request_id=%" PRIu64 " value=%" PRIu64 " response=%" PRIu64 " profile=%u\n",
               ops->server_tag,
               request_id,
               request.value,
               response.value,
               ops->server_negotiated_profile(server));
    } else {
        printf("%s request_id=%" PRIu64 " value=%" PRIu64 " response=%" PRIu64 "\n",
               ops->server_tag,
               request_id,
               request.value,
               response.value);
    }

    ops->server_destroy(server);
    return 0;
}

static int run_client_iterations(const struct transport_ops *ops,
                                 const char *run_dir,
                                 const char *service,
                                 uint64_t value,
                                 uint64_t iterations) {
    void *client = NULL;

    if (ops->client_open(run_dir, service, &client, 10000u) != 0) {
        perror("client_open");
        return 1;
    }

    for (uint64_t i = 0; i < iterations; ++i) {
        struct netipc_increment_response response;
        struct netipc_increment_request request = {.value = value};
        if (ops->client_call_increment(client, &request, &response, 10000u) != 0) {
            perror("client_call_increment");
            ops->client_destroy(client);
            return 1;
        }

        if (response.status != NETIPC_STATUS_OK) {
            fprintf(stderr, "server returned status=%d\n", response.status);
            ops->client_destroy(client);
            return 1;
        }
        if (response.value != value + 1u) {
            fprintf(stderr,
                    "unexpected response value: got=%" PRIu64 " expected=%" PRIu64 "\n",
                    response.value,
                    value + 1u);
            ops->client_destroy(client);
            return 1;
        }

        if (ops->has_profile && ops->client_negotiated_profile) {
            printf("%s request=%" PRIu64 " response=%" PRIu64 " profile=%u\n",
                   ops->client_tag,
                   value,
                   response.value,
                   ops->client_negotiated_profile(client));
        } else {
            printf("%s request=%" PRIu64 " response=%" PRIu64 "\n",
                   ops->client_tag,
                   value,
                   response.value);
        }
        value = response.value;
    }

    ops->client_destroy(client);
    return 0;
}

static int run_client_once(const struct transport_ops *ops,
                           const char *run_dir,
                           const char *service,
                           uint64_t value) {
    return run_client_iterations(ops, run_dir, service, value, 1u);
}

static int run_server_loop_internal(const struct transport_ops *ops,
                                    const char *run_dir,
                                    const char *service,
                                    uint64_t max_requests,
                                    uint32_t receive_timeout_ms,
                                    bool allow_disconnect_exit,
                                    bool allow_signal_stop,
                                    uint64_t *out_handled_requests) {
    void *server = NULL;

    if (ops->server_open(run_dir, service, &server) != 0) {
        perror("server_open");
        return 1;
    }

    uint64_t handled = 0u;
    while (max_requests == 0u || handled < max_requests) {
        uint64_t request_id = 0u;
        struct netipc_increment_request request;
        struct netipc_increment_response response;

        if (allow_signal_stop && g_bench_stop_requested) {
            break;
        }

        if (ops->server_receive_increment(server, &request_id, &request, receive_timeout_ms) != 0) {
            int saved = errno;
            if (allow_signal_stop && g_bench_stop_requested && saved == ETIMEDOUT) {
                break;
            }
            if (saved == ETIMEDOUT) {
                continue;
            }
            if (allow_disconnect_exit && is_disconnect_errno(saved)) {
                break;
            }
            errno = saved;
            perror("server_receive_increment");
            ops->server_destroy(server);
            return 1;
        }

        response.status = NETIPC_STATUS_OK;
        response.value = request.value + 1u;
        if (ops->server_send_increment(server, request_id, &response, 0u) != 0) {
            int saved = errno;
            if (allow_disconnect_exit && is_disconnect_errno(saved)) {
                break;
            }
            errno = saved;
            perror("server_send_increment");
            ops->server_destroy(server);
            return 1;
        }
        handled++;
    }

    if (out_handled_requests) {
        *out_handled_requests = handled;
    }
    ops->server_destroy(server);
    return 0;
}

static int run_server_loop(const struct transport_ops *ops,
                           const char *run_dir,
                           const char *service,
                           uint64_t max_requests) {
    return run_server_loop_internal(ops, run_dir, service, max_requests, 0u, false, false, NULL);
}

static int run_server_bench(const struct transport_ops *ops,
                            const char *run_dir,
                            const char *service,
                            uint64_t max_requests) {
    uint64_t handled_requests = 0u;
    uint64_t start_ns = monotonic_ns();
    double cpu_start = self_cpu_seconds();
    int rc = run_server_loop_internal(ops,
                                      run_dir,
                                      service,
                                      max_requests,
                                      ops->has_profile ? 0u : 100u,
                                      ops->has_profile,
                                      !ops->has_profile,
                                      &handled_requests);
    uint64_t elapsed_ns = monotonic_ns() - start_ns;
    double elapsed_sec = ns_to_seconds(elapsed_ns);
    double server_cpu_cores = elapsed_sec > 0.0 ? (self_cpu_seconds() - cpu_start) / elapsed_sec : 0.0;
    if (rc == 0) {
        print_server_bench_output_row(ops->label, handled_requests, elapsed_sec, server_cpu_cores);
    }
    return rc;
}

static int run_client_bench_capture(const struct transport_ops *ops,
                                    const char *run_dir,
                                    const char *service,
                                    int32_t duration_sec,
                                    int32_t target_rps,
                                    struct bench_result *out_result) {
    if (duration_sec <= 0) {
        return protocol_error("duration_sec must be > 0");
    }
    if (target_rps < 0) {
        return protocol_error("target_rps must be >= 0");
    }
    if (!out_result) {
        return protocol_error("out_result must be provided");
    }
    memset(out_result, 0, sizeof(*out_result));

    void *client = NULL;
    if (ops->client_open(run_dir, service, &client, 10000u) != 0) {
        perror("client_open");
        return 1;
    }

    uint64_t start_ns = monotonic_ns();
    uint64_t end_ns = start_ns + (uint64_t)duration_sec * 1000000000ull;
    double cpu_start = self_cpu_seconds();

    struct latency_samples samples = {0};
    uint64_t counter = 1u;
    uint64_t requests = 0u;
    uint64_t responses = 0u;
    uint64_t interval_ns = 0u;
    uint64_t next_send_ns = start_ns;

    if (target_rps > 0) {
        interval_ns = 1000000000ull / (uint64_t)target_rps;
        if (interval_ns == 0u) {
            interval_ns = 1u;
        }
    }

    while (monotonic_ns() < end_ns) {
        if (interval_ns != 0u) {
            sleep_until_ns(next_send_ns);
            next_send_ns += interval_ns;
        }

        struct netipc_increment_request request = {.value = counter};
        struct netipc_increment_response response;
        uint64_t send_start_ns = monotonic_ns();

        requests++;
        if (ops->client_call_increment(client, &request, &response, 0u) != 0) {
            perror("client_call_increment");
            free(samples.values);
            ops->client_destroy(client);
            return 1;
        }

        if (response.status != NETIPC_STATUS_OK) {
            fprintf(stderr, "server returned status=%d during benchmark\n", response.status);
            free(samples.values);
            ops->client_destroy(client);
            return 1;
        }
        if (response.value != counter + 1u) {
            fprintf(stderr,
                    "benchmark counter mismatch: got=%" PRIu64 " expected=%" PRIu64 "\n",
                    response.value,
                    counter + 1u);
            free(samples.values);
            ops->client_destroy(client);
            return 1;
        }

        counter = response.value;
        responses++;
        if (latency_samples_push(&samples, monotonic_ns() - send_start_ns) != 0) {
            fprintf(stderr, "failed to record latency sample\n");
            free(samples.values);
            ops->client_destroy(client);
            return 1;
        }
    }

    uint64_t elapsed_ns = monotonic_ns() - start_ns;
    double elapsed_sec = ns_to_seconds(elapsed_ns);
    out_result->duration_sec = duration_sec;
    out_result->target_rps = target_rps;
    out_result->requests = requests;
    out_result->responses = responses;
    out_result->mismatches = 0u;
    out_result->elapsed_sec = elapsed_sec;
    out_result->client_cpu_cores = elapsed_sec <= 0.0 ? 0.0 : (self_cpu_seconds() - cpu_start) / elapsed_sec;
    out_result->throughput_rps = elapsed_sec <= 0.0 ? 0.0 : (double)responses / elapsed_sec;

    if (responses != requests) {
        fprintf(stderr, "benchmark request/response mismatch: requests=%" PRIu64 " responses=%" PRIu64 "\n", requests, responses);
        free(samples.values);
        ops->client_destroy(client);
        return 1;
    }
    if (counter != responses + 1u) {
        fprintf(stderr, "benchmark final counter mismatch: counter=%" PRIu64 " expected=%" PRIu64 "\n", counter, responses + 1u);
        free(samples.values);
        ops->client_destroy(client);
        return 1;
    }

    qsort(samples.values, samples.used, sizeof(uint64_t), compare_u64);
    out_result->p50_us = percentile_micros(&samples, 50.0);
    out_result->p95_us = percentile_micros(&samples, 95.0);
    out_result->p99_us = percentile_micros(&samples, 99.0);

    free(samples.values);
    ops->client_destroy(client);
    return 0;
}

static int run_client_bench(const struct transport_ops *ops,
                            const char *run_dir,
                            const char *service,
                            int32_t duration_sec,
                            int32_t target_rps) {
    struct bench_result result;
    if (run_client_bench_capture(ops, run_dir, service, duration_sec, target_rps, &result) != 0) {
        return 1;
    }

    print_bench_output_header();
    print_bench_output_row(ops->label, &result, 0.0);
    return 0;
}

static double rusage_cpu_seconds(const struct rusage *usage) {
    if (!usage) {
        return 0.0;
    }

    return (double)usage->ru_utime.tv_sec + (double)usage->ru_utime.tv_usec / 1e6 +
           (double)usage->ru_stime.tv_sec + (double)usage->ru_stime.tv_usec / 1e6;
}

static int wait_for_child_exit(pid_t child_pid, int *out_status, struct rusage *out_usage, uint32_t timeout_ms) {
    uint64_t start_ns = monotonic_ns();
    for (;;) {
        int status = 0;
        struct rusage usage;
        memset(&usage, 0, sizeof(usage));
        pid_t rc = wait4(child_pid, &status, WNOHANG, &usage);
        if (rc == child_pid) {
            if (out_status) {
                *out_status = status;
            }
            if (out_usage) {
                *out_usage = usage;
            }
            return 0;
        }
        if (rc < 0) {
            return -1;
        }

        uint64_t now_ns = monotonic_ns();
        if (now_ns == 0u || now_ns - start_ns >= (uint64_t)timeout_ms * 1000000ull) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct timespec req = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };
        nanosleep(&req, NULL);
    }
}

static int run_orchestrated_bench(const struct transport_ops *ops,
                                  const char *run_dir,
                                  const char *service,
                                  int32_t duration_sec,
                                  int32_t target_rps) {
    bool uses_signal_stop = !ops->has_profile;
    uint64_t child_start_ns = monotonic_ns();
    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        g_bench_stop_requested = 0;
        if (uses_signal_stop && !install_benchmark_stop_handlers()) {
            perror("install_benchmark_stop_handlers");
            _exit(1);
        }

        int rc = run_server_loop_internal(
            ops,
            run_dir,
            service,
            0u,
            uses_signal_stop ? 100u : 0u,
            ops->has_profile,
            uses_signal_stop,
            NULL);
        _exit(rc == 0 ? 0 : 1);
    }

    if (!wait_for_endpoint_path(ops, run_dir, service, 5000u)) {
        fprintf(stderr, "benchmark server endpoint was not created in time\n");
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        return 1;
    }

    struct bench_result result;
    if (run_client_bench_capture(ops, run_dir, service, duration_sec, target_rps, &result) != 0) {
        if (uses_signal_stop) {
            kill(child_pid, SIGTERM);
        }
        waitpid(child_pid, NULL, 0);
        return 1;
    }

    if (uses_signal_stop) {
        kill(child_pid, SIGTERM);
    }

    int status = 0;
    struct rusage child_usage;
    if (wait_for_child_exit(child_pid, &status, &child_usage, 5000u) != 0) {
        perror("wait_for_child_exit");
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        return 1;
    }

    double server_cpu_sec = rusage_cpu_seconds(&child_usage);
    if (server_cpu_sec < 0.0) {
        server_cpu_sec = 0.0;
    }
    uint64_t child_end_ns = monotonic_ns();
    double child_elapsed_sec = ns_to_seconds(child_end_ns - child_start_ns);
    double server_cpu_cores = child_elapsed_sec > 0.0 ? server_cpu_sec / child_elapsed_sec : 0.0;

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "benchmark server child exited abnormally\n");
        return 1;
    }

    print_bench_output_header();
    print_bench_output_row(ops->label, &result, server_cpu_cores);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "uds-server-once") == 0) {
        if (argc < 4 || argc > 8) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 4, true) != 0) {
            return 2;
        }
        return run_server_once(&uds_ops, argv[2], argv[3]);
    }
    if (strcmp(argv[1], "uds-client-once") == 0) {
        if (argc < 5 || argc > 9) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 5, true) != 0) {
            return 2;
        }
        return run_client_once(&uds_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "uds-client-loop") == 0) {
        if (argc != 6 && argc != 9) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 6, false) != 0) {
            return 2;
        }
        return run_client_iterations(&uds_ops, argv[2], argv[3], parse_u64(argv[4]), parse_u64(argv[5]));
    }
    if (strcmp(argv[1], "uds-server-loop") == 0) {
        if (argc != 5 && argc != 8) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 5, false) != 0) {
            return 2;
        }
        return run_server_loop(&uds_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "uds-server-bench") == 0) {
        if (argc != 5 && argc != 8) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 5, false) != 0) {
            return 2;
        }
        return run_server_bench(&uds_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "uds-client-bench") == 0) {
        if (argc != 6 && argc != 9) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 6, false) != 0) {
            return 2;
        }
        return run_client_bench(&uds_ops, argv[2], argv[3], parse_i32(argv[4]), parse_i32(argv[5]));
    }
    if (strcmp(argv[1], "uds-bench") == 0) {
        if (argc != 6 && argc != 9) {
            usage(argv[0]);
            return 2;
        }
        if (configure_uds_runtime_options(argc, argv, 6, false) != 0) {
            return 2;
        }
        return run_orchestrated_bench(&uds_ops, argv[2], argv[3], parse_i32(argv[4]), parse_i32(argv[5]));
    }
    if (strcmp(argv[1], "shm-server-once") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }
        return run_server_once(&shm_ops, argv[2], argv[3]);
    }
    if (strcmp(argv[1], "shm-client-once") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }
        return run_client_once(&shm_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "shm-server-loop") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }
        return run_server_loop(&shm_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "shm-server-bench") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }
        return run_server_bench(&shm_ops, argv[2], argv[3], parse_u64(argv[4]));
    }
    if (strcmp(argv[1], "shm-client-bench") == 0) {
        if (argc != 6) {
            usage(argv[0]);
            return 2;
        }
        return run_client_bench(&shm_ops, argv[2], argv[3], parse_i32(argv[4]), parse_i32(argv[5]));
    }
    if (strcmp(argv[1], "shm-bench") == 0) {
        if (argc != 6) {
            usage(argv[0]);
            return 2;
        }
        return run_orchestrated_bench(&shm_ops, argv[2], argv[3], parse_i32(argv[4]), parse_i32(argv[5]));
    }

    usage(argv[0]);
    return 2;
}
