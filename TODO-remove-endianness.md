# TODO: Remove Endianness Conversion — Optimize for Localhost IPC

## TL;DR
Remove all endianness conversion from plugin-ipc. The system is localhost-only, so both
peers always share the same byte order. Replace per-field encode/decode with whole-struct
memcpy (C), to_ne_bytes (Rust), and binary.NativeEndian (Go).

## Purpose
Fit for purpose: maximum performance localhost IPC for Netdata plugin communication.

## Decisions (approved by Costa)
1. C: Whole-struct memcpy — eliminate all put_u16/get_u16 helpers, add _Static_assert on field offsets
2. Rust: to_ne_bytes / from_ne_bytes — safe, clean, compiler-optimized
3. Go: binary.NativeEndian — idiomatic, safe, compiler-optimized

## Plan

### Phase 1: C — netipc_protocol.h and netipc_protocol.c
- [ ] Add explicit `_reserved` padding fields to `nipc_hello_t` (offset 28) and `nipc_hello_ack_t` (offset 36)
- [ ] Add `_Static_assert` for sizeof and field offsets on all wire structs
- [ ] Replace header encode/decode with single memcpy (sizeof matches wire: 32 bytes)
- [ ] Replace chunk_header encode/decode with single memcpy (sizeof matches wire: 32 bytes)
- [ ] Replace batch_dir encode/decode: memcpy per entry (8 bytes each)
- [ ] Replace hello encode/decode with memcpy (44 bytes, not sizeof due to trailing padding)
- [ ] Replace hello_ack encode/decode with memcpy (sizeof matches wire: 48 bytes)
- [ ] Replace cgroups_req encode/decode with memcpy (4 bytes)
- [ ] Replace cgroups_resp_decode: memcpy for 24-byte header, per-field for directory validation
- [ ] Replace cgroups_resp_item: inline memcpy for scalar reads (no helpers)
- [ ] Replace cgroups_builder_add/finish: inline memcpy for per-field writes
- [ ] Delete all put_u16/put_u32/put_u64/get_u16/get_u32/get_u64 helpers
- [ ] Update all comments (remove "little-endian" references)

### Phase 2: Rust — protocol.rs, transport/posix.rs
- [ ] Replace all `.to_le_bytes()` with `.to_ne_bytes()`
- [ ] Replace all `::from_le_bytes()` with `::from_ne_bytes()`
- [ ] Update doc comments

### Phase 3: Go — protocol/frame.go + 6 other files
- [ ] Change `var le = binary.LittleEndian` to `var ne = binary.NativeEndian`
- [ ] Replace all `le.PutUint*` → `ne.PutUint*`, `le.Uint*` → `ne.Uint*`
- [ ] Update all other Go files using binary.LittleEndian
- [ ] Update doc comments

### Phase 4: Tests and benchmarks
- [ ] Update test fixture files (test_chaos.c, interop_uds/main.go)
- [ ] Update benchmark drivers
- [ ] Run C tests (Linux)
- [ ] Run Rust tests (Linux)
- [ ] Run Go tests (Linux)
- [ ] Run cross-language interop tests
- [ ] Run Windows tests (C, Rust, Go)

### Phase 5: Documentation
- [ ] Update docs/level1-wire-envelope.md: "host byte order" instead of "little-endian"
- [ ] Update netipc_protocol.h header comment
- [ ] Update protocol.rs doc comment
- [ ] Update frame.go package comment

## Files affected
- src/libnetdata/netipc/include/netipc/netipc_protocol.h
- src/libnetdata/netipc/src/protocol/netipc_protocol.c
- src/crates/netipc/src/protocol.rs
- src/crates/netipc/src/transport/posix.rs
- src/go/pkg/netipc/protocol/frame.go
- src/go/pkg/netipc/transport/posix/shm_linux.go
- src/go/pkg/netipc/transport/posix/shm_linux_test.go
- src/go/pkg/netipc/transport/posix/uds_test.go
- src/go/pkg/netipc/transport/windows/shm.go
- src/go/pkg/netipc/service/cgroups/client.go
- src/go/pkg/netipc/service/cgroups/client_windows.go
- bench/drivers/go/main.go
- bench/drivers/go/main_windows.go
- tests/fixtures/c/test_chaos.c
- tests/fixtures/go/cmd/interop_uds/main.go
- docs/level1-wire-envelope.md
