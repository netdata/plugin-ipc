# TODO: SHM Per-Session Redesign

## Purpose
- Fix the SHM transport so that multiple concurrent clients each get
  their own SHM region, as required by the multi-client server spec.
- Fix the SHM receive length validation security bug.
- This is a design change, not a patch. It affects the wire protocol,
  the specs, and all 6 SHM implementations.

## TL;DR
- The current SHM design uses one `.ipcshm` file per service.
- Only one client can use SHM at a time. Others fall back to UDS silently.
- This violates the multi-client spec and degrades performance for all
  but one client without their knowledge.
- Additionally, SHM receive trusts peer-controlled `req_len`/`resp_len`
  without checking them against the mapped area capacity — a security bug
  that can cause out-of-bounds reads.

## MANDATORY: Read Before Any Work

Read every file in `docs/` in full. Especially:
- `docs/level1-posix-shm.md` — current SHM spec
- `docs/level1-windows-shm.md` — current Windows SHM spec
- `docs/level1-transport.md` — multi-client requirement
- `docs/level2-typed-api.md` — managed server multi-client requirement
- `docs/level1-wire-envelope.md` — hello/hello-ack wire format

Read `TODO-rewrite.md` for the quality mandate.
Read `TODO-hardening.md` for the hardening phases completed.

## Current State

### What works
- Phases 0-16: full rewrite complete (C/Rust/Go, POSIX/Windows)
- Phases H1-H9: hardening complete (multi-worker server, sanitizers,
  stress testing, fuzz testing, coverage, benchmarks)
- H10: multiple review rounds found and fixed many issues
- All POSIX tests pass (549+ C, 112 Rust, Go all green)
- All Windows tests pass (26 NP + 25 SHM + 9+9 interop)
- ASAN/TSAN/valgrind clean
- SHM performance: 3.0M req/s (POSIX), meeting 1M floor

### What's broken (from Codex pass 3 review)

1. **CRITICAL — SHM receive length not validated**
   - `req_len`/`resp_len` read from shared memory (peer-controlled)
     used directly for memcpy without checking against area capacity
   - Affects ALL 6 SHM implementations:
     - `netipc_shm.c:565`
     - `netipc_win_shm.c:609`
     - `shm.rs:512`
     - `win_shm.rs:577`
     - `shm_linux.go:485`
     - `shm.go:572`
   - Impact: forged length → out-of-bounds read, UB in C/Rust, panic in Go

2. **CRITICAL — SHM not session-scoped**
   - SHM region named per service (`{service}.ipcshm`), not per session
   - Only first client gets SHM via `shm_in_use` flag
   - Others silently fall back to UDS — 15x performance degradation
   - A second client could theoretically attach to the first's region
     (race condition before `shm_in_use` is set)
   - Violates: multi-client spec, transparent SHM upgrade spec

3. **HIGH — L2 handler uses raw payload, not typed builder**
   - Spec requires typed single-item handlers with response builder
   - Implementation uses raw `(method_code, request_payload) → response_bytes`
   - Functionally correct but not spec-compliant

4. **MEDIUM — SHM chaos tests missing in Rust/Go**
   - C has SHM chaos tests (test_chaos.c)
   - Rust and Go don't have equivalent malformed-SHM-length tests

5. **MEDIUM — SEQPACKET first-packet truncation edge case**
   - If caller buffer < first packet, kernel truncates silently
   - Mitigated by dynamic buffer allocation but edge case exists

## Decisions Needed

1. **SHM session scoping mechanism**
   - How does the client learn the session-specific SHM path?
   - Options:
     a. Extend hello-ack with a session ID field (wire format change)
     b. Add a post-handshake CONTROL message with SHM path info
     c. Deterministic derivation from values both sides know
   - Each option has implications for the wire spec
   - This needs Costa's decision before implementation

2. **L2 handler shape**
   - Should the handler signature change to use typed builder pattern?
   - Or is the raw payload pattern acceptable for v1?
   - This is a spec-vs-implementation alignment question

## Plan (after decisions)

1. Update `docs/level1-posix-shm.md` and `docs/level1-windows-shm.md`
   with the per-session SHM design
2. Update `docs/level1-wire-envelope.md` if wire format changes
3. Fix SHM receive length validation in all 6 implementations
4. Implement per-session SHM in all 6 implementations
5. Add SHM chaos tests for Rust and Go
6. Update L2 service layer for new SHM session model
7. Run ASAN/TSAN on updated code
8. Run all interop tests
9. Run stress tests with multiple SHM clients
10. Re-run external reviewers (same scope, fourth pass)

## Files Affected

### Specs
- docs/level1-posix-shm.md
- docs/level1-windows-shm.md
- docs/level1-wire-envelope.md (if wire format changes)

### C
- src/libnetdata/netipc/include/netipc/netipc_shm.h
- src/libnetdata/netipc/src/transport/posix/netipc_shm.c
- src/libnetdata/netipc/include/netipc/netipc_win_shm.h
- src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c
- src/libnetdata/netipc/include/netipc/netipc_service.h
- src/libnetdata/netipc/src/service/netipc_service.c
- src/libnetdata/netipc/src/service/netipc_service_win.c

### Rust
- src/crates/netipc/src/transport/shm.rs
- src/crates/netipc/src/transport/win_shm.rs
- src/crates/netipc/src/service/cgroups.rs

### Go
- src/go/pkg/netipc/transport/posix/shm_linux.go
- src/go/pkg/netipc/transport/windows/shm.go
- src/go/pkg/netipc/service/cgroups/client.go
- src/go/pkg/netipc/service/cgroups/client_windows.go

### Tests
- tests/fixtures/c/test_shm.c
- tests/fixtures/c/test_chaos.c
- tests/fixtures/c/test_stress.c
- All SHM interop tests
- New: Rust/Go SHM chaos tests

## Git State
- Branch: main
- Latest commit: `064d905` (Fix unbiased review findings)
- All pushed to origin
- 32 commits total on main
