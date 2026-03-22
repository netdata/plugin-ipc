# Windows Coverage

This document records the Windows-native coverage workflow that matches the
known-good `win11` environment used for this repository.

It intentionally documents only what is implemented and validated in this repo.
It does not claim generic MSVC/CI support that has not been exercised here.

## Current Verified Results

Verified on `2026-03-23`:

- `ctest --test-dir build --output-on-failure -j4`: `28/28` passing on `win11`
- `bash tests/run-coverage-c-windows.sh 80`:
  - script works
  - coverage-only Windows test subset passes inside the script
  - total coverage result: `83.7%`
  - per-file:
    - `netipc_service_win.c`: `82.5%`
    - `netipc_named_pipe.c`: `85.8%`
    - `netipc_win_shm.c`: `83.2%`
  - current status: script passes, including the per-file `80%` gate
- `bash tests/run-coverage-go-windows.sh 80`:
  - script prints valid coverage results on `win11`
  - total coverage result: `85.8%`
  - selected key files:
    - `service/cgroups/client_windows.go`: `72.9%`
    - `service/cgroups/types.go`: `100.0%`
    - `transport/windows/pipe.go`: `83.3%`
    - `transport/windows/shm.go`: `84.5%`
  - current status:
    - reported above the draft `80%` target
    - Windows Go service/cache tests are now also wired into `ctest`
    - latest malformed-response and SHM corruption/timeout tests materially raised the weak Windows client/SHM paths
- `bash tests/run-coverage-rust-windows.sh`:
  - script works on `win11`
  - workflow:
    - `cargo-llvm-cov`
    - `rustup component add llvm-tools-preview`
    - Windows-native Rust L2/L3 unit tests
    - Windows Rust interop ctests
  - current threshold policy: same total `80%` gate as Linux Rust coverage
  - current measured result after excluding Rust bin / benchmark noise from the report:
    - `service/cgroups.rs`: `77.28%`
    - `transport/windows.rs`: `76.17%`
    - `transport/win_shm.rs`: `78.86%`
    - total line coverage: `87.98%`
  - current caveat:
    - `test_retry_on_failure_windows` is intentionally ignored because Windows managed-server shutdown/reconnect still needs a dedicated investigation

Brutal truth:

- Windows coverage measurement is real and useful now.
- Windows coverage parity is much closer, but not finished.
- Windows C is no longer the red gate.
- The Windows Go script reliability issue is fixed.
- The remaining Windows Go work is now concentrated in `client_windows.go` and `shm.go`, plus the deferred Windows managed-server retry/shutdown behavior.
- One transient `test_protocol_rust` failure was observed once under parallel `ctest`, but it did not reproduce on immediate isolated or full reruns. This is not a confirmed active blocker.

## Scope

### C coverage script

Script: `tests/run-coverage-c-windows.sh`

Measured Windows-specific C files:

- `src/libnetdata/netipc/src/service/netipc_service_win.c`
- `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
- `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c`

What it does:

1. configures a fresh Windows coverage build with `NETIPC_COVERAGE=ON`
2. builds with the native `win11` MinGW64 toolchain and Ninja
3. runs the Windows `ctest` suite
4. collects `gcov` line coverage for the Windows C sources above

### Go coverage script

Script: `tests/run-coverage-go-windows.sh`

Measured packages:

- `./pkg/netipc/protocol/`
- `./pkg/netipc/service/cgroups/`
- `./pkg/netipc/transport/windows/`

What it does:

1. runs the package tests natively on Windows
2. merges the generated coverage profiles
3. prints per-function and per-file coverage
4. fails if no Windows transport coverage was collected

### Rust coverage

Script: `tests/run-coverage-rust-windows.sh`

Facts:

- Windows Rust code exists in:
- `src/crates/netipc/src/transport/windows.rs`
- `src/crates/netipc/src/transport/win_shm.rs`
- `src/crates/netipc/src/service/cgroups.rs`
- Linux coverage runs do not execute these Windows modules.
- The current validated Windows Rust workflow enforces the same total `80%` threshold policy as Linux Rust coverage.
- It now produces meaningful Windows Rust service coverage on `win11`.
- The remaining Windows Rust caveat is the ignored retry/shutdown test that belongs to the separate managed-server investigation.

## win11 environment

Use the native Windows Rust and Go toolchains plus MinGW64 GCC.

Recommended environment:

```bash
export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:$PATH"
export MSYSTEM=MINGW64
export CC=/mingw64/bin/gcc
export CXX=/mingw64/bin/g++
```

Sanity check:

```bash
type -a cargo go gcc g++ cmake ninja gcov
```

Expected shape:

- `cargo` from `/c/Users/costa/.cargo/bin`
- `go` from `/c/Program Files/Go/bin`
- `gcc` / `g++` / `gcov` from `/mingw64/bin`

## Commands

### C coverage

```bash
bash tests/run-coverage-c-windows.sh 80
```

### Go coverage

```bash
bash tests/run-coverage-go-windows.sh 80
```

### Rust coverage

```bash
bash tests/run-coverage-rust-windows.sh 80
```

## Current limitations

### Native Windows execution is required

The Windows code paths cannot be covered from Linux.

Reasons:

- Windows transport code depends on Win32 APIs
- Windows-only Go files are behind `//go:build windows`
- Rust Windows transport modules are behind `#[cfg(windows)]`

### 100% Windows coverage is not realistic without extra infrastructure

Some branches require:

- allocation failure injection
- Win32 API fault injection
- timing/race orchestration
- resource exhaustion and handle-creation failures

Examples:

- `CreateNamedPipe` / `ConnectNamedPipe` failure paths
- `CreateFileMapping` / `MapViewOfFile` failure paths
- overlapped or wait-related timeout/error branches
- low-memory allocation failures in the Windows L2 layer

### Rust Windows coverage is threshold-enforced, but not coverage-complete

This repository now has validated Windows C, Go, and Rust coverage entrypoints.
Rust Windows coverage still needs:

- more test work for the lower-coverage transport files
- the deferred retry/shutdown investigation to be resolved separately from the normal coverage gate

### Windows stress scope is intentionally narrow in default `ctest`

Facts:

- `test_win_stress` is wired into Windows `ctest`
- the currently validated default scope is the repeated WinSHM lifecycle case

Reason:

- the managed-service shutdown stress cases exposed a separate Windows
  investigation area and are intentionally not part of the default `ctest`
  path until that behavior is understood and stabilized

## Relationship to `COVERAGE-EXCLUSIONS.md`

`COVERAGE-EXCLUSIONS.md` is the cross-platform explanation of lines that still
need special infrastructure or fault injection.

This document is narrower:

- how to measure Windows coverage here
- what Windows coverage is currently implemented
- what is still missing on Windows specifically
