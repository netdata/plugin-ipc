/*
 * rdtsc_test.c - test RDTSC overhead under Hyper-V
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

static inline uint64_t rdtsc_fenced(void) {
    /* Use LFENCE+RDTSC for ordered reads without RDTSCP overhead */
    __asm__ volatile("lfence" ::: "memory");
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_bare(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    int N = 2000000;

    /* Calibrate RDTSC against QPC */
    LARGE_INTEGER freq, qpc0, qpc1;
    QueryPerformanceFrequency(&freq);

    uint64_t tsc_start = rdtsc_fenced();
    QueryPerformanceCounter(&qpc0);
    Sleep(500);
    uint64_t tsc_end = rdtsc_fenced();
    QueryPerformanceCounter(&qpc1);

    double wall_sec = (double)(qpc1.QuadPart - qpc0.QuadPart) / (double)freq.QuadPart;
    double tsc_hz = (double)(tsc_end - tsc_start) / wall_sec;
    double tsc_ns_per_tick = 1e9 / tsc_hz;
    printf("TSC frequency: %.0f Hz (%.2f GHz)\n", tsc_hz, tsc_hz / 1e9);
    printf("TSC ns/tick: %.3f\n\n", tsc_ns_per_tick);

    /* Measure RDTSC overhead (bare) */
    uint64_t t0 = rdtsc_bare();
    for (int i = 0; i < N; i++) {
        (void)rdtsc_bare();
    }
    uint64_t t1 = rdtsc_bare();
    double rdtsc_bare_cycles = (double)(t1 - t0) / (double)N;
    double rdtsc_bare_ns = rdtsc_bare_cycles * tsc_ns_per_tick;
    printf("RDTSC (bare) overhead:   %6.1f cycles  %6.1f ns\n",
           rdtsc_bare_cycles, rdtsc_bare_ns);

    /* Measure RDTSC overhead (fenced) */
    t0 = rdtsc_fenced();
    for (int i = 0; i < N; i++) {
        (void)rdtsc_fenced();
    }
    t1 = rdtsc_fenced();
    double rdtsc_fenced_cycles = (double)(t1 - t0) / (double)N;
    double rdtsc_fenced_ns = rdtsc_fenced_cycles * tsc_ns_per_tick;
    printf("RDTSC (fenced) overhead: %6.1f cycles  %6.1f ns\n",
           rdtsc_fenced_cycles, rdtsc_fenced_ns);

    /* Measure QPC overhead for comparison */
    t0 = rdtsc_fenced();
    for (int i = 0; i < N; i++) {
        LARGE_INTEGER dummy;
        QueryPerformanceCounter(&dummy);
    }
    t1 = rdtsc_fenced();
    double qpc_cycles = (double)(t1 - t0) / (double)N;
    double qpc_ns = qpc_cycles * tsc_ns_per_tick;
    printf("QPC overhead:            %6.1f cycles  %6.1f ns\n",
           qpc_cycles, qpc_ns);

    /* Test monotonicity */
    int regressions = 0;
    uint64_t prev = rdtsc_bare();
    for (int i = 0; i < N; i++) {
        uint64_t cur = rdtsc_bare();
        if (cur < prev) regressions++;
        prev = cur;
    }
    printf("\nRDTSC monotonicity: %d regressions out of %d reads (%.4f%%)\n",
           regressions, N, 100.0 * regressions / N);

    return 0;
}
