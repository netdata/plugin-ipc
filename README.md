# plugin-ipc

Cross-language IPC library for Netdata plugins and helper services.

This repository contains the C library, Rust crate, and Go package for the
same wire contracts and typed APIs. The goal is simple:

- one specification
- one interoperable protocol stack
- one typed service model
- one local snapshot/cache helper layer
- three implementations: C, Rust, Go

This README is a summary of the current verified state of the repository.
The authoritative specifications live under [docs/](docs/README.md).

## What This Repository Implements

- **Level 1 transport**
  - connection lifecycle
  - handshake and profile negotiation
  - framing, chunking, batching, pipelining
  - baseline and shared-memory transports
- **Codec**
  - wire encode/decode
  - typed views
  - response builders
  - validation rules
- **Level 2 typed API**
  - typed client calls
  - managed typed servers
  - retry / reconnect behavior
  - internal reusable buffers
- **Level 3 snapshot API**
  - refresh
  - local cache construction
  - fast hash lookup
  - cache preservation on failure

The design is layered:

- **Level 1** and **Codec** are parallel building blocks
- **Level 2** composes Level 1 + Codec
- **Level 3** builds on Level 2

See:

- [docs/README.md](docs/README.md)
- [docs/level2-typed-api.md](docs/level2-typed-api.md)
- [docs/level3-snapshot-api.md](docs/level3-snapshot-api.md)
- [docs/getting-started.md](docs/getting-started.md)

## Platforms And Transports

| Platform | Baseline transport | Negotiated fast path | Languages |
|---|---|---|---|
| POSIX / Linux | Unix domain `SOCK_SEQPACKET` | POSIX shared memory | C, Rust, Go |
| Windows | Named Pipes | Windows shared memory | C, Rust, Go |

Important facts:

- the same wire contracts are implemented in all three languages
- cross-language interoperability is mandatory
- Go stays pure Go, without `cgo`
- Level 2 and Level 3 are transport-agnostic from the caller perspective

## API Levels

### Level 1: Transport

Level 1 works with framed byte messages.

It owns:

- send / receive
- message IDs
- batch directories
- chunk continuation
- profile negotiation
- transport-specific session details

Relevant specs:

- [docs/level1-transport.md](docs/level1-transport.md)
- [docs/level1-wire-envelope.md](docs/level1-wire-envelope.md)
- [docs/level1-posix-uds.md](docs/level1-posix-uds.md)
- [docs/level1-posix-shm.md](docs/level1-posix-shm.md)
- [docs/level1-windows-np.md](docs/level1-windows-np.md)
- [docs/level1-windows-shm.md](docs/level1-windows-shm.md)

### Codec

Codec is pure wire-format logic.

It owns:

- encode / decode
- typed views over payload bytes
- response builders
- validation of field layout and bounds

It does **not** own:

- sockets
- pipes
- shared memory mappings
- retries
- cache policy

Relevant specs:

- [docs/codec.md](docs/codec.md)
- [docs/codec-cgroups-snapshot.md](docs/codec-cgroups-snapshot.md)

### Level 2: Typed API

Level 2 is the public convenience layer.

The public contract is:

- clients issue typed calls
- servers register typed handlers
- callers do not manage transport scratch buffers
- callers do not manipulate raw payload bytes

Relevant specs:

- [docs/level2-typed-api.md](docs/level2-typed-api.md)
- [docs/getting-started.md](docs/getting-started.md)

### Level 3: Snapshot / Cache

Level 3 provides:

- typed snapshot refresh
- local materialization
- O(1)-style hash lookup on the hot path
- cache retention across refresh failures

Relevant spec:

- [docs/level3-snapshot-api.md](docs/level3-snapshot-api.md)

## Interoperability

This repository is intentionally built around interoperability, not
single-language wrappers.

What is covered:

- C client -> C / Rust / Go server
- Rust client -> C / Rust / Go server
- Go client -> C / Rust / Go server
- baseline transport matrices on POSIX and Windows
- shared-memory matrices on POSIX and Windows
- typed Level 2 services
- snapshot refresh and local lookup flows
- benchmark matrices across all directed pairs

The interop results are validated by the test suite and by the checked-in
benchmark reports:

- [benchmarks-posix.md](benchmarks-posix.md)
- [benchmarks-windows.md](benchmarks-windows.md)

## Performance Snapshot

The repository includes checked-in benchmark reports with complete,
fail-closed matrices:

- POSIX report: [benchmarks-posix.md](benchmarks-posix.md)
  - generated `2026-03-22`
  - machine: `costa-desktop`
  - complete matrix rows: `201`
- Windows report: [benchmarks-windows.md](benchmarks-windows.md)
  - generated `2026-03-17`
  - machine: `win11`
  - complete matrix rows: `201`

Headline numbers from the current checked-in reports:

- **POSIX baseline UDS ping-pong**
  - `166.9k` to `190.5k` req/s across the 3x3 language matrix
- **POSIX SHM ping-pong**
  - `2.41M` to `3.33M` req/s
- **POSIX UDS batch ping-pong**
  - `19.38M` to `28.65M` req/s
- **POSIX SHM batch ping-pong**
  - `24.94M` to `44.88M` req/s
- **POSIX snapshot refresh**
  - baseline: `139.3k` to `166.0k` req/s
  - SHM: `988.4k` to `1.67M` req/s
- **POSIX local cache lookup**
  - C: `73.12M` req/s
  - Go: `110.46M` req/s
  - Rust: `198.75M` req/s

- **Windows Named Pipe ping-pong**
  - `15.6k` to `18.5k` req/s
- **Windows SHM ping-pong**
  - `1.74M` to `2.55M` req/s
- **Windows Named Pipe batch ping-pong**
  - `5.39M` to `7.77M` req/s
- **Windows SHM batch ping-pong**
  - `12.96M` to `56.95M` req/s
- **Windows snapshot refresh**
  - Named Pipe: `15.9k` to `17.9k` req/s
  - SHM: `246.7k` to `1.04M` req/s
- **Windows local cache lookup**
  - C: `123.71M` req/s
  - Go: `107.37M` req/s
  - Rust: `155.79M` req/s

The full reports include:

- per-pair throughput
- latency percentiles
- client and server CPU
- complete scenario validation summaries
- performance floor checks

## Reliability, Testing, And Coverage

The repo is not asking the reader to trust the design on words alone.
The current validation story includes:

- CMake-based build and `ctest` workflows
- unit tests
- cross-language interop tests
- typed service tests
- transport tests
- shared-memory tests
- coverage scripts for C, Go, and Rust
- benchmark generators that reject incomplete matrices

### Current verified state

Linux / POSIX:

- build: passing
- `ctest`: `37/37` passing
- C coverage: `90.5%`
- Go coverage: `95.8%`
- Rust coverage: `90.06%`
  - measured with `tarpaulin` on this host
  - current total still includes some Windows-tagged lines in shared Rust files

Windows (`win11`):

- build: passing
- `ctest`: `28/28` passing
- C coverage: `83.9%`
- Go coverage: `96.7%`
- Rust coverage: `93.59%`

Important honesty point:

- core build, transport, service, interop, and benchmark validation is strong
  on both Linux and Windows
- Linux still has broader chaos / hardening / stress breadth than Windows
- so the platforms are in **good functional parity**, but not yet in
  **full validation parity**

Coverage details:

- [WINDOWS-COVERAGE.md](WINDOWS-COVERAGE.md)
- [COVERAGE-EXCLUSIONS.md](COVERAGE-EXCLUSIONS.md)

## Specifications And Trust Model

The specs are authoritative.

Rule:

- when code and spec disagree, the spec wins unless explicitly revised

Start here:

- [docs/README.md](docs/README.md)

Recommended reading order:

1. [docs/README.md](docs/README.md)
2. [docs/getting-started.md](docs/getting-started.md)
3. [docs/level2-typed-api.md](docs/level2-typed-api.md)
4. [docs/level3-snapshot-api.md](docs/level3-snapshot-api.md)
5. the relevant Level 1 transport spec for your platform

## Building And Running

### Linux / POSIX

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

Coverage:

```bash
bash tests/run-coverage-c.sh
bash tests/run-coverage-go.sh
bash tests/run-coverage-rust.sh
```

### Windows (`win11`)

Use a `mingw64` shell with native Windows `cargo` and `go` ahead of any MSYS
toolchain copies in `PATH`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

Coverage:

```bash
bash tests/run-coverage-c-windows.sh
bash tests/run-coverage-go-windows.sh
bash tests/run-coverage-rust-windows.sh
```

For practical Windows workflow details, see:

- [WINDOWS-COVERAGE.md](WINDOWS-COVERAGE.md)
- [TODO-pending-from-rewrite.md](TODO-pending-from-rewrite.md)

## Repository Layout

```text
.
├── bench/
│   └── drivers/
├── docs/
├── src/
│   ├── crates/netipc/
│   ├── go/pkg/netipc/
│   └── libnetdata/netipc/
└── tests/
    ├── fixtures/
    └── run-*.sh
```

Main implementation roots:

- C: [src/libnetdata/netipc/](src/libnetdata/netipc/)
- Rust: [src/crates/netipc/](src/crates/netipc/)
- Go: [src/go/pkg/netipc/](src/go/pkg/netipc/)

## Current Limits

This is the honest current state:

- coverage thresholds are enforced, but they are **not** at `100%`
- Linux and Windows are functionally close, but Windows still has less
  chaos / hardening / stress breadth
- some documented exclusions still require special infrastructure such as:
  - allocation-failure injection
  - kernel / OS failure injection
  - race-window orchestration

Those limits are tracked explicitly instead of being hidden:

- [COVERAGE-EXCLUSIONS.md](COVERAGE-EXCLUSIONS.md)
- [TODO-pending-from-rewrite.md](TODO-pending-from-rewrite.md)

## Summary

If you need the shortest trustworthy summary:

- the protocol stack is specified, implemented, and measured in C, Rust, and Go
- interop across the three languages is a core requirement, not an afterthought
- both POSIX and Windows have validated baseline and SHM benchmark matrices
- the public service API is typed
- the local snapshot/cache layer is implemented and benchmarked
- the repo has real tests, real coverage, and real benchmark artifacts checked in

For design details, start with [docs/README.md](docs/README.md).
