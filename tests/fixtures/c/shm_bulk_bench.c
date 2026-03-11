/*
 * shm_bulk_bench.c - Bulk SHM ping-pong benchmark
 *
 * Measures round-trip latency using only 2 QPC calls total (not per-iteration).
 * This eliminates QPC overhead from the measurement to get the true
 * spin-wait cost under Hyper-V.
 *
 * Also measures with the full protocol stack (encode/decode/memcpy) to
 * compare against bare spin-wait.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CACHELINE 64
#define DEFAULT_ITERATIONS 2000000
#define FRAME_SIZE 64

/* Spin mode */
#define SPIN_PAUSE   0
#define SPIN_NOPAUSE 1

static int g_spin_mode = SPIN_PAUSE;

/* Cache-line aligned shared region */
struct bulk_shm_region {
    volatile int64_t request_seq __attribute__((aligned(64)));
    char pad1[CACHELINE - sizeof(int64_t)];

    volatile int64_t response_seq __attribute__((aligned(64)));
    char pad2[CACHELINE - sizeof(int64_t)];

    volatile LONG ready __attribute__((aligned(64)));
    volatile LONG done;
    int32_t spin_mode;
    int32_t pad3;
    char pad4[CACHELINE - 2 * sizeof(LONG) - 2 * sizeof(int32_t)];

    /* Optional frame buffers for simulating protocol overhead */
    uint8_t request_frame[FRAME_SIZE] __attribute__((aligned(64)));
    uint8_t response_frame[FRAME_SIZE] __attribute__((aligned(64)));
} __attribute__((aligned(64)));

static inline int64_t load_acquire_i64(volatile int64_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store_release_i64(volatile int64_t *p, int64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline void spin_wait(void) {
    if (g_spin_mode == SPIN_PAUSE) {
        YieldProcessor();
    } else {
        __asm__ volatile("" ::: "memory");
    }
}

/* Server thread: echoes request_seq to response_seq */
static DWORD WINAPI server_bare(void *arg) {
    struct bulk_shm_region *shm = (struct bulk_shm_region *)arg;
    const char *aff = getenv("NETIPC_CPU_AFFINITY_SERVER");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (getenv("NETIPC_HIGH_PRIORITY"))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    g_spin_mode = shm->spin_mode;
    __atomic_store_n(&shm->ready, 1, __ATOMIC_RELEASE);

    int64_t last = 0;
    while (!__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE)) {
        int64_t seq;
        for (;;) {
            seq = load_acquire_i64(&shm->request_seq);
            if (seq != last) break;
            if (__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE)) return 0;
            spin_wait();
        }
        last = seq;
        store_release_i64(&shm->response_seq, seq);
    }
    return 0;
}

/* Server thread: copies request_frame, processes, copies response_frame */
static DWORD WINAPI server_framed(void *arg) {
    struct bulk_shm_region *shm = (struct bulk_shm_region *)arg;
    const char *aff = getenv("NETIPC_CPU_AFFINITY_SERVER");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (getenv("NETIPC_HIGH_PRIORITY"))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    g_spin_mode = shm->spin_mode;
    __atomic_store_n(&shm->ready, 1, __ATOMIC_RELEASE);

    int64_t last = 0;
    uint8_t local_frame[FRAME_SIZE];
    while (!__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE)) {
        int64_t seq;
        for (;;) {
            seq = load_acquire_i64(&shm->request_seq);
            if (seq != last) break;
            if (__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE)) return 0;
            spin_wait();
        }
        last = seq;

        /* Simulate server-side protocol: read request, write response */
        memcpy(local_frame, (const void *)shm->request_frame, FRAME_SIZE);
        /* "process" - just copy to response */
        memset((void *)shm->response_frame, 0, FRAME_SIZE);
        memcpy((void *)shm->response_frame, local_frame, FRAME_SIZE);

        store_release_i64(&shm->response_seq, seq);
    }
    return 0;
}

static void run_bench(const char *label, LPTHREAD_START_ROUTINE server_fn,
                      int iterations, int spin_mode, int with_frames) {
    struct bulk_shm_region *shm = (struct bulk_shm_region *)_aligned_malloc(sizeof(*shm), CACHELINE);
    if (!shm) { fprintf(stderr, "alloc failed\n"); return; }
    memset(shm, 0, sizeof(*shm));
    shm->spin_mode = spin_mode;
    g_spin_mode = spin_mode;

    if (getenv("NETIPC_HIGH_PRIORITY"))
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    const char *aff = getenv("NETIPC_CPU_AFFINITY_CLIENT");
    if (aff) {
        DWORD_PTR mask = (DWORD_PTR)strtoull(aff, NULL, 0);
        if (mask) SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (getenv("NETIPC_HIGH_PRIORITY"))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    HANDLE thread = CreateThread(NULL, 0, server_fn, shm, 0, NULL);
    if (!thread) { fprintf(stderr, "CreateThread failed\n"); _aligned_free(shm); return; }

    while (!__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE)) Sleep(1);

    /* Warmup */
    uint8_t local_frame[FRAME_SIZE];
    for (int i = 0; i < 10000; i++) {
        if (with_frames) {
            memset(local_frame, 0, FRAME_SIZE);
            memcpy((void *)shm->request_frame, local_frame, FRAME_SIZE);
        }
        store_release_i64(&shm->request_seq, (int64_t)(i + 1));
        while (load_acquire_i64(&shm->response_seq) < (int64_t)(i + 1))
            spin_wait();
        if (with_frames) {
            memcpy(local_frame, (const void *)shm->response_frame, FRAME_SIZE);
        }
    }

    /* Reset */
    store_release_i64(&shm->request_seq, 0);
    store_release_i64(&shm->response_seq, 0);
    __asm__ volatile("" ::: "memory");
    Sleep(10);

    /* === BULK MEASUREMENT: only 2 QPC calls === */
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    for (int i = 0; i < iterations; i++) {
        int64_t seq = (int64_t)(i + 1);
        if (with_frames) {
            memset(local_frame, 0, FRAME_SIZE);
            memcpy((void *)shm->request_frame, local_frame, FRAME_SIZE);
        }
        store_release_i64(&shm->request_seq, seq);
        while (load_acquire_i64(&shm->response_seq) < seq)
            spin_wait();
        if (with_frames) {
            memcpy(local_frame, (const void *)shm->response_frame, FRAME_SIZE);
        }
    }

    QueryPerformanceCounter(&end);

    double elapsed = (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    double rps = elapsed > 0 ? (double)iterations / elapsed : 0;
    double avg_ns = elapsed * 1e9 / (double)iterations;

    printf("%-40s  %9.0f req/s  avg=%.0f ns  (%.3f sec for %d iters)\n",
           label, rps, avg_ns, elapsed, iterations);

    __atomic_store_n(&shm->done, 1, __ATOMIC_RELEASE);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    _aligned_free(shm);
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : DEFAULT_ITERATIONS;
    if (iterations <= 0) iterations = DEFAULT_ITERATIONS;

    printf("Iterations: %d\n\n", iterations);

    /* Bare spin-wait benchmarks */
    run_bench("bare/PAUSE",           server_bare,   iterations, SPIN_PAUSE,   0);
    run_bench("bare/no-pause",        server_bare,   iterations, SPIN_NOPAUSE, 0);

    /* With frame copy (simulating protocol overhead) */
    run_bench("framed/PAUSE",         server_framed, iterations, SPIN_PAUSE,   1);
    run_bench("framed/no-pause",      server_framed, iterations, SPIN_NOPAUSE, 1);

    return 0;
}
