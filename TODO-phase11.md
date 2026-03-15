# TODO: Phase 11 - L2 Orchestration in Rust and Go

## TL;DR
Implement Level 2 (typed client context + managed server) in Rust and Go, wire-compatible with the C implementation from Phase 10. Create cross-language L2 interop tests covering the full 9-pair matrix (C/Rust/Go server x C/Rust/Go client).

## Purpose
Provide Rust and Go plugins with the same L2 convenience orchestration that C already has: blocking typed calls with at-least-once retry, managed servers with handler dispatch, and SHM upgrade when negotiated.

## Analysis of Current State

### C L2 (reference, complete)
- `src/libnetdata/netipc/include/netipc/netipc_service.h` — public API
- `src/libnetdata/netipc/src/service/netipc_service.c` — implementation
- `tests/fixtures/c/test_service.c` — 6 integration tests

### Existing Rust L1 + Codec
- `src/crates/netipc/src/transport/posix.rs` — UDS (UdsSession, UdsListener)
- `src/crates/netipc/src/transport/shm.rs` — SHM (ShmContext)
- `src/crates/netipc/src/protocol.rs` — Codec (Header, CgroupsRequest, CgroupsResponseView, CgroupsBuilder)
- `src/crates/netipc/src/lib.rs` — declares `protocol` and `transport` modules
- `src/crates/netipc/src/service/` — exists but empty

### Existing Go L1 + Codec
- `src/go/pkg/netipc/transport/posix/uds.go` — UDS (Session, Connect, Listen, etc.)
- `src/go/pkg/netipc/transport/posix/shm_linux.go` — SHM (ShmContext)
- `src/go/pkg/netipc/protocol/frame.go` — Codec (Header, CgroupsRequest, CgroupsResponseView, CgroupsBuilder)
- `src/go/pkg/netipc/service/` — exists but empty

### Build system
- `CMakeLists.txt` — has test_service for C, Rust/Go UDS and SHM tests, interop scripts

## Plan

### 1. Rust L2: `src/crates/netipc/src/service/mod.rs`
- Module declaration + re-exports
- Add `pub mod service;` to `lib.rs`

### 2. Rust L2: `src/crates/netipc/src/service/cgroups.rs`
Client:
- `CgroupsClient` struct with state machine, transport config, session, SHM, counters
- `new()` — creates context, does NOT connect
- `refresh()` — connect/reconnect, returns state change
- `ready()` — cheap boolean
- `status()` — diagnostics snapshot
- `call_snapshot()` — blocking typed call with mandatory retry
- `close()` — tear down

Server:
- `CgroupsServer` struct
- `new()` with config + handler closure
- `run()` — blocking acceptor loop
- `stop()` — signal shutdown

Handler signature: `FnMut(u16, &[u8]) -> Option<Vec<u8>>`

Tests (in `#[cfg(test)]` module):
- Client lifecycle
- Typed call with snapshot verification
- Retry on server restart
- Multiple clients
- Handler failure
- Status counters

### 3. Go L2: `src/go/pkg/netipc/service/cgroups/client.go`
Build-tagged `//go:build unix`

Client:
- `Client` struct
- `NewClient()` — creates context
- `Refresh()` — connect/reconnect
- `Ready()` — cheap boolean
- `Status()` — diagnostics
- `CallSnapshot()` — blocking typed call with retry
- `Close()` — tear down

Server:
- `Server` struct
- `NewServer()` with config + handler func
- `Run()` — blocking
- `Stop()` — signal shutdown

Handler: `func(methodCode uint16, request []byte) ([]byte, bool)`

Tests in `client_test.go`:
- Same coverage as Rust

### 4. Interop binaries
- C: `tests/fixtures/c/interop_service.c` (server + client subcommands using L2)
- Rust: `tests/fixtures/rust/src/bin/interop_service.rs` (server + client)
- Go: `tests/fixtures/go/cmd/interop_service/main.go` (server + client)

### 5. Interop test script
- `tests/test_service_interop.sh` — full 9-pair matrix

### 6. CMakeLists.txt updates
- Add Rust L2 test target
- Add Go L2 test target
- Add C interop_service binary target
- Add L2 interop test target

## Key Design Decisions (mirrors C)

1. **State machine**: DISCONNECTED, CONNECTING, READY, NOT_FOUND, AUTH_FAILED, INCOMPATIBLE, BROKEN
2. **Retry**: When call fails AND was previously READY → disconnect, reconnect (full handshake), retry ONCE
3. **transport_status check**: Before decode, if transport_status != OK, return error
4. **Handler failure**: INTERNAL_ERROR + empty payload
5. **SHM upgrade**: If L1 session negotiated SHM profile, attach and route through SHM
6. **No hidden threads** in client; server uses blocking acceptor loop
7. **Handler signature**: receives raw method_code + request bytes, returns Option/bool for success/failure

## Testing Requirements
- All 6 test scenarios matching C test_service.c
- Cross-language 9-pair interop matrix
- 100% code path coverage

## Status: COMPLETE - Pending user review

### Files Created/Modified
- `src/crates/netipc/src/service/mod.rs` — Rust service module declaration
- `src/crates/netipc/src/service/cgroups.rs` — Rust L2 client + server + tests (6 tests)
- `src/crates/netipc/src/lib.rs` — added `pub mod service`
- `src/crates/netipc/Cargo.toml` — added interop_service binary
- `src/go/pkg/netipc/service/cgroups/client.go` — Go L2 client + server (pure Go, no cgo)
- `src/go/pkg/netipc/service/cgroups/client_test.go` — Go L2 tests (6 tests)
- `tests/fixtures/c/interop_service.c` — C interop binary
- `tests/fixtures/rust/src/bin/interop_service.rs` — Rust interop binary
- `tests/fixtures/go/cmd/interop_service/main.go` — Go interop binary
- `tests/test_service_interop.sh` — 9-pair cross-language interop test
- `CMakeLists.txt` — added test_service_rust, test_service_go, interop targets

### Test Results
- 16/16 tests pass (all pre-existing + 3 new: test_service_rust, test_service_go, test_service_interop)
- 9/9 interop pairs pass (C/Rust/Go server x C/Rust/Go client)
