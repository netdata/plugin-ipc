# SOW-0016 - Bounded Synchronous Client Calls And Client-Side Abort

## Status

Status: completed

Sub-state: regression repair completed on 2026-06-11 after CI failures on commit `aaec3d5`; `SOW-0015` remains paused and can be resumed after this repair is committed.

## Requirements

### Purpose

Make every synchronous NetIPC client call bounded on every transport, and make a blocked client call abortable from another thread, so consumers can guarantee collection liveness and prompt shutdown even when the peer process wedges without closing the connection.

### User Request

The user decided to fix this in plugin-ipc upstream, independently of the consuming netdata PR. Source: the topology-containers defense review in netdata/netdata PR #22601 verified that a wedged peer can hang a NetIPC client forever on the UDS transport, that the SHM transport is bounded only by a hardcoded constant buried in the call layer, and that no client-side abort exists, which can hang consumer cleanup paths that join a worker blocked in a call.

### Assistant Understanding

Facts:

- The POSIX client call layer bounds SHM receives with a hardcoded 30000 ms timeout but calls the UDS receive path with no timeout at all (`src/libnetdata/netipc/src/service/netipc_service_posix_client_call.c:74-90`).
- The UDS transport has no timeout plumbing anywhere; the bottom of the receive path is a plain blocking `recv()` (`src/libnetdata/netipc/src/transport/posix/netipc_uds.c:74-76`, `nipc_uds_raw_recv()`); `grep timeout` over `netipc_uds.c` and `netipc_uds_receive.c` returns zero matches.
- The Windows client call layer passes the same hardcoded 30000 ms to the Windows SHM transport (`src/libnetdata/netipc/src/service/netipc_service_win_client_call.c:69`), which does enforce timeouts (`src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:744,808`). The named-pipe receive path needs the same audit as UDS.
- The public API has no client-side abort or cancel; only the service acceptor has a shutdown signal (`src/libnetdata/netipc/include/netipc/netipc_service.h`, acceptor shutdown comment).
- Consumer evidence (netdata/netdata, PR #22601 branch): `src/collectors/apps.plugin/apps-cgroups-lookup-client.c:469` makes the synchronous CGROUPS_LOOKUP call from a dedicated worker; cleanup signals a stop flag and joins that worker (`src/collectors/apps.plugin/apps-cgroups-lookup-client.c:596-604`); a worker blocked inside `recv()` never observes the flag, so plugin shutdown hangs indefinitely on UDS and waits up to 30 s on SHM.
- Go transport implementations and interop fixtures exist in this repository (`tests/fixtures/go/cmd/interop_uds`, `tests/fixtures/go/cmd/interop_named_pipe`, `tests/fixtures/go/cmd/interop_cache_win`, `bench/drivers/go`); API changes must keep C and Go behavior symmetric.

Inferences:

- A synchronous call API that is bounded on one transport and unbounded on another is a library defect, not a consumer misuse: consumers cannot make a UDS call safe from outside the library.
- The hardcoded 30000 in two call-layer files is itself a maintainability smell; bounding UDS should not add a third copy.

Unknowns:

- Whether the Windows named-pipe client receive is bounded today (audit item; expected unbounded like UDS).
- Whether any existing consumer depends on the unbounded UDS behavior (expected none; a timeout surfaces as a failed call, which consumers must already handle for disconnects).

### Acceptance Criteria

- Every synchronous client call accepts or inherits an explicit timeout and returns within it on every transport (UDS, POSIX SHM, Windows SHM, named pipe), verified by wedged-peer tests (server accepts, never responds).
- The hardcoded 30000 ms constants in the client call layers are replaced by one defined default in one place, used by all transports.
- A client-side abort API unblocks a call blocked in receive promptly (target: under one second), is safe to invoke from a different thread than the caller, and is covered by an abort-during-call test on each transport.
- Timeout and abort produce distinguishable error results so consumers can choose retry versus shutdown handling.
- C and Go transports behave symmetrically for timeout and abort, verified through the existing interop fixtures extended with the new cases.
- Existing benchmarks show no regression on the happy path (`bench/`, generate-benchmarks scripts).

## Analysis

Sources checked:

- `src/libnetdata/netipc/src/service/netipc_service_posix_client_call.c`
- `src/libnetdata/netipc/src/service/netipc_service_win_client_call.c`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c`
- `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c`
- `src/libnetdata/netipc/include/netipc/netipc_service.h`
- Consumer review evidence from netdata/netdata PR #22601 (defense-review finding on apps.plugin CGROUPS_LOOKUP liveness/cleanup).

Current state:

- SHM (POSIX and Windows) receives are bounded at a hardcoded 30000 ms inside the call layer; UDS receives are unbounded; named-pipe receive bound is unverified; no client abort exists.

Risks:

- A wedged peer (stopped process with an open socket, stuck event loop) permanently hangs the consumer worker on UDS, freezing dependent features and hanging consumer shutdown joins.
- Even bounded SHM forces consumer shutdown to wait up to 30 s, which exceeds typical plugin shutdown watchdog budgets.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The call layer promises synchronous request/response semantics but delegates receive bounding to transports inconsistently: SHM transports accept a timeout parameter, the UDS path does not even have one. The missing piece is timeout plumbing through the UDS (and possibly named-pipe) receive path, plus an abort primitive that no transport currently offers.

Evidence reviewed:

- File:line evidence in Assistant Understanding, all verified at netdata/plugin-ipc @ 0d0bdce.
- Consumer evidence in netdata/netdata PR #22601 as cited above.

Affected contracts and surfaces:

- Public client call API (timeout parameter or context default; new abort entry point; new error codes).
- All four transports' receive paths.
- Go transport parity and interop fixtures.
- Vendored copy in netdata (refreshed by the existing vendor scripts; consumer adoption is out of scope here and tracked by the consumer).

Existing patterns to reuse:

- SHM timeout enforcement (`netipc_win_shm.c:744,808` and the POSIX SHM equivalent) as the semantic model for timeout behavior and error reporting.
- Existing interop fixture pattern under `tests/fixtures/go/cmd/` for cross-language behavior tests.
- ASAN/TSAN/valgrind build harnesses already present (`build-asan`, `build-tsan`, `build-valgrind`) for race/abort validation.

Risk and blast radius:

- Every NetIPC consumer's call path changes behavior on peer wedge (hang becomes timeout error). Consumers already handle call failures for disconnects, so this is expected to be strictly safer, but each consumer's retry path should be sanity-checked after adoption.
- Abort introduces cross-thread interaction with the session; TSAN coverage required.
- Windows named-pipe abort likely needs `CancelIoEx` or an event-based wait; isolate platform specifics in the transport layer.

Sensitive data handling plan:

- Pure library code work; no secrets, tokens, customer data, or private endpoints are involved. SOW, code comments, and tests must use synthetic service names and paths only.

Implementation plan:

1. Decide API shape (open decisions below); add timeout plumbing to UDS receive (poll/deadline around `nipc_uds_raw_recv()`), honoring the same semantics as SHM.
2. Audit and, if unbounded, bound the Windows named-pipe client receive the same way.
3. Replace the two hardcoded 30000 constants with one default-timeout definition used by all call layers.
4. Add the client abort primitive (self-pipe/eventfd in the poll set on POSIX; `CancelIoEx` or event handle on Windows); define thread-safety contract.
5. Mirror timeout/abort semantics in the Go transport; extend interop fixtures with wedged-peer and abort-during-call cases for each transport.
6. Documentation and spec updates for call semantics and error codes.

Validation plan:

- New wedged-peer unit/interop tests per transport (server accepts, never responds): call returns timeout error within deadline.
- Abort-during-call test per transport: abort from another thread unblocks within the target bound.
- TSAN build for the abort path; ASAN for receive-path changes; existing fuzz target (`tests/fuzz_protocol.c`) unaffected.
- Benchmarks before/after on the happy path.

Artifact impact plan:

- AGENTS.md: likely unaffected (no workflow change).
- Runtime project skills: likely unaffected.
- Specs: update the service/call-semantics spec for timeout defaults, abort semantics, and error codes.
- End-user/operator docs: README/API docs for the new parameters and error codes.
- End-user/operator skills: none exist for this library; likely unaffected.
- SOW lifecycle: this SOW covers the library change only; consumer adoption (netdata apps.plugin and others) is tracked by the consumer (netdata/netdata PR #22601 follow-up), and the vendor refresh into netdata is the consumer-side step.

Open-source reference evidence:

- netdata/plugin-ipc @ 0d0bdce (this repository; all file:line citations above).
- netdata/netdata PR #22601 branch for consumer evidence (`src/collectors/apps.plugin/apps-cgroups-lookup-client.c:469,596-604`).

Open decisions:

1. Timeout API shape:
   - A. Per-call timeout parameter, with a context-level default used when the parameter is zero (recommended: callers that need deadline semantics get them; existing call sites keep working with the default).
   - B. Context-level default only (smallest API change; no per-call control).
   - C. Per-call parameter only (forces every call site to choose; most churn).
2. Default timeout value:
   - A. Keep 30000 ms as the single named default (recommended: no behavior change for SHM users).
   - B. Lower the default (faster failure, more behavior change risk).
3. Timeout error reporting:
   - A. New explicit timeout error code distinct from disconnect/not-ready (recommended: consumers can distinguish retry-worthy timeout from protocol failure).
   - B. Reuse an existing error code (no API growth; consumers cannot distinguish).
4. Abort mechanism:
   - A. POSIX: poll on socket fd plus an abort eventfd/self-pipe; Windows: event-based wait or `CancelIoEx` (recommended: portable, no signal games).
   - B. `shutdown()` on the socket from the aborting thread (simplest; conflates abort with disconnect and complicates reconnect semantics).

Routing decision:

- Decision: A. Pause `SOW-0015` and move this SOW to `current/`.
- Benefit: fixes the correctness/liveness issue before continuing maintainability cleanup.
- Implication: Codacy maintainability work remains paused and must be resumed through `SOW-0015`.
- Risk: Codacy complexity/duplication cleanup pauses temporarily, but no runtime behavior depends on that cleanup.

## Implications And Decisions

1. Timeout API shape

- Decision: A. Add a per-call timeout parameter, with a context-level default used when the parameter is zero.
- Benefit: callers that need explicit deadline behavior can request it per call, while existing call sites can keep using the default.
- Implication: public C, Go, and Rust service APIs need additive timeout-capable call forms or equivalent options without breaking existing no-timeout-argument wrappers.
- Risk: API surface grows; tests must prove old call paths and new timeout-specific call paths stay behaviorally consistent.

2. Default timeout value

- Decision: A. Keep `30000 ms` as the single named default.
- Benefit: preserves existing SHM behavior while fixing unbounded stream-transport behavior.
- Implication: UDS and named-pipe calls that previously hung forever will now fail after the default timeout.
- Risk: a legitimately slow server response beyond 30 seconds becomes a timeout failure; consumers must treat this as a failed call and use the normal reconnect/retry logic.

3. Timeout error reporting

- Decision: A. Add an explicit timeout error code distinct from disconnect/not-ready/protocol errors.
- Benefit: consumers can distinguish a wedged peer from malformed data, disconnect, or service unavailability.
- Implication: C, Go, and Rust error definitions and transport-to-service error mapping must grow in sync.
- Risk: every error display/stringification and tests that enumerate known errors must be updated.

4. Abort mechanism

- Decision: A. Use an explicit abortable wait primitive per platform.
- POSIX: poll the socket fd plus an abort eventfd/self-pipe.
- Windows: use the appropriate event-based wait or synchronous I/O cancellation path for the current named-pipe implementation.
- Benefit: abort is separate from peer disconnect, and a blocked call can be released by another thread during shutdown.
- Implication: client context state needs a thread-safe abort signal/handle and documented lifecycle rules.
- Risk: cross-thread cancellation must be validated with race-focused tests; Windows implementation must match the actual synchronous/overlapped I/O mode used by the transport.

Routing was resolved: `SOW-0015` was paused while this SOW was active.

5. Validation blocker handling

- Decision: A. Keep this SOW open and fix the validation blockers before closure.
- Benefit: the timeout/abort change is not merged with known incomplete sanitizer or benchmark validation.
- Implication: this SOW also covers narrow harness hardening where existing tests can hang instead of reporting pass/fail.
- Risk: scope expands slightly beyond the API change, but only to unblock required validation of the same changed call paths.

## Plan

1. Implement timeout plumbing (UDS, then named-pipe audit/fix); single default constant.
2. Implement abort primitive per decision 4; thread-safety contract documented.
3. Go parity plus interop fixtures for timeout/abort.
4. Tests (wedged-peer, abort-during-call), TSAN/ASAN runs, benchmarks.
5. Specs/docs updates; SOW validation and completion.

## Execution Log

### 2026-06-10

- SOW created in `pending/` from the netdata/netdata PR #22601 defense review (apps.plugin CGROUPS_LOOKUP liveness finding).
- All library evidence verified against this repository at commit 0d0bdce before filing.

### 2026-06-11

- User selected routing option A: pause `SOW-0015` and make this timeout/abort SOW current.
- User selected implementation decisions 1A, 2A, 3A, and 4A:
  per-call timeout with context default, keep `30000 ms` as the named default, add explicit timeout error reporting, and use explicit abortable waits per platform.
- Implemented C timeout and abort contracts:
  - added `NIPC_ERR_TIMEOUT` and `NIPC_ERR_ABORTED`;
  - added `NIPC_CLIENT_CALL_TIMEOUT_DEFAULT_MS` as the single `30000 ms` default;
  - added per-call timeout entry points for cgroups snapshot, cgroups lookup, and apps lookup;
  - added client context timeout, abort, and clear-abort APIs;
  - added POSIX UDS timeout/abort receive using `poll()` on the socket plus client self-pipe;
  - added Windows Named Pipe timeout/abort receive for typed calls while preserving the raw blocking `nipc_np_receive()` behavior for direct L1 callers;
  - made timeout and abort terminal for the current call, with no reconnect/retry of the same request.
- Implemented Go parity:
  - added timeout/abort protocol errors;
  - added raw client default timeout, per-call timeout variants, abort and clear-abort;
  - added POSIX UDS and Windows Named Pipe timeout-aware receives;
  - added public typed client/cache wrappers for timeout and abort controls.
- Implemented Rust parity:
  - added `NipcError::Timeout` and `NipcError::Aborted`;
  - added raw client timeout, abort handle, abort, clear-abort, and timeout call variants;
  - added POSIX UDS and Windows Named Pipe timeout-aware receives;
  - added public typed service/cache wrappers without changing existing Rust config struct literals.
- Added regression coverage:
  - POSIX C `test_service`: wedged-peer timeout and abort-unblocks-call;
  - Windows C `test_win_service`: wedged-peer timeout and abort-unblocks-call;
  - Go raw service tests for POSIX and Windows;
  - Rust raw service tests for POSIX and Windows.
- Updated public specs/docs:
  - `docs/level1-posix-uds.md`;
  - `docs/level1-windows-np.md`;
  - `docs/level2-typed-api.md`;
  - `docs/level3-snapshot-api.md`;
  - `docs/getting-started.md`;
  - `docs/netipc-integrator-skill.md`.

## Validation

Acceptance criteria evidence:

- Bounded synchronous calls:
  - POSIX C `test_service` includes `Client call timeout on wedged peer`; the call returns `NIPC_ERR_TIMEOUT` promptly.
  - Windows C `test_win_service` includes `Client call timeout on wedged peer`; the call returns `NIPC_ERR_TIMEOUT` promptly.
  - Go raw service tests include `TestUnixClientCallTimeoutOnWedgedPeer` and `TestWinClientCallTimeoutOnWedgedPeer`.
  - Rust raw service tests include `test_client_call_timeout_on_wedged_peer` on POSIX and Windows.
- Single default timeout:
  - C default is `NIPC_CLIENT_CALL_TIMEOUT_DEFAULT_MS` in `netipc_service.h`, used by common client timeout resolution.
  - Go default is `ClientCallTimeoutDefaultMs`.
  - Rust default is `CLIENT_CALL_TIMEOUT_DEFAULT_MS`.
- Abort from another thread:
  - POSIX C and Windows C service tests include `Client abort unblocks in-flight call`.
  - Go raw service tests include `TestUnixClientAbortUnblocksCall` and `TestWinClientAbortUnblocksCall`.
  - Rust raw service tests include `test_client_abort_unblocks_call` on POSIX and Windows.
- Distinguishable errors:
  - C exposes `NIPC_ERR_TIMEOUT` and `NIPC_ERR_ABORTED`.
  - Go exposes `protocol.ErrTimeout` and `protocol.ErrAborted`.
  - Rust exposes `NipcError::Timeout` and `NipcError::Aborted`.
- Docs/specs:
  - Level 1 UDS and Windows Named Pipe specs document timeout-aware receive forms.
  - Level 2 typed API spec documents default timeout, per-call zero-as-default, terminal timeout/abort behavior, and sticky abort lifecycle.
  - Level 3 snapshot spec documents cache preservation on timeout/abort.
  - Integrator skill documents bounded calls, abort lifecycle, and synchronization implications.

Tests or equivalent validation:

- Local POSIX/Linux:
  - `gofmt` on changed Go files: passed.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml`: passed.
  - `cmake --build build`: passed.
  - `cd src/go && go test ./pkg/netipc/...`: passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`: 338 tests passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed in 449.05 seconds.
  - `GOOS=windows GOARCH=amd64 go test ./pkg/netipc/... -run TestDoesNotExist`: passed.
  - `cargo check --manifest-path src/crates/netipc/Cargo.toml --target x86_64-pc-windows-gnu`: passed.
- Windows on `win11` with `MSYSTEM=MSYS`:
  - First remote validation exposed a real compile error in `netipc_named_pipe.c`: `inflight_fail_all` was used before declaration. Fixed with a forward declaration before the new receive helpers.
  - Second remote validation exposed a real C Named Pipe regression: the new timeout-aware receive path was accidentally used by raw blocking `nipc_np_receive()`, causing `test_named_pipe` to hang at `Receive after zero-byte message`.
  - Fixed by preserving raw blocking `ReadFile()` behavior for `nipc_np_receive()` and using the timeout-aware readiness loop only when a timeout or abort event is requested.
  - Focused `test_named_pipe.exe`: 268 passed, 0 failed.
  - Focused CTest on `test_named_pipe|test_win_service`: 2/2 passed.
  - `tests/run-windows-msys-validation.sh /tmp/netipc-msys-validation-sow0016 1` with `NIPC_BENCH_COMPARE_REPETITIONS=3`:
    - functional tests all passed through `test_win_stress`;
    - benchmark comparison failed on `shm-ping-pong rust->c @ max` under the MSYS C toolchain after three short-window targeted attempts.
  - Focused longer benchmark for that failed row passed:
    - command shape: `NIPC_WINDOWS_TOOLCHAIN=msys`, `NIPC_BENCH_REPETITIONS=5`, `NIPC_BENCH_MAX_DURATION=5`, `NIPC_BENCH_DIAGNOSE_FAILURES=1`, scenario `shm-ping-pong`, client `rust`, server `c`, target `0`;
    - result CSV: `/tmp/netipc-msys-validation-sow0016/focused-shm-rust-c-msys.csv`;
    - median throughput `2216685/s`, p50 `4.100us`, p95 `15.400us`, p99 `22.400us`;
    - raw outlier warning kept by policy because stable core ratio was `1.062027`.
  - Fixed the comparison wrapper so it rejects too-few samples and defaults max-tier comparison rows to at least 5 second samples.
  - Final `win11` validation command:
    `MSYSTEM=MSYS NIPC_MSYS_VALIDATION_WIN_SHM_REPEATS=1 timeout 3600 bash tests/run-windows-msys-validation.sh /tmp/netipc-msys-validation-sow0016-final 1`.
  - Final `win11` result: functional Windows tests passed, benchmark comparison policy passed, and summary was written to `/tmp/netipc-msys-validation-sow0016-final/summary.txt`.
  - The final comparison retried one noisy policy row (`shm-max-rust-client-vs-c-server`) and passed on attempt 2 with MSYS at `94.2%` of mingw64 for that row.
- Sanitizers:
  - The older `Client-side SHM attach failure falls back to baseline` test used blocking accepts and could hang sanitizer runs instead of reporting pass/fail. Fixed by adding a bounded accept helper in the fake server.
  - `bash tests/run-sanitizer-asan.sh`: 7/7 passed; zero ASAN/UBSAN findings.
  - `bash tests/run-sanitizer-tsan.sh`: 6/6 passed; zero ThreadSanitizer findings.
- Final local validation after harness/script updates:
  - `./build/bin/test_service`: 315 passed, 0 failed.
  - `bash tests/test_windows_compare_policy_retry.sh`: passed, including the new invalid-repetition guard.
  - `bash tests/test_windows_bench_stability_policy.sh`: passed.
  - `bash -n tests/compare-windows-bench-toolchains.sh && bash -n tests/test_windows_compare_policy_retry.sh && bash -n tests/run-windows-msys-validation.sh`: passed.
  - `git diff --check`: passed.
  - `cmake --build build && /usr/bin/ctest --test-dir build --output-on-failure`: 46/46 passed in 450.01 seconds.
  - `cd src/go && go test ./pkg/netipc/...`: passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`: 338 passed, 0 failed.

Real-use evidence:

- POSIX and Windows wedged-peer behavior is now represented by runnable test fixtures rather than only static reasoning.
- Windows validation used the user's `win11` MSYS environment and exercised C, Go, Rust, Named Pipe, WinSHM, service interop, cache interop, and benchmark paths.

Reviewer findings:

- No external reviewer was run for this SOW yet.
- During validation, the Windows C Named Pipe receive regression was found and fixed before continuing.
- The sanitizer harness hang was found, fixed with bounded fake-server accepts, and revalidated cleanly under ASAN and TSAN.
- The Windows comparison false-fail mode was found, fixed by enforcing a compatible repetition count and safer max-tier duration defaults, and revalidated on `win11`.

Same-failure search results:

- Searched for hardcoded `30000` timeout uses. Remaining production uses are the named C/Go/Rust defaults or docs; test-only waits remain as test harness bounds.
- Searched typed client call paths for raw `nipc_uds_receive()` / `nipc_np_receive()` use. Production client call paths now route through timeout-aware receive wrappers; raw blocking receive remains in server-side and L1/raw-test paths by design.
- Searched timeout/abort API propagation across C, Go, and Rust service wrappers; timeout variants and abort controls are present for the typed services touched by this SOW.

Artifact maintenance gate:

- `AGENTS.md`: no update needed; repository workflow, SOW rules, and project guardrails did not change.
- Runtime project skills: no runtime `project-*` skills exist, and this SOW did not add a reusable repo-working procedure that belongs there.
- Specs: updated `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level2-typed-api.md`, and `docs/level3-snapshot-api.md`.
- End-user/operator docs: updated `docs/getting-started.md`.
- End-user/operator skills: updated `docs/netipc-integrator-skill.md`.
- SOW lifecycle: `SOW-0015` was marked paused; this SOW records decisions, validation, outcome, and no remaining follow-up items.

Lessons:

- Timeout-aware receive code must not replace raw blocking L1 receive behavior blindly. Existing L1 tests depend on raw `ReadFile()`/`recv()` semantics for disconnect and zero-byte/error handling.
- Windows benchmark validation must use a repetition count compatible with the configured minimum stable sample count. With the default `3` stable samples and trimmed raw-outlier publication, comparison validation needs at least `5` total samples.
- Negative-path fake servers must not wait forever for the next connection. They need bounded accepts so sanitizer and CI runs report a deterministic failure instead of hanging.

Follow-up mapping:

- No follow-up SOW is needed for the sanitizer harness issue; it was fixed here and ASAN/TSAN are clean.
- No follow-up SOW is needed for the Windows benchmark comparison issue; the wrapper now rejects under-sampled comparison runs and the final `win11` validation passed.

## Outcome

Completed.

Synchronous Level 2 client calls are now bounded across C, Go, and Rust on POSIX and Windows transports. Clients expose explicit timeout controls and a sticky abort signal that can release blocked calls from another thread. Timeout and abort are distinct errors. Raw Level 1 blocking receive behavior is preserved for callers that intentionally use the raw transport API.

## Lessons Extracted

- Additive timeout APIs avoided breaking existing typed call sites while still giving consumers explicit deadline control.
- Raw L1 receive semantics and typed L2 call semantics are different contracts; timeout-aware receive support must be opt-in at L1 and mandatory at L2.
- Benchmark comparison policy must reject configurations that cannot satisfy its own stable-sample model.

## Followup

None.

## Regression Log

### 2026-06-11

CI failures after commit `aaec3d5` reopened this SOW:

- Static Analysis run `27336192003`, job `Go Static Analysis (src/go)`, failed because `gosec` exited with status 1 after uploading SARIF. Local reproduction with `gosec -fmt=json -exclude=G404 ./...` under `src/go` reports `G115` at `src/go/pkg/netipc/service/raw/client_unix.go:198`: integer overflow conversion `int64 -> uint32` in the SHM receive polling timeout calculation.
- CodeQL run `27336192249`, job `Analyze Go Windows`, failed in the MSYS2 build step while running Go tests. The failing test was `TestSessionReceiveRejectsMalformedChunks/continuation_recv_disconnect` in `src/go/pkg/netipc/transport/windows/pipe_edge_test.go`; CI observed `peer disconnected` while the existing test expected the old wrapped continuation receive error.

Why previous validation missed it:

- The local Go validation did not run `gosec`; the static-analysis workflow did.
- The final `win11` validation did run Windows Go tests, but the CI failure is a Windows named-pipe close timing variation. The changed framing code returned raw continuation receive errors, so one platform/timing path exposed a direct `ErrDisconnected` instead of the pre-existing wrapped `ErrRecv` contract.

Repair plan:

- Keep timeout and abort errors distinct through chunk continuation receives.
- Restore pre-SOW behavior for ordinary continuation receive failures by wrapping them as `ErrRecv("continuation recv: ...")`.
- Replace duplicated timeout polling conversions with bounded helpers that prove the narrowed value is below the poll cap before converting to `uint32`.

Validation plan:

- Re-run `gosec -fmt=json -exclude=G404 ./...` under `src/go`.
- Re-run `cd src/go && go test ./pkg/netipc/...` locally.
- Re-run the failing Windows package/test on `win11` with `MSYSTEM=MSYS`.
- Re-run `git diff --check`.

Repair results:

- Updated `src/go/pkg/netipc/transport/internal/framing/receive.go` so chunk continuation receive errors are wrapped as `ErrRecv("continuation recv: ...")` unless the transport explicitly marks them as timeout/abort control errors.
- Updated POSIX UDS and Windows Named Pipe receive config to propagate only timeout and abort receive errors raw.
- Replaced duplicated Go SHM timeout narrowing logic in `src/go/pkg/netipc/service/raw/client_unix.go` and `src/go/pkg/netipc/service/raw/client_windows.go` with `boundedClientWaitMs()`, which caps the value before the justified `G115`-suppressed conversion.
- Updated Windows named-pipe receive polling to use the same bounded conversion pattern.

Validation results:

- `cd src/go && go test ./pkg/netipc/...`: passed.
- `cd src/go && go vet ./...`: passed.
- `cd src/go && staticcheck ./...`: passed.
- Exact CI-style `gosec -quiet -fmt sarif -out gosec.sarif -exclude=G404 ./...` passed for `src/go`, `tests/fixtures/go`, and `bench/drivers/go`.
- Verbose local `gosec -fmt=json -exclude=G404 ./...` under `src/go` now reports zero issues; it still reports the pre-existing assembly-stub SSA warning for `pkg/netipc/transport/posix/shm_pause_amd64.go`, but the CI-style SARIF command exits successfully with zero findings.
- `win11` MSYS CodeQL-style loop passed: `CGO_ENABLED=0 go test ./...` in `src/go`, `tests/fixtures/go`, and `bench/drivers/go`.
- `win11` focused failure reproduction passed 50 times: `CGO_ENABLED=0 go test ./pkg/netipc/transport/windows -run 'TestSessionReceiveRejectsMalformedChunks/continuation_recv_disconnect' -count=50`.
- `git diff --check`: passed.

Regression artifact maintenance:

- `AGENTS.md`: no update needed; workflow and project guardrails did not change.
- Runtime project skills: no update needed; this was a narrow code regression repair, not a reusable workflow change.
- Specs and end-user/operator docs: no update needed; public timeout/abort behavior remains as documented.
- End-user/operator skills: no update needed; integrator guidance remains correct.
- SOW lifecycle: SOW was reopened from `done/`, repaired, validated, marked completed, and moved back to `done/` with the repair commit.
