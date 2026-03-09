# plugin-ipc

Cross-language IPC library repository for Netdata plugins.

This repository mirrors the eventual Netdata monorepo layout:

- C library code: `src/libnetdata/netipc/`
- Go package home: `src/go/pkg/netipc/`
- Rust crate home: `src/crates/netipc/`
- Test fixtures and helper apps: `tests/fixtures/`
- Benchmark drivers: `bench/drivers/`

Current implemented surfaces:

- C typed frame/schema library
- POSIX `UDS_SEQPACKET` transport with profile negotiation
- POSIX `SHM_HYBRID` transport
- Windows Named Pipe baseline for the C library
- Windows negotiated `SHM_HYBRID` fast profile for the C library
- Rust/Go helper binaries for interop and benchmark validation

The reusable Go package and Rust crate locations are in place, but their full Windows transport ports are still pending.

## Build

`CMake` is the authoritative top-level build/test interface for this repository.
`Cargo.toml` and `go.mod` remain native package metadata for Rust and Go, but
repo-level workflows should be driven through CMake targets.

POSIX:

```bash
cmake -S . -B build
cmake --build build
```

Windows under MSYS2:

```bash
# Run from mingw64.exe or ucrt64.exe, not the plain msys shell.
cmake -S . -B build-mingw -G Ninja
cmake --build build-mingw
```

The Windows C backend is native Win32 code. It uses Named Pipes as the baseline
transport and a negotiated shared-memory hybrid fast profile. The fast profile
is intentionally limited to Win32 primitives that can be ported to Rust and pure
Go without `cgo`. It is intended to be built from an MSYS2 MinGW/UCRT shell,
but it must not target the MSYS runtime.

Compatibility wrapper only:

```bash
make
```

Primary C build artifacts:

- POSIX: `build/lib/libnetipc.a`
- POSIX: `build/bin/netipc-live-c`
- POSIX: `build/bin/netipc-codec-c`
- Windows: `build-mingw/lib/libnetipc.a`
- Windows: `build-mingw/bin/netipc-live-c.exe`

## Repository Layout

```text
.
|-- CMakeLists.txt
|-- bench/
|   `-- drivers/
|-- src/
|   |-- libnetdata/netipc/
|   |-- go/pkg/netipc/
|   `-- crates/netipc/
`-- tests/
    |-- fixtures/
    `-- run-*.sh
```

## POSIX Baseline

- Baseline profile: `UDS_SEQPACKET`
- Negotiation: fixed-binary hello/ack with supported/preferred bitmasks and server-selected profile
- Optional fast profile: `SHM_HYBRID` (`profile 2`) for C/Rust live paths
- Go remains `UDS_SEQPACKET` only in the current phase

## C Library Paths

Headers:

- `src/libnetdata/netipc/include/netipc/netipc_schema.h`
- `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h` (Windows)
- `src/libnetdata/netipc/include/netipc/netipc_shm_hybrid.h`
- `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`

Sources:

- `src/libnetdata/netipc/src/protocol/netipc_schema.c`
- `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c` (Windows)
- `src/libnetdata/netipc/src/transport/windows/netipc_shm_hybrid_win.c` (Windows, internal fast profile)
- `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`

## Test And Benchmark Helpers

Schema fixtures:

- C fixture: `tests/fixtures/c/netipc_codec_tool.c`
- Rust fixture crate: `tests/fixtures/rust/`
- Go fixture module: `tests/fixtures/go/`

Benchmark drivers:

- C live fixture: `tests/fixtures/c/netipc_live_posix_c.c` (POSIX)
- C live fixture: `tests/fixtures/c/netipc_live_c.c` (Windows)
- Rust UDS driver crate: `bench/drivers/rust/`
- Go UDS driver module: `bench/drivers/go/`

## Validation

Primary CMake-driven workflows:

```bash
cmake -S . -B build
cmake --build build
cmake --build build --target test
cmake --build build --target netipc-check
cmake --build build --target netipc-bench
cmake --build build --target netipc-clean-generated
```

Available CMake workflow targets:

- `netipc-check`: runs the registered validation suite
- `test`: runs the registered CTest suite through the active CMake generator
- `netipc-bench`: runs the registered benchmark suite
- `netipc-validate-all`: runs both validation and benchmark targets
- `netipc-clean-generated`: removes generated Rust/Go outputs outside `build/`

Interop and protocol tests:

```bash
./tests/run-interop.sh
./tests/run-live-interop.sh
./tests/run-live-uds-interop.sh
./tests/run-live-npipe-smoke.sh
./tests/run-live-win-profile-bench.sh
./tests/run-uds-seqpacket.sh
./tests/run-uds-negotiation-negative.sh
```

Benchmarks:

```bash
./tests/run-live-uds-bench.sh
./tests/run-live-shm-bench.sh
./tests/run-negotiated-profile-bench.sh
```

Generated benchmark reports:

```bash
./tests/generate-benchmarks-posix.sh
./tests/generate-benchmarks-windows.sh
```

The generator scripts are intentionally separate from the CMake workflow. They
run the benchmark matrix, stage machine-readable results, validate that the
matrix is complete, and only then replace `benchmarks-posix.md` or
`benchmarks-windows.md` atomically. A failed or partial run leaves the committed
markdown untouched.

The benchmark scripts only orchestrate matrix runs. CPU reporting is produced by the helper binaries themselves, not by the reusable library and not by external shell-side `/proc` sampling.

The shell scripts remain in the repository because they are library validation and benchmark assets, but they are now intended to be launched by CMake/CTest as first-class repo workflows.

- `./tests/run-live-uds-bench.sh` runs the full Linux UDS `C/Rust/Go` client-server matrix (`9` directed pairs) at:
  - `max`
  - `100k/s`
  - `10k/s`
- `./tests/run-live-shm-bench.sh` runs the full Linux direct `SHM_HYBRID` `C/Rust` client-server matrix (`4` directed pairs) at:
  - `max`
  - `100k/s`
  - `10k/s`
- The UDS benchmark matrix fails hard on:
  - any non-OK response status
  - any `response != request + 1` mismatch
  - any `requests != responses` mismatch
  - any client/server handled-count mismatch
- The direct SHM benchmark matrix fails hard on the same correctness conditions.
- `./tests/run-live-uds-interop.sh` runs the full directed baseline profile-`1` UDS matrix, plus the negotiated C<->Rust profile-`2` SHM cases.
- `./tests/run-live-interop.sh` runs direct `SHM_HYBRID` interop for `c->rust` and `rust->c`.

Quick manual runs:

```bash
build/bin/netipc-live-c uds-server-once /tmp netflow
build/bin/netipc-live-c uds-client-once /tmp netflow 41
build/bin/netipc-live-c uds-client-loop /tmp netflow 41 2
build/bin/netipc-live-c uds-server-once /tmp netflow 1 3 2 0
build/bin/netipc-live-c uds-client-once /tmp netflow 41 1 3 2 0
build/bin/netipc-live-c uds-server-bench /tmp netflow 0
build/bin/netipc-live-c uds-client-bench /tmp netflow 5 0
build/bin/netipc-live-c uds-bench /tmp netflow 5 0

build/bin/netipc-live-c shm-server-once /tmp netflow
build/bin/netipc-live-c shm-client-once /tmp netflow 41
build/bin/netipc-live-c shm-server-loop /tmp netflow 2
build/bin/netipc-live-c shm-client-bench /tmp netflow 5 0
build/bin/netipc-live-c shm-server-bench /tmp netflow 0
build/bin/netipc-live-c shm-bench /tmp netflow 5 0

build/bin/netipc_live_rs server-once /tmp netflow
build/bin/netipc_live_rs client-once /tmp netflow 41
build/bin/netipc_live_rs server-loop /tmp netflow 2
build/bin/netipc_live_rs client-bench /tmp netflow 5 0

build/bin/netipc-codec-c encode-req 123 41 /tmp/netipc.req
build/bin/netipc-codec-c decode-req /tmp/netipc.req

build-mingw/bin/netipc-live-c.exe server-once /tmp netflow
build-mingw/bin/netipc-live-c.exe client-once /tmp netflow 41

NETIPC_SUPPORTED_PROFILES=3 NETIPC_PREFERRED_PROFILES=2 \
  build-mingw/bin/netipc-live-c.exe client-bench /tmp netflow 5 0
```

## Current Limits

- The reusable C library is the only fully implemented library surface in this repo today.
- Go and Rust still rely on helper binaries for the validated live interop/benchmark paths.
- The Windows C transport is implemented for native Named Pipes plus a negotiated shared-memory hybrid fast profile, but Rust/Go Windows transports are still placeholders.
- Windows validation is still limited to the C smoke path; cross-language Windows interop and benchmark coverage are still pending.
- Netdata integration wiring is intentionally out of scope for this repository phase.
