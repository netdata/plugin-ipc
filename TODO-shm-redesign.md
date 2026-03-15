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

## Decisions Made

### 1. SHM session scoping — DECIDED: Extend hello-ack with session_id

**Decision**: Option A — add `session_id` (u64) to hello-ack payload.

- Hello-ack grows from 36 → 48 bytes (4 bytes padding at offset 36 + 8 bytes session_id at offset 40)
- New field: `session_id` (u64) at offset 40, little-endian
- Server generates session_id via monotonic counter (starts at 1, increments per accepted session)
- Counter resets on server restart (all clients reconnect anyway, getting new session IDs)
- No layout_version bump needed (no existing external consumers — all code is in this repo)
- SHM path derivation:
  - POSIX: `{run_dir}/{service_name}-{session_id:016x}.ipcshm`
  - Windows: include `session_id` in the FNV-1a hash for kernel object names

**Rationale**: No production deployments exist. All clients are in this repo.
Session ID as a first-class protocol concept is cleaner than repurposing
message_id or adding extra control messages.

### 2. L2 handler shape — DECIDED: Keep raw payload for v1

**Decision**: Option B — keep raw `(method_code, request_payload) → response_bytes`
handler signature for v1. The typed builder pattern is an ergonomic improvement
that can be added as a non-breaking wrapper later.

**Rationale**: Functionally correct. Priority is the SHM security and
multi-client fix. Typed builders are API sugar, not a correctness issue.

### 3. SHM region cleanup — DECIDED: On close + on startup

- **On session close/exit**: server immediately unlinks the per-session `.ipcshm` file
- **On server startup**: server scans for and cleans up stale `.ipcshm` files
  from previous crashes/hard reboots (using owner_pid/generation stale detection)
- Stale detection remains the safety net for crashes; immediate cleanup is the
  normal path

### 4. Session ID generation — DECIDED: Monotonic counter

- Server maintains a monotonic counter (u64), starting at 1
- Incremented per accepted session
- Resets to 1 on server restart (all clients reconnect, getting fresh IDs)
- Simple, deterministic, sufficient (no need for random — no info leakage concern)

## Plan

### Phase 1: Spec updates — DONE
1. ~~Update `docs/level1-wire-envelope.md`~~ — hello-ack 48 bytes (36 + 4 padding + 8 session_id)
2. ~~Update `docs/level1-posix-shm.md`~~ — per-session paths, lifecycle, receive validation, stale cleanup
3. ~~Update `docs/level1-windows-shm.md`~~ — per-session kernel objects, receive validation

### Phase 2: SHM receive length validation fix (security) — DONE
4. ~~Fix all 6 SHM implementations~~ — validate against `min(buf_size, area_capacity)` before memcpy
5. SHM chaos tests for Rust/Go — deferred to Phase 6

### Phase 3: Protocol — hello-ack session_id — DONE
6. ~~Update hello-ack encode/decode in C, Rust, Go~~ — session_id at offset 40 (u64)
7. ~~Add session_id counter~~ — atomic monotonic counter in server accept path (all 3 languages)
8. ~~Update hello-ack size validation~~ — 36 → 48 bytes everywhere

### Phase 4: Per-session SHM implementation — DONE
9. ~~SHM path derivation~~ — `{service_name}-{session_id:016x}.ipcshm` in all implementations
10. ~~Server SHM creation~~ — per-session with O_EXCL after handshake
11. ~~Server SHM cleanup~~ — unlink on session close/exit (destroy unlinks path)
12. ~~Stale SHM cleanup~~ — `cleanup_stale` function in all 3 languages
13. ~~Client SHM attachment~~ — uses session_id from hello-ack
14. ~~Removed single-region assumption~~ — no more shm_in_use flag

### Phase 5: L2 managed server — multi-client SHM — DONE
The architecture already spawns one handler thread per accepted client.
Each thread reads from its own SHM region (or UDS fd), dispatches the
handler, and sends the response. No separate "reader thread" needed.

15. ~~Removed `shm_in_use` flag~~ — was gating SHM to one client; now all
    clients get their own per-session SHM region when SHM is negotiated
16. ~~Changed shutdown detection timeout~~ — 500ms → 100ms (poll/futex
    timeout between `running` flag checks), named constant in all 3 languages
17. Per-session SHM lifecycle already handled: server creates region after
    handshake, handler thread destroys it on session close

### Phase 6: Testing and validation — DONE
18. ~~ASAN clean~~ — all 8 C tests pass (memory growth threshold adjusted for sanitizer overhead)
19. ~~TSAN clean~~ — all 8 C tests pass
20. ~~Multi-client SHM tests~~ — added `test_shm_multi_client` in Rust and Go (3 concurrent
    independent SHM sessions, unique payloads, no cross-contamination)
21. ~~SHM chaos tests~~ — added `test_shm_chaos_forged_length` in Rust and Go (forged req_len/
    resp_len values: 0, capacity-1, capacity, capacity+1, 0xFFFFFFFF — no panic, no OOB read)
22. ~~All interop/stress tests pass~~ — 33/33 ctest targets pass
23. External reviewers — deferred (separate step, not blocking)

## Follow-up: Eliminate endianness overhead
- Separate TODO: remove unnecessary little-endian encoding for localhost IPC
- Replace with native byte order (mechanical change: C memcpy, Rust to_ne_bytes, Go NativeEndian)
- No production deployments exist — now is the cheapest time

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
