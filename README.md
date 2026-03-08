# plugin-ipc

Cross-language IPC workbench and library repository for Netdata plugins.

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
- Windows Named Pipe transport for the C library
- Rust/Go helper binaries for interop and benchmark validation

The reusable Go package and Rust crate locations are in place, but their full Windows transport ports are still pending.

## Build

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

The Windows C backend is native Win32 code using Named Pipes. It is intended to
be built from an MSYS2 MinGW/UCRT shell, but it must not target the MSYS runtime.

Compatibility wrapper:

```bash
make
```

Primary C build artifacts:

- POSIX: `build/lib/libnetipc.a`
- POSIX: `build/bin/ipc-bench`
- POSIX: `build/bin/netipc-shm-server-demo`
- POSIX: `build/bin/netipc-shm-client-demo`
- POSIX: `build/bin/netipc-uds-server-demo`
- POSIX: `build/bin/netipc-uds-client-demo`
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
- `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`

## Test And Benchmark Helpers

Schema fixtures:

- C fixture: `tests/fixtures/c/netipc_codec_tool.c`
- Rust fixture crate: `tests/fixtures/rust/`
- Go fixture module: `tests/fixtures/go/`

Benchmark drivers:

- C driver: `bench/drivers/c/ipc_bench.c`
- Rust UDS driver crate: `bench/drivers/rust/`
- Go UDS driver module: `bench/drivers/go/`

## Validation

Interop and protocol tests:

```bash
./tests/run-interop.sh
./tests/run-live-interop.sh
./tests/run-live-uds-interop.sh
./tests/run-live-npipe-smoke.sh
./tests/run-uds-seqpacket.sh
./tests/run-uds-negotiation-negative.sh
```

Benchmarks:

```bash
./tests/run-live-uds-bench.sh
./tests/run-negotiated-profile-bench.sh
```

Quick manual runs:

```bash
build/bin/netipc-shm-server-demo /tmp netflow 3
build/bin/netipc-shm-client-demo /tmp netflow 41 3

build/bin/netipc-uds-server-demo /tmp netflow 3
build/bin/netipc-uds-client-demo /tmp netflow 41 3

build-mingw/bin/netipc-live-c.exe server-once /tmp netflow
build-mingw/bin/netipc-live-c.exe client-once /tmp netflow 41
```

## Current Limits

- The reusable C library is the only fully implemented library surface in this repo today.
- Go and Rust still rely on helper binaries for the validated live interop/benchmark paths.
- The Windows C transport is implemented for native Named Pipes, but Rust/Go Windows transports are still placeholders.
- Windows validation is still limited to the C smoke path; cross-language Windows interop and benchmark coverage are still pending.
- Netdata integration wiring is intentionally out of scope for this repository phase.
