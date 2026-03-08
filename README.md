# plugin-ipc

Cross-language IPC workbench and library repository for Netdata plugins.

This repository is now organized to mirror the eventual Netdata monorepo layout:

- C library code: `src/libnetdata/netipc/`
- Go package home: `src/go/pkg/netipc/`
- Rust crate home: `src/crates/netipc/`
- Test fixtures and helper apps: `tests/fixtures/`
- Benchmark drivers: `bench/drivers/`

The current validated implementation is still primarily the POSIX prototype:

- C typed frame/schema library
- POSIX shared-memory hybrid transport
- POSIX `UDS_SEQPACKET` transport with profile negotiation
- Rust/Go helper binaries for interop and benchmark validation

The reusable Go package and Rust crate locations are now scaffolded, but their full library port is still pending.

## Build

Canonical build entry point:

```bash
cmake -S . -B build
cmake --build build
```

Compatibility wrapper:

```bash
make
```

Primary build artifacts:

- `build/lib/libnetipc.a`
- `build/bin/ipc-bench`
- `build/bin/netipc-codec-c`
- `build/bin/netipc-shm-server-demo`
- `build/bin/netipc-shm-client-demo`
- `build/bin/netipc-uds-server-demo`
- `build/bin/netipc-uds-client-demo`
- `build/bin/netipc-codec-rs`
- `build/bin/netipc_live_rs`
- `build/bin/netipc_live_uds_rs`
- `build/bin/netipc-codec-go`
- `build/bin/netipc-live-go`

## Repository Layout

```text
.
├── CMakeLists.txt
├── bench/
│   └── drivers/
├── docs/
├── src/
│   ├── libnetdata/
│   │   └── netipc/
│   ├── go/
│   │   └── pkg/netipc/
│   └── crates/
│       └── netipc/
└── tests/
    ├── fixtures/
    └── run-*.sh
```

## POSIX Baseline

- Baseline profile: `UDS_SEQPACKET`
- Negotiation: fixed-binary hello/ack with supported/preferred bitmasks and server-selected profile
- Optional fast profile: `SHM_HYBRID` (`profile 2`) for C/Rust live paths
- Go remains `UDS_SEQPACKET` only in the current phase

## C Library Paths

Headers:

- `src/libnetdata/netipc/include/netipc/netipc_schema.h`
- `src/libnetdata/netipc/include/netipc/netipc_shm_hybrid.h`
- `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`

Sources:

- `src/libnetdata/netipc/src/protocol/netipc_schema.c`
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

build/bin/ipc-bench --transport stream --mode pingpong --clients 1 --payloads 32 --duration 1
build/bin/ipc-bench --transport shm-hybrid --mode pingpong --clients 1 --payloads 32 --duration 5 --target-rps 100000
```

## Current Limits

- The reusable C library is the only fully implemented library surface in this repo today.
- Go and Rust still rely on helper binaries for the validated live interop/benchmark paths.
- Windows transport directories are scaffolded, not implemented.
- Netdata integration wiring is intentionally out of scope for this repository phase.
