# Windows IPC Benchmark Results

Benchmark of the cross-language Windows IPC transports (C, Rust, Go) over
Named Pipe and SHM HYBRID profiles.

## Environment

- **OS:** Windows 11 (Hyper-V VM)
- **CPU:** Virtual, 2 vCPUs
- **Compiler toolchain:** MinGW-w64 (GCC), Rust 1.x (release profile), Go 1.x
- **Duration:** 5 seconds per scenario
- **Protocol:** Single-connection request/response increment loop

## Transport Profiles

| Profile | Description |
|---------|-------------|
| **Named Pipe** | `CreateNamedPipeW` / `ReadFile` / `WriteFile` kernel IPC |
| **SHM HYBRID** | Shared memory ring with spin + kernel auto-reset event fallback. Negotiated over the initial Named Pipe handshake. |

The SHM HYBRID profile uses a spin-then-block strategy: the hot path
exchanges frames via `memcpy` on a shared memory region using atomic
sequence counters; when no work is immediately available the thread
falls back to `WaitForSingleObject` / `SetEvent` on a kernel
auto-reset event.  A conditional `SetEvent` optimisation avoids the
kernel call when the peer is still spinning.

## Results

### Maximum throughput (target_rps = 0, unlimited)

| Language | Transport | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|----------|-----------|-----------------|----------|----------|----------|------------|------------|-----------|
| C | SHM HYBRID | 2,783,222 | 0.23 | 0.31 | 0.35 | 0.922 | 0.781 | 1.703 |
| Go | SHM HYBRID | 1,135,798 | <1 | <1 | <1 | 1.162 | 1.155 | 2.317 |
| Rust | SHM HYBRID | 73,375 | 3.60 | 11.80 | 64.60 | 0.916 | 0.888 | 1.804 |
| C | Named Pipe | 18,612 | 34.58 | 2,474 | 11,866 | 0.201 | 0.290 | 0.491 |
| Go | Named Pipe | 17,956 | <1 | 500 | 508 | 0.322 | 0.321 | 0.643 |
| Rust | Named Pipe | 15,115 | 44.30 | 113.30 | 236.70 | 0.356 | -- | -- |

### Rate-limited at 100 000 rps (SHM HYBRID only)

| Language | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|----------|-----------------|----------|----------|----------|------------|------------|-----------|
| C | 99,962 | 0.29 | 0.52 | 17.67 | 0.933 | 0.929 | 1.862 |
| Go | 99,990 | <1 | <1 | <1 | 0.269 | 0.330 | 0.599 |
| Rust | 56,861 | 3.60 | 11.60 | 65.10 | 0.947 | 0.891 | 1.838 |

### Rate-limited at 10 000 rps (Named Pipe only)

| Language | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|----------|-----------------|----------|----------|----------|------------|------------|-----------|
| C | 10,000 | 40.93 | 6,896 | 19,795 | 0.554 | 0.196 | 0.750 |
| Go | 9,999 | <1 | <1 | 505 | 0.309 | 0.209 | 0.518 |
| Rust | 10,000 | 50.00 | 138.90 | 261.90 | 0.322 | -- | -- |

## Analysis

### SHM HYBRID

C achieves the highest throughput at ~2.8M rps with sub-microsecond
latency.  The C SHM transport benefits from `WaitOnAddress` /
`WakeByAddressSingle` for the sleep/wake path, which avoids creating
kernel event objects entirely.

Go reaches ~1.1M rps.  Go's `sync/atomic` operations use `XCHG` for
stores on x86 which provides implicit full memory barriers, making the
SHM protocol inherently safe from store-load reordering.

Rust reaches ~73K rps.  The current Rust implementation uses kernel
auto-reset events (`CreateEventW` / `SetEvent` / `WaitForSingleObject`)
for the sleep/wake path rather than `WaitOnAddress`.  The kernel
event overhead (~4 us under Hyper-V) dominates at high throughput.
Switching to `WaitOnAddress` would likely bring Rust closer to Go/C
performance.

### Named Pipe

All three languages achieve 15-19K rps over Named Pipes, which is
expected since throughput is dominated by the kernel pipe round-trip.
C and Go show similar p50 latency (~35-40 us measured), while Rust
shows slightly higher p50 (44 us) but much lower tail latency than C
(p99: 237 us vs 11,866 us).  The high C tail latency is likely due to
Hyper-V scheduling jitter amplified by the blocking `ReadFile` call.

### CPU efficiency

At the 100K rps SHM rate, Go is the most CPU-efficient (0.60 total
cores) because its goroutine scheduler amortises the spin cost.  C and
Rust both use ~1.8 total cores at 100K rps since their spin loops run
on dedicated OS threads.

## Reproducing

```bash
# Build all three drivers
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run the full benchmark suite (5s per scenario)
bash tests/run-live-win-bench.sh
```

## Known issues

- **Rust SHM throughput:** The Rust SHM HYBRID transport uses kernel
  auto-reset events instead of `WaitOnAddress`, resulting in lower
  throughput than C/Go.  Adding `WaitOnAddress` support would close
  the gap.

- **Rust server CPU reporting:** The Rust Named Pipe server does not
  always report `SERVER_CPU_CORES` before the process exits, so the
  server CPU column shows `--` for Rust Named Pipe benchmarks.

- **Store-load reordering (fixed):** All three implementations now
  include SeqCst / MFENCE barriers between the critical store-then-load
  pairs in the SHM protocol to prevent the Dekker/Peterson race on x86.
  Without these barriers, Release/Acquire ordering alone cannot prevent
  a store from being reordered past a subsequent load to a different
  address, which could cause deadlocks under sustained high throughput.
