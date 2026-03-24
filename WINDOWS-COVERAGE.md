# Windows Coverage

This document records the Windows-native coverage workflow that matches the
known-good `win11` environment used for this repository.

It intentionally documents only what is implemented and validated in this repo.
It does not claim generic MSVC/CI support that has not been exercised here.

## Current Verified Results

Verified on `2026-03-24`:

- `ctest --test-dir build --output-on-failure -j4`: `28/28` passing on `win11`
- `bash tests/run-coverage-c-windows.sh 90`:
  - script works
  - coverage-only Windows test subset passes inside the script
  - the script now runs three bounded direct executables before the generic `ctest` loop:
    - `build-windows-coverage-c/bin/test_win_service_guards.exe`
    - `build-windows-coverage-c/bin/test_win_service_guards_extra.exe`
    - `build-windows-coverage-c/bin/test_win_service_extra.exe`
  - current validated direct-guard results:
    - `test_win_service_guards.exe`: `150 passed, 0 failed`
    - `test_win_service_guards_extra.exe`: `33 passed, 0 failed`
    - `test_win_service_extra.exe`: `81 passed, 0 failed`
  - the remaining Windows C subset then runs one-by-one with `ctest --timeout 60`
  - total coverage result: `92.2%`
  - per-file:
    - `netipc_service_win.c`: `91.3%`
    - `netipc_named_pipe.c`: `92.4%`
    - `netipc_win_shm.c`: `94.1%`
  - current status: script passes, including the Linux-matching per-file `90%` gate
  - latest ordinary Windows C transport gains came from:
    - dedicated coverage-only service-guard tests staying split into smaller executables
    - manual HYBRID mapping setup to reach the client-attach event-name overflow branch honestly
    - deterministic HYBRID and BUSYWAIT receive timeout / disconnect tests
    - client-side oversized-response `MSG_TOO_LARGE` coverage for WinSHM
    - second chunked round-trip coverage proving client receive-buffer reuse on the same session
  - script detail:
    - `test_win_service_guards.exe`, `test_win_service_guards_extra.exe`, and `test_win_service_extra.exe` now run under `timeout 120`
    - the generic Windows C subset no longer relies on `test_win_service_extra` inside the unordered `ctest` loop
    - every remaining `ctest` item now uses `--timeout 60`
- `bash tests/run-coverage-go-windows.sh 90`:
  - script prints valid coverage results on `win11`
  - total coverage result: `96.7%`
  - package coverage:
    - `service/cgroups`: `96.5%`
    - `transport/windows`: `95.2%`
  - selected key files:
    - `service/cgroups/client_windows.go`: `96.7%`
    - `service/cgroups/types.go`: `100.0%`
    - `transport/windows/pipe.go`: `97.1%`
    - `transport/windows/shm.go`: `92.9%`
  - current status:
    - reported above the Linux-matching `90%` target
    - Windows Go service/cache tests are now also wired into `ctest`
    - latest WinSHM service tests, direct raw WinSHM L2 tests, batch failure/recovery tests, and the listener shutdown fix materially raised both the weak Windows client paths and the Windows transport package
    - malformed raw WinSHM request tests now also cover the real SHM server-side teardown / reconnect path
    - the latest create / attach edge tests materially raised the remaining ordinary Windows Go transport file
    - the latest raw I/O, handshake, `Listen()`, chunked batch, and disconnect tests pushed `pipe.go` above `97%` and Windows Go total to `96.7%`
- `bash tests/run-coverage-rust-windows.sh 90`:
  - script works on `win11`
  - workflow:
    - `cargo-llvm-cov`
    - `rustup component add llvm-tools-preview`
    - Windows-native Rust L2/L3 unit tests
    - Windows Rust interop ctests
  - current threshold policy: same total `90%` gate as Linux Rust coverage
  - current measured result after excluding Rust bin / benchmark noise from the report:
    - `service/cgroups.rs`: `83.83%` line coverage
    - `transport/windows.rs`: `94.43%` line coverage
    - `transport/win_shm.rs`: `88.27%` line coverage
    - total line coverage: `93.68%`
  - current caveat:
    - `test_retry_on_failure_windows` is intentionally ignored because Windows managed-server shutdown/reconnect still needs a dedicated investigation

Brutal truth:

- Windows coverage measurement is real and useful now.
- Windows coverage parity is much closer, but not finished.
- Windows C is no longer below the Linux-matching `90%` gate.
- One transient `test_win_service_guards.exe` timeout and one later `test_win_service_guards_extra.exe` timeout were both traced to coverage-harness test placement/racing, not to a proven runtime regression.
- The current validated layout keeps the hybrid-attach mismatch test in `test_win_service_guards.exe` and the worker-limit / destroy / send-failure cases in `test_win_service_guards_extra.exe`.
- The Windows Go script reliability issue is fixed.
- The recent Rust Windows coverage work materially raised `service/cgroups.rs` and `win_shm.rs`.
- The recent Windows named-pipe transport tests materially raised `transport/windows.rs`.
- The current weakest Windows Rust file is now `service/cgroups.rs`, but it is above the current `90%` threshold.
- The remaining Windows Go work is no longer a named-pipe transport coverage problem.
- The ordinary `client_windows.go` targets are largely exhausted.
- The remaining Windows Go work is now mostly:
  - the tiny remaining low-level `pipe.go` branches:
    - short write
    - zero-byte read
    - `SetNamedPipeHandleState` failure during `Connect()`
    - defensive nil-map checks
    - `Accept()` / next-pipe-creation failure paths
  - any still-honest residual `transport/windows/shm.go` gap after the create / attach edge tests
  - fixed-size encode / defensive server branches in `client_windows.go`
  - the deferred Windows managed-server retry/shutdown behavior
- Some malformed named-pipe response cases do not reach Go L2 coverage points because the Windows session layer rejects them first.
- Direct raw WinSHM tests now cover the Windows-only L2 branches that named pipes cannot reach honestly.
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
3. runs the Windows coverage-focused `ctest` subset
4. runs the dedicated Windows C coverage-only guard executable
5. collects `gcov` line coverage for the Windows C sources above

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
- The current validated Windows Rust workflow enforces the same total `90%` threshold policy as Linux Rust coverage.
- It now produces meaningful Windows Rust service coverage on `win11`.
- It now also produces strong Windows named-pipe transport coverage on `win11`.
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
bash tests/run-coverage-c-windows.sh
```

### Go coverage

```bash
bash tests/run-coverage-go-windows.sh 90
```

### Rust coverage

```bash
bash tests/run-coverage-rust-windows.sh 90
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
