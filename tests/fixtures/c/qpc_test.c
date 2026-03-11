#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>

int main(void) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    printf("QPC frequency: %lld Hz (%.1f ns resolution)\n",
           (long long)freq.QuadPart, 1e9 / (double)freq.QuadPart);

    int N = 1000000;

    /* Measure QPC overhead */
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < N; i++) {
        LARGE_INTEGER dummy;
        QueryPerformanceCounter(&dummy);
    }
    QueryPerformanceCounter(&t1);
    double qpc_ns = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart * 1e9 / (double)N;
    printf("QPC call overhead: %.1f ns\n", qpc_ns);

    /* Measure RDTSC overhead */
    unsigned int aux;
    uint64_t tsc0 = __rdtscp(&aux);
    for (int i = 0; i < N; i++) {
        (void)__rdtscp(&aux);
    }
    uint64_t tsc1 = __rdtscp(&aux);
    double tsc_cycles = (double)(tsc1 - tsc0) / (double)N;
    printf("RDTSCP overhead: %.1f cycles (%.1f ns at 3.19 GHz)\n",
           tsc_cycles, tsc_cycles / 3.19);

    /* Measure single-thread atomic store+load */
    volatile int64_t counter __attribute__((aligned(64))) = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < N; i++) {
        __atomic_store_n(&counter, (int64_t)i, __ATOMIC_RELEASE);
        int64_t v = __atomic_load_n(&counter, __ATOMIC_ACQUIRE);
        (void)v;
    }
    QueryPerformanceCounter(&t1);
    double single_ns = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart * 1e9 / (double)N;
    printf("Single-thread atomic store+load: %.1f ns\n", single_ns);

    /* Measure YieldProcessor / PAUSE cost */
    tsc0 = __rdtscp(&aux);
    for (int i = 0; i < N; i++) {
        YieldProcessor();
    }
    tsc1 = __rdtscp(&aux);
    double pause_cycles = (double)(tsc1 - tsc0) / (double)N;
    printf("YieldProcessor/PAUSE: %.1f cycles (%.1f ns at 3.19 GHz)\n",
           pause_cycles, pause_cycles / 3.19);

    return 0;
}
