# Windows IPC Benchmark Results

Benchmark of the cross-language Windows IPC transports across the full
directed matrix:

- `c-native`
- `c-msys`
- `rust-native`
- `go-native`

Direction matters here. `client=A -> server=B` is measured separately
from `client=B -> server=A`.

## Environment

- OS: Windows 11
- Host type: Hyper-V VM
- Duration: 5 seconds per timed run
- Protocol: single-connection request/response increment loop
- C native build: MinGW-w64 native Windows binary
- C MSYS build: MSYS runtime binary linked against `msys-2.0.dll`
- Rust build: native Windows binary
- Go build: native Windows binary

## Transport Profiles

| Profile | Description |
|---------|-------------|
| Named Pipe | `CreateNamedPipeW` / `ReadFile` / `WriteFile` kernel IPC |
| SHM HYBRID | Shared memory ring with spin plus kernel event fallback, negotiated over the initial Named Pipe handshake |

## Results

### Maximum throughput, SHM HYBRID

| Pair | Method | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|------|--------|-----------------:|---------:|---------:|---------:|-----------:|-----------:|----------:|
| c-native->c-native | c-shm-hybrid | 3279438.39 | 0.20 | 0.26 | 0.28 | 0.894 | 0.769 | 1.663 |
| c-native->c-msys | c-shm-hybrid | 3259295.13 | 0.23 | 0.29 | 0.33 | 0.903 | 0.799 | 1.702 |
| c-native->rust-native | rust-shm-hybrid | 3210545.77 | 0.22 | 0.29 | 0.31 | 0.850 | 0.829 | 1.679 |
| c-native->go-native | go-shm-hybrid | 2046836.10 | 0.00 | 0.00 | 0.00 | 1.200 | 0.882 | 2.082 |
| c-msys->c-native | c-shm-hybrid | 3375322.22 | 0.21 | 0.26 | 0.29 | 0.925 | 0.756 | 1.681 |
| c-msys->c-msys | c-shm-hybrid | 3409185.94 | 0.22 | 0.27 | 0.30 | 0.906 | 0.826 | 1.732 |
| c-msys->rust-native | rust-shm-hybrid | 3332492.49 | 0.20 | 0.27 | 0.30 | 0.950 | 0.842 | 1.792 |
| c-msys->go-native | go-shm-hybrid | 2065578.26 | 0.00 | 0.00 | 0.00 | 1.206 | 0.876 | 2.082 |
| rust-native->c-native | c-shm-hybrid | 3196945.33 | 0.22 | 0.27 | 0.30 | 0.891 | 0.758 | 1.649 |
| rust-native->c-msys | c-shm-hybrid | 3148056.03 | 0.24 | 0.28 | 0.32 | 0.900 | 0.802 | 1.702 |
| rust-native->rust-native | rust-shm-hybrid | 3344758.28 | 0.21 | 0.27 | 0.29 | 0.894 | 0.838 | 1.732 |
| rust-native->go-native | go-shm-hybrid | 2047591.61 | 0.00 | 0.00 | 0.00 | 1.159 | 0.857 | 2.016 |
| go-native->c-native | c-shm-hybrid | 1590707.25 | 0.35 | 0.48 | 7.18 | 0.916 | 1.094 | 2.010 |
| go-native->c-msys | c-shm-hybrid | 1667772.62 | 0.31 | 0.41 | 6.51 | 0.869 | 1.058 | 1.927 |
| go-native->rust-native | rust-shm-hybrid | 1647485.91 | 0.32 | 0.41 | 6.45 | 0.887 | 1.169 | 2.056 |
| go-native->go-native | go-shm-hybrid | 1093866.75 | 0.00 | 0.00 | 0.00 | 1.231 | 1.158 | 2.389 |

### Maximum throughput, Named Pipe

| Pair | Method | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|------|--------|-----------------:|---------:|---------:|---------:|-----------:|-----------:|----------:|
| c-native->c-native | c-npipe | 17604.37 | 39.08 | 3742.59 | 18560.49 | 0.212 | 0.190 | 0.402 |
| c-native->c-msys | c-npipe | 18051.11 | 32.74 | 3037.73 | 15262.83 | 0.235 | 0.232 | 0.467 |
| c-native->rust-native | rust-named-pipe | 18418.94 | 39.83 | 3756.52 | 15068.80 | 0.259 | 0.221 | 0.480 |
| c-native->go-native | go-named-pipe | 16811.07 | 0.00 | 500.60 | 511.30 | 0.297 | 0.216 | 0.513 |
| c-msys->c-native | c-npipe | 17969.51 | 39.14 | 3695.47 | 14829.90 | 0.221 | 0.246 | 0.467 |
| c-msys->c-msys | c-npipe | 17172.68 | 41.78 | 4153.14 | 15806.03 | 0.244 | 0.188 | 0.432 |
| c-msys->rust-native | rust-named-pipe | 18336.34 | 39.21 | 3689.14 | 14787.17 | 0.202 | 0.218 | 0.420 |
| c-msys->go-native | go-named-pipe | 15153.88 | 0.00 | 501.80 | 517.90 | 0.325 | 0.221 | 0.546 |
| rust-native->c-native | c-npipe | 18138.95 | 41.04 | 3882.04 | 19255.78 | 0.264 | N/A | N/A |
| rust-native->c-msys | c-npipe | 17296.77 | 38.44 | 7327.25 | 18483.16 | 0.233 | N/A | N/A |
| rust-native->rust-native | rust-named-pipe | 17621.21 | 37.91 | 3598.02 | 14551.40 | 0.284 | N/A | N/A |
| rust-native->go-native | go-named-pipe | 18182.87 | 0.00 | 500.30 | 511.60 | 0.337 | N/A | N/A |
| go-native->c-native | c-npipe | 18424.69 | 38.31 | 7175.56 | 17896.40 | 0.259 | 0.318 | 0.577 |
| go-native->c-msys | c-npipe | 18603.50 | 39.27 | 7300.46 | 18339.88 | 0.246 | 0.318 | 0.564 |
| go-native->rust-native | rust-named-pipe | 16048.27 | 40.06 | 11052.28 | 22003.45 | 0.201 | 0.318 | 0.519 |
| go-native->go-native | go-named-pipe | 18161.59 | 0.00 | 500.20 | 509.60 | 0.359 | 0.333 | 0.692 |

### Rate-limited at 100k rps, SHM HYBRID

| Pair | Method | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|------|--------|-----------------:|---------:|---------:|---------:|-----------:|-----------:|----------:|
| c-native->c-native | c-shm-hybrid | 99842.34 | 0.28 | 0.36 | 26.97 | 0.921 | 0.918 | 1.839 |
| c-native->c-msys | c-shm-hybrid | 99998.05 | 0.24 | 0.31 | 24.45 | 0.930 | 0.917 | 1.847 |
| c-native->rust-native | rust-shm-hybrid | 99995.00 | 0.26 | 0.35 | 27.50 | 0.549 | 0.543 | 1.092 |
| c-native->go-native | go-shm-hybrid | 99990.75 | 0.00 | 0.00 | 0.00 | 0.228 | 0.159 | 0.387 |
| c-msys->c-native | c-shm-hybrid | 99783.79 | 0.26 | 0.33 | 27.11 | 0.880 | 0.866 | 1.746 |
| c-msys->c-msys | c-shm-hybrid | 100000.07 | 0.24 | 0.33 | 32.40 | 0.864 | 0.867 | 1.731 |
| c-msys->rust-native | rust-shm-hybrid | 99996.54 | 0.03 | 0.04 | 2.91 | 0.599 | 0.561 | 1.160 |
| c-msys->go-native | go-shm-hybrid | 99990.45 | 0.00 | 0.00 | 0.00 | 0.228 | 0.178 | 0.406 |
| rust-native->c-native | c-shm-hybrid | 100000.05 | 0.26 | 0.33 | 24.58 | 0.933 | 0.915 | 1.848 |
| rust-native->c-msys | c-shm-hybrid | 100000.18 | 0.27 | 0.35 | 25.63 | 0.936 | 0.911 | 1.847 |
| rust-native->rust-native | rust-shm-hybrid | 99959.98 | 0.03 | 0.04 | 2.94 | 0.614 | 0.616 | 1.230 |
| rust-native->go-native | go-shm-hybrid | 99990.29 | 0.00 | 0.00 | 0.00 | 0.237 | 0.144 | 0.381 |
| go-native->c-native | c-shm-hybrid | 100000.06 | 0.40 | 12.70 | 67.25 | 0.889 | 1.174 | 2.063 |
| go-native->c-msys | c-shm-hybrid | 99999.30 | 0.38 | 11.81 | 62.84 | 0.864 | 1.099 | 1.963 |
| go-native->rust-native | rust-shm-hybrid | 99990.86 | 0.38 | 12.04 | 52.92 | 0.736 | 1.015 | 1.751 |
| go-native->go-native | go-shm-hybrid | 99990.49 | 0.00 | 0.00 | 0.00 | 0.325 | 0.352 | 0.677 |

### Rate-limited at 10k rps, Named Pipe

| Pair | Method | Throughput (rps) | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|------|--------|-----------------:|---------:|---------:|---------:|-----------:|-----------:|----------:|
| c-native->c-native | c-npipe | 10000.11 | 42.25 | 7437.44 | 18565.18 | 0.529 | 0.208 | 0.737 |
| c-native->c-msys | c-npipe | 9998.91 | 40.02 | 7128.49 | 17762.96 | 0.489 | 0.124 | 0.613 |
| c-native->rust-native | rust-named-pipe | 9962.93 | 42.47 | 7442.37 | 14872.91 | 0.199 | 0.142 | 0.341 |
| c-native->go-native | go-named-pipe | 9999.33 | 0.00 | 456.90 | 507.70 | 0.262 | 0.178 | 0.440 |
| c-msys->c-native | c-npipe | 9998.49 | 43.69 | 7478.05 | 21937.61 | 0.486 | 0.168 | 0.654 |
| c-msys->c-msys | c-npipe | 10000.08 | 42.97 | 7966.41 | 19704.49 | 0.489 | 0.156 | 0.645 |
| c-msys->rust-native | rust-named-pipe | 10000.03 | 41.33 | 7277.43 | 18222.23 | 0.224 | 0.131 | 0.355 |
| c-msys->go-native | go-named-pipe | 9999.24 | 0.00 | 490.40 | 508.00 | 0.303 | 0.134 | 0.437 |
| rust-native->c-native | c-npipe | 10000.07 | 42.78 | 7461.31 | 21943.83 | 0.592 | N/A | N/A |
| rust-native->c-msys | c-npipe | 9999.57 | 38.74 | 6488.23 | 16139.50 | 0.495 | N/A | N/A |
| rust-native->rust-native | rust-named-pipe | 9999.50 | 42.14 | 7557.97 | 18370.55 | 0.206 | N/A | N/A |
| rust-native->go-native | go-named-pipe | 9999.26 | 0.00 | 493.20 | 510.50 | 0.275 | N/A | N/A |
| go-native->c-native | c-npipe | 10000.11 | 43.01 | 11043.92 | 21672.94 | 0.517 | 0.177 | 0.694 |
| go-native->c-msys | c-npipe | 9998.50 | 43.15 | 11215.60 | 21993.44 | 0.402 | 0.221 | 0.623 |
| go-native->rust-native | rust-named-pipe | 9993.73 | 44.78 | 11219.74 | 21994.85 | 0.277 | 0.226 | 0.503 |
| go-native->go-native | go-named-pipe | 9999.20 | 0.00 | 499.50 | 512.00 | 0.275 | 0.231 | 0.506 |

## Key Findings

- `c-msys` and `c-native` are effectively in the same performance class on the Windows transport.
  - Example, SHM max:
    - `c-native->c-native`: 3.28M rps
    - `c-native->c-msys`: 3.26M rps
    - `c-msys->c-native`: 3.38M rps
    - `c-msys->c-msys`: 3.41M rps
- The transition requirement is therefore met in practice:
  - MSYS-built C can talk to native Rust and Go.
  - Native Rust and Go can talk back to both `c-native` and `c-msys`.
- SHM HYBRID remains the only path that reaches multi-million request rates on Windows.
  - C and Rust clients driving C or Rust servers cluster around 3.1M to 3.4M rps.
  - Go clients are materially slower in SHM max-rate runs:
    - about 1.59M to 1.67M rps to C/Rust servers
    - about 1.09M rps for `go-native->go-native`
- Named Pipe throughput stays in the same general band across the full matrix:
  - about 15k to 18.6k rps at max rate
  - about 10k rps when rate-limited
- At 100k rps SHM, all directed pairs stay near the target rate, but client choice still affects tail latency and CPU.
  - Go clients show much higher p95/p99 tails against C/Rust SHM servers than C/Rust clients do.

## Reproducing

```bash
# Native Windows helper build (C native + Rust + Go)
cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw

# MSYS-runtime C helper build
cmake -S . -B build-msys-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release -DNETIPC_BUILD_HELPERS=OFF
cmake --build build-msys-ninja --target netipc-live-c

# Full 64-row matrix
bash tests/run-live-win-bench.sh
```

## Known Limitations

- Go latency columns currently print `0.00` for many SHM rows, so the useful signal there is throughput and CPU rather than the raw latency fields.
- Rust client rows still miss some Named Pipe server CPU values (`N/A`) because the benchmark metadata is not always emitted before the server exits.
- These numbers are from a Windows 11 Hyper-V VM. Relative differences inside this matrix are useful; absolute numbers should not be treated as bare-metal limits.
