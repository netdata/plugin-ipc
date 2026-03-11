/*
 * shm_raw_bench.c - Bare-metal SHM ping-pong benchmark
 *
 * Measures the raw round-trip latency of cache-line spin-wait IPC
 * with no protocol encoding, no memcpy of frames, just atomic
 * seq-number exchange. Tests both thread-based and process-based.
 *
 * Usage:
 *   shm_raw_bench threads [iterations] [spin_mode]
 *   shm_raw_bench procs   [iterations] [spin_mode]
 *   shm_raw_bench server                              (for procs mode)
 *
 * spin_mode: 0=PAUSE (default), 1=no-pause, 2=mm_pause, 3=SwitchToThread
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CACHELINE 64
#define DEFAULT_ITERATIONS 500000
#define SHM_NAME L"Local\\netipc-raw-bench-shm"

/* Spin mode: controls what happens in the spin loop */
#define SPIN_PAUSE      0  /* YieldProcessor() / PAUSE */
#define SPIN_NOPAUSE    1  /* compiler barrier only */
#define SPIN_SWITCH     2  /* SwitchToThread() */

static int g_spin_mode = SPIN_PAUSE;

/* Cache-line aligned shared region - use GCC attributes */
struct raw_shm_region {
    /* Request cache line */
    volatile int64_t request_seq __attribute__((aligned(64)));
    char pad1[CACHELINE - sizeof(int64_t)];

    /* Response cache line */
    volatile int64_t response_seq __attribute__((aligned(64)));
    char pad2[CACHELINE - sizeof(int64_t)];

    /* Control cache line */
    volatile LONG ready __attribute__((aligned(64)));
    volatile LONG done;
    int32_t iterations;
    int32_t spin_mode;
    char pad3[CACHELINE - 2 * sizeof(LONG) - 2 * sizeof(int32_t)];
} __attribute__((aligned(64)));

/* --- Atomic helpers ---------------------------------------------------- */

static inline int64_t load_acquire_i64(volatile int64_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_release_i64(volatile int64_t *p, int64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline LONG load_acquire_long(volatile LONG *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_release_long(volatile LONG *p, LONG v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline void spin_wait(void) {
    switch (g_spin_mode) {
        case SPIN_PAUSE:
            YieldProcessor();
            break;
        case SPIN_NOPAUSE:
            __asm__ volatile("" ::: "memory");
            break;
        case SPIN_SWITCH:
            SwitchToThread();
            break;
    }
}

/* --- Server (responder) ------------------------------------------------ */

static DWORD WINAPI server_thread(void *arg) {
    struct raw_shm_region *shm = (struct raw_shm_region *)arg;

    /* Apply affinity: server on a specific core */
    const char *aff = getenv("NETIPC_CPU_AFFINITY_SERVER");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }

    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    g_spin_mode = shm->spin_mode;

    /* Signal ready */
    store_release_long(&shm->ready, 1);

    int64_t last_seq = 0;
    while (!load_acquire_long(&shm->done)) {
        /* Wait for request */
        int64_t seq;
        for (;;) {
            seq = load_acquire_i64(&shm->request_seq);
            if (seq != last_seq) break;
            if (load_acquire_long(&shm->done)) return 0;
            spin_wait();
        }
        last_seq = seq;

        /* Send response (just echo the seq) */
        store_release_i64(&shm->response_seq, seq);
    }

    return 0;
}

static void server_process_loop(struct raw_shm_region *shm) {
    const char *aff = getenv("NETIPC_CPU_AFFINITY_SERVER");
    if (!aff) aff = getenv("NETIPC_CPU_AFFINITY");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    g_spin_mode = shm->spin_mode;

    store_release_long(&shm->ready, 1);

    int64_t last_seq = 0;
    while (!load_acquire_long(&shm->done)) {
        int64_t seq;
        for (;;) {
            seq = load_acquire_i64(&shm->request_seq);
            if (seq != last_seq) break;
            if (load_acquire_long(&shm->done)) return;
            spin_wait();
        }
        last_seq = seq;
        store_release_i64(&shm->response_seq, seq);
    }
}

/* --- Client (requester) ------------------------------------------------ */

struct bench_result {
    int64_t iterations;
    double elapsed_sec;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    double min_ns;
    double max_ns;
    double avg_ns;
};

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static struct bench_result run_client(struct raw_shm_region *shm, int iterations) {
    const char *aff = getenv("NETIPC_CPU_AFFINITY_CLIENT");
    if (!aff) aff = getenv("NETIPC_CPU_AFFINITY");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    /* Wait for server ready */
    while (!load_acquire_long(&shm->ready)) {
        Sleep(1);
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double ns_per_tick = 1e9 / (double)freq.QuadPart;

    /* Allocate latency samples */
    uint64_t *samples = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)iterations);
    if (!samples) {
        fprintf(stderr, "failed to allocate samples\n");
        exit(1);
    }

    /* Warmup */
    for (int i = 0; i < 1000 && i < iterations; i++) {
        int64_t seq = i + 1;
        store_release_i64(&shm->request_seq, seq);
        while (load_acquire_i64(&shm->response_seq) < seq) {
            spin_wait();
        }
    }

    /* Reset seqs for actual benchmark */
    store_release_i64(&shm->request_seq, 0);
    store_release_i64(&shm->response_seq, 0);
    __asm__ volatile("" ::: "memory");
    Sleep(1);

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    for (int i = 0; i < iterations; i++) {
        int64_t seq = i + 1;

        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);

        store_release_i64(&shm->request_seq, seq);

        while (load_acquire_i64(&shm->response_seq) < seq) {
            spin_wait();
        }

        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);

        samples[i] = (uint64_t)(t1.QuadPart - t0.QuadPart);
    }

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    /* Compute stats */
    struct bench_result result;
    result.iterations = iterations;
    result.elapsed_sec = (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;

    qsort(samples, (size_t)iterations, sizeof(uint64_t), cmp_u64);

    result.min_ns = (double)samples[0] * ns_per_tick;
    result.max_ns = (double)samples[iterations - 1] * ns_per_tick;
    result.p50_ns = (double)samples[(size_t)(iterations * 0.50)] * ns_per_tick;
    result.p95_ns = (double)samples[(size_t)(iterations * 0.95)] * ns_per_tick;
    result.p99_ns = (double)samples[(size_t)(iterations * 0.99)] * ns_per_tick;

    double total = 0;
    for (int i = 0; i < iterations; i++) {
        total += (double)samples[i] * ns_per_tick;
    }
    result.avg_ns = total / (double)iterations;

    free(samples);
    return result;
}

static void print_result(const char *label, const struct bench_result *r) {
    double rps = r->elapsed_sec > 0 ? (double)r->iterations / r->elapsed_sec : 0;
    printf("%-30s  %8.0f req/s  avg=%7.0fns  min=%7.0fns  p50=%7.0fns  p95=%7.0fns  p99=%7.0fns  max=%9.0fns\n",
           label, rps, r->avg_ns, r->min_ns, r->p50_ns, r->p95_ns, r->p99_ns, r->max_ns);
}

static const char *spin_mode_name(int mode) {
    switch (mode) {
        case SPIN_PAUSE:    return "PAUSE";
        case SPIN_NOPAUSE:  return "no-pause";
        case SPIN_SWITCH:   return "SwitchToThread";
        default:            return "unknown";
    }
}

/* --- Thread mode ------------------------------------------------------- */

static int run_threads(int iterations, int spin_mode) {
    struct raw_shm_region *shm = (struct raw_shm_region *)_aligned_malloc(sizeof(*shm), CACHELINE);
    if (!shm) {
        fprintf(stderr, "failed to allocate shared region\n");
        return 1;
    }
    memset(shm, 0, sizeof(*shm));
    shm->iterations = iterations;
    shm->spin_mode = spin_mode;
    g_spin_mode = spin_mode;

    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }

    HANDLE thread = CreateThread(NULL, 0, server_thread, shm, 0, NULL);
    if (!thread) {
        fprintf(stderr, "CreateThread failed: %lu\n", GetLastError());
        _aligned_free(shm);
        return 1;
    }

    struct bench_result result = run_client(shm, iterations);

    store_release_long(&shm->done, 1);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);

    char label[64];
    snprintf(label, sizeof(label), "threads/%s", spin_mode_name(spin_mode));
    print_result(label, &result);

    _aligned_free(shm);
    return 0;
}

/* --- Process mode ------------------------------------------------------ */

static int run_procs(int iterations, int spin_mode) {
    /* Create shared memory */
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, (DWORD)sizeof(struct raw_shm_region), SHM_NAME);
    if (!mapping) {
        fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        return 1;
    }

    struct raw_shm_region *shm = (struct raw_shm_region *)MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shm));
    if (!shm) {
        fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    memset((void *)shm, 0, sizeof(*shm));
    shm->iterations = iterations;
    shm->spin_mode = spin_mode;
    g_spin_mode = spin_mode;

    if (getenv("NETIPC_HIGH_PRIORITY")) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }

    /* Get own exe path and spawn server process */
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "\"%s\" server", exe_path);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        getenv("NETIPC_HIGH_PRIORITY") ? HIGH_PRIORITY_CLASS : 0,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        UnmapViewOfFile(shm);
        CloseHandle(mapping);
        return 1;
    }

    struct bench_result result = run_client(shm, iterations);

    store_release_long(&shm->done, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile(shm);
    CloseHandle(mapping);

    char label[64];
    snprintf(label, sizeof(label), "procs/%s", spin_mode_name(spin_mode));
    print_result(label, &result);

    return 0;
}

static int run_server(void) {
    /* Open shared memory created by parent */
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (!mapping) {
        fprintf(stderr, "server: OpenFileMapping failed: %lu\n", GetLastError());
        return 1;
    }

    struct raw_shm_region *shm = (struct raw_shm_region *)MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shm));
    if (!shm) {
        fprintf(stderr, "server: MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    server_process_loop(shm);

    UnmapViewOfFile(shm);
    CloseHandle(mapping);
    return 0;
}

/* --- Main -------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s threads|procs|server [iterations] [spin_mode]\n", argv[0]);
        fprintf(stderr, "  spin_mode: 0=PAUSE 1=no-pause 2=SwitchToThread\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "env vars:\n");
        fprintf(stderr, "  NETIPC_CPU_AFFINITY_CLIENT=0x1   (client core mask)\n");
        fprintf(stderr, "  NETIPC_CPU_AFFINITY_SERVER=0x4   (server core mask)\n");
        fprintf(stderr, "  NETIPC_HIGH_PRIORITY=1           (use HIGH_PRIORITY_CLASS)\n");
        return 2;
    }

    if (strcmp(argv[1], "server") == 0) {
        return run_server();
    }

    int iterations = argc > 2 ? atoi(argv[2]) : DEFAULT_ITERATIONS;
    int spin_mode = argc > 3 ? atoi(argv[3]) : SPIN_PAUSE;

    if (iterations <= 0) iterations = DEFAULT_ITERATIONS;
    if (spin_mode < 0 || spin_mode > 2) spin_mode = SPIN_PAUSE;

    printf("Iterations: %d  Spin mode: %s\n", iterations, spin_mode_name(spin_mode));

    if (strcmp(argv[1], "threads") == 0) {
        return run_threads(iterations, spin_mode);
    }
    if (strcmp(argv[1], "procs") == 0) {
        return run_procs(iterations, spin_mode);
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
