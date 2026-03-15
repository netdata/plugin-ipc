# TODO-hardening: plugin-ipc Pre-Integration Hardening

## Purpose

Close every gap between the current implementation and production
readiness for Netdata integration. This library will be downloaded
1.5M+ times per day and installed on physical servers, VMs, IoT
devices, and exotic environments.

**We ship ONLY when 100% confident in quality, reliability,
performance, and security. Not before.**

## MANDATORY: Read Before Any Work

Read every file in `docs/` in full. Read `TODO-rewrite.md` for the
quality mandate. Then read this file.

## Rules

1. Each phase is completed fully, tested, reviewed, committed.
2. Between phases: multi-agent review (Codex, Qwen, Kimi).
3. If a phase goes wrong: git reset to last checkpoint, retry.
4. No shortcuts, no deferring, no "good enough."
5. Costa reviews between phases.

---

## Phase H1: Multi-Client Multi-Worker Managed Server [DONE]

**The #1 gap. The managed server is single-session, single-worker.**
**STATUS: IMPLEMENTED AND TESTED in C, Rust, and Go.**

### Problem
The current managed server accepts one client, handles it until
disconnect, then accepts the next. The `worker_count` field exists
but is unused. There is no concurrent client handling, no worker
pool, no batch dispatch.

### Requirements (from docs/level2-typed-api.md)
- Multiple concurrent client sessions
- Fixed worker pool (configured at init, no runtime scaling)
- Per-method-type callback dispatch, one item at a time
- Batch messages split across workers, responses reassembled in order
- Per-connection write serialization (one complete response at a time)
- Handler failure on any batch item fails the entire batch

### Implementation plan
1. Redesign the managed server in C:
   - Acceptor thread: accepts clients, creates sessions
   - Per-session reader thread: reads requests from a session
   - Shared work queue: reader threads push work items
   - Worker threads (fixed count): pull from work queue, call handler
   - Per-session writer: workers push responses, writer serializes
   - Shutdown: set flag, join all threads
2. Same design in Rust (using std::thread, crossbeam or mpsc channels)
3. Same design in Go (using goroutines, channels)
4. Tests:
   - 10 concurrent clients, all getting correct responses
   - Batch of 100 items split across 4 workers, responses in order
   - Worker failure on one batch item → entire batch INTERNAL_ERROR
   - Graceful shutdown with in-flight requests
   - Stress: 100 clients, 1000 requests each, verify all responses

### Validation
- All existing L2/L3 tests still pass
- New multi-client/multi-worker tests pass
- Cross-language interop with concurrent clients
- No deadlocks, no data races (TSAN on C, --release with debug on Rust)

### Files affected
- src/libnetdata/netipc/src/service/netipc_service.c
- src/libnetdata/netipc/src/service/netipc_service_win.c
- src/crates/netipc/src/service/cgroups.rs
- src/go/pkg/netipc/service/cgroups/client.go
- src/go/pkg/netipc/service/cgroups/client_windows.go
- Tests in all languages

---

## Phase H2: Coverage Measurement and Gaps [IN PROGRESS]

**We have ~600 test assertions but zero proof of coverage percentage.**
**STATUS: Infrastructure built. Coverage measured. Gap-closing tests written.**

### Coverage Infrastructure (DONE)
- CMakeLists.txt: `NETIPC_COVERAGE` option adds `-fprofile-arcs -ftest-coverage -O0 -g`
- `tests/run-coverage-c.sh`: Builds+runs all C tests, reports per-file gcov coverage
- `tests/run-coverage-rust.sh`: Runs cargo-tarpaulin on library code
- `tests/run-coverage-go.sh`: Runs go test with -coverprofile, reports per-file/function

### Coverage Results

#### C Library Coverage (84.2% total, 1400/1662 lines)
| File | Coverage | Lines |
|------|----------|-------|
| netipc_protocol.c | 98.7% | 394/399 |
| netipc_uds.c | 85.9% | 377/439 |
| netipc_shm.c | 86.9% | 265/305 |
| netipc_service.c | 70.1% | 364/519 |

#### Rust Library Coverage (80.0% total, 1159/1448 lines)
| File | Coverage | Lines |
|------|----------|-------|
| protocol.rs | 90.1% | 353/392 |
| transport/posix.rs | 83.1% | 310/373 |
| transport/shm.rs | 69.0% | 216/313 |
| service/cgroups.rs | 76.9% | 280/364 |

#### Go Library Coverage (80.3% total, 981/1221 stmts)
| File | Coverage | Stmts |
|------|----------|-------|
| protocol/frame.go | 98.0% | 291/297 |
| transport/posix/uds.go | 86.3% | 297/344 |
| transport/posix/shm_linux.go | 70.8% | 226/319 |
| service/cgroups/client.go | 60.2% | 139/231 |
| service/cgroups/cache.go | 93.3% | 28/30 |

### Gap Analysis

**Protocol (all languages ~98%)**: Only 64-bit-unreachable overflow guards uncovered. Effectively complete.

**UDS transport (~85%)**: Uncovered: chunked receive path (multi-packet reassembly), some handshake error paths, socket failure paths. Chunked receive is the biggest gap -- needs a test that sends messages larger than packet_size.

**SHM transport (~70-87%)**: Uncovered: error paths for invalid params, open/truncate/mmap failures, stale detection edge cases. Many are OS-level failure paths hard to trigger in tests.

**Service/client (~60-70%)**: Largest gap across all languages. The SHM transport path through the managed client/server API is completely untested. This is the #1 gap to close.

### Tests Added (Phase H2)
- `test_protocol.c`: +19 tests covering chunk header flags/zero-payload, batch dir encode overflow, batch_item_get edge cases, hello-ack bad flags, cgroups resp item corruption
- `test_shm.c`: +17 tests covering server create validation (null params, bad names, path too long), client attach validation, close edge cases, bad magic/version/truncated files
- `test_uds.c`: +9 tests covering send/receive parameter validation, close listener/session edge cases, path too long

### Remaining Gaps (to close in subsequent iterations)
1. SHM-mode L2 service test (client+server with SHM profile negotiation)
2. Chunked receive path in UDS
3. Server session capacity and growth paths
4. Cache build failure paths (malloc failure injection)

### Implementation plan
1. C: Add CMake option for coverage build (`-fprofile-arcs -ftest-coverage`)
   - Run all C tests with coverage enabled
   - Generate report with `gcov` or `lcov`
   - Target: 100% line, 100% branch on library source files
   - Identify uncovered lines/branches and write targeted tests
2. Rust: Add `cargo-tarpaulin` or `cargo-llvm-cov`
   - Run `cargo tarpaulin` with all tests
   - Target: 100% line coverage on src/
   - Write tests for any uncovered paths
3. Go: Add `-coverprofile` to go test
   - Run `go test -coverprofile=coverage.out ./...`
   - Target: 100% line coverage on pkg/netipc/
   - Write tests for any uncovered paths
4. Add coverage CI targets to CMakeLists.txt
5. Document the coverage methodology

### Validation
- Coverage reports generated for all 3 languages
- All library source files at 100% line coverage
- Branch coverage documented (may be <100% for defensive unreachable paths)

---

## Phase H3: Sanitizer Validation [DONE]

**STATUS: All sanitizers pass clean. Bugs found and fixed.**

### Scripts created
- `tests/run-sanitizer-asan.sh`: ASAN + UBSAN on all 6 C tests
- `tests/run-sanitizer-tsan.sh`: TSAN on 5 multi-threaded C tests
- `tests/run-valgrind.sh`: Valgrind memcheck on all 6 C tests
- `tests/run-go-race.sh`: Go race detector on all 3 Go packages

### Findings and Fixes

**1. Memory leak in `nipc_server_destroy` (ASAN)**
- `session_handler_thread` removed itself from the server's session array
  via `server_remove_session()`, then nobody freed the `sctx` pointer.
- Fix: removed `server_remove_session()` call from session handler thread;
  the reap loop and destroy path now handle join+free for all sessions.
- File: `src/libnetdata/netipc/src/service/netipc_service.c`

**2. Data races on `server->running` and `sctx->active` flags (TSAN)**
- `nipc_server_stop()` wrote `running=false` while `nipc_server_run()`
  read it concurrently.  `volatile bool` is not recognized by TSAN.
- Fix: replaced all cross-thread `running`/`active` accesses with
  `__atomic_load_n`/`__atomic_store_n` builtins (RELAXED/RELEASE/ACQUIRE).
- Files: `netipc_service.c`, `netipc_service.h`

**3. Data races on test helper `ready`/`done`/`connected` flags (TSAN)**
- All test files used `volatile int` for cross-thread signaling, which
  TSAN does not recognize as synchronized.
- Fix: converted all accesses to `__atomic` builtins in all 5 test files.
- Files: `test_multi_server.c`, `test_service.c`, `test_cache.c`,
  `test_uds.c`, `test_shm.c`

**4. Uninitialized trailing padding in batch builder (Valgrind)**
- `nipc_batch_builder_finish()` returned aligned length but did not zero
  the trailing alignment bytes, causing Valgrind to flag `sendmsg`.
- Fix: added `memset` for trailing padding in `nipc_batch_builder_finish()`.
- File: `src/libnetdata/netipc/src/protocol/netipc_protocol.c`

**5. Rust unsafe code (documented, cross-validated)**
- Rust `unsafe` is limited to: mmap/munmap, futex syscall, pointer
  arithmetic on mmap'd regions, file operations (open/close/ftruncate/
  unlink), and atomic operations on shared memory.
- Miri cannot run tests with syscalls, so direct validation is not possible.
- The same SHM memory layout is validated by C ASAN, providing
  cross-language validation of the shared memory protocol.

### Final Results
- **ASAN (C)**: 6/6 tests pass, zero findings
- **TSAN (C)**: 5/5 tests pass, zero data races
- **Valgrind (C)**: 6/6 tests pass, zero errors, zero leaks
- **Go race detector**: 3/3 packages pass, zero data races
- **Rust**: `cargo test` passes (no unsafe-specific sanitizer available)

---

## Phase H4: Large-Scale and Stress Testing [DONE]

**STATUS: All stress tests pass in C, Go, and Rust.**

### Tests Implemented

#### C (`tests/fixtures/c/test_stress.c`) — 8 tests, 32 assertions
1. **1000-item snapshot**: build, encode, send via UDS, receive, decode, verify ALL 1000 items (0.5ms)
2. **5000-item snapshot**: same via chunked UDS (packet_size=65536), all 5000 verified (1.9ms)
3. **10 concurrent L3 cache clients x 100 refreshes**: 1000/1000, zero errors, no cross-talk
4. **50 concurrent L3 cache clients x 10 refreshes**: 500/500, zero errors
5. **1000 rapid connect/disconnect cycles**: all succeeded (53ms), server still healthy, 7 fds open
6. **60-second long-running stability**: 5 clients, 281K refreshes, 0 errors, 52 kB VmRSS growth
7. **1000 SHM create/destroy cycles**: 0 leaked files, 0 leaked mmap regions
8. **Mixed transport (2 SHM + 1 UDS)**: all 3 clients get correct responses concurrently

#### Go (`src/go/.../stress_test.go`) — 7 tests
1. **TestStress1000Items**: 1000 items, all verified (660µs)
2. **TestStress5000Items**: 5000 items, spot-checked (3.4ms)
3. **TestStress50Clients**: 50 concurrent x 10 req = 500/500, 0 failures
4. **TestStressConcurrentCacheClients**: 10 cache clients x 100 = 1000/1000
5. **TestStressRapidConnectDisconnect**: 1000 cycles, all succeeded (50ms)
6. **TestStressLongRunning60s**: 5 clients, 60s, 0 errors
7. **TestStressMixedTransport**: 2 SHM + 1 UDS clients, all correct

#### Rust (`src/crates/netipc/src/service/cgroups.rs`) — 7 tests
1. **test_stress_1000_items**: 1000 items, all verified
2. **test_stress_5000_items**: 5000 items, spot-checked
3. **test_stress_concurrent_clients**: 50 concurrent x 10 req
4. **test_stress_rapid_connect_disconnect**: 1000 cycles
5. **test_stress_cache_concurrent**: 10 cache clients x 100 req
6. **test_stress_long_running**: 5 clients, 60s continuous

### Key Finding: SEQPACKET packet_size
- SEQPACKET sockets cannot send messages larger than SO_SNDBUF (default 212992)
- For payloads > ~200KB, `packet_size` must be set to 65536 to force chunking
- This affects the 5000-item snapshot test (~440KB payload)
- Fixed by explicitly setting `packet_size = 65536` in both server and client configs
- The library's chunking mechanism works correctly when packet_size is appropriate

### Validation
- All scale tests pass without errors in all 3 languages
- Memory stable over 60s run (52 kB growth)
- No resource leaks (fds, SHM files, mmap regions)
- CTest: test_stress (C, 180s timeout), test_stress_go (240s), test_stress_rust (240s)

---

## Phase H5: Pipelining Performance and Correctness [DONE]

**STATUS: All pipelining correctness tests pass. Benchmark shows 3.7x throughput improvement at depth=32.**

### Changes Made

**1. Increased C MAX_INFLIGHT from 64 to 128**
- File: `src/libnetdata/netipc/include/netipc/netipc_uds.h`
- The fixed-size in-flight message_id array limited pipeline depth to 64
- Rust (HashSet) and Go (map) had no limit; now all languages support 128

**2. Expanded pipelining correctness tests (all 3 languages)**

C (`tests/fixtures/c/test_uds.c`) — 4 new tests:
- `test_pipeline_10`: 10 pipelined requests, all verified by message_id + payload
- `test_pipeline_100`: 100 pipelined requests (stress), all verified
- `test_pipeline_mixed_sizes`: mixed sizes (8, 256, 1024 bytes), all verified
- `test_pipeline_chunked`: 5 chunked messages (200-800 bytes, packet_size=128), all verified

Rust (`src/crates/netipc/src/transport/posix.rs`) — 4 new tests:
- `test_pipeline_10`, `test_pipeline_100`, `test_pipeline_mixed_sizes`, `test_pipeline_chunked_multi`

Go (`src/go/pkg/netipc/transport/posix/uds_test.go`) — 4 new tests:
- `TestPipeline10`, `TestPipeline100`, `TestPipelineMixedSizes`, `TestPipelineChunked`

**3. Cross-language pipelining (interop binaries + test script)**
- Added `pipeline-server` and `pipeline-client` subcommands to all 3 interop binaries
- C: `tests/fixtures/c/interop_uds.c`
- Rust: `tests/fixtures/rust/src/bin/interop_uds.rs`
- Go: `tests/fixtures/go/cmd/interop_uds/main.go`
- Updated `tests/test_uds_interop.sh`: 9 pipeline tests (3 same-language + 6 cross-language, 20 requests each)
- All 18 interop tests pass (9 original + 9 pipeline)

**4. Pipeline benchmark driver**
- Added `uds-pipeline-client` subcommand to `bench/drivers/c/bench_posix.c`
- Sends `depth` requests, reads `depth` responses, repeats for duration
- Measures throughput, p50/p95/p99 latency per batch round-trip
- Uses the existing ping-pong server (it already echoes)
- Added pipeline benchmark section to `tests/run-posix-bench.sh` (depths 1,4,8,16,32)

### Benchmark Results (C→C, 5s duration, max rate)

| Depth | Throughput (req/s) | p50 (µs) | p95 (µs) | p99 (µs) |
|-------|-------------------|----------|----------|----------|
| 1     | 175,585           | 4        | 8        | 12       |
| 4     | 395,562           | 8        | 14       | 22       |
| 8     | 533,681           | 13       | 22       | 26       |
| 16    | 629,206           | 22       | 41       | 44       |
| 32    | 656,392           | 42       | 76       | 85       |

**Key finding**: Pipelining at depth 32 achieves 3.7x the throughput of ping-pong (depth 1). Throughput scales sublinearly — diminishing returns above depth 16, as the transport becomes saturated.

### Validation
- [x] Pipelining throughput scales with depth (3.7x at depth 32)
- [x] No corruption at any pipeline depth (message_id + payload verified)
- [x] Cross-language pipelining verified (all 9 directed pairs)
- [x] All existing tests pass (zero regressions)

---

## Phase H6: SHM Benchmark Correctness [DONE]

**STATUS: Root cause found and fixed in the library. Zero errors at
default spin count. No workarounds.**

### Root Cause

Bug in `nipc_shm_receive()` futex wait path
(`src/libnetdata/netipc/src/transport/posix/netipc_shm.c`).

When the spin loop (128 iterations) failed to observe the sequence
advance, the function fell into the futex path.  The futex path had
a single-attempt design:

1. Read signal word
2. Check sequence — if still behind, call `futex_wait(signal, val, timeout)`
3. If `futex_wait` returned `EAGAIN` (signal word changed between
   the read and the syscall) or `EINTR` (signal delivery), do ONE
   re-check of the sequence number
4. If the sequence still hadn't advanced, return `NIPC_SHM_ERR_TIMEOUT`

Step 4 was incorrect: the caller specified a 30-second timeout, but
the function gave up after a single spurious wakeup — effectively
nanoseconds of actual waiting.  This caused false timeouts, which
desynchronized the client's local sequence tracking from the shared
region (the send already incremented `local_req_seq`, but the failed
receive did not advance `local_resp_seq`).  All subsequent iterations
then read stale response data, producing the "counter chain broken"
off-by-one errors.

### Fix

Replaced the single-attempt futex path with a deadline-based retry
loop.  The loop:
- Computes a wall-clock deadline from the caller's `timeout_ms`
- Re-reads the signal word and sequence on each iteration
- Recomputes the remaining timeout for each `futex_wait` call
- Only returns `NIPC_SHM_ERR_TIMEOUT` on actual `ETIMEDOUT` or
  when the deadline is exceeded

This ensures the full timeout is honored regardless of spurious
`EAGAIN` / `EINTR` returns.

### Files Changed
- `src/libnetdata/netipc/src/transport/posix/netipc_shm.c` — fixed
  `nipc_shm_receive()` futex wait path
- `bench/drivers/c/bench_posix.c` — removed `spin_tries = 4096`
  workaround

### Benchmark removed workaround
The benchmark client no longer overrides `spin_tries`.  It uses the
library default (`NIPC_SHM_DEFAULT_SPIN = 128`).

### Validation
- 5 consecutive runs of `shm-ping-pong-client` (5s each, max rate):
  ZERO counter chain errors, ~2.9–3.0M req/s
- All 9 C SHM/service/stress tests pass (100%)
- All Rust and Go SHM tests pass
- All SHM interop tests pass

---

## Phase H7: Complete Benchmark Suite

**benchmarks-posix.md is stale. benchmarks-windows.md doesn't exist.**

### Implementation plan
1. POSIX benchmark suite:
   - UDS ping-pong: full 9-pair matrix at max, 100k/s, 10k/s
   - SHM ping-pong: full 9-pair matrix at max, 100k/s, 10k/s
   - UDS pipelining: depth 1/4/8/16/32 (C/Rust/Go)
   - Negotiated profile: UDS vs SHM comparison
   - Snapshot baseline refresh: full 9-pair matrix
   - Snapshot SHM refresh: full 9-pair matrix
   - Local cache lookup: C, Rust, Go
   - Multi-client throughput: 1, 5, 10 clients
   - All benchmarks verify correctness (counter chain or item verification)
2. Windows benchmark suite:
   - Named Pipe ping-pong: full matrix (c-native, c-msys, rust, go)
   - Windows SHM ping-pong: full matrix
   - Snapshot Named Pipe refresh: full matrix
   - Snapshot SHM refresh: full matrix
   - Local cache lookup
3. Benchmark document generation:
   - `tests/generate-benchmarks-posix.sh` regenerated
   - `tests/generate-benchmarks-windows.sh` created and run
   - Atomic write (temp file → rename)
4. Performance floors verified:
   - SHM max: >= 1M req/s for all pairs (POSIX and Windows)
   - UDS max: >= 150k req/s for all pairs
   - Named Pipe max: documented baseline
   - Local cache lookup: >= 10M lookups/s

### Validation
- Both benchmark documents generated from complete runs
- All performance floors met
- Results committed to repo

---

## Phase H8: Code Organization and Quality

**Large files, no developer docs, linear-scan cache lookup.**

### Implementation plan
1. Split large transport files:
   - C: split netipc_uds.c into connection.c, handshake.c, send_recv.c
   - Rust: split posix.rs into uds.rs, handshake.rs, chunking.rs
   - Go: split uds.go similarly
   - Target: no file exceeds 500 lines
2. C L3 cache: replace linear scan with hash table lookup
   - Use simple open-addressing hash table keyed by hash+name
   - Benchmark improvement for 1000+ item datasets
3. Add developer documentation:
   - `docs/getting-started.md`: how to use the library from C/Rust/Go
   - Examples for: connect, call, batch, cache, managed server
4. Graceful server drain:
   - Server stop waits for in-flight requests to complete (with timeout)
   - Then closes sessions and exits

### Validation
- No source file exceeds 500 lines
- Hash table lookup benchmarks show improvement
- Developer docs are usable

---

## Phase H9: Extended Fuzz Testing

**Current fuzz runs are 30 seconds. Need longer runs and transport-
level fuzzing.**

### Implementation plan
1. Extended codec fuzz (10 minutes per target):
   - Go: all 8 fuzz targets
   - Rust: all proptest targets with 100000 iterations
   - C: libfuzzer with corpus for 10 minutes
2. Transport-level fuzz:
   - Create a "chaos client" that sends random bytes after handshake
   - Verify the server doesn't crash, leak, or hang
   - Test with malformed headers, truncated messages, wrong message_ids
   - Test mid-chunking disconnect
3. SHM fuzz:
   - Write random data into SHM request area
   - Verify the server rejects it cleanly
4. Save interesting corpus entries for regression

### Validation
- No crashes or panics in extended runs
- Chaos client tests pass
- Corpus saved in tests/corpus/

---

## Phase H10: Final Multi-Agent Review

**The last gate before declaring production-ready.**

### Implementation plan
1. Run all 4 external reviewers (Codex, GLM-5, Kimi, Qwen) on the
   full implementation
2. Each reviewer checks:
   - Spec compliance (every docs/*.md requirement)
   - Wire compatibility (identical bytes across languages)
   - Test coverage (100% proven)
   - Security (every input validated, no overflows, no panics)
   - Concurrency (multi-client, multi-worker correct)
   - Performance (meets all floors)
   - Code quality (clean, maintainable, fits Netdata patterns)
3. Fix every finding
4. Re-review until clean
5. Costa final review

### Validation
- All reviewers agree: production-ready
- Costa approves

---

## Completion Criteria

The library is production-ready when ALL of the following are true:

- [ ] Multi-client multi-worker managed server implemented and tested
- [ ] 100% line coverage proven in all 3 languages
- [ ] Zero ASAN/TSAN/valgrind findings
- [ ] Zero Go race detector findings
- [ ] 1000+ item snapshot tests pass in all languages
- [ ] 100 concurrent client stress test passes
- [ ] 10-minute long-running stability test passes
- [ ] Pipelining correctness and performance documented
- [x] SHM benchmark root cause resolved (no workarounds)
- [ ] benchmarks-posix.md generated from current code
- [ ] benchmarks-windows.md generated from current code
- [ ] All performance floors met
- [ ] No source file exceeds 500 lines
- [ ] Hash table cache lookup implemented
- [ ] Developer documentation written
- [ ] Extended fuzz testing (10+ minutes per target) clean
- [ ] Transport-level chaos testing clean
- [ ] All 4 external reviewers agree: production-ready
- [ ] Costa approves
