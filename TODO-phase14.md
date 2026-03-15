# TODO Phase 14: Windows Named Pipe Transport

## TL;DR
Implement Windows Named Pipe transport in C, Rust, and Go — mirroring the POSIX UDS patterns. Pipe name derivation uses FNV-1a 64-bit hash. Message mode pipes with handshake, chunking, in-flight tracking identical to UDS.

## Purpose
Enable plugin-ipc to operate on Windows using native Named Pipes as the baseline transport (profile bit 0).

## Status: IN PROGRESS

## Analysis

### Files to create:
1. **C header**: `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
2. **C impl**: `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
3. **Rust**: `src/crates/netipc/src/transport/windows.rs`
4. **Go**: `src/go/pkg/netipc/transport/windows/pipe.go`
5. **C test**: `tests/fixtures/c/test_named_pipe.c`
6. **C interop**: `tests/fixtures/c/interop_named_pipe.c`
7. **Rust interop**: `tests/fixtures/rust/src/bin/interop_named_pipe.rs`
8. **Go interop**: `tests/fixtures/go/cmd/interop_named_pipe/main.go`
9. **Go test**: `src/go/pkg/netipc/transport/windows/pipe_test.go`
10. **Interop script**: `tests/test_named_pipe_interop.sh`
11. **CMakeLists.txt**: update with Windows targets
12. **Rust transport/mod.rs**: add `#[cfg(windows)] pub mod windows;`
13. **Cargo.toml**: add `windows-sys` dependency (target-specific)

### Key design decisions (from spec):
- Pipe name: `\\.\pipe\netipc-{FNV1a_64(run_dir):016x}-{service}`
- FNV-1a: offset=0xcbf29ce484222325, prime=0x00000100000001B3
- Service name validation: [a-zA-Z0-9._-] only
- Message mode pipes: PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE
- Multi-client: create new pipe instance before/after each accept
- Disconnect: ERROR_BROKEN_PIPE, ERROR_NO_DATA, ERROR_PIPE_NOT_CONNECTED → graceful
- Handshake, chunking, in-flight tracking: identical to UDS
- Default packet size: 65536 (no SO_SNDBUF to query on Named Pipes)
- Profile bit 0 (0x01) = NAMED_PIPE = Windows baseline

### Platform guards:
- C: `#ifdef _WIN32`
- Rust: `#[cfg(windows)]`
- Go: `//go:build windows`

## Plan
1. Create C header + implementation
2. Create Rust implementation
3. Create Go implementation
4. Create C test binary
5. Create interop binaries (C, Rust, Go)
6. Create interop test script
7. Update CMakeLists.txt
8. Update Rust mod.rs and Cargo.toml
9. Verify Go cross-compilation: `GOOS=windows go build`
10. Verify Rust cross-check: `cargo check --target x86_64-pc-windows-gnu`
