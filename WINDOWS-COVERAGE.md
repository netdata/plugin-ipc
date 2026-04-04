# Windows Coverage

This document records the Windows-native coverage workflow that matches the
known-good `win11` environment used for this repository.

It intentionally documents only what is implemented and validated in this repo.
It does not claim generic MSVC/CI support that has not been exercised here.

## Current Verified Results

Verified on `2026-03-28`:

- `ctest --test-dir build --output-on-failure -j4`: `28/28` passing on `win11`
- Windows C coverage on the latest clean `win11` coverage build:
  - the bounded direct coverage executables pass:
    - `test_win_service_guards.exe`: `198 passed, 0 failed`
    - `test_win_service_guards_extra.exe`: `93 passed, 0 failed`
    - `test_win_service_extra.exe`: `165 passed, 0 failed`
  - the remaining Windows C interop / stress subset then passes one-by-one under `ctest --timeout 60`
  - the raw `bash tests/run-coverage-c-windows.sh 90` flow completed end to end
  - measured total coverage result: `93.2%`
  - per-file:
    - `netipc_service_win.c`: `90.2%`
    - `netipc_named_pipe.c`: `95.4%`
    - `netipc_win_shm.c`: `97.2%`
  - current status:
    - measured Windows C remains above the Linux-matching per-file and total `90%` gates
    - the aggregate Windows C script is trustworthy again on the validated `win11` flow
  - latest ordinary Windows C service gains came from:
    - NULL config default coverage for typed client/server init
    - minimum response-buffer growth coverage
    - deterministic client-side buffer and SHM-context allocation faults
    - deterministic server-side SHM create and cache allocation faults
    - typed hybrid malformed SHM reply coverage on the real `nipc_client_call_cgroups_snapshot()` path
    - typed hybrid `LIMIT_EXCEEDED` / `UNSUPPORTED` response-status coverage
  - latest ordinary Windows C transport gains came from:
    - manual HYBRID mapping setup to reach the client-attach event-name overflow branch honestly
    - deterministic HYBRID and BUSYWAIT receive timeout / disconnect tests
    - client-side oversized-response `MSG_TOO_LARGE` coverage for WinSHM
    - second chunked round-trip coverage proving client receive-buffer reuse on the same session
    - deterministic fake-server continuation-packet tests now covering the chunked receive error cluster in `netipc_named_pipe.c`
- `bash tests/run-coverage-c-windows.sh 90`:
  - script still configures and builds correctly
  - the script now runs three bounded direct executables before the generic `ctest` loop:
    - `build-windows-coverage-c/bin/test_win_service_guards.exe`
    - `build-windows-coverage-c/bin/test_win_service_guards_extra.exe`
    - `build-windows-coverage-c/bin/test_win_service_extra.exe`
  - script detail:
    - `test_win_service_guards.exe` and `test_win_service_guards_extra.exe` now run under `timeout 120`
    - `test_win_service_extra.exe` now runs under `timeout 600`
    - the generic Windows C subset no longer relies on `test_win_service_extra` inside the unordered `ctest` loop
    - every remaining `ctest` item now uses `--timeout 60`
- `bash tests/run-coverage-go-windows.sh 90`:
  - script prints valid coverage results on `win11`
  - total coverage result: `95.4%`
  - selected key files:
    - `service/cgroups/cache_windows.go`: `100.0%`
    - `service/cgroups/client_windows.go`: `100.0%`
    - `transport/windows/pipe.go`: `92.1%`
    - `transport/windows/shm.go`: `94.2%`
  - current status:
    - reported above the Linux-matching `90%` target
    - Windows Go service/cache tests are now also wired into `ctest`
    - latest public typed cgroups wrapper tests now cover:
      - `Cache.Ready()`
      - `Cache.Status()`
      - `Client.Status()`
      - `NewServerWithWorkers()`
    - removing the dead private `Handler.snapshotMaxItems()` helper also eliminated a fake `types.go` denominator from the Windows Go typed-service package
    - latest WinSHM service tests, direct raw WinSHM L2 tests, batch failure/recovery tests, and the listener shutdown fix materially raised both the Windows client paths and the Windows transport package
    - malformed raw WinSHM request tests now also cover the real SHM server-side teardown / reconnect path
    - the latest create / attach edge tests materially raised the remaining ordinary Windows Go transport file
    - the remaining ordinary Windows Go work is now concentrated in low-level transport branches such as:
      - `peekNamedPipeAvailable`
      - `WaitReadable()`
      - `SetPayloadLimits()`
      - short write / zero-byte read / next-pipe-creation failures
- `bash tests/run-coverage-rust-windows.sh 90`:
  - script works on `win11`
  - workflow:
    - `cargo-llvm-cov`
    - `rustup component add llvm-tools-preview`
    - Windows-native Rust L2/L3 unit tests
    - Windows Rust interop ctests
  - current threshold policy:
    - total Windows Rust line coverage must stay above `90%`
    - critical Windows runtime files must each stay above `90%` line coverage:
      - `service\cgroups.rs`
      - `transport\windows.rs`
      - `transport\win_shm.rs`
  - current measured result after excluding Rust bin / benchmark noise from the report:
    - `service/cgroups.rs`: `92.74%` line coverage
    - `transport/windows.rs`: `94.74%` line coverage
    - `transport/win_shm.rs`: `95.76%` line coverage
    - total line coverage: `92.08%`
  - current status:
    - the old ignored Windows retry/shutdown test is gone
    - `test_retry_on_failure_windows` now runs in the normal Windows Rust suite
    - the stricter per-file runtime gate now passes on a fresh native `win11` run
    - Phase 1 deterministic Win32 fault injection now covers forced failure and
      recovery for:
      - `CreateFileMappingW`
      - `OpenFileMappingW`
      - `MapViewOfFile`
      - `CreateEventW`
      - `OpenEventW`
    - the current `transport/win_shm.rs` result is backed by native cleanup-
      sensitive fault tests, not only by ordinary happy-path / malformed-header
      coverage

Brutal truth:

- Windows coverage measurement is real and useful now.
- Windows coverage parity is much closer, but not finished.
- Windows C is no longer below the Linux-matching `90%` gate.
- The old Windows C coverage-script instability was real.
- The current validated layout fixes it by keeping the early HYBRID / malformed-request guards in `test_win_service_guards.exe` and the late dispatch / cache / drain / worker-limit cases in `test_win_service_guards_extra.exe`.
- That means the current trustworthy Windows C baseline is the full script result above: `93.2%` total, with every tracked file above `90%`.
- The Windows Go script reliability issue is fixed.
- The recent Rust Windows coverage work materially raised `service/cgroups.rs` and `win_shm.rs`.
- The recent Windows named-pipe transport tests materially raised `transport/windows.rs`.
- The current weakest critical Windows Rust runtime file is `service/cgroups.rs`, and it is already above the per-file `90%` gate at `92.74%`.
- The remaining Windows Go work is no longer a public typed-wrapper coverage problem.
- The remaining Windows Go work is now mostly:
  - the tiny remaining low-level `pipe.go` branches:
    - short write
    - zero-byte read
    - `SetNamedPipeHandleState` failure during `Connect()`
    - defensive nil-map checks
    - `Accept()` / next-pipe-creation failure paths
  - any still-honest residual `transport/windows/shm.go` gap after the create / attach edge tests
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
3. runs the dedicated Windows C coverage-only guard executables
4. runs the Windows coverage-focused C subset
5. collects `gcov` line coverage for the Windows C sources above
6. if `test_win_service` is noisy under `ctest`, direct executable validation on the same clean coverage build is the authoritative fallback for deciding whether the problem is harness noise or a runtime regression

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
- The current validated Windows Rust workflow enforces:
  - the same total `90%` line threshold as Linux Rust coverage
  - plus per-file `90%` line gates for the three critical Windows Rust runtime files above
- It now produces meaningful Windows Rust service coverage on `win11`.
- It now also produces strong Windows named-pipe transport coverage on `win11`.
- There is no longer an ignored Windows Rust retry/shutdown caveat in the normal suite.

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

This remains the authoritative Windows coverage / sign-off environment. The
separate MSYS transition lane is:

```bash
bash tests/run-windows-msys-validation.sh
```

That script validates the MSYS-built C path with targeted functional tests,
repeats `test_win_shm`, and runs bounded native-vs-MSYS benchmark
comparisons with explicit throughput floors per scenario. It is a compatibility
lane, not a replacement for the native MinGW64 coverage flow below.

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

### Application Verifier + PageHeap

```bash
bash tests/run-verifier-windows.sh
```

## Current limitations

### Native Windows execution is required

The Windows code paths cannot be covered from Linux.

Reasons:

- Windows transport code depends on Win32 APIs
- Windows-only Go files are behind `//go:build windows`
- Rust Windows transport modules are behind `#[cfg(windows)]`

### Ordinary tests are not enough for the last Windows gaps

Some remaining branches require extra infrastructure such as:

- allocation failure injection
- Win32 API fault injection
- timing/race orchestration
- resource exhaustion and handle-creation failures

Examples:

- `CreateNamedPipe` / `ConnectNamedPipe` failure paths
- `CreateFileMapping` / `MapViewOfFile` failure paths
- overlapped or wait-related timeout/error branches
- low-memory allocation failures in the Windows L2 layer

The repository now includes a first-class verifier entrypoint for the core
Windows C executables:

- `tests/run-verifier-windows.sh`

It enables:

- Application Verifier layers:
  - `Handles`
  - `Heaps`
  - `Locks`
- full PageHeap via `gflags /p /enable ... /full`

Default validated targets:

- `test_named_pipe.exe`
- `test_win_shm.exe`
- `test_win_service.exe`
- `test_win_service_extra.exe`

### Rust Windows coverage is threshold-enforced and stable

This repository now has validated Windows C, Go, and Rust coverage entrypoints.
Rust Windows coverage still needs:

- more deterministic OS-failure and low-resource validation
- verifier and long-runtime evidence beyond line coverage

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
