# Windows Coverage

This document records the Windows-native coverage workflow that matches the
known-good `win11` environment used for this repository.

It intentionally documents only what is implemented and validated in this repo.
It does not claim generic MSVC/CI support that has not been exercised here.

## Current Verified Results

Verified on `2026-03-18`:

- `ctest --test-dir build --output-on-failure -j4`: `24/24` passing on `win11`
- `bash tests/run-coverage-c-windows.sh 80`:
  - script works
  - full Windows `ctest` suite passes inside the script
  - coverage result: `67.5%`
  - current status: below the draft `80%` target
- `bash tests/run-coverage-go-windows.sh 80`:
  - script works
  - coverage result: `52.4%`
  - current status: below the draft `80%` target

Brutal truth:

- Windows coverage measurement is now implemented and validated.
- Windows coverage parity is not complete yet.

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

There is no validated Windows Rust coverage script in this repository yet.

Facts:

- Windows Rust code exists in:
  - `src/crates/netipc/src/transport/windows.rs`
  - `src/crates/netipc/src/transport/win_shm.rs`
- Linux coverage runs do not execute these modules.
- A reliable Windows-native Rust coverage workflow still needs to be designed and validated separately.

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

### Rust Windows coverage remains open

This repository now has validated Windows C and Go coverage entrypoints.
Rust Windows coverage still needs:

- a chosen tool
- a validated `win11` workflow
- an agreed threshold or an explicit “report-only” policy

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
