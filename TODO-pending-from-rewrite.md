# TODO: Pending Items from Rewrite

## TL;DR

Two categories of incomplete work remaining from the plugin-ipc rewrite:

1. **Test coverage**: Target is 100% line coverage across all 3 languages on both POSIX and Windows. Exceptions allowed only if justified and documented.
2. **Benchmark/stress-test matrix**: Must cover all scenarios × all language pairs × both transports × both platforms, including batching and pipelining.

---

## 1. Test Coverage — Target: 100%

### 1.1 Current State

Coverage is measured on **Linux only** via 3 scripts:

| Script | Tool | Files Measured |
|--------|------|----------------|
| `tests/run-coverage-c.sh` | gcov (GCC) | 4 C files: protocol, uds, shm, service |
| `tests/run-coverage-rust.sh` | cargo-llvm-cov or cargo-tarpaulin | netipc crate library code |
| `tests/run-coverage-go.sh` | `go test -coverprofile` | 3 packages: protocol, transport/posix, service/cgroups |

Current threshold: 90%. **New target: 100%.**

Windows: **no instrumented coverage**. Only pass/fail unit tests and interop scripts.

### 1.2 Gaps to Close

#### A. Coverage Infrastructure

- [ ] **Raise threshold from 90% to 100% in all 3 scripts**
  - `run-coverage-c.sh`: change `THRESHOLD=${1:-90}` → `THRESHOLD=${1:-100}`
  - `run-coverage-rust.sh` and `run-coverage-go.sh`: add enforcement (currently report-only)
- [ ] **Add Windows coverage measurement** or document why it's impossible per-tool:
  - C: gcov works with MinGW/GCC on Windows. Feasible.
  - Rust: cargo-tarpaulin is Linux-only. cargo-llvm-cov works on Windows with MSVC toolchain but we cross-compile with `x86_64-pc-windows-gnu`. **Needs investigation**: does llvm-cov work with windows-gnu target?
  - Go: `go test -coverprofile` works natively on Windows. Feasible.
- [ ] **Add coverage to CI** (no CI pipeline exists today — everything is manual)

#### B. Untested Code Paths (POSIX)

- [ ] **Identify and test all uncovered lines** — run coverage scripts, review `.gcov` / tarpaulin / go cover output, add tests for every untested branch
- [ ] **Document justified exclusions** — for each line that cannot reach 100%, document WHY in a `COVERAGE-EXCLUSIONS.md`:
  - Unreachable defensive code (e.g., malloc failure paths in C)
  - Platform-specific branches (e.g., `#ifdef __APPLE__` on Linux-only builds)
  - Each exclusion must state: file, line range, reason, and whether it's tested on another platform

#### C. Untested Code Paths (Windows)

- [ ] **Rust L2 service tests are unix-only** (`#[cfg(all(test, unix))]` at `cgroups.rs:1579`)
  - The entire Rust L2/L3 service module has ZERO unit tests on Windows
  - Only coverage: interop shell scripts (pass/fail, no line-level measurement)
  - **Action**: Add `#[cfg(all(test, windows))]` test module, or make existing tests cross-platform
- [ ] **Go coverage script excludes Windows packages**
  - `run-coverage-go.sh` only measures `transport/posix/`, not `transport/windows/`
  - `pipe_test.go` and `ping_pong_windows_test.go` exist but are not in coverage
  - **Action**: Create `run-coverage-go-windows.sh` that measures Windows packages on Windows
- [ ] **C Windows files not in coverage**
  - `netipc_service_win.c`, `netipc_named_pipe.c`, `netipc_win_shm.c` have no gcov measurement
  - Unit tests exist (`test_named_pipe.c`, `test_win_shm.c`) but coverage not collected
  - **Action**: Create `run-coverage-c-windows.sh` using gcov with MinGW

#### D. Missing Test Types

- [ ] **Fuzz tests on Windows** — currently fuzz tests exist only for POSIX (C: `run-extended-fuzz.sh`, Go: `fuzz_test.go`, Rust: protocol fuzz). Verify they run on Windows or document why not.
- [ ] **Windows stress tests** — `test_stress.c` is POSIX-only (uses pthreads, UDS). No Windows equivalent.

---

## 2. Benchmarking / Stress-Testing

### 2.1 Current State

#### What Exists

**POSIX** (`tests/run-posix-bench.sh`):
- 3 languages: C, Rust, Go
- 4 scenarios: UDS ping-pong, SHM ping-pong, snapshot-baseline, snapshot-SHM
- Full 9-pair cross-language matrix (3×3) at 3 rate tiers (max, 100k/s, 10k/s)
- UDS pipelining: **C-only**, depths 1/4/8/16/32 (C client, C server)
- Local cache lookup: 3 languages
- Negotiated profile comparison: Rust→Rust only

**Windows** (`tests/run-windows-bench.sh`):
- 2 languages: **C and Go only** (Rust missing)
- 4 scenarios: NP ping-pong, Win SHM ping-pong, snapshot-baseline, snapshot-SHM
- 4-pair matrix (2×2) at 3 rate tiers
- Local cache lookup: C, Go
- **No pipelining**
- **No batching**

**Stress tests** (separate from benchmarks):
- C: `test_stress.c` — POSIX only (1000/5000 items, 10/50 concurrent clients, rapid connect/disconnect, 60s stability, SHM lifecycle, mixed transport)
- Rust: 6 stress test functions in `cgroups.rs` — POSIX only (`#[cfg(all(test, unix))]`)
- Go: `stress_test.go` — POSIX only (`//go:build unix`)
- **No Windows stress tests in any language**

**Report generation**:
- `tests/generate-benchmarks-posix.sh` → `benchmarks-posix.md`
- `tests/generate-benchmarks-windows.sh` → `benchmarks-windows.md`
- Performance floor checks: SHM ≥1M req/s, UDS ≥150k req/s, lookup ≥10M/s

### 2.2 Required Full Matrix

#### Scenarios (6 benchmark types)

| # | Scenario | Transport | Description |
|---|----------|-----------|-------------|
| 1 | Ping-pong | Baseline (UDS / Named Pipe) | Single request-response, measures raw latency |
| 2 | Ping-pong | SHM | Single request-response over SHM transport |
| 3 | Ping-pong + batching | Baseline | Batch of random 1–1000 items per request-response |
| 4 | Ping-pong + batching | SHM | Batch of random 1–1000 items per request-response over SHM |
| 5 | Pipelining | Baseline only | Multiple in-flight requests (depth=N), no SHM |
| 6 | Pipelining + batching | Baseline only | Pipelined requests where each carries a random 1–1000 batch |

*Note: Pipelining over SHM is not applicable — SHM is a single shared region, inherently serialized.*

#### Rate Tiers

All scenarios must be tested at **max throughput** (rate=0).

Ping-pong scenarios (1–4) must additionally be tested at:
- 1,000 req/s
- 10,000 req/s
- 100,000 req/s

#### Language Matrix

**Both platforms**: C, Rust, Go — **9 directed pairs** (3×3), full cross-language interop.

#### Full Count

| Platform | Scenario | Pairs | Rate tiers | Subtotal |
|----------|----------|-------|------------|----------|
| POSIX | Ping-pong baseline | 9 | 4 (max, 100k, 10k, 1k) | 36 |
| POSIX | Ping-pong SHM | 9 | 4 | 36 |
| POSIX | Ping-pong + batch baseline | 9 | 4 | 36 |
| POSIX | Ping-pong + batch SHM | 9 | 4 | 36 |
| POSIX | Pipelining baseline | 9 | 1 (max) | 9 |
| POSIX | Pipelining + batch baseline | 9 | 1 (max) | 9 |
| Windows | Ping-pong baseline | 9 | 4 | 36 |
| Windows | Ping-pong SHM | 9 | 4 | 36 |
| Windows | Ping-pong + batch baseline | 9 | 4 | 36 |
| Windows | Ping-pong + batch SHM | 9 | 4 | 36 |
| Windows | Pipelining baseline | 9 | 1 (max) | 9 |
| Windows | Pipelining + batch baseline | 9 | 1 (max) | 9 |
| **Total** | | | | **324** |

*(Plus local cache lookup: 3 languages × 2 platforms = 6 additional)*

### 2.3 Gaps to Close

#### A. Missing Scenarios

- [ ] **Ping-pong + batching (baseline + SHM)** — not implemented in any benchmark driver
  - All bench drivers hardcode `max_request_batch_items = 1` / `max_response_batch_items = 1`
  - Needs: batch builder in bench client (random 1–1000 items per request), batch dispatch in bench server
  - All 3 languages, both POSIX and Windows
- [ ] **Pipelining + batching** — not implemented
  - Currently pipelining exists only for C-on-POSIX, without batching
  - Needs: pipelined requests where each message contains a random 1–1000 batch
- [ ] **Pipelining on Rust and Go** — only C implements `uds-pipeline-client`
  - Rust and Go bench drivers have no pipelining subcmd
  - Full matrix requires all 9 pairs for pipelining (any client → any server)

#### B. Missing Language Coverage

- [ ] **Rust bench driver on Windows** — does not exist
  - `run-windows-bench.sh` only references `bench_windows_c.exe` and `bench_windows_go.exe`
  - CMakeLists.txt has no `bench_windows_rs` target
  - **Action**: Create `bench_windows.rs` (or `--features windows` in existing crate) + CMake target
- [ ] **Windows bench matrix is 2×2 instead of 3×3**
  - Missing: all Rust pairs (Rust→C, C→Rust, Rust→Go, Go→Rust, Rust→Rust)
  - 5 additional pairs per scenario per rate tier

#### C. Missing Platform Coverage

- [ ] **Windows pipelining** — not implemented at all
  - Named Pipe message mode should support multiple in-flight — implement and test
- [ ] **Windows stress tests** — none exist
  - C stress tests use pthreads/UDS (POSIX-only)
  - Need Windows equivalents using Win32 threads + Named Pipes

#### D. Missing Rate Tiers

- [ ] **1k/s rate tier** — not in current scripts
  - `run-posix-bench.sh` uses rates (0, 100000, 10000) — missing 1000
  - `run-windows-bench.sh` uses rates (0, 100000, 10000) — missing 1000
  - **Action**: Add 1000 to `RATES_PING_PONG` arrays in both scripts

#### E. Benchmark Infrastructure

- [ ] **Batch mode in bench drivers** — all bench drivers need batching subcmds
  - Random 1–1000 batch size per request (not a fixed size)
  - Negotiated batch limits must be set to 1000 during handshake
  - Server must handle per-item dispatch and reassemble batch responses
- [ ] **Run scripts need updating** for all new scenarios and rate tiers
  - `run-posix-bench.sh`: add batch ping-pong (baseline+SHM), pipelining+batch, Rust/Go pipelining, 1k/s rate tier
  - `run-windows-bench.sh`: add Rust language, all new scenarios, pipelining, 1k/s rate tier
- [ ] **Report generators need updating**
  - `generate-benchmarks-posix.sh`: add batch and pipeline+batch tables, 1k/s tier
  - `generate-benchmarks-windows.sh`: same + add Rust column

#### F. Benchmark Reporting Requirements (from TODO-rewrite.md, TODO-hardening.md)

Each benchmark run must report (CSV columns already partially exist):
- [ ] **Actual throughput** (req/s) — already present
- [ ] **Server CPU utilization** (% of one core) — already present (from `SERVER_CPU_SEC`)
- [ ] **Client CPU utilization** (% of one core) — already present
- [ ] **Total CPU utilization** (client + server) — already present
- [ ] **Latency percentiles**: p50, p95, p99 (µs) — already present
- [ ] **Correctness verification** — counter chain / payload verification per run (from TODO-rewrite.md: "Each benchmark validates correctness (counter chain verification)")

Additional reporting requirements from TODO-rewrite.md Phase 13:
- [ ] **Performance floors must be enforced** (below = bug, not acceptable):
  - SHM ping-pong max: ≥ 1M req/s for all pairs
  - SHM snapshot refresh max: ≥ 1M req/s for C/Rust pairs, ≥ 800k for Go pairs
  - UDS ping-pong max: ≥ 150k req/s for all pairs
  - UDS snapshot refresh max: ≥ 100k req/s for all pairs
  - Local cache lookup max: ≥ 10M lookups/s for all languages
  - Windows SHM max: ≥ 1M req/s for C/Rust pairs
- [ ] **Reference targets** (match or exceed old implementation):
  - SHM ping-pong: C→C ~3.2M, Rust→Rust ~2.9M, Go→Go ~1.2M
  - UDS ping-pong: C→C ~220k, Rust→Rust ~240k, Go→Go ~164k
  - Local lookup: C ~25M (now ~79M), Rust ~23M (now ~185M), Go ~13M (now ~109M)
- [ ] **Benchmark docs generated from complete runs, never hand-edited**:
  - `benchmarks-posix.md`
  - `benchmarks-windows.md`

---

## 2b. Requirements Extracted from Other TODOs

### From TODO-plugin-ipc.md (decisions 42-43)

> "The level 1 / level 2 / level 3 specs must explicitly require:
> 100% testing coverage, fuzz testing / fuzziness coverage,
> explicit corner-case and abnormal-path coverage, no exceptions."
>
> "Nothing is acceptable for Netdata integration unless the specs
> and implementation together provide enough coverage to make crashes
> from malformed IPC, corner cases, and abnormal situations
> unacceptable by design."

### From TODO-rewrite.md (quality rules)

> "100% test coverage (line + branch) for all library code."
> "Fuzz testing for all decode/parse paths."
> "Cross-language interop tests for all wire contracts."
> "Abnormal path coverage for all failure modes."
> "No exceptions. Nothing integrates into Netdata without this."

### From TODO-hardening.md (completion criteria, unchecked items)

- [ ] 100% line coverage proven in all 3 languages
- [ ] benchmarks-posix.md generated from current code
- [ ] benchmarks-windows.md generated from current code
- [ ] All performance floors met
- [ ] All 4 external reviewers agree: production-ready
- [ ] Costa approves

### From TODO-spec-compliance.md (unchecked items)

- [ ] 1. L2 typed call spec — update docs to match returned-view API
- [ ] 2. L3 status fields — ~~done in previous session~~
- [ ] 3. L2 batch client calls — ~~done in previous session~~
- [ ] 4. L2 server batch dispatch — ~~done in previous session~~
- [ ] 5. SHM interop tests — ~~done in previous session~~
- [ ] 6. Windows service/cache test coverage — still open (Rust service tests unix-only)
- [ ] 7. Coverage tooling — 100% target, not 90%
- [ ] 8. File size discipline — large files still exist (cgroups.rs ~3000 lines)

### From TODO-production-readiness-review.md (blocking findings)

These were identified as blocking for production deployment:
- [ ] SHM session isolation — ~~fixed in SHM redesign~~
- [ ] Protocol violation handling — service layers skip unexpected non-request messages instead of terminating session
- [ ] Client typed-call paths don't validate received header kind/code/message_id before decoding
- [ ] Codec decoders accept overlapping name/path string regions (spec forbids it)
- [ ] No test for unexpected message kinds/codes terminating session
- [ ] No test for rejection of overlapping variable-length fields in cgroups response items

### From TODO-hardening.md Phase H5

> "C MAX_INFLIGHT from 64 to 128" — **now FIXED to unbounded dynamic array**

Pipeline benchmark results from that phase (C→C only):
- depth=1: 175k, depth=4: 395k, depth=8: 533k, depth=16: 629k, depth=32: 656k
- **3.7x throughput improvement at depth 32**
- "Pipelining at Rust and Go bench drivers" was never implemented

### From TODO-plugin-ipc.md (Phase I: Hardening revalidation)

Coverage required for hardening revalidation that is NOT yet done:
- [ ] Disconnect with multiple in-flight requests (pipelining)
- [ ] Reconnect semantics at level 2 and level 3
- [ ] Out-of-order replies under concurrent workers

---

## 7. Feature / Architecture / Decision Gaps (Non-Benchmark)

### 7.1 Pending User Decisions (from TODO-plugin-ipc.md decisions 44-46) — NOW RESOLVED

**Decision 44 — L2 managed server API layering: RESOLVED**
> Yes, expose a generic managed server surface. The current implementation already
> does this: `nipc_server_init()` takes a generic `nipc_server_handler_fn(method_code,
> request_payload)`. Rust `CgroupsServer` and Go `NewServer` similarly accept generic
> handler callbacks. The naming (`CgroupsServer`) is misleading — it's actually generic.
>
> **Action**: Rename `CgroupsServer` → `Server` (or `ManagedServer`) in Rust and Go
> to reflect that it is the generic L2 managed server, not cgroups-specific.

**Decision 45 — Managed server worker-count: RESOLVED**
> `worker_count` 0 or 1 = single-threaded server (no worker pool, inline dispatch).
>
> **Action**: Verify current code handles worker_count=0 and worker_count=1 identically
> (single-threaded). Currently `worker_count` in C controls max concurrent sessions,
> not a separate worker pool. Need to verify behavior at 0 and 1.

**Decision 46 — Managed server shutdown: RESOLVED — Abort is primary, keep drain as optional convenience**
> `stop()` = abort (primary). `drain(timeout_ms)` = optional graceful shutdown with
> deadline, degrades to abort on timeout expiry.
>
> **Action**: Keep both. Document that `stop()` is the primary shutdown path and
> `drain()` is an optional convenience for graceful transitions (config reload,
> clean test teardown). No code changes needed — current implementation is correct.

### 7.2 Protocol Violation Handling (from TODO-production-readiness-review.md)

**VERIFIED** — mostly fixed, one remaining issue:

- [x] ~~Service layers skip unexpected non-request messages~~ — **FIXED**: All 6 implementations (C/Rust/Go × POSIX/Windows) now break out of the session loop when `kind != REQUEST`, terminating the session.
- [x] ~~Client typed-call paths don't validate response headers~~ — **FIXED in single-call paths**: All 6 implementations validate kind, code, message_id, and transport_status in `do_raw_call()`.
- [ ] **C batch path missing message_id validation** — `do_increment_batch_attempt()` in `netipc_service.c` and `netipc_service_win.c` checks kind, code, transport_status, item_count but **NOT message_id**. Rust and Go batch paths DO check message_id. This is a cross-language inconsistency and a correctness bug.
- [ ] No test verifies that unexpected message kinds/codes terminate the session (test gap, not code gap).

### 7.3 Codec Overlap Validation (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 3 languages already implement overlap rejection:
- C: `netipc_protocol.c:529-537` — explicit `name_start < path_end && path_start < name_end` check
- Rust: `cgroups.rs:231-240` — identical logic
- Go: `cgroups.go:241-250` — identical logic

The spec (`docs/codec-cgroups-snapshot.md`) does NOT explicitly mention overlap rejection in its validation rules. **Action**: Add overlap rejection to the spec's validation rules section to document existing behavior.

### 7.4 L2 Handler Shape (from TODO-shm-redesign.md finding #3)

**VERIFIED — consistent**: The server dispatch is generic `(method_code, raw_payload) → raw_response`. Typed dispatch helpers exist at the codec layer (dispatch_increment, dispatch_string_reverse, dispatch_cgroups_snapshot) for use INSIDE handlers, but the server itself is method-agnostic. This is correct and consistent.

### 7.5 Server Naming (from Decision 44 verification)

**VERIFIED — naming is misleading**: `CgroupsServer` (Rust), `package cgroups` (Go) contain zero cgroups-specific logic. The managed server is 100% generic: accepts any method_code, passes raw bytes to the handler. Only the typed CLIENT calls and L3 cache are cgroups-specific.

**Action**: Rename and/or restructure:
- Rust: `CgroupsServer` → `ManagedServer` or move to `service/mod.rs`
- Go: Move generic server to `service/server.go`, keep cgroups client/cache in `service/cgroups/`
- C: Already generic (`nipc_managed_server_t`) — no rename needed

### 7.6 File Size Discipline (from TODO-spec-compliance.md #8, TODO-hardening.md)

Large files still exist. TODO-rewrite.md says "Small files, small functions, single purpose":
- `cgroups.rs`: ~3000 lines
- `posix.rs`: ~2081 lines
- `netipc_service.c`: ~1482 lines
- `netipc_service_win.c`: ~1300 lines
- `client.go`: ~829 lines

Decision from H8: "DEFERRED: file splitting after integration." This needs a decision from Costa.

### 7.7 Rust/Go L3 Cache Lookup Performance (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 3 languages now use O(1) hash lookup:
- C: Open-addressing hash table (Phase H8)
- Rust: `HashMap<(u32, String), usize>` at `cgroups.rs:1419`, lookup at `cgroups.rs:1541`
- Go: `map[cacheKey]int` at `cache.go:25`, lookup at `cache.go:110`

No action needed.

### 7.8 L3 Response Buffer Sizing (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 4 implementations derive the buffer from config:
- If `max_response_payload_bytes > 0`: uses `header_size + max_response_payload_bytes`
- Otherwise: falls back to 65536 default

This is correct. The 64KB fallback is only when no explicit config is provided.

### 7.9 L2 Typed Call Spec (from TODO-spec-compliance.md #1)

> `level2-typed-api.md` describes callback-based delivery but code
> returns decoded views/values directly.

**Action**: Update `docs/level2-typed-api.md` to match the actual returned-view API pattern. Low priority — documentation alignment only.

### 7.10 Windows SHM Interop Failures (from TODO-plugin-ipc.md current status)

Documented failing live pairs (pre-SHM redesign). **Need to verify on win11** whether still failing after per-session SHM redesign.

### 7.11 SEQPACKET First-Packet Truncation (from TODO-shm-redesign.md finding #5)

> "If caller buffer < first packet, kernel truncates silently. Mitigated
> by dynamic buffer allocation but edge case exists."

No test covers this edge case.

### 7.12 C Helper Negative Test Gap (from TODO-hardening.md Phase H2)

> "The C helper still relies more on live/integration coverage than on
> small deterministic helper-only tests."

Rust and Go have deterministic helper-level negative tests. C does not.

### 7.13 Remaining SHM Coverage Gaps (from TODO-hardening.md Phase H2)

- [ ] SHM-mode L2 service test (client+server with SHM profile negotiation)
- [ ] Server session capacity and growth paths
- [ ] Cache build failure paths (malloc failure injection)

### 7.14 Worker Count Semantics (from Decision 45 verification)

**VERIFIED — inconsistent across languages:**

| Language | worker_count=0 | worker_count=1 | Mechanism |
|----------|---------------|----------------|-----------|
| C (POSIX/Windows) | **REJECTED** (`ERR_BAD_LAYOUT`) | 1 concurrent session (thread per session) | Counter check at accept |
| Rust | Silently clamped → 1 | 1 concurrent session (thread per session) | Thread handle tracking |
| Go POSIX | Silently clamped → 1 | 1 concurrent session (goroutine) | Buffered channel semaphore |
| Go Windows | N/A (no parameter) | Always serial (inline) | No concurrency |

**Issues:**
- ~~C rejects 0~~ — **FIXED**: C now clamps `worker_count < 1` → 1, matching Rust/Go.
- None of the implementations have true "inline dispatch" — worker_count=1 still spawns a thread/goroutine for the single session. This is fine functionally.
- Go Windows has no worker_count parameter at all — always serial.

---

## 8. Consolidated Priority List (Post-Analysis)

### BLOCKING for production (must fix before Netdata integration)

1. ~~Protocol violation handling~~ — **VERIFIED FIXED** (session terminates on bad kind). ~~C batch path missing message_id check~~ — **NOW FIXED** in both `netipc_service.c` and `netipc_service_win.c`.
2. ~~Client response validation~~ — **VERIFIED FIXED** in all paths (single + batch).
3. ~~Codec overlap rejection~~ — **VERIFIED ALREADY IMPLEMENTED**. Add to spec docs.
4. 100% test coverage across all 3 languages (current: 70-98% depending on file)
5. ~~Rust/Go L3 cache O(1)~~ — **VERIFIED ALREADY IMPLEMENTED** (HashMap/map).

### HIGH priority (spec compliance / performance)

6. Complete benchmark matrix (324 runs + cache lookups)
7. Rust Windows bench driver (does not exist)
8. Pipelining for Rust and Go bench drivers (only C has it)
9. Batching benchmarks (random 1-1000, no bench driver supports batching today)
10. Windows stress tests (none exist in any language)
11. Rust L2 service unit tests on Windows (`#[cfg(all(test, unix))]` — zero Windows tests)
12. ~~Resolve pending decisions (44, 45, 46)~~ — **ALL RESOLVED** (see section 7.1)
13. Verify Windows SHM interop post-redesign (need to test on win11)
14. ~~Fix C `worker_count=0` rejection~~ — **NOW FIXED** (clamps to 1, matching Rust/Go)
15. Rename `CgroupsServer` → generic name in Rust/Go (misleading naming)

### MEDIUM priority (quality / completeness)

16. ~~L3 response buffer sizing~~ — **VERIFIED ALREADY CORRECT** (derived from config)
17. File size discipline — large files need splitting (Costa decision needed)
18. L2 typed call spec document alignment (`level2-typed-api.md`)
19. Add codec overlap rejection to spec (`codec-cgroups-snapshot.md`)
20. SEQPACKET first-packet truncation test
21. C helper deterministic negative tests
22. SHM-mode L2 service test (L2 client+server with SHM negotiation)
23. Test: unexpected message kind terminates session
24. Performance floors enforcement in CI

---

## 3. Decisions (Resolved)

1. **Windows Rust coverage**: Documented exclusion. Rust Windows coverage relies on interop tests + code review. Justified: cargo-tarpaulin is Linux-only, llvm-cov doesn't work with windows-gnu target.
2. **Pipelining on Windows**: Implement and test. Named Pipes should support pipelining — investigate any claim otherwise.
3. **Batch sizes for benchmarks**: Random 1–1000 per request.

---

## 4. Implementation Plan

### Phase A: Quick Fixes
Small, independent, low-risk items.

1. Rename `CgroupsServer` → `ManagedServer` in Rust; restructure Go server naming
2. Add overlap rejection rule to `docs/codec-cgroups-snapshot.md`
3. Update `docs/level2-typed-api.md` to match returned-view API
4. Add 1k/s rate tier to both run scripts (one-line each)
5. Add test: unexpected message kind terminates session (C, Rust, Go)
6. Add test: SEQPACKET first-packet truncation edge case
7. Add C helper deterministic negative tests (matching Rust/Go pattern)
8. Add SHM-mode L2 service test (client+server with SHM profile negotiation)
9. Commit, push, verify on win11

### Phase B: POSIX Coverage to 100%
1. Run all 3 coverage scripts, record baseline numbers
2. Analyze every uncovered line — categorize: testable vs exclusion
3. Write targeted tests for all testable uncovered lines
4. Create `COVERAGE-EXCLUSIONS.md` for genuinely untestable paths
5. Raise thresholds to 100%
6. Verify all 3 scripts pass at 100% (with exclusion list)
7. Commit, push

### Phase C: Windows Coverage
1. Create `run-coverage-c-windows.sh` (gcov with MinGW)
2. Create `run-coverage-go-windows.sh` (`go test -coverprofile` for Windows packages)
3. Document Rust Windows coverage exclusion
4. Add Rust L2 service tests for Windows
5. ssh win11, run coverage, identify gaps, write tests
6. Commit, push

### Phase D: Benchmark Drivers — Batching + Pipelining

**D1: POSIX batching** (C, Rust, Go):
- Add batch ping-pong subcmds (baseline + SHM)
- Random 1–1000 batch size, negotiate batch_items=1000
- Server per-item dispatch + reassemble

**D2: POSIX pipelining for Rust + Go**:
- Add `uds-pipeline-client` to Rust and Go bench drivers

**D3: POSIX pipelining + batching** (C, Rust, Go):
- Add `uds-pipeline-batch-client` combining D1 + D2

**D4: Rust Windows bench driver**:
- Create `bench_windows.rs` + CMake target
- Mirror C/Go Windows driver subcmds

**D5: Windows batching + pipelining** (C, Rust, Go):
- Port D1/D2/D3 to Windows bench drivers
- Test on win11

### Phase E: Benchmark Run Scripts + Reports
1. Update `run-posix-bench.sh` for full 162-run matrix
2. Update `run-windows-bench.sh` for full 162-run matrix
3. Update both report generators for new scenarios + rate tiers
4. Run full POSIX matrix, generate `benchmarks-posix.md`
5. ssh win11, run Windows matrix, generate `benchmarks-windows.md`
6. Verify all performance floors
7. Commit, push

### Phase F: Stress Test Parity
1. Port C stress tests to Windows (`test_stress_win.c`)
2. Add Windows stress tests to Rust and Go
3. Add batch stress tests (random 1–1000 under concurrent load)
4. Test on win11
5. Commit, push

### Phase G: Windows SHM Verification
1. ssh win11, run full SHM interop matrix
2. Verify the 4 previously-failing pairs
3. Fix if still broken
4. Commit, push

### Phase H: Final Validation + Multi-Agent Review
1. Run all tests on Linux and Windows
2. Run full benchmark matrices on both platforms
3. Run 4 external reviewers
4. Fix findings, re-review until clean
5. Costa final review

### Decisions
- File splitting: **DEFERRED** to post-integration (Costa decision 2026-03-16)

### Dependencies
- A, B, D, F, G: independent — can run in parallel
- C depends on B (methodology)
- E depends on D (bench drivers must exist)
- H depends on all others

---

## 5. Size Limitations Analysis

Costa asked: "Are there any limitations in packet size, message size, batch size, etc.? There shouldn't be any apart from numeric overflow."

### 5.1 Wire Format Field Types — The Real Constraints

All wire format fields that govern sizes are **u32** (4 bytes unsigned):

| Field | Type | In Header | Max Theoretical | Constrains |
|-------|------|-----------|-----------------|------------|
| `payload_len` | u32 | Outer header | 4,294,967,295 bytes (~4GB) | Max payload per message |
| `item_count` | u32 | Outer header | 4,294,967,295 | Max batch items per message |
| `total_message_len` | u32 | Chunk header | 4,294,967,295 bytes (~4GB) | Max reassembled chunked message |
| `chunk_count` | u32 | Chunk header | 4,294,967,295 | Max chunks per message |
| Batch `offset` | u32 | Batch dir entry | 4,294,967,295 | Max offset within payload |
| Batch `length` | u32 | Batch dir entry | 4,294,967,295 | Max single batch item size |
| SHM `request_capacity` | u32 | SHM region header | 4,294,967,295 | Max SHM request area |
| SHM `response_capacity` | u32 | SHM region header | 4,294,967,295 | Max SHM response area |

**Verdict**: No arbitrary limits. All size fields are u32 — the practical ceiling is ~4GB per message, which is the u32 numeric overflow boundary. This is by design.

### 5.2 Negotiated Limits — Runtime, Not Hardcoded

All directional limits are negotiated per-session during handshake:

| Negotiated Field | Type | Default | Meaning |
|------------------|------|---------|---------|
| `max_request_payload_bytes` | u32 | 1024 | Per-direction payload ceiling |
| `max_response_payload_bytes` | u32 | 1024 | Per-direction payload ceiling |
| `max_request_batch_items` | u32 | 1 | Per-direction batch ceiling |
| `max_response_batch_items` | u32 | 1 | Per-direction batch ceiling |
| `packet_size` | u32 | SO_SNDBUF or 65536 | Triggers chunking, not a message limit |

**Negotiation rule**: `min(client_value, server_value)` per field.

Defaults are conservative (1024 bytes, 1 batch item) but fully configurable up to u32 max by both client and server. This is correct — no artificial caps.

### 5.3 Actual Bugs / Issues Found

These are theoretical u32 overflow issues at ~4GB boundaries. Costa confirmed: "u32 limits are ok. We will never pass that many data in a single message or batch." No action needed.

### 5.4 Hardcoded Limits That Are NOT Size-Related

| Limit | Value | Where | Why |
|-------|-------|-------|-----|
| Inflight IDs (C) | Unbounded | Dynamic `realloc` array | Grows from 16, doubling (FIXED) |
| Inflight IDs (Rust) | Unbounded | `HashSet<u64>` | No limit |
| Inflight IDs (Go) | Unbounded | `map[uint64]struct{}` | No limit |
| `run_dir` buffer | 256 chars | C service structs | Fixed char array |
| `service_name` buffer | 128 chars | C service structs | Fixed char array |
| `sun_path` (UDS) | 108 chars | OS (POSIX) | Kernel AF_UNIX limit |
| `NIPC_NP_MAX_PIPE_NAME` | 256 chars | C Named Pipe | Pipe name buffer |
| `NIPC_WIN_SHM_MAX_NAME` | 256 chars | C Win SHM | Kernel object name |

**Cross-language inconsistency**: ~~C had hard inflight caps (128/64) via fixed arrays.~~ **FIXED**: C inflight tracking now uses dynamically grown arrays (realloc, starting at 16, doubling), matching Rust (HashSet) and Go (map) — no cap.

### 5.5 Summary

**Costa's expectation is correct**: there are no arbitrary size limitations beyond the u32 field types (~4GB ceiling). The protocol is designed with:
- Negotiated limits (configurable up to u32 max)
- Chunking (transparent, handles payloads larger than packet_size)
- No hardcoded payload caps

**Two correctness bugs** exist at the u32 overflow boundary (SHM region layout, send truncation), but they're not practical concerns.

**One cross-language inconsistency**: C inflight caps (128/64) vs Rust/Go unbounded. This matters for pipelining benchmarks.

---

## 6. Summary of What's Missing (Coverage + Benchmarks)

| Area | POSIX | Windows |
|------|-------|---------|
| Coverage measurement | C, Rust, Go (at 90%) | None |
| Coverage target | 90% → 100% | N/A → 100% |
| Ping-pong bench (baseline) | C, Rust, Go — 9 pairs, 3 rates | C, Go — 4 pairs, 3 rates (no Rust, no 1k/s) |
| Ping-pong bench (SHM) | C, Rust, Go — 9 pairs, 3 rates | C, Go — 4 pairs, 3 rates (no Rust, no 1k/s) |
| Ping-pong + batch bench | **Missing entirely** | **Missing entirely** |
| Pipelining bench | **C only** (no Rust, no Go) | **Missing entirely** |
| Pipelining + batch bench | **Missing entirely** | **Missing entirely** |
| Rate tiers | max, 100k, 10k (missing 1k) | max, 100k, 10k (missing 1k) |
| Rust bench driver | ✓ (POSIX only) | **Missing** |
| Stress tests | C, Rust, Go ✓ | **Missing entirely** |
| Rust service unit tests | ✓ (unix) | **Missing** (`#[cfg(all(test, unix))]`) |
