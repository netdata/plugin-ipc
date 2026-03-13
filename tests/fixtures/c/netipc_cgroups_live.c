#include <netipc/netipc_cgroups_snapshot.h>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define NETIPC_CGROUPS_WINDOWS_RUNTIME 1
#include <netipc/netipc_named_pipe.h>
#else
#define NETIPC_CGROUPS_WINDOWS_RUNTIME 0
#include <netipc/netipc_uds_seqpacket.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if NETIPC_CGROUPS_WINDOWS_RUNTIME
#include <windows.h>
#else
#include <sys/resource.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

struct latency_samples {
    uint64_t *values;
    size_t used;
    size_t capacity;
};

static void usage(const char *argv0) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s server-once <service_namespace> <service> [auth_token]\n", argv0);
    fprintf(stderr, "  %s server-loop <service_namespace> <service> <max_requests|0> [auth_token]\n", argv0);
    fprintf(stderr, "  %s server-bench <service_namespace> <service> <max_requests|0> [auth_token]\n", argv0);
    fprintf(stderr,
            "  %s client-refresh-once <service_namespace> <service> <lookup_hash> <lookup_name> [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s client-refresh-loop <service_namespace> <service> <iterations> <lookup_hash> <lookup_name> [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s client-refresh-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]\n",
            argv0);
    fprintf(stderr,
            "  %s client-lookup-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]\n",
            argv0);
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

static uint32_t parse_env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    return parse_u32(value);
}

static uint32_t server_receive_timeout_ms(void) {
    const char *value = getenv("NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS");
    if (!value || value[0] == '\0') {
        return 10000u;
    }
    return parse_u32(value);
}

static int is_disconnect_error(int err) {
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    return err == EPIPE || err == ENOTCONN || err == EPROTO || err == ECONNRESET;
#else
    return err == EPIPE || err == ENOTCONN || err == EPROTO || err == ECONNRESET;
#endif
}

static int is_idle_timeout_error(int err) {
    return err == ETIMEDOUT;
}

static void init_client_config(struct netipc_cgroups_snapshot_client_config *config,
                               const char *service_namespace,
                               const char *service_name,
                               uint64_t auth_token) {
    netipc_cgroups_snapshot_client_config_init(config, service_namespace, service_name);
    config->supported_profiles =
        parse_env_u32("NETIPC_SUPPORTED_PROFILES", config->supported_profiles);
    config->preferred_profiles =
        parse_env_u32("NETIPC_PREFERRED_PROFILES", config->preferred_profiles);
    config->auth_token = auth_token;
}

static size_t request_message_capacity(void) {
    return netipc_msg_max_batch_total_size(NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN, 1u);
}

static size_t dummy_response_message_capacity(void) {
    return netipc_msg_max_batch_total_size(NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES, 2u);
}

#if NETIPC_CGROUPS_WINDOWS_RUNTIME
static LARGE_INTEGER performance_frequency(void) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

static uint64_t monotonic_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

static double ticks_to_seconds(uint64_t ticks, LARGE_INTEGER freq) {
    if (freq.QuadPart <= 0) {
        return 0.0;
    }
    return (double)ticks / (double)freq.QuadPart;
}

static double ticks_to_micros(uint64_t ticks, LARGE_INTEGER freq) {
    return ticks_to_seconds(ticks, freq) * 1e6;
}

static double self_cpu_seconds(void) {
    FILETIME create_time, exit_time, kernel_time, user_time;
    ULARGE_INTEGER kernel, user;

    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time, &user_time)) {
        return 0.0;
    }

    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    return (double)(kernel.QuadPart + user.QuadPart) / 10000000.0;
}

static void sleep_until_tick(uint64_t target_tick, LARGE_INTEGER freq) {
    for (;;) {
        uint64_t now_tick = monotonic_ticks();
        if (now_tick >= target_tick) {
            return;
        }

        double remaining_ns =
            ((double)(target_tick - now_tick) * 1000000000.0) / (double)freq.QuadPart;
        if (remaining_ns > 2000000.0) {
            Sleep(1u);
        } else if (remaining_ns > 200000.0) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}
#else
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double self_cpu_seconds(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
           (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

static void sleep_until_ns(uint64_t target_ns) {
    for (;;) {
        uint64_t now_ns = monotonic_ns();
        if (now_ns == 0u || now_ns >= target_ns) {
            return;
        }

        uint64_t remaining_ns = target_ns - now_ns;
        if (remaining_ns > 1000000ull) {
            struct timespec req = {
                .tv_sec = 0,
                .tv_nsec = (long)(remaining_ns > 10000000ull ? 1000000ull : remaining_ns),
            };
            nanosleep(&req, NULL);
        } else {
            sched_yield();
        }
    }
}
#endif

static int latency_samples_push(struct latency_samples *samples, uint64_t value) {
    if (!samples) {
        errno = EINVAL;
        return -1;
    }

    if (samples->used == samples->capacity) {
        size_t next_capacity = samples->capacity == 0u ? 4096u : samples->capacity * 2u;
        uint64_t *next = realloc(samples->values, next_capacity * sizeof(uint64_t));
        if (!next) {
            return -1;
        }
        samples->values = next;
        samples->capacity = next_capacity;
    }

    samples->values[samples->used++] = value;
    return 0;
}

static int compare_u64(const void *a, const void *b) {
    const uint64_t va = *(const uint64_t *)a;
    const uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static double percentile_micros(const struct latency_samples *samples, double pct
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                ,
                                LARGE_INTEGER freq
#endif
                                ) {
    size_t index;

    if (!samples || samples->used == 0u) {
        return 0.0;
    }

    index = (size_t)((pct / 100.0) * (double)(samples->used - 1u));
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    return ticks_to_micros(samples->values[index], freq);
#else
    return (double)samples->values[index] / 1000.0;
#endif
}

static void print_bench_header(void) {
    printf("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores\n");
}

static void print_bench_row(const char *mode,
                            int32_t duration_sec,
                            int32_t target_rps,
                            uint64_t requests,
                            uint64_t responses,
                            uint64_t mismatches,
                            double throughput_rps,
                            double p50_us,
                            double p95_us,
                            double p99_us,
                            double client_cpu_cores) {
    printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f\n",
           mode,
           duration_sec,
           target_rps,
           requests,
           responses,
           mismatches,
           throughput_rps,
           p50_us,
           p95_us,
           p99_us,
           client_cpu_cores);
}

#if NETIPC_CGROUPS_WINDOWS_RUNTIME
typedef netipc_named_pipe_server_t baseline_server_t;

static void init_server_config(struct netipc_named_pipe_config *config,
                               const char *service_namespace,
                               const char *service_name,
                               uint64_t auth_token) {
    memset(config, 0, sizeof(*config));
    config->run_dir = service_namespace;
    config->service_name = service_name;
    config->supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", NETIPC_PROFILE_NAMED_PIPE);
    config->preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", NETIPC_PROFILE_NAMED_PIPE);
    config->max_request_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
    config->max_request_batch_items = 1u;
    config->max_response_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config->max_response_batch_items = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config->auth_token = auth_token;
    config->shm_spin_tries = NETIPC_SHM_HYBRID_DEFAULT_SPIN_TRIES;
}

static int server_create_and_accept(const char *service_namespace,
                                    const char *service_name,
                                    uint64_t auth_token,
                                    baseline_server_t **out_server) {
    struct netipc_named_pipe_config config;
    init_server_config(&config, service_namespace, service_name, auth_token);
    if (netipc_named_pipe_server_create(&config, out_server) != 0) {
        return -1;
    }
    if (netipc_named_pipe_server_accept(*out_server, 10000u) != 0) {
        int saved = errno;
        netipc_named_pipe_server_destroy(*out_server);
        *out_server = NULL;
        errno = saved;
        return -1;
    }
    return 0;
}

static int server_receive_message(baseline_server_t *server,
                                  uint8_t *message,
                                  size_t capacity,
                                  size_t *out_len,
                                  uint32_t timeout_ms) {
    return netipc_named_pipe_server_receive_message(server, message, capacity, out_len, timeout_ms);
}

static int server_send_message(baseline_server_t *server,
                               const uint8_t *message,
                               size_t message_len) {
    return netipc_named_pipe_server_send_message(server, message, message_len, 10000u);
}

static void server_destroy(baseline_server_t *server) {
    netipc_named_pipe_server_destroy(server);
}
#else
typedef netipc_uds_seqpacket_server_t baseline_server_t;

static void init_server_config(struct netipc_uds_seqpacket_config *config,
                               const char *service_namespace,
                               const char *service_name,
                               uint64_t auth_token) {
    memset(config, 0, sizeof(*config));
    config->run_dir = service_namespace;
    config->service_name = service_name;
    config->file_mode = 0660u;
    config->supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", NETIPC_PROFILE_UDS_SEQPACKET);
    config->preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", NETIPC_PROFILE_UDS_SEQPACKET);
    config->max_request_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
    config->max_request_batch_items = 1u;
    config->max_response_payload_bytes = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config->max_response_batch_items = NETIPC_CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config->auth_token = auth_token;
}

static int server_create_and_accept(const char *service_namespace,
                                    const char *service_name,
                                    uint64_t auth_token,
                                    baseline_server_t **out_server) {
    struct netipc_uds_seqpacket_config config;
    init_server_config(&config, service_namespace, service_name, auth_token);
    if (netipc_uds_seqpacket_server_create(&config, out_server) != 0) {
        return -1;
    }
    if (netipc_uds_seqpacket_server_accept(*out_server, 10000u) != 0) {
        int saved = errno;
        netipc_uds_seqpacket_server_destroy(*out_server);
        *out_server = NULL;
        errno = saved;
        return -1;
    }
    return 0;
}

static int server_receive_message(baseline_server_t *server,
                                  uint8_t *message,
                                  size_t capacity,
                                  size_t *out_len,
                                  uint32_t timeout_ms) {
    return netipc_uds_seqpacket_server_receive_message(server, message, capacity, out_len, timeout_ms);
}

static int server_send_message(baseline_server_t *server,
                               const uint8_t *message,
                               size_t message_len) {
    return netipc_uds_seqpacket_server_send_message(server, message, message_len, 10000u);
}

static void server_destroy(baseline_server_t *server) {
    netipc_uds_seqpacket_server_destroy(server);
}
#endif

static int build_snapshot_response_message(uint64_t message_id,
                                           uint8_t *message,
                                           size_t message_capacity,
                                           size_t *out_message_len) {
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_builder builder;
    struct netipc_cgroups_snapshot_item item;
    size_t payload_len = 0u;

    if (!message || !out_message_len || message_capacity < NETIPC_MSG_HEADER_LEN) {
        errno = EINVAL;
        return -1;
    }

    if (netipc_cgroups_snapshot_builder_init(&builder,
                                             message + NETIPC_MSG_HEADER_LEN,
                                             message_capacity - NETIPC_MSG_HEADER_LEN,
                                             42u,
                                             1u,
                                             3u,
                                             2u) != 0) {
        return -1;
    }

    memset(&item, 0, sizeof(item));
    item.hash = 123u;
    item.options = 0x2u;
    item.enabled = 1u;
    item.name = "system.slice-nginx";
    item.name_len = (uint32_t)strlen(item.name);
    item.path = "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs";
    item.path_len = (uint32_t)strlen(item.path);
    if (netipc_cgroups_snapshot_builder_add_item(&builder, &item) != 0) {
        return -1;
    }

    item.hash = 456u;
    item.options = 0x4u;
    item.enabled = 0u;
    item.name = "docker-1234";
    item.name_len = (uint32_t)strlen(item.name);
    item.path = "";
    item.path_len = 0u;
    if (netipc_cgroups_snapshot_builder_add_item(&builder, &item) != 0) {
        return -1;
    }

    if (netipc_cgroups_snapshot_builder_finish(&builder, &payload_len) != 0) {
        return -1;
    }

    header.magic = NETIPC_MSG_MAGIC;
    header.version = NETIPC_MSG_VERSION;
    header.header_len = NETIPC_MSG_HEADER_LEN;
    header.kind = NETIPC_MSG_KIND_RESPONSE;
    header.flags = NETIPC_MSG_FLAG_BATCH;
    header.code = NETIPC_METHOD_CGROUPS_SNAPSHOT;
    header.transport_status = NETIPC_TRANSPORT_STATUS_OK;
    header.payload_len = (uint32_t)payload_len;
    header.item_count = 2u;
    header.message_id = message_id;
    if (netipc_encode_msg_header(message, message_capacity, &header) != 0) {
        return -1;
    }

    *out_message_len = NETIPC_MSG_HEADER_LEN + payload_len;
    return 0;
}

static int validate_snapshot_request(const uint8_t *message,
                                     size_t message_len,
                                     struct netipc_msg_header *header) {
    struct netipc_cgroups_snapshot_request_view request_view;

    if (netipc_decode_msg_header(message, message_len, header) != 0) {
        return -1;
    }

    if (header->kind != NETIPC_MSG_KIND_REQUEST ||
        header->flags != 0u ||
        header->code != NETIPC_METHOD_CGROUPS_SNAPSHOT ||
        header->transport_status != NETIPC_TRANSPORT_STATUS_OK ||
        header->item_count != 1u ||
        header->payload_len != NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN ||
        message_len != NETIPC_MSG_HEADER_LEN + NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN) {
        errno = EPROTO;
        return -1;
    }

    return netipc_decode_cgroups_snapshot_request_view(message + NETIPC_MSG_HEADER_LEN,
                                                       header->payload_len,
                                                       &request_view);
}

static void print_cache(const struct netipc_cgroups_snapshot_cache *cache) {
    uint32_t i;

    printf("CGROUPS_CACHE\t%" PRIu64 "\t%u\t%u\n",
           cache->generation,
           cache->systemd_enabled,
           cache->item_count);
    for (i = 0u; i < cache->item_count; ++i) {
        printf("ITEM\t%u\t%u\t%u\t%u\t%s\t%s\n",
               i,
               cache->items[i].hash,
               cache->items[i].options,
               cache->items[i].enabled,
               cache->items[i].name,
               cache->items[i].path);
    }
}

static int server_once(const char *service_namespace, const char *service_name, uint64_t auth_token) {
    baseline_server_t *server = NULL;
    uint8_t *request_message = NULL;
    uint8_t *response_message = NULL;
    struct netipc_msg_header header;
    size_t request_len = 0u;
    size_t response_len = 0u;
    size_t request_capacity = request_message_capacity();
    size_t response_capacity = dummy_response_message_capacity();

    if (server_create_and_accept(service_namespace, service_name, auth_token, &server) != 0) {
        return 1;
    }

    request_message = malloc(request_capacity);
    response_message = malloc(response_capacity);
    if (!request_message || !response_message) {
        perror("malloc");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    if (server_receive_message(server, request_message, request_capacity, &request_len, 10000u) != 0) {
        perror("server_receive_message");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    if (validate_snapshot_request(request_message, request_len, &header) != 0) {
        perror("validate_snapshot_request");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    if (build_snapshot_response_message(header.message_id,
                                        response_message,
                                        response_capacity,
                                        &response_len) != 0) {
        perror("build_snapshot_response_message");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    if (server_send_message(server, response_message, response_len) != 0) {
        perror("server_send_message");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    printf("CGROUPS_SERVER\t%" PRIu64 "\t2\n", header.message_id);
    free(response_message);
    free(request_message);
    server_destroy(server);
    return 0;
}

static int server_loop(const char *service_namespace,
                       const char *service_name,
                       uint64_t max_requests,
                       uint64_t auth_token) {
    baseline_server_t *server = NULL;
    uint8_t *request_message = NULL;
    uint8_t *response_message = NULL;
    uint64_t handled = 0u;
    size_t request_capacity = request_message_capacity();
    size_t response_capacity = dummy_response_message_capacity();

    if (server_create_and_accept(service_namespace, service_name, auth_token, &server) != 0) {
        return 1;
    }

    request_message = malloc(request_capacity);
    response_message = malloc(response_capacity);
    if (!request_message || !response_message) {
        perror("malloc");
        free(response_message);
        free(request_message);
        server_destroy(server);
        return 1;
    }

    while (max_requests == 0u || handled < max_requests) {
        struct netipc_msg_header header;
        size_t request_len = 0u;
        size_t response_len = 0u;

        if (server_receive_message(server,
                                   request_message,
                                   request_capacity,
                                   &request_len,
                                   server_receive_timeout_ms()) != 0) {
            if (is_disconnect_error(errno)) {
                break;
            }
            if (max_requests == 0u && handled > 0u && is_idle_timeout_error(errno)) {
                break;
            }
            perror("server_receive_message");
            free(response_message);
            free(request_message);
            server_destroy(server);
            return 1;
        }

        if (validate_snapshot_request(request_message, request_len, &header) != 0) {
            perror("validate_snapshot_request");
            free(response_message);
            free(request_message);
            server_destroy(server);
            return 1;
        }

        if (build_snapshot_response_message(header.message_id,
                                            response_message,
                                            response_capacity,
                                            &response_len) != 0) {
            perror("build_snapshot_response_message");
            free(response_message);
            free(request_message);
            server_destroy(server);
            return 1;
        }

        if (server_send_message(server, response_message, response_len) != 0) {
            perror("server_send_message");
            free(response_message);
            free(request_message);
            server_destroy(server);
            return 1;
        }
        handled++;
    }

    printf("CGROUPS_SERVER_LOOP\t%" PRIu64 "\n", handled);
    free(response_message);
    free(request_message);
    server_destroy(server);
    return 0;
}

static int client_refresh_once(const char *service_namespace,
                               const char *service_name,
                               uint32_t lookup_hash,
                               const char *lookup_name,
                               uint64_t auth_token) {
    struct netipc_cgroups_snapshot_client_config config;
    netipc_cgroups_snapshot_client_t *client = NULL;
    const struct netipc_cgroups_snapshot_cache *cache;
    const struct netipc_cgroups_snapshot_cache_item *item;

    init_client_config(&config, service_namespace, service_name, auth_token);
    if (netipc_cgroups_snapshot_client_create(&config, &client) != 0) {
        perror("netipc_cgroups_snapshot_client_create");
        return 1;
    }

    if (netipc_cgroups_snapshot_client_refresh(client, 10000u) != 0) {
        perror("netipc_cgroups_snapshot_client_refresh");
        netipc_cgroups_snapshot_client_destroy(client);
        return 1;
    }

    cache = netipc_cgroups_snapshot_client_cache(client);
    print_cache(cache);
    item = netipc_cgroups_snapshot_client_lookup(client, lookup_hash, lookup_name);
    if (item) {
        printf("LOOKUP\t%u\t%u\t%u\t%s\t%s\n",
               item->hash,
               item->options,
               item->enabled,
               item->name,
               item->path);
    } else {
        printf("LOOKUP_MISS\t%u\t%s\n", lookup_hash, lookup_name);
    }

    netipc_cgroups_snapshot_client_destroy(client);
    return 0;
}

static int client_refresh_loop(const char *service_namespace,
                               const char *service_name,
                               uint64_t iterations,
                               uint32_t lookup_hash,
                               const char *lookup_name,
                               uint64_t auth_token) {
    struct netipc_cgroups_snapshot_client_config config;
    netipc_cgroups_snapshot_client_t *client = NULL;
    const struct netipc_cgroups_snapshot_cache *cache;
    const struct netipc_cgroups_snapshot_cache_item *item;
    uint64_t i;

    init_client_config(&config, service_namespace, service_name, auth_token);
    if (netipc_cgroups_snapshot_client_create(&config, &client) != 0) {
        perror("netipc_cgroups_snapshot_client_create");
        return 1;
    }

    for (i = 0u; i < iterations; ++i) {
        if (netipc_cgroups_snapshot_client_refresh(client, 10000u) != 0) {
            perror("netipc_cgroups_snapshot_client_refresh");
            netipc_cgroups_snapshot_client_destroy(client);
            return 1;
        }
    }

    cache = netipc_cgroups_snapshot_client_cache(client);
    printf("REFRESHES\t%" PRIu64 "\n", iterations);
    print_cache(cache);
    item = netipc_cgroups_snapshot_client_lookup(client, lookup_hash, lookup_name);
    if (item) {
        printf("LOOKUP\t%u\t%u\t%u\t%s\t%s\n",
               item->hash,
               item->options,
               item->enabled,
               item->name,
               item->path);
    } else {
        printf("LOOKUP_MISS\t%u\t%s\n", lookup_hash, lookup_name);
    }

    netipc_cgroups_snapshot_client_destroy(client);
    return 0;
}

static int validate_lookup_item(netipc_cgroups_snapshot_client_t *client,
                                uint32_t lookup_hash,
                                const char *lookup_name) {
    const struct netipc_cgroups_snapshot_cache_item *item =
        netipc_cgroups_snapshot_client_lookup(client, lookup_hash, lookup_name);
    if (!item || item->hash != lookup_hash || strcmp(item->name, lookup_name) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int server_bench(const char *service_namespace,
                        const char *service_name,
                        uint64_t max_requests,
                        uint64_t auth_token) {
    double cpu_start = self_cpu_seconds();
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    LARGE_INTEGER freq = performance_frequency();
    uint64_t start_tick = monotonic_ticks();
    int rc = server_loop(service_namespace, service_name, max_requests, auth_token);
    uint64_t end_tick = monotonic_ticks();
    double elapsed_sec = ticks_to_seconds(end_tick - start_tick, freq);
#else
    uint64_t start_ns = monotonic_ns();
    int rc = server_loop(service_namespace, service_name, max_requests, auth_token);
    uint64_t end_ns = monotonic_ns();
    double elapsed_sec = end_ns > start_ns ? (double)(end_ns - start_ns) / 1e9 : 0.0;
#endif
    double server_cpu_cores =
        elapsed_sec > 0.0 ? (self_cpu_seconds() - cpu_start) / elapsed_sec : 0.0;
    fprintf(stderr, "SERVER_CPU_CORES=%.3f\n", server_cpu_cores);
    return rc;
}

static int client_refresh_bench(const char *service_namespace,
                                const char *service_name,
                                int32_t duration_sec,
                                int32_t target_rps,
                                uint32_t lookup_hash,
                                const char *lookup_name,
                                uint64_t auth_token) {
    struct netipc_cgroups_snapshot_client_config config;
    netipc_cgroups_snapshot_client_t *client = NULL;
    struct latency_samples samples = {0};
    double cpu_start;
    double elapsed_sec;
    uint64_t requests = 0u;
    uint64_t responses = 0u;
    int rc = 1;
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    LARGE_INTEGER freq = performance_frequency();
    uint64_t start_tick;
    uint64_t end_tick;
#else
    uint64_t start_ns;
    uint64_t end_ns;
#endif

    if (duration_sec <= 0 || target_rps < 0) {
        errno = EINVAL;
        return 1;
    }

    init_client_config(&config, service_namespace, service_name, auth_token);
    if (netipc_cgroups_snapshot_client_create(&config, &client) != 0) {
        perror("netipc_cgroups_snapshot_client_create");
        return 1;
    }

    cpu_start = self_cpu_seconds();
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    start_tick = monotonic_ticks();
    end_tick = start_tick + (uint64_t)freq.QuadPart * (uint64_t)duration_sec;
    while (target_rps <= 0 || monotonic_ticks() < end_tick) {
        uint64_t call_start_tick;
        uint64_t now_tick;
        if (target_rps > 0) {
            uint64_t due_tick = start_tick + ((requests * (uint64_t)freq.QuadPart) / (uint64_t)target_rps);
            sleep_until_tick(due_tick, freq);
            now_tick = monotonic_ticks();
            if (now_tick >= end_tick) {
                break;
            }
        } else if (monotonic_ticks() >= end_tick) {
            break;
        }

        requests++;
        call_start_tick = monotonic_ticks();
        if (netipc_cgroups_snapshot_client_refresh(client, 10000u) != 0) {
            perror("netipc_cgroups_snapshot_client_refresh");
            goto cleanup;
        }
        if (validate_lookup_item(client, lookup_hash, lookup_name) != 0) {
            perror("validate_lookup_item");
            goto cleanup;
        }
        if (latency_samples_push(&samples, monotonic_ticks() - call_start_tick) != 0) {
            perror("latency_samples_push");
            goto cleanup;
        }
        responses++;
    }
    elapsed_sec = ticks_to_seconds(monotonic_ticks() - start_tick, freq);
#else
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)duration_sec * 1000000000ull;
    while (target_rps <= 0 || monotonic_ns() < end_ns) {
        uint64_t call_start_ns;
        uint64_t now_ns;
        if (target_rps > 0) {
            uint64_t due_ns = start_ns + ((requests * 1000000000ull) / (uint64_t)target_rps);
            sleep_until_ns(due_ns);
            now_ns = monotonic_ns();
            if (now_ns == 0u || now_ns >= end_ns) {
                break;
            }
        } else if (monotonic_ns() >= end_ns) {
            break;
        }

        requests++;
        call_start_ns = monotonic_ns();
        if (netipc_cgroups_snapshot_client_refresh(client, 10000u) != 0) {
            perror("netipc_cgroups_snapshot_client_refresh");
            goto cleanup;
        }
        if (validate_lookup_item(client, lookup_hash, lookup_name) != 0) {
            perror("validate_lookup_item");
            goto cleanup;
        }
        if (latency_samples_push(&samples, monotonic_ns() - call_start_ns) != 0) {
            perror("latency_samples_push");
            goto cleanup;
        }
        responses++;
    }
    elapsed_sec = (double)(monotonic_ns() - start_ns) / 1e9;
#endif

    qsort(samples.values, samples.used, sizeof(uint64_t), compare_u64);
    print_bench_header();
    print_bench_row("c-cgroups-refresh",
                    duration_sec,
                    target_rps,
                    requests,
                    responses,
                    0u,
                    elapsed_sec > 0.0 ? (double)responses / elapsed_sec : 0.0,
                    percentile_micros(&samples, 50.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    percentile_micros(&samples, 95.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    percentile_micros(&samples, 99.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    elapsed_sec > 0.0 ? (self_cpu_seconds() - cpu_start) / elapsed_sec : 0.0);
    rc = 0;

cleanup:
    free(samples.values);
    netipc_cgroups_snapshot_client_destroy(client);
    return rc;
}

static int client_lookup_bench(const char *service_namespace,
                               const char *service_name,
                               int32_t duration_sec,
                               int32_t target_rps,
                               uint32_t lookup_hash,
                               const char *lookup_name,
                               uint64_t auth_token) {
    struct netipc_cgroups_snapshot_client_config config;
    netipc_cgroups_snapshot_client_t *client = NULL;
    struct latency_samples samples = {0};
    double cpu_start;
    double elapsed_sec;
    uint64_t requests = 0u;
    uint64_t responses = 0u;
    int rc = 1;
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    LARGE_INTEGER freq = performance_frequency();
    uint64_t start_tick;
    uint64_t end_tick;
#else
    uint64_t start_ns;
    uint64_t end_ns;
#endif

    if (duration_sec <= 0 || target_rps < 0) {
        errno = EINVAL;
        return 1;
    }

    init_client_config(&config, service_namespace, service_name, auth_token);
    if (netipc_cgroups_snapshot_client_create(&config, &client) != 0) {
        perror("netipc_cgroups_snapshot_client_create");
        return 1;
    }
    if (netipc_cgroups_snapshot_client_refresh(client, 10000u) != 0) {
        perror("netipc_cgroups_snapshot_client_refresh");
        goto cleanup;
    }
    if (validate_lookup_item(client, lookup_hash, lookup_name) != 0) {
        perror("validate_lookup_item");
        goto cleanup;
    }

    cpu_start = self_cpu_seconds();
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
    start_tick = monotonic_ticks();
    end_tick = start_tick + (uint64_t)freq.QuadPart * (uint64_t)duration_sec;
    while (target_rps <= 0 || monotonic_ticks() < end_tick) {
        uint64_t call_start_tick;
        uint64_t now_tick;
        if (target_rps > 0) {
            uint64_t due_tick = start_tick + ((requests * (uint64_t)freq.QuadPart) / (uint64_t)target_rps);
            sleep_until_tick(due_tick, freq);
            now_tick = monotonic_ticks();
            if (now_tick >= end_tick) {
                break;
            }
        } else if (monotonic_ticks() >= end_tick) {
            break;
        }

        requests++;
        call_start_tick = monotonic_ticks();
        if (validate_lookup_item(client, lookup_hash, lookup_name) != 0) {
            perror("validate_lookup_item");
            goto cleanup;
        }
        if (latency_samples_push(&samples, monotonic_ticks() - call_start_tick) != 0) {
            perror("latency_samples_push");
            goto cleanup;
        }
        responses++;
    }
    elapsed_sec = ticks_to_seconds(monotonic_ticks() - start_tick, freq);
#else
    start_ns = monotonic_ns();
    end_ns = start_ns + (uint64_t)duration_sec * 1000000000ull;
    while (target_rps <= 0 || monotonic_ns() < end_ns) {
        uint64_t call_start_ns;
        uint64_t now_ns;
        if (target_rps > 0) {
            uint64_t due_ns = start_ns + ((requests * 1000000000ull) / (uint64_t)target_rps);
            sleep_until_ns(due_ns);
            now_ns = monotonic_ns();
            if (now_ns == 0u || now_ns >= end_ns) {
                break;
            }
        } else if (monotonic_ns() >= end_ns) {
            break;
        }

        requests++;
        call_start_ns = monotonic_ns();
        if (validate_lookup_item(client, lookup_hash, lookup_name) != 0) {
            perror("validate_lookup_item");
            goto cleanup;
        }
        if (latency_samples_push(&samples, monotonic_ns() - call_start_ns) != 0) {
            perror("latency_samples_push");
            goto cleanup;
        }
        responses++;
    }
    elapsed_sec = (double)(monotonic_ns() - start_ns) / 1e9;
#endif

    qsort(samples.values, samples.used, sizeof(uint64_t), compare_u64);
    print_bench_header();
    print_bench_row("c-cgroups-lookup",
                    duration_sec,
                    target_rps,
                    requests,
                    responses,
                    0u,
                    elapsed_sec > 0.0 ? (double)responses / elapsed_sec : 0.0,
                    percentile_micros(&samples, 50.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    percentile_micros(&samples, 95.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    percentile_micros(&samples, 99.0
#if NETIPC_CGROUPS_WINDOWS_RUNTIME
                                      , freq
#endif
                                      ),
                    elapsed_sec > 0.0 ? (self_cpu_seconds() - cpu_start) / elapsed_sec : 0.0);
    rc = 0;

cleanup:
    free(samples.values);
    netipc_cgroups_snapshot_client_destroy(client);
    return rc;
}

int main(int argc, char **argv) {
    uint64_t auth_token = 0u;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "server-once") == 0) {
        if (argc != 4 && argc != 5) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 5) {
            auth_token = parse_u64(argv[4]);
        }
        return server_once(argv[2], argv[3], auth_token);
    }

    if (strcmp(argv[1], "server-loop") == 0) {
        if (argc != 5 && argc != 6) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 6) {
            auth_token = parse_u64(argv[5]);
        }
        return server_loop(argv[2], argv[3], parse_u64(argv[4]), auth_token);
    }

    if (strcmp(argv[1], "server-bench") == 0) {
        if (argc != 5 && argc != 6) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 6) {
            auth_token = parse_u64(argv[5]);
        }
        return server_bench(argv[2], argv[3], parse_u64(argv[4]), auth_token);
    }

    if (strcmp(argv[1], "client-refresh-once") == 0) {
        if (argc != 6 && argc != 7) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 7) {
            auth_token = parse_u64(argv[6]);
        }
        return client_refresh_once(argv[2], argv[3], parse_u32(argv[4]), argv[5], auth_token);
    }

    if (strcmp(argv[1], "client-refresh-loop") == 0) {
        if (argc != 7 && argc != 8) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 8) {
            auth_token = parse_u64(argv[7]);
        }
        return client_refresh_loop(argv[2],
                                   argv[3],
                                   parse_u64(argv[4]),
                                   parse_u32(argv[5]),
                                   argv[6],
                                   auth_token);
    }

    if (strcmp(argv[1], "client-refresh-bench") == 0) {
        if (argc != 8 && argc != 9) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 9) {
            auth_token = parse_u64(argv[8]);
        }
        return client_refresh_bench(argv[2],
                                    argv[3],
                                    (int32_t)parse_u32(argv[4]),
                                    (int32_t)parse_u32(argv[5]),
                                    parse_u32(argv[6]),
                                    argv[7],
                                    auth_token);
    }

    if (strcmp(argv[1], "client-lookup-bench") == 0) {
        if (argc != 8 && argc != 9) {
            usage(argv[0]);
            return 2;
        }
        if (argc == 9) {
            auth_token = parse_u64(argv[8]);
        }
        return client_lookup_bench(argv[2],
                                   argv[3],
                                   (int32_t)parse_u32(argv[4]),
                                   (int32_t)parse_u32(argv[5]),
                                   parse_u32(argv[6]),
                                   argv[7],
                                   auth_token);
    }

    usage(argv[0]);
    return 2;
}
