#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IPC_MAGIC 0x49504331u
#define IPC_VERSION 1u
#define MSG_KIND_REQUEST 1u
#define MSG_KIND_RESPONSE 2u

#define MAX_CLIENTS 256
#define MAX_PAYLOADS 16
#define MAX_TRANSPORTS 8
#define MAX_MODES 4
#define MAX_PATH_LEN 108
#define LAT_HIST_BINS 512
#define DEFAULT_DURATION_SEC 5
#define DEFAULT_PIPELINE_DEPTH 16

#define SHM_MAX_CLIENTS 256
#define SHM_MAX_SLOTS 256
#ifndef SHM_HYBRID_SPIN_TRIES
#define SHM_HYBRID_SPIN_TRIES 20
#endif

#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

enum transport_kind {
    TRANSPORT_STREAM = 0,
    TRANSPORT_SEQPACKET,
    TRANSPORT_DGRAM,
    TRANSPORT_SHM_SPIN,
    TRANSPORT_SHM_SEM,
    TRANSPORT_SHM_HYBRID,
    TRANSPORT_COUNT
};

enum benchmark_mode {
    MODE_PINGPONG = 0,
    MODE_PIPELINE,
    MODE_COUNT
};

enum shm_sync_mode {
    SHM_SYNC_SPIN = 0,
    SHM_SYNC_SEM = 1,
    SHM_SYNC_HYBRID = 2
};

struct benchmark_case {
    enum transport_kind transport;
    enum benchmark_mode mode;
    int clients;
    size_t payload_size;
    int duration_sec;
    int pipeline_depth;
    int target_rps;
};

struct benchmark_options {
    enum transport_kind transports[MAX_TRANSPORTS];
    int transport_count;
    enum benchmark_mode modes[MAX_MODES];
    int mode_count;
    int clients[MAX_CLIENTS];
    int client_count;
    size_t payloads[MAX_PAYLOADS];
    int payload_count;
    int duration_sec;
    int pipeline_depth;
    int target_rps;
    bool quiet;
};

struct wire_message {
    uint16_t kind;
    uint16_t flags;
    uint32_t payload_len;
    uint64_t client_id;
    uint64_t seq;
    uint64_t counter;
};

struct client_result {
    int client_index;
    int pid;
    uint64_t requests_sent;
    uint64_t responses_received;
    uint64_t mismatches;
    uint64_t last_counter;
    uint64_t latency_hist[LAT_HIST_BINS];
    double cpu_user_sec;
    double cpu_sys_sec;
    double wall_sec;
};

struct server_result {
    int pid;
    uint64_t requests_processed;
    uint64_t responses_sent;
    double cpu_user_sec;
    double cpu_sys_sec;
};

struct benchmark_result {
    struct benchmark_case bench;
    uint64_t total_requests;
    uint64_t total_responses;
    uint64_t total_mismatches;
    uint64_t last_counter_max;
    uint64_t merged_hist[LAT_HIST_BINS];
    double p50_us;
    double p95_us;
    double p99_us;
    double throughput_rps;
    double client_cpu_inproc_cores;
    double server_cpu_inproc_cores;
    double client_cpu_external_cores;
    double server_cpu_external_cores;
    double elapsed_sec;
    uint64_t requests_processed_server;
    uint64_t responses_sent_server;
    int failed_clients;
};

struct pid_cpu_tracker {
    pid_t pid;
    bool active;
    bool initialized;
    uint64_t prev_ticks;
    uint64_t accum_ticks;
};

struct cpu_sampler {
    struct pid_cpu_tracker trackers[1 + MAX_CLIENTS];
    int tracker_count;
    bool initialized;
    uint64_t prev_total_ticks;
    uint64_t accum_total_ticks;
    struct timespec start_ts;
    struct timespec end_ts;
};

struct shm_ring_meta {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    sem_t items;
    sem_t spaces;
};

struct shm_channel_meta {
    _Atomic uint32_t connected;
    struct shm_ring_meta req;
    struct shm_ring_meta resp;
};

struct shm_region {
    uint32_t num_clients;
    uint32_t slots;
    uint32_t payload_size;
    uint32_t mode; /* enum shm_sync_mode */
    _Atomic uint32_t stop;
    sem_t global_requests;
    struct shm_channel_meta channels[];
};

struct shm_layout {
    struct shm_region *region;
    size_t total_size;
};

struct transport_context {
    enum transport_kind kind;
    char socket_path[MAX_PATH_LEN];
    struct shm_layout shm;
};

struct server_thread_args {
    int conn_fd;
    size_t payload_size;
    _Atomic uint64_t *requests_processed;
    _Atomic uint64_t *responses_sent;
};

struct client_start_sync {
    int read_fd;
};

static volatile sig_atomic_t g_server_stop = 0;

static void server_signal_handler(int signo) {
    (void)signo;
    g_server_stop = 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double time_diff_sec(const struct timespec *a, const struct timespec *b) {
    time_t sec = b->tv_sec - a->tv_sec;
    long nsec = b->tv_nsec - a->tv_nsec;
    return (double)sec + (double)nsec / 1e9;
}

static void sleep_until_ns(uint64_t target_ns) {
#if defined(TIMER_ABSTIME)
    struct timespec ts = {
        .tv_sec = (time_t)(target_ns / 1000000000ull),
        .tv_nsec = (long)(target_ns % 1000000000ull),
    };

    for (;;) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        if (rc == 0 || rc == ETIMEDOUT) {
            return;
        }
        if (rc != EINTR) {
            return;
        }
    }
#else
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= target_ns) {
            return;
        }

        uint64_t diff = target_ns - now;
        uint64_t sleep_ns = diff;
        if (diff > 2000000ull) {
            sleep_ns = diff - 200000ull;
        } else if (diff > 200000ull) {
            sleep_ns = diff - 50000ull;
        }

        struct timespec ts;
        ts.tv_sec = (time_t)(sleep_ns / 1000000000ull);
        ts.tv_nsec = (long)(sleep_ns % 1000000000ull);
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR) {
                return;
            }
        }
    }
#endif
}

static uint64_t htole64_u64(uint64_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

static uint32_t htole32_u32(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap32(v);
#endif
}

static uint16_t htole16_u16(uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap16(v);
#endif
}

static uint64_t le64toh_u64(uint64_t v) { return htole64_u64(v); }
static uint32_t le32toh_u32(uint32_t v) { return htole32_u32(v); }
static uint16_t le16toh_u16(uint16_t v) { return htole16_u16(v); }

static void write_u16_le(uint8_t *dst, uint16_t v) {
    uint16_t t = htole16_u16(v);
    memcpy(dst, &t, sizeof(t));
}

static void write_u32_le(uint8_t *dst, uint32_t v) {
    uint32_t t = htole32_u32(v);
    memcpy(dst, &t, sizeof(t));
}

static void write_u64_le(uint8_t *dst, uint64_t v) {
    uint64_t t = htole64_u64(v);
    memcpy(dst, &t, sizeof(t));
}

static uint16_t read_u16_le(const uint8_t *src) {
    uint16_t t;
    memcpy(&t, src, sizeof(t));
    return le16toh_u16(t);
}

static uint32_t read_u32_le(const uint8_t *src) {
    uint32_t t;
    memcpy(&t, src, sizeof(t));
    return le32toh_u32(t);
}

static uint64_t read_u64_le(const uint8_t *src) {
    uint64_t t;
    memcpy(&t, src, sizeof(t));
    return le64toh_u64(t);
}

static int encode_message(uint8_t *buf, size_t buf_len, const struct wire_message *msg) {
    const size_t header_len = 40;
    if (buf_len < header_len) {
        return -1;
    }
    if (msg->payload_len != buf_len) {
        return -1;
    }

    write_u32_le(buf + 0, IPC_MAGIC);
    write_u16_le(buf + 4, IPC_VERSION);
    write_u16_le(buf + 6, msg->kind);
    write_u32_le(buf + 8, msg->flags);
    write_u32_le(buf + 12, msg->payload_len);
    write_u64_le(buf + 16, msg->client_id);
    write_u64_le(buf + 24, msg->seq);
    write_u64_le(buf + 32, msg->counter);
    return 0;
}

static int decode_message(const uint8_t *buf, size_t buf_len, struct wire_message *msg) {
    const size_t header_len = 40;
    if (buf_len < header_len) {
        return -1;
    }

    uint32_t magic = read_u32_le(buf + 0);
    uint16_t version = read_u16_le(buf + 4);
    if (magic != IPC_MAGIC || version != IPC_VERSION) {
        return -1;
    }

    msg->kind = read_u16_le(buf + 6);
    msg->flags = (uint16_t)(read_u32_le(buf + 8) & 0xffffu);
    msg->payload_len = read_u32_le(buf + 12);
    msg->client_id = read_u64_le(buf + 16);
    msg->seq = read_u64_le(buf + 24);
    msg->counter = read_u64_le(buf + 32);

    if (msg->payload_len != buf_len) {
        return -1;
    }
    return 0;
}

static uint32_t latency_bin_from_ns(uint64_t ns) {
    if (ns == 0) {
        return 0;
    }

    int l = 63 - __builtin_clzll(ns);
    if (l < 0) {
        l = 0;
    }
    if (l > 63) {
        l = 63;
    }

    uint64_t base = 1ull << l;
    uint64_t offset = ns - base;
    uint32_t sub = (uint32_t)((offset * 8ull) / base);
    if (sub > 7) {
        sub = 7;
    }

    uint32_t idx = (uint32_t)l * 8u + sub;
    if (idx >= LAT_HIST_BINS) {
        idx = LAT_HIST_BINS - 1;
    }
    return idx;
}

static uint64_t latency_bin_mid_ns(uint32_t idx) {
    uint32_t l = idx / 8u;
    uint32_t sub = idx % 8u;
    if (l >= 63) {
        return UINT64_MAX / 2;
    }
    uint64_t base = 1ull << l;
    uint64_t span = base / 8ull;
    uint64_t start = base + (uint64_t)sub * span;
    return start + span / 2ull;
}

static double histogram_percentile_us(const uint64_t *hist, uint64_t total, double q) {
    if (total == 0) {
        return 0.0;
    }

    uint64_t target = (uint64_t)ceil((double)total * q);
    if (target == 0) {
        target = 1;
    }

    uint64_t seen = 0;
    for (uint32_t i = 0; i < LAT_HIST_BINS; i++) {
        seen += hist[i];
        if (seen >= target) {
            return (double)latency_bin_mid_ns(i) / 1000.0;
        }
    }

    return (double)latency_bin_mid_ns(LAT_HIST_BINS - 1) / 1000.0;
}

static ssize_t read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t nread = 0;
    while (nread < len) {
        ssize_t rc = read(fd, p + nread, len - nread);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        nread += (size_t)rc;
    }
    return (ssize_t)nread;
}

static ssize_t write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t nwritten = 0;
    while (nwritten < len) {
        ssize_t rc = write(fd, p + nwritten, len - nwritten);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        nwritten += (size_t)rc;
    }
    return (ssize_t)nwritten;
}

static int make_unix_listener(const char *path, int sock_type) {
    int fd = socket(AF_UNIX, sock_type, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET) {
        if (listen(fd, 1024) < 0) {
            close(fd);
            return -1;
        }
    }

    return fd;
}

static int connect_unix_socket(const char *path, int sock_type) {
    int fd = socket(AF_UNIX, sock_type, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int send_socket_message(int fd, enum transport_kind kind, const uint8_t *buf, size_t len) {
    if (kind == TRANSPORT_STREAM) {
        return write_full(fd, buf, len) == (ssize_t)len ? 0 : -1;
    }

    ssize_t n = send(fd, buf, len, 0);
    return n == (ssize_t)len ? 0 : -1;
}

static int recv_socket_message(int fd, enum transport_kind kind, uint8_t *buf, size_t len) {
    if (kind == TRANSPORT_STREAM) {
        ssize_t n = read_full(fd, buf, len);
        return n == (ssize_t)len ? 0 : -1;
    }

    ssize_t n = recv(fd, buf, len, 0);
    return n == (ssize_t)len ? 0 : -1;
}

static int parse_csv_ints(const char *s, int *out, int max_items) {
    char *copy = strdup(s);
    if (!copy) {
        return -1;
    }

    int count = 0;
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok && count < max_items) {
        int v = atoi(tok);
        if (v <= 0) {
            free(copy);
            return -1;
        }
        out[count++] = v;
        tok = strtok_r(NULL, ",", &save);
    }
    free(copy);
    return count;
}

static int parse_csv_sizes(const char *s, size_t *out, int max_items) {
    char *copy = strdup(s);
    if (!copy) {
        return -1;
    }

    int count = 0;
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok && count < max_items) {
        long long v = atoll(tok);
        if (v <= 0 || v > (1 << 20)) {
            free(copy);
            return -1;
        }
        out[count++] = (size_t)v;
        tok = strtok_r(NULL, ",", &save);
    }
    free(copy);
    return count;
}

static const char *transport_name(enum transport_kind t) {
    switch (t) {
        case TRANSPORT_STREAM:
            return "uds-stream";
        case TRANSPORT_SEQPACKET:
            return "uds-seqpacket";
        case TRANSPORT_DGRAM:
            return "uds-dgram";
        case TRANSPORT_SHM_SPIN:
            return "shm-spin";
        case TRANSPORT_SHM_SEM:
            return "shm-sem";
        case TRANSPORT_SHM_HYBRID:
            return "shm-hybrid";
        default:
            return "unknown";
    }
}

static const char *mode_name(enum benchmark_mode m) {
    switch (m) {
        case MODE_PINGPONG:
            return "pingpong";
        case MODE_PIPELINE:
            return "pipeline";
        default:
            return "unknown";
    }
}

static bool parse_transport(const char *s, enum transport_kind *out) {
    if (strcmp(s, "stream") == 0 || strcmp(s, "uds-stream") == 0) {
        *out = TRANSPORT_STREAM;
        return true;
    }
    if (strcmp(s, "seqpacket") == 0 || strcmp(s, "uds-seqpacket") == 0) {
        *out = TRANSPORT_SEQPACKET;
        return true;
    }
    if (strcmp(s, "dgram") == 0 || strcmp(s, "uds-dgram") == 0) {
        *out = TRANSPORT_DGRAM;
        return true;
    }
    if (strcmp(s, "shm-spin") == 0) {
        *out = TRANSPORT_SHM_SPIN;
        return true;
    }
    if (strcmp(s, "shm-sem") == 0) {
        *out = TRANSPORT_SHM_SEM;
        return true;
    }
    if (strcmp(s, "shm-hybrid") == 0) {
        *out = TRANSPORT_SHM_HYBRID;
        return true;
    }
    return false;
}

static bool parse_mode(const char *s, enum benchmark_mode *out) {
    if (strcmp(s, "pingpong") == 0) {
        *out = MODE_PINGPONG;
        return true;
    }
    if (strcmp(s, "pipeline") == 0) {
        *out = MODE_PIPELINE;
        return true;
    }
    return false;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --transport all|stream|seqpacket|dgram|shm-spin|shm-sem|shm-hybrid\n"
            "  --mode both|pingpong|pipeline\n"
            "  --clients 1,8,64,256\n"
            "  --payloads 32,256,4096,65536\n"
            "  --duration 5\n"
            "  --pipeline-depth 16\n"
            "  --target-rps 100000   (per client, 0 = unlimited)\n"
            "  --quiet\n",
            argv0);
}

static void options_set_defaults(struct benchmark_options *opts) {
    memset(opts, 0, sizeof(*opts));

    opts->transports[0] = TRANSPORT_STREAM;
    opts->transports[1] = TRANSPORT_SEQPACKET;
    opts->transports[2] = TRANSPORT_DGRAM;
    opts->transports[3] = TRANSPORT_SHM_SPIN;
    opts->transports[4] = TRANSPORT_SHM_SEM;
    opts->transports[5] = TRANSPORT_SHM_HYBRID;
    opts->transport_count = 6;

    opts->modes[0] = MODE_PINGPONG;
    opts->modes[1] = MODE_PIPELINE;
    opts->mode_count = 2;

    opts->clients[0] = 1;
    opts->clients[1] = 8;
    opts->clients[2] = 64;
    opts->clients[3] = 256;
    opts->client_count = 4;

    opts->payloads[0] = 32;
    opts->payloads[1] = 256;
    opts->payloads[2] = 4096;
    opts->payloads[3] = 65536;
    opts->payload_count = 4;

    opts->duration_sec = DEFAULT_DURATION_SEC;
    opts->pipeline_depth = DEFAULT_PIPELINE_DEPTH;
    opts->target_rps = 0;
    opts->quiet = false;
}

static int parse_options(int argc, char **argv, struct benchmark_options *opts) {
    options_set_defaults(opts);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--transport") == 0) {
            if (i + 1 >= argc) return -1;
            const char *v = argv[++i];
            opts->transport_count = 0;
            if (strcmp(v, "all") == 0) {
                opts->transports[opts->transport_count++] = TRANSPORT_STREAM;
                opts->transports[opts->transport_count++] = TRANSPORT_SEQPACKET;
                opts->transports[opts->transport_count++] = TRANSPORT_DGRAM;
                opts->transports[opts->transport_count++] = TRANSPORT_SHM_SPIN;
                opts->transports[opts->transport_count++] = TRANSPORT_SHM_SEM;
                opts->transports[opts->transport_count++] = TRANSPORT_SHM_HYBRID;
            } else {
                enum transport_kind t;
                if (!parse_transport(v, &t)) return -1;
                opts->transports[opts->transport_count++] = t;
            }
            continue;
        }

        if (strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) return -1;
            const char *v = argv[++i];
            opts->mode_count = 0;
            if (strcmp(v, "both") == 0) {
                opts->modes[opts->mode_count++] = MODE_PINGPONG;
                opts->modes[opts->mode_count++] = MODE_PIPELINE;
            } else {
                enum benchmark_mode m;
                if (!parse_mode(v, &m)) return -1;
                opts->modes[opts->mode_count++] = m;
            }
            continue;
        }

        if (strcmp(argv[i], "--clients") == 0) {
            if (i + 1 >= argc) return -1;
            int count = parse_csv_ints(argv[++i], opts->clients, MAX_CLIENTS);
            if (count <= 0) return -1;
            opts->client_count = count;
            continue;
        }

        if (strcmp(argv[i], "--payloads") == 0) {
            if (i + 1 >= argc) return -1;
            int count = parse_csv_sizes(argv[++i], opts->payloads, MAX_PAYLOADS);
            if (count <= 0) return -1;
            opts->payload_count = count;
            continue;
        }

        if (strcmp(argv[i], "--duration") == 0) {
            if (i + 1 >= argc) return -1;
            opts->duration_sec = atoi(argv[++i]);
            if (opts->duration_sec <= 0 || opts->duration_sec > 120) return -1;
            continue;
        }

        if (strcmp(argv[i], "--pipeline-depth") == 0) {
            if (i + 1 >= argc) return -1;
            opts->pipeline_depth = atoi(argv[++i]);
            if (opts->pipeline_depth <= 0 || opts->pipeline_depth > 4096) return -1;
            continue;
        }

        if (strcmp(argv[i], "--target-rps") == 0) {
            if (i + 1 >= argc) return -1;
            opts->target_rps = atoi(argv[++i]);
            if (opts->target_rps < 0 || opts->target_rps > 100000000) return -1;
            continue;
        }

        if (strcmp(argv[i], "--quiet") == 0) {
            opts->quiet = true;
            continue;
        }

        return -1;
    }

    return 0;
}

static bool read_proc_total_ticks(uint64_t *ticks) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return false;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    if (strncmp(line, "cpu ", 4) != 0) {
        return false;
    }

    uint64_t vals[10] = {0};
    int n = sscanf(line + 4,
                   "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   (unsigned long long *)&vals[0],
                   (unsigned long long *)&vals[1],
                   (unsigned long long *)&vals[2],
                   (unsigned long long *)&vals[3],
                   (unsigned long long *)&vals[4],
                   (unsigned long long *)&vals[5],
                   (unsigned long long *)&vals[6],
                   (unsigned long long *)&vals[7],
                   (unsigned long long *)&vals[8],
                   (unsigned long long *)&vals[9]);
    if (n < 4) {
        return false;
    }

    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += vals[i];
    }
    *ticks = sum;
    return true;
}

static bool read_proc_pid_ticks(pid_t pid, uint64_t *ticks) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    char *rparen = strrchr(buf, ')');
    if (!rparen || *(rparen + 1) != ' ') {
        return false;
    }

    char *p = rparen + 2;
    int field = 3;
    char *save = NULL;
    uint64_t utime = 0;
    uint64_t stime = 0;

    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 14) {
            utime = strtoull(tok, NULL, 10);
        } else if (field == 15) {
            stime = strtoull(tok, NULL, 10);
            break;
        }
    }

    *ticks = utime + stime;
    return true;
}

static int connect_unix_dgram_client(const char *server_path, char *client_path, size_t client_path_len, int client_index) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;

    snprintf(client_path, client_path_len, "/tmp/ipc-bench-c-%d-%d.sock", getpid(), client_index);
    unlink(client_path);
    snprintf(local_addr.sun_path, sizeof(local_addr.sun_path), "%s", client_path);

    if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
        close(fd);
        unlink(client_path);
        return -1;
    }

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    snprintf(server_addr.sun_path, sizeof(server_addr.sun_path), "%s", server_path);

    if (connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        close(fd);
        unlink(client_path);
        return -1;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void cpu_sampler_init(struct cpu_sampler *sampler, pid_t server_pid, const pid_t *client_pids, int num_clients) {
    memset(sampler, 0, sizeof(*sampler));
    sampler->tracker_count = 1 + num_clients;
    sampler->trackers[0].pid = server_pid;
    sampler->trackers[0].active = true;

    for (int i = 0; i < num_clients; i++) {
        sampler->trackers[1 + i].pid = client_pids[i];
        sampler->trackers[1 + i].active = true;
    }

    clock_gettime(CLOCK_MONOTONIC, &sampler->start_ts);
}

static void cpu_sampler_tick(struct cpu_sampler *sampler) {
    uint64_t total_ticks = 0;
    if (!read_proc_total_ticks(&total_ticks)) {
        return;
    }

    if (!sampler->initialized) {
        sampler->initialized = true;
        sampler->prev_total_ticks = total_ticks;
        for (int i = 0; i < sampler->tracker_count; i++) {
            uint64_t proc_ticks = 0;
            if (sampler->trackers[i].active && read_proc_pid_ticks(sampler->trackers[i].pid, &proc_ticks)) {
                sampler->trackers[i].prev_ticks = proc_ticks;
                sampler->trackers[i].initialized = true;
            }
        }
        return;
    }

    if (total_ticks > sampler->prev_total_ticks) {
        sampler->accum_total_ticks += (total_ticks - sampler->prev_total_ticks);
        sampler->prev_total_ticks = total_ticks;
    }

    for (int i = 0; i < sampler->tracker_count; i++) {
        if (!sampler->trackers[i].active) {
            continue;
        }

        uint64_t proc_ticks = 0;
        if (!read_proc_pid_ticks(sampler->trackers[i].pid, &proc_ticks)) {
            sampler->trackers[i].active = false;
            continue;
        }

        if (!sampler->trackers[i].initialized) {
            sampler->trackers[i].prev_ticks = proc_ticks;
            sampler->trackers[i].initialized = true;
            continue;
        }

        if (proc_ticks >= sampler->trackers[i].prev_ticks) {
            sampler->trackers[i].accum_ticks += (proc_ticks - sampler->trackers[i].prev_ticks);
        }
        sampler->trackers[i].prev_ticks = proc_ticks;
    }
}

static void cpu_sampler_finish(struct cpu_sampler *sampler) {
    cpu_sampler_tick(sampler);
    clock_gettime(CLOCK_MONOTONIC, &sampler->end_ts);
}

static double cpu_sampler_pid_cores(const struct cpu_sampler *sampler, pid_t pid) {
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        return 0.0;
    }

    double elapsed = time_diff_sec(&sampler->start_ts, &sampler->end_ts);
    if (elapsed <= 0.0) {
        return 0.0;
    }

    for (int i = 0; i < sampler->tracker_count; i++) {
        if (sampler->trackers[i].pid == pid) {
            double cpu_sec = (double)sampler->trackers[i].accum_ticks / (double)hz;
            return cpu_sec / elapsed;
        }
    }

    return 0.0;
}

static struct shm_layout shm_layout_create(int num_clients, int slots, size_t payload_size, enum shm_sync_mode sync_mode) {
    struct shm_layout layout;
    memset(&layout, 0, sizeof(layout));

    size_t channel_meta_size = sizeof(struct shm_channel_meta) * (size_t)num_clients;
    size_t ring_data_size = (size_t)num_clients * (size_t)slots * payload_size;
    size_t total_size = sizeof(struct shm_region) + channel_meta_size + ring_data_size * 2;

    void *mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return layout;
    }

    memset(mem, 0, total_size);
    layout.region = (struct shm_region *)mem;
    layout.total_size = total_size;

    layout.region->num_clients = (uint32_t)num_clients;
    layout.region->slots = (uint32_t)slots;
    layout.region->payload_size = (uint32_t)payload_size;
    layout.region->mode = (uint32_t)sync_mode;
    atomic_store(&layout.region->stop, 0);

    bool use_sem = sync_mode != SHM_SYNC_SPIN;
    if (use_sem) {
        sem_init(&layout.region->global_requests, 1, 0);
    }

    for (int i = 0; i < num_clients; i++) {
        struct shm_channel_meta *ch = &layout.region->channels[i];
        atomic_store(&ch->connected, 0);
        atomic_store(&ch->req.head, 0);
        atomic_store(&ch->req.tail, 0);
        atomic_store(&ch->resp.head, 0);
        atomic_store(&ch->resp.tail, 0);
        if (use_sem) {
            sem_init(&ch->req.items, 1, 0);
            sem_init(&ch->req.spaces, 1, (unsigned int)slots);
            sem_init(&ch->resp.items, 1, 0);
            sem_init(&ch->resp.spaces, 1, (unsigned int)slots);
        }
    }

    return layout;
}

static void shm_layout_destroy(struct shm_layout *layout) {
    if (!layout || !layout->region) {
        return;
    }

    bool use_sem = layout->region->mode != SHM_SYNC_SPIN;
    if (use_sem) {
        sem_destroy(&layout->region->global_requests);
    }

    for (uint32_t i = 0; i < layout->region->num_clients; i++) {
        struct shm_channel_meta *ch = &layout->region->channels[i];
        if (use_sem) {
            sem_destroy(&ch->req.items);
            sem_destroy(&ch->req.spaces);
            sem_destroy(&ch->resp.items);
            sem_destroy(&ch->resp.spaces);
        }
    }

    munmap(layout->region, layout->total_size);
    layout->region = NULL;
    layout->total_size = 0;
}

static uint8_t *shm_req_buffer(struct shm_region *r) {
    return (uint8_t *)(&r->channels[r->num_clients]);
}

static uint8_t *shm_resp_buffer(struct shm_region *r) {
    uint8_t *req = shm_req_buffer(r);
    size_t req_size = (size_t)r->num_clients * (size_t)r->slots * (size_t)r->payload_size;
    return req + req_size;
}

static uint8_t *shm_req_slot(struct shm_region *r, int client_idx, uint32_t slot_idx) {
    size_t offset = ((size_t)client_idx * r->slots + slot_idx) * (size_t)r->payload_size;
    return shm_req_buffer(r) + offset;
}

static uint8_t *shm_resp_slot(struct shm_region *r, int client_idx, uint32_t slot_idx) {
    size_t offset = ((size_t)client_idx * r->slots + slot_idx) * (size_t)r->payload_size;
    return shm_resp_buffer(r) + offset;
}

static void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    sched_yield();
#endif
}

static inline bool shm_uses_semaphore_mode(const struct shm_region *r) {
    return r->mode == SHM_SYNC_SEM || r->mode == SHM_SYNC_HYBRID;
}

static inline bool shm_is_hybrid_mode(const struct shm_region *r) {
    return r->mode == SHM_SYNC_HYBRID;
}

static int shm_client_send(struct shm_region *r, int client_idx, const uint8_t *msg) {
    struct shm_channel_meta *ch = &r->channels[client_idx];
    uint32_t slots = r->slots;

    if (shm_uses_semaphore_mode(r)) {
        bool acquired = false;
        if (shm_is_hybrid_mode(r)) {
            for (int i = 0; i < SHM_HYBRID_SPIN_TRIES; i++) {
                if (sem_trywait(&ch->req.spaces) == 0) {
                    acquired = true;
                    break;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    return -1;
                }
                cpu_relax();
            }
        }

        if (!acquired) {
            if (sem_wait(&ch->req.spaces) != 0) {
                return -1;
            }
        }

        uint32_t head = atomic_fetch_add(&ch->req.head, 1);
        uint32_t slot = head % slots;
        memcpy(shm_req_slot(r, client_idx, slot), msg, r->payload_size);
        sem_post(&ch->req.items);
        sem_post(&r->global_requests);
        return 0;
    }

    for (;;) {
        uint32_t head = atomic_load_explicit(&ch->req.head, memory_order_acquire);
        uint32_t tail = atomic_load_explicit(&ch->req.tail, memory_order_acquire);
        if ((head - tail) < slots) {
            uint32_t slot = head % slots;
            memcpy(shm_req_slot(r, client_idx, slot), msg, r->payload_size);
            atomic_store_explicit(&ch->req.head, head + 1, memory_order_release);
            return 0;
        }
        cpu_relax();
    }
}

static int shm_client_recv(struct shm_region *r, int client_idx, uint8_t *msg) {
    struct shm_channel_meta *ch = &r->channels[client_idx];
    uint32_t slots = r->slots;

    if (shm_uses_semaphore_mode(r)) {
        bool acquired = false;
        if (shm_is_hybrid_mode(r)) {
            for (int i = 0; i < SHM_HYBRID_SPIN_TRIES; i++) {
                if (sem_trywait(&ch->resp.items) == 0) {
                    acquired = true;
                    break;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    return -1;
                }
                cpu_relax();
            }
        }

        if (!acquired) {
            if (sem_wait(&ch->resp.items) != 0) {
                return -1;
            }
        }

        uint32_t tail = atomic_fetch_add(&ch->resp.tail, 1);
        uint32_t slot = tail % slots;
        memcpy(msg, shm_resp_slot(r, client_idx, slot), r->payload_size);
        sem_post(&ch->resp.spaces);
        return 0;
    }

    for (;;) {
        uint32_t head = atomic_load_explicit(&ch->resp.head, memory_order_acquire);
        uint32_t tail = atomic_load_explicit(&ch->resp.tail, memory_order_acquire);
        if (head != tail) {
            uint32_t slot = tail % slots;
            memcpy(msg, shm_resp_slot(r, client_idx, slot), r->payload_size);
            atomic_store_explicit(&ch->resp.tail, tail + 1, memory_order_release);
            return 0;
        }
        if (atomic_load_explicit(&r->stop, memory_order_acquire)) {
            return -1;
        }
        cpu_relax();
    }
}

static int shm_server_process_one(struct shm_region *r, int client_idx, _Atomic uint64_t *reqs, _Atomic uint64_t *resps) {
    struct shm_channel_meta *ch = &r->channels[client_idx];
    uint32_t slots = r->slots;

    if (shm_uses_semaphore_mode(r)) {
        if (sem_trywait(&ch->req.items) != 0) {
            if (errno == EAGAIN) {
                return 0;
            }
            return -1;
        }

        uint32_t req_tail = atomic_fetch_add(&ch->req.tail, 1);
        uint32_t req_slot = req_tail % slots;

        uint8_t *req_buf = shm_req_slot(r, client_idx, req_slot);
        struct wire_message msg;
        if (decode_message(req_buf, r->payload_size, &msg) != 0) {
            sem_post(&ch->req.spaces);
            return -1;
        }
        sem_post(&ch->req.spaces);

        msg.kind = MSG_KIND_RESPONSE;
        msg.counter += 1;
        encode_message(req_buf, r->payload_size, &msg);

        if (sem_wait(&ch->resp.spaces) != 0) {
            return -1;
        }
        uint32_t resp_head = atomic_fetch_add(&ch->resp.head, 1);
        uint32_t resp_slot = resp_head % slots;
        memcpy(shm_resp_slot(r, client_idx, resp_slot), req_buf, r->payload_size);
        sem_post(&ch->resp.items);

        atomic_fetch_add(reqs, 1);
        atomic_fetch_add(resps, 1);
        return 1;
    }

    uint32_t req_head = atomic_load_explicit(&ch->req.head, memory_order_acquire);
    uint32_t req_tail = atomic_load_explicit(&ch->req.tail, memory_order_acquire);
    if (req_head == req_tail) {
        return 0;
    }

    uint32_t in_slot = req_tail % slots;
    uint8_t *in_buf = shm_req_slot(r, client_idx, in_slot);

    struct wire_message msg;
    if (decode_message(in_buf, r->payload_size, &msg) != 0) {
        return -1;
    }

    atomic_store_explicit(&ch->req.tail, req_tail + 1, memory_order_release);

    msg.kind = MSG_KIND_RESPONSE;
    msg.counter += 1;
    encode_message(in_buf, r->payload_size, &msg);

    for (;;) {
        uint32_t resp_head = atomic_load_explicit(&ch->resp.head, memory_order_acquire);
        uint32_t resp_tail = atomic_load_explicit(&ch->resp.tail, memory_order_acquire);
        if ((resp_head - resp_tail) < slots) {
            uint32_t out_slot = resp_head % slots;
            memcpy(shm_resp_slot(r, client_idx, out_slot), in_buf, r->payload_size);
            atomic_store_explicit(&ch->resp.head, resp_head + 1, memory_order_release);
            atomic_fetch_add(reqs, 1);
            atomic_fetch_add(resps, 1);
            return 1;
        }
        cpu_relax();
    }
}

static int server_worker_socket(struct server_thread_args *args) {
    uint8_t *buf = (uint8_t *)malloc(args->payload_size);
    if (!buf) {
        return -1;
    }

    while (!g_server_stop) {
        if (read_full(args->conn_fd, buf, args->payload_size) != (ssize_t)args->payload_size) {
            break;
        }

        struct wire_message msg;
        if (decode_message(buf, args->payload_size, &msg) != 0 || msg.kind != MSG_KIND_REQUEST) {
            break;
        }

        msg.kind = MSG_KIND_RESPONSE;
        msg.counter += 1;

        if (encode_message(buf, args->payload_size, &msg) != 0) {
            break;
        }

        if (write_full(args->conn_fd, buf, args->payload_size) != (ssize_t)args->payload_size) {
            break;
        }

        atomic_fetch_add(args->requests_processed, 1);
        atomic_fetch_add(args->responses_sent, 1);
    }

    free(buf);
    close(args->conn_fd);
    return 0;
}

static void *server_socket_thread_main(void *arg) {
    struct server_thread_args *args = (struct server_thread_args *)arg;
    server_worker_socket(args);
    free(args);
    return NULL;
}

static int run_server_socket(enum transport_kind kind,
                             const struct benchmark_case *bench,
                             const struct transport_context *ctx,
                             int ready_fd,
                             int stats_fd) {
    int sock_type = SOCK_STREAM;
    if (kind == TRANSPORT_SEQPACKET) {
        sock_type = SOCK_SEQPACKET;
    } else if (kind == TRANSPORT_DGRAM) {
        sock_type = SOCK_DGRAM;
    }

    int listen_fd = make_unix_listener(ctx->socket_path, sock_type);
    if (listen_fd < 0) {
        return -1;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    uint8_t ready = 1;
    if (write_full(ready_fd, &ready, 1) != 1) {
        close(listen_fd);
        return -1;
    }
    close(ready_fd);

    _Atomic uint64_t requests_processed = 0;
    _Atomic uint64_t responses_sent = 0;

    if (kind == TRANSPORT_DGRAM) {
        uint8_t *buf = (uint8_t *)malloc(bench->payload_size);
        if (!buf) {
            close(listen_fd);
            return -1;
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (!g_server_stop) {
            struct sockaddr_un peer;
            socklen_t peer_len = sizeof(peer);
            ssize_t n = recvfrom(listen_fd, buf, bench->payload_size, 0, (struct sockaddr *)&peer, &peer_len);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((size_t)n != bench->payload_size) {
                continue;
            }

            struct wire_message msg;
            if (decode_message(buf, bench->payload_size, &msg) != 0 || msg.kind != MSG_KIND_REQUEST) {
                continue;
            }

            msg.kind = MSG_KIND_RESPONSE;
            msg.counter += 1;
            if (encode_message(buf, bench->payload_size, &msg) != 0) {
                continue;
            }

            ssize_t sn = sendto(listen_fd, buf, bench->payload_size, 0, (struct sockaddr *)&peer, peer_len);
            if (sn == (ssize_t)bench->payload_size) {
                atomic_fetch_add(&requests_processed, 1);
                atomic_fetch_add(&responses_sent, 1);
            }
        }

        free(buf);
    } else {
        int flags = fcntl(listen_fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
        }

        pthread_t threads[MAX_CLIENTS];
        int thread_count = 0;

        while (!g_server_stop) {
            int conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
                    nanosleep(&ts, NULL);
                    continue;
                }
                break;
            }

            struct server_thread_args *args = (struct server_thread_args *)calloc(1, sizeof(*args));
            if (!args) {
                close(conn_fd);
                continue;
            }
            args->conn_fd = conn_fd;
            args->payload_size = bench->payload_size;
            args->requests_processed = &requests_processed;
            args->responses_sent = &responses_sent;

            if (thread_count >= MAX_CLIENTS || pthread_create(&threads[thread_count], NULL, server_socket_thread_main, args) != 0) {
                free(args);
                close(conn_fd);
                continue;
            }
            thread_count++;
        }

        for (int i = 0; i < thread_count; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    close(listen_fd);
    unlink(ctx->socket_path);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    struct server_result sr;
    memset(&sr, 0, sizeof(sr));
    sr.pid = getpid();
    sr.requests_processed = atomic_load(&requests_processed);
    sr.responses_sent = atomic_load(&responses_sent);
    sr.cpu_user_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
    sr.cpu_sys_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;

    write_full(stats_fd, &sr, sizeof(sr));
    close(stats_fd);
    return 0;
}

static int run_server_shm(const struct benchmark_case *bench,
                          const struct transport_context *ctx,
                          int ready_fd,
                          int stats_fd) {
    (void)bench;

    uint8_t ready = 1;
    if (write_full(ready_fd, &ready, 1) != 1) {
        return -1;
    }
    close(ready_fd);

    struct shm_region *r = ctx->shm.region;
    _Atomic uint64_t requests_processed = 0;
    _Atomic uint64_t responses_sent = 0;

    while (!g_server_stop && !atomic_load_explicit(&r->stop, memory_order_acquire)) {
        int work_done = 0;

        if (shm_uses_semaphore_mode(r)) {
            bool acquired = false;
            if (shm_is_hybrid_mode(r)) {
                for (int i = 0; i < SHM_HYBRID_SPIN_TRIES; i++) {
                    if (sem_trywait(&r->global_requests) == 0) {
                        acquired = true;
                        break;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        break;
                    }
                    cpu_relax();
                }
            }

            if (!acquired) {
                if (sem_wait(&r->global_requests) != 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                }
            }
        }

        for (uint32_t i = 0; i < r->num_clients; i++) {
            if (!atomic_load_explicit(&r->channels[i].connected, memory_order_acquire)) {
                continue;
            }
            int rc = shm_server_process_one(r, (int)i, &requests_processed, &responses_sent);
            if (rc < 0) {
                continue;
            }
            if (rc > 0) {
                work_done += rc;
                if (r->mode == SHM_SYNC_SPIN) {
                    int burst = 0;
                    while (burst < 64) {
                        rc = shm_server_process_one(r, (int)i, &requests_processed, &responses_sent);
                        if (rc <= 0) {
                            break;
                        }
                        work_done += rc;
                        burst++;
                    }
                }
            }
        }

        if (r->mode == SHM_SYNC_SPIN && work_done == 0) {
            cpu_relax();
        }
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    struct server_result sr;
    memset(&sr, 0, sizeof(sr));
    sr.pid = getpid();
    sr.requests_processed = atomic_load(&requests_processed);
    sr.responses_sent = atomic_load(&responses_sent);
    sr.cpu_user_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
    sr.cpu_sys_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;

    write_full(stats_fd, &sr, sizeof(sr));
    close(stats_fd);
    return 0;
}

static int run_server_process(const struct benchmark_case *bench,
                              const struct transport_context *ctx,
                              int ready_fd,
                              int stats_fd) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = server_signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    switch (ctx->kind) {
        case TRANSPORT_STREAM:
        case TRANSPORT_SEQPACKET:
        case TRANSPORT_DGRAM:
            return run_server_socket(ctx->kind, bench, ctx, ready_fd, stats_fd);
        case TRANSPORT_SHM_SPIN:
        case TRANSPORT_SHM_SEM:
        case TRANSPORT_SHM_HYBRID:
            return run_server_shm(bench, ctx, ready_fd, stats_fd);
        default:
            return -1;
    }
}

struct client_runtime {
    struct benchmark_case bench;
    struct transport_context transport;
    int client_index;
    int start_fd;
    int result_fd;
};

static int client_wait_start(int fd) {
    uint8_t b = 0;
    ssize_t n = read_full(fd, &b, 1);
    close(fd);
    return n == 1 ? 0 : -1;
}

static int run_client_socket(const struct client_runtime *rt, struct client_result *out) {
    int sock_type = SOCK_STREAM;
    if (rt->transport.kind == TRANSPORT_SEQPACKET) {
        sock_type = SOCK_SEQPACKET;
    } else if (rt->transport.kind == TRANSPORT_DGRAM) {
        sock_type = SOCK_DGRAM;
    }

    char client_dgram_path[MAX_PATH_LEN] = {0};
    int fd = -1;
    if (rt->transport.kind == TRANSPORT_DGRAM) {
        fd = connect_unix_dgram_client(rt->transport.socket_path, client_dgram_path, sizeof(client_dgram_path),
                                       rt->client_index);
    } else {
        fd = connect_unix_socket(rt->transport.socket_path, sock_type);
    }
    if (fd < 0) {
        return -1;
    }

    size_t payload_size = rt->bench.payload_size;
    uint8_t *send_buf = (uint8_t *)malloc(payload_size);
    uint8_t *recv_buf = (uint8_t *)malloc(payload_size);
    if (!send_buf || !recv_buf) {
        close(fd);
        free(send_buf);
        free(recv_buf);
        return -1;
    }
    memset(send_buf, 0, payload_size);
    memset(recv_buf, 0, payload_size);

    int depth = (rt->bench.mode == MODE_PINGPONG) ? 1 : rt->bench.pipeline_depth;
    if (depth < 1) {
        depth = 1;
    }
    uint64_t *expected = (uint64_t *)calloc((size_t)depth * 2u, sizeof(uint64_t));
    uint64_t *sent_ts = (uint64_t *)calloc((size_t)depth * 2u, sizeof(uint64_t));
    if (!expected || !sent_ts) {
        close(fd);
        free(send_buf);
        free(recv_buf);
        free(expected);
        free(sent_ts);
        return -1;
    }

    uint64_t client_base = ((uint64_t)(rt->client_index + 1) << 48);
    uint64_t next_counter = client_base;
    uint64_t next_seq = 1;

    uint64_t q_head = 0;
    uint64_t q_tail = 0;
    int inflight = 0;

    uint64_t start_ns = monotonic_ns();
    uint64_t end_ns = start_ns + (uint64_t)rt->bench.duration_sec * 1000000000ull;
    bool rate_limited = rt->bench.target_rps > 0;
    uint64_t interval_ns = rate_limited ? (1000000000ull / (uint64_t)rt->bench.target_rps) : 0;
    if (rate_limited && interval_ns == 0) {
        interval_ns = 1;
    }
    uint64_t next_send_ns = start_ns;

    while (monotonic_ns() < end_ns || inflight > 0) {
        while (monotonic_ns() < end_ns && inflight < depth) {
            uint64_t now = monotonic_ns();
            if (rate_limited && now < next_send_ns) {
                break;
            }

            struct wire_message req = {
                .kind = MSG_KIND_REQUEST,
                .flags = 0,
                .payload_len = (uint32_t)payload_size,
                .client_id = (uint64_t)(rt->client_index + 1),
                .seq = next_seq,
                .counter = next_counter,
            };

            if (encode_message(send_buf, payload_size, &req) != 0) {
                goto out_fail;
            }
            uint64_t ts = monotonic_ns();
            if (send_socket_message(fd, rt->transport.kind, send_buf, payload_size) != 0) {
                goto out_fail;
            }

            expected[q_head % ((uint64_t)depth * 2ull)] = next_counter + 1;
            sent_ts[q_head % ((uint64_t)depth * 2ull)] = ts;
            q_head++;
            inflight++;
            next_counter++;
            next_seq++;
            out->requests_sent++;
            if (rate_limited) {
                next_send_ns += interval_ns;
            }

            if (rt->bench.mode == MODE_PINGPONG) {
                break;
            }
        }

        if (inflight == 0) {
            if (rate_limited) {
                uint64_t now = monotonic_ns();
                if (now < next_send_ns) {
                    sleep_until_ns(next_send_ns);
                }
            }
            continue;
        }

        if (recv_socket_message(fd, rt->transport.kind, recv_buf, payload_size) != 0) {
            if (rt->transport.kind == TRANSPORT_DGRAM &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (monotonic_ns() < end_ns || inflight > 0) {
                    continue;
                }
            }
            goto out_fail;
        }

        struct wire_message rsp;
        memset(&rsp, 0, sizeof(rsp));
        if (decode_message(recv_buf, payload_size, &rsp) != 0 || rsp.kind != MSG_KIND_RESPONSE) {
            out->mismatches++;
        }

        uint64_t exp = expected[q_tail % ((uint64_t)depth * 2ull)];
        uint64_t send_ts_ns = sent_ts[q_tail % ((uint64_t)depth * 2ull)];
        q_tail++;
        inflight--;

        uint64_t now_ns = monotonic_ns();
        uint64_t latency_ns = now_ns > send_ts_ns ? now_ns - send_ts_ns : 0;
        out->latency_hist[latency_bin_from_ns(latency_ns)]++;

        if (rsp.counter != exp) {
            out->mismatches++;
        }

        out->last_counter = rsp.counter;
        out->responses_received++;
    }

    out->wall_sec = (double)(monotonic_ns() - start_ns) / 1e9;

    close(fd);
    if (client_dgram_path[0]) {
        unlink(client_dgram_path);
    }
    free(send_buf);
    free(recv_buf);
    free(expected);
    free(sent_ts);
    return 0;

out_fail:
    out->wall_sec = (double)(monotonic_ns() - start_ns) / 1e9;
    close(fd);
    if (client_dgram_path[0]) {
        unlink(client_dgram_path);
    }
    free(send_buf);
    free(recv_buf);
    free(expected);
    free(sent_ts);
    return -1;
}

static int run_client_shm(const struct client_runtime *rt, struct client_result *out) {
    struct shm_region *r = rt->transport.shm.region;
    int idx = rt->client_index;

    atomic_store_explicit(&r->channels[idx].connected, 1, memory_order_release);

    size_t payload_size = rt->bench.payload_size;
    uint8_t *send_buf = (uint8_t *)malloc(payload_size);
    uint8_t *recv_buf = (uint8_t *)malloc(payload_size);
    if (!send_buf || !recv_buf) {
        free(send_buf);
        free(recv_buf);
        return -1;
    }
    memset(send_buf, 0, payload_size);
    memset(recv_buf, 0, payload_size);

    int depth = (rt->bench.mode == MODE_PINGPONG) ? 1 : rt->bench.pipeline_depth;
    if (depth < 1) {
        depth = 1;
    }

    uint64_t *expected = (uint64_t *)calloc((size_t)depth * 2u, sizeof(uint64_t));
    uint64_t *sent_ts = (uint64_t *)calloc((size_t)depth * 2u, sizeof(uint64_t));
    if (!expected || !sent_ts) {
        free(send_buf);
        free(recv_buf);
        free(expected);
        free(sent_ts);
        return -1;
    }

    uint64_t client_base = ((uint64_t)(idx + 1) << 48);
    uint64_t next_counter = client_base;
    uint64_t next_seq = 1;

    uint64_t q_head = 0;
    uint64_t q_tail = 0;
    int inflight = 0;

    uint64_t start_ns = monotonic_ns();
    uint64_t end_ns = start_ns + (uint64_t)rt->bench.duration_sec * 1000000000ull;
    bool rate_limited = rt->bench.target_rps > 0;
    uint64_t interval_ns = rate_limited ? (1000000000ull / (uint64_t)rt->bench.target_rps) : 0;
    if (rate_limited && interval_ns == 0) {
        interval_ns = 1;
    }
    uint64_t next_send_ns = start_ns;

    while (monotonic_ns() < end_ns || inflight > 0) {
        while (monotonic_ns() < end_ns && inflight < depth) {
            uint64_t now = monotonic_ns();
            if (rate_limited && now < next_send_ns) {
                break;
            }

            struct wire_message req = {
                .kind = MSG_KIND_REQUEST,
                .flags = 0,
                .payload_len = (uint32_t)payload_size,
                .client_id = (uint64_t)(idx + 1),
                .seq = next_seq,
                .counter = next_counter,
            };

            if (encode_message(send_buf, payload_size, &req) != 0) {
                goto out_fail;
            }

            uint64_t ts = monotonic_ns();
            if (shm_client_send(r, idx, send_buf) != 0) {
                goto out_fail;
            }

            expected[q_head % ((uint64_t)depth * 2ull)] = next_counter + 1;
            sent_ts[q_head % ((uint64_t)depth * 2ull)] = ts;
            q_head++;
            inflight++;
            next_counter++;
            next_seq++;
            out->requests_sent++;
            if (rate_limited) {
                next_send_ns += interval_ns;
            }

            if (rt->bench.mode == MODE_PINGPONG) {
                break;
            }
        }

        if (inflight == 0) {
            if (rate_limited) {
                uint64_t now = monotonic_ns();
                if (now < next_send_ns) {
                    sleep_until_ns(next_send_ns);
                }
            }
            continue;
        }

        if (shm_client_recv(r, idx, recv_buf) != 0) {
            goto out_fail;
        }

        struct wire_message rsp;
        memset(&rsp, 0, sizeof(rsp));
        if (decode_message(recv_buf, payload_size, &rsp) != 0 || rsp.kind != MSG_KIND_RESPONSE) {
            out->mismatches++;
        }

        uint64_t exp = expected[q_tail % ((uint64_t)depth * 2ull)];
        uint64_t send_ts_ns = sent_ts[q_tail % ((uint64_t)depth * 2ull)];
        q_tail++;
        inflight--;

        uint64_t now_ns = monotonic_ns();
        uint64_t latency_ns = now_ns > send_ts_ns ? now_ns - send_ts_ns : 0;
        out->latency_hist[latency_bin_from_ns(latency_ns)]++;

        if (rsp.counter != exp) {
            out->mismatches++;
        }

        out->last_counter = rsp.counter;
        out->responses_received++;
    }

    out->wall_sec = (double)(monotonic_ns() - start_ns) / 1e9;

    free(send_buf);
    free(recv_buf);
    free(expected);
    free(sent_ts);
    return 0;

out_fail:
    out->wall_sec = (double)(monotonic_ns() - start_ns) / 1e9;
    free(send_buf);
    free(recv_buf);
    free(expected);
    free(sent_ts);
    return -1;
}

static int run_client_process(const struct client_runtime *rt) {
    struct client_result result;
    memset(&result, 0, sizeof(result));
    result.client_index = rt->client_index;
    result.pid = getpid();

    int rc = 0;
    if (client_wait_start(rt->start_fd) != 0) {
        rc = 2;
        goto out;
    }

    if (rt->transport.kind == TRANSPORT_STREAM || rt->transport.kind == TRANSPORT_SEQPACKET ||
        rt->transport.kind == TRANSPORT_DGRAM) {
        rc = run_client_socket(rt, &result);
    } else {
        rc = run_client_shm(rt, &result);
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    result.cpu_user_sec = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
    result.cpu_sys_sec = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;

out:
    write_full(rt->result_fd, &result, sizeof(result));
    close(rt->result_fd);

    if (rc == 0) {
        return 0;
    }
    if (rc == 2) {
        return 2;
    }
    return 3;
}

static int setup_transport_context(const struct benchmark_case *bench, struct transport_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->kind = bench->transport;

    if (bench->transport == TRANSPORT_SHM_SPIN || bench->transport == TRANSPORT_SHM_SEM ||
        bench->transport == TRANSPORT_SHM_HYBRID) {
        int slots = bench->mode == MODE_PINGPONG ? 2 : bench->pipeline_depth * 2;
        if (slots < 2) {
            slots = 2;
        }
        if (slots > SHM_MAX_SLOTS) {
            slots = SHM_MAX_SLOTS;
        }

        enum shm_sync_mode sync_mode = SHM_SYNC_SPIN;
        if (bench->transport == TRANSPORT_SHM_SEM) {
            sync_mode = SHM_SYNC_SEM;
        } else if (bench->transport == TRANSPORT_SHM_HYBRID) {
            sync_mode = SHM_SYNC_HYBRID;
        }

        ctx->shm = shm_layout_create(bench->clients, slots, bench->payload_size, sync_mode);
        if (!ctx->shm.region) {
            return -1;
        }
        return 0;
    }

    snprintf(ctx->socket_path, sizeof(ctx->socket_path), "/tmp/ipc-bench-%d-%s-%d-%zu.sock", getpid(),
             transport_name(bench->transport), bench->clients, bench->payload_size);
    return 0;
}

static void cleanup_transport_context(struct transport_context *ctx) {
    if (ctx->kind == TRANSPORT_SHM_SPIN || ctx->kind == TRANSPORT_SHM_SEM ||
        ctx->kind == TRANSPORT_SHM_HYBRID) {
        shm_layout_destroy(&ctx->shm);
    } else if (ctx->socket_path[0] != '\0') {
        unlink(ctx->socket_path);
    }
}

static int run_single_case(const struct benchmark_case *bench, struct benchmark_result *out) {
    memset(out, 0, sizeof(*out));
    out->bench = *bench;

    struct transport_context tctx;
    if (setup_transport_context(bench, &tctx) != 0) {
        fprintf(stderr, "failed to setup transport context for %s\n", transport_name(bench->transport));
        return -1;
    }

    int ready_pipe[2] = {-1, -1};
    int stats_pipe[2] = {-1, -1};
    if (pipe(ready_pipe) != 0 || pipe(stats_pipe) != 0) {
        cleanup_transport_context(&tctx);
        return -1;
    }

    pid_t server_pid = fork();
    if (server_pid < 0) {
        cleanup_transport_context(&tctx);
        return -1;
    }

    if (server_pid == 0) {
        close(ready_pipe[0]);
        close(stats_pipe[0]);
        int rc = run_server_process(bench, &tctx, ready_pipe[1], stats_pipe[1]);
        _exit(rc == 0 ? 0 : 2);
    }

    close(ready_pipe[1]);
    close(stats_pipe[1]);

    uint8_t ready = 0;
    if (read_full(ready_pipe[0], &ready, 1) != 1 || ready != 1) {
        close(ready_pipe[0]);
        kill(server_pid, SIGTERM);
        waitpid(server_pid, NULL, 0);
        cleanup_transport_context(&tctx);
        return -1;
    }
    close(ready_pipe[0]);

    int start_pipes[MAX_CLIENTS][2];
    int result_pipes[MAX_CLIENTS][2];
    pid_t client_pids[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        start_pipes[i][0] = -1;
        start_pipes[i][1] = -1;
        result_pipes[i][0] = -1;
        result_pipes[i][1] = -1;
        client_pids[i] = 0;
    }

    int clients_started = 0;
    for (int i = 0; i < bench->clients; i++) {
        if (pipe(start_pipes[i]) != 0 || pipe(result_pipes[i]) != 0) {
            goto spawn_fail;
        }

        pid_t pid = fork();
        if (pid < 0) {
            goto spawn_fail;
        }

        if (pid == 0) {
            int keep_start_read = start_pipes[i][0];
            int keep_result_write = result_pipes[i][1];

            for (int j = 0; j <= i; j++) {
                int fds[4] = {start_pipes[j][0], start_pipes[j][1], result_pipes[j][0], result_pipes[j][1]};
                for (int k = 0; k < 4; k++) {
                    int fd = fds[k];
                    if (fd < 0) {
                        continue;
                    }
                    if (fd == keep_start_read || fd == keep_result_write) {
                        continue;
                    }
                    close(fd);
                }
            }

            struct client_runtime rt;
            memset(&rt, 0, sizeof(rt));
            rt.bench = *bench;
            rt.transport = tctx;
            rt.client_index = i;
            rt.start_fd = keep_start_read;
            rt.result_fd = keep_result_write;

            int rc = run_client_process(&rt);
            _exit(rc);
        }

        client_pids[i] = pid;
        close(start_pipes[i][0]);
        close(result_pipes[i][1]);
        clients_started++;
    }

    {
        struct cpu_sampler sampler;
        cpu_sampler_init(&sampler, server_pid, client_pids, bench->clients);

        for (int i = 0; i < bench->clients; i++) {
            uint8_t b = 1;
            write_full(start_pipes[i][1], &b, 1);
            close(start_pipes[i][1]);
        }

        bool all_done = false;
        int finished = 0;
        int failed_clients = 0;
        struct client_result results[MAX_CLIENTS];
        memset(results, 0, sizeof(results));

        while (!all_done) {
            cpu_sampler_tick(&sampler);

            all_done = true;
            for (int i = 0; i < bench->clients; i++) {
                if (client_pids[i] <= 0) {
                    continue;
                }
                int status = 0;
                pid_t rc = waitpid(client_pids[i], &status, WNOHANG);
                if (rc == 0) {
                    all_done = false;
                    continue;
                }
                if (rc == client_pids[i]) {
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        failed_clients++;
                    }
                    client_pids[i] = -client_pids[i];
                    finished++;
                }
            }

            if (!all_done) {
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
                nanosleep(&ts, NULL);
            }
        }

        for (int i = 0; i < bench->clients; i++) {
            waitpid(abs(client_pids[i]), NULL, 0);
        }

        cpu_sampler_finish(&sampler);

        for (int i = 0; i < bench->clients; i++) {
            if (read_full(result_pipes[i][0], &results[i], sizeof(results[i])) != (ssize_t)sizeof(results[i])) {
                memset(&results[i], 0, sizeof(results[i]));
            }
            close(result_pipes[i][0]);
        }

        if (tctx.kind == TRANSPORT_SHM_SPIN || tctx.kind == TRANSPORT_SHM_SEM ||
            tctx.kind == TRANSPORT_SHM_HYBRID) {
            atomic_store_explicit(&tctx.shm.region->stop, 1, memory_order_release);
            if (tctx.kind != TRANSPORT_SHM_SPIN) {
                sem_post(&tctx.shm.region->global_requests);
            }
        }

        kill(server_pid, SIGTERM);
        int server_status = 0;
        waitpid(server_pid, &server_status, 0);

        struct server_result sr;
        memset(&sr, 0, sizeof(sr));
        read_full(stats_pipe[0], &sr, sizeof(sr));
        close(stats_pipe[0]);

        uint64_t total_latency_samples = 0;
        double wall_max = 0.0;

        for (int i = 0; i < bench->clients; i++) {
            out->total_requests += results[i].requests_sent;
            out->total_responses += results[i].responses_received;
            out->total_mismatches += results[i].mismatches;
            if (results[i].last_counter > out->last_counter_max) {
                out->last_counter_max = results[i].last_counter;
            }
            double wall = results[i].wall_sec > 0.0 ? results[i].wall_sec : (double)bench->duration_sec;
            out->client_cpu_inproc_cores += (results[i].cpu_user_sec + results[i].cpu_sys_sec) / wall;
            if (wall > wall_max) {
                wall_max = wall;
            }

            for (int b = 0; b < LAT_HIST_BINS; b++) {
                out->merged_hist[b] += results[i].latency_hist[b];
                total_latency_samples += results[i].latency_hist[b];
            }
        }

        out->requests_processed_server = sr.requests_processed;
        out->responses_sent_server = sr.responses_sent;

        out->p50_us = histogram_percentile_us(out->merged_hist, total_latency_samples, 0.50);
        out->p95_us = histogram_percentile_us(out->merged_hist, total_latency_samples, 0.95);
        out->p99_us = histogram_percentile_us(out->merged_hist, total_latency_samples, 0.99);

        out->failed_clients = failed_clients;
        out->elapsed_sec = (double)bench->duration_sec;
        if (wall_max > out->elapsed_sec) {
            out->elapsed_sec = wall_max;
        }

        out->throughput_rps = out->total_responses / out->elapsed_sec;
        out->server_cpu_inproc_cores = (sr.cpu_user_sec + sr.cpu_sys_sec) /
                                       (out->elapsed_sec > 0.0 ? out->elapsed_sec : 1.0);

        out->server_cpu_external_cores = cpu_sampler_pid_cores(&sampler, server_pid);
        for (int i = 0; i < bench->clients; i++) {
            out->client_cpu_external_cores += cpu_sampler_pid_cores(&sampler, abs(client_pids[i]));
        }
    }

    cleanup_transport_context(&tctx);
    return 0;

spawn_fail:
    for (int i = 0; i < clients_started; i++) {
        if (start_pipes[i][1] >= 0) {
            close(start_pipes[i][1]);
        }
        if (result_pipes[i][0] >= 0) {
            close(result_pipes[i][0]);
        }
        kill(client_pids[i], SIGTERM);
        waitpid(client_pids[i], NULL, 0);
    }
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    close(stats_pipe[0]);
    cleanup_transport_context(&tctx);
    return -1;
}

static void print_result_header(void) {
    printf("transport,mode,clients,payload_bytes,duration_sec,target_rps,failed_clients,requests_sent,responses_received,mismatches,last_counter,throughput_rps,p50_us,p95_us,p99_us,client_cpu_inproc_cores,server_cpu_inproc_cores,client_cpu_external_cores,server_cpu_external_cores,server_requests,server_responses\n");
}

static void print_result_row(const struct benchmark_result *r) {
    printf("%s,%s,%d,%zu,%d,%d,%d,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%llu,%llu\n",
           transport_name(r->bench.transport),
           mode_name(r->bench.mode),
           r->bench.clients,
           r->bench.payload_size,
           r->bench.duration_sec,
           r->bench.target_rps,
           r->failed_clients,
           (unsigned long long)r->total_requests,
           (unsigned long long)r->total_responses,
           (unsigned long long)r->total_mismatches,
           (unsigned long long)r->last_counter_max,
           r->throughput_rps,
           r->p50_us,
           r->p95_us,
           r->p99_us,
           r->client_cpu_inproc_cores,
           r->server_cpu_inproc_cores,
           r->client_cpu_external_cores,
           r->server_cpu_external_cores,
           (unsigned long long)r->requests_processed_server,
           (unsigned long long)r->responses_sent_server);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    struct benchmark_options opts;
    if (parse_options(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 1;
    }

    print_result_header();

    for (int ti = 0; ti < opts.transport_count; ti++) {
        for (int mi = 0; mi < opts.mode_count; mi++) {
            for (int ci = 0; ci < opts.client_count; ci++) {
                for (int pi = 0; pi < opts.payload_count; pi++) {
                    struct benchmark_case bc;
                    memset(&bc, 0, sizeof(bc));
                    bc.transport = opts.transports[ti];
                    bc.mode = opts.modes[mi];
                    bc.clients = opts.clients[ci];
                    bc.payload_size = opts.payloads[pi];
                    bc.duration_sec = opts.duration_sec;
                    bc.pipeline_depth = opts.pipeline_depth;
                    bc.target_rps = opts.target_rps;

                    if (bc.payload_size < 40) {
                        bc.payload_size = 40;
                    }

                    if (!opts.quiet) {
                        fprintf(stderr,
                                "running transport=%s mode=%s clients=%d payload=%zu duration=%d depth=%d target_rps=%d\n",
                                transport_name(bc.transport), mode_name(bc.mode), bc.clients, bc.payload_size,
                                bc.duration_sec, bc.pipeline_depth, bc.target_rps);
                    }

                    struct benchmark_result br;
                    int rc = run_single_case(&bc, &br);
                    if (rc != 0) {
                        fprintf(stderr,
                                "case failed transport=%s mode=%s clients=%d payload=%zu\n",
                                transport_name(bc.transport), mode_name(bc.mode), bc.clients, bc.payload_size);
                        continue;
                    }

                    print_result_row(&br);
                    fflush(stdout);
                }
            }
        }
    }

    return 0;
}
