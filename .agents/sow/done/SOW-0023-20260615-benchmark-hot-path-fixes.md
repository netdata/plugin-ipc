# SOW-0023 - Benchmark Hot Path Fixes

## Status

Status: completed

Sub-state: implementation, validation, benchmark regeneration, and external review closure completed on 2026-06-15.

## Requirements

### Purpose

Keep NetIPC fast enough for Netdata-scale typed service use across C, Rust, and Go. When the same benchmark scenario shows large language deltas, the project should fix proven implementation hot-path waste instead of treating the gap as unavoidable runtime overhead.

### User Request

The user accepted the SOW-0022 recommendations and asked to start the implementation.

### Assistant Understanding

Facts:

- SOW-0022 proved three root causes with targeted diagnostics and profiles.
- Rust Windows typed SHM snapshot calls are slow because the typed receive wrapper performs per-call timeout/abort bookkeeping before reaching the fast WinSHM receive path.
- Go raw batch server throughput is limited by repeated generic per-item batch validation and response-builder overhead in the production raw server path.
- Go lookup-method throughput is limited by repeated directory, string, label, and builder validation work in codec hot paths.
- The user selected SOW-0022 decisions `1A`, `2A`, `3A`, and `4A`.

Inferences:

- The fixes should be internal fast paths and should not change public APIs, wire formats, or the Level 2 scale contract.
- Rust Windows timeout and abort behavior must be preserved while avoiding avoidable fast-path `Instant::now()` work.
- Go optimizations must preserve malformed-input rejection and SOW-0021 oversized/overflow semantics.

Unknowns:

- Final benchmark deltas after implementation.
- Whether all three hot-path fixes can be completed without further design decisions. If implementation exposes a contract choice, work must stop and the user must decide.

### Acceptance Criteria

- Rust Windows typed `snapshot-shm` throughput no longer has the SOW-0022 order-of-magnitude client-side penalty.
- Go raw batch/pipeline server hot-path overhead is reduced without weakening batch validation.
- Go lookup codec hot-path overhead is reduced without public API or wire-format changes.
- Existing C, Rust, and Go tests still pass on POSIX and Windows for touched areas.
- Focused benchmarks show the targeted deltas improved or the remaining gap is explained with evidence.
- Full benchmark validation is run or any inability to run it is recorded with evidence.
- Durable artifacts are updated or explicitly marked unaffected with evidence.

## Analysis

Sources checked:

- `.agents/sow/done/SOW-0022-20260615-benchmark-language-deltas.md`
- `.agents/sow/current/SOW-0021-20260613-netipc-at-scale.md`
- `docs/code-organization.md`
- `docs/level2-typed-api.md`
- `docs/level3-snapshot-api.md`
- `docs/level1-windows-shm.md`
- `src/crates/netipc/src/service/raw/client_call.rs`
- `src/crates/netipc/src/transport/win_shm.rs`
- `src/go/pkg/netipc/service/raw/server.go`
- `src/go/pkg/netipc/protocol/frame.go`
- `src/go/pkg/netipc/protocol/apps_lookup.go`
- `src/go/pkg/netipc/protocol/cgroups_lookup.go`
- `src/go/pkg/netipc/protocol/lookup_common.go`

Current state:

- SOW-0022 is completed and moved to `.agents/sow/done/`.
- SOW-0015 and SOW-0021 remain paused in `.agents/sow/current/`.
- A tracked benchmark executable, `bench/drivers/go/go`, is already modified in the working tree and is outside this SOW's intended source changes.
- No product-code fix has been implemented yet for the three SOW-0022 root causes.

Risks:

- Rust Windows receive changes can break timeout, abort, peer-death, or slow-peer behavior if the fast path bypasses required control checks.
- Go batch fast paths can weaken malformed batch rejection if they assume validation in paths where validation has not happened.
- Go lookup codec changes can break SOW-0021 scale guarantees if they skip status, label, echoed-key, overflow, or oversized-item checks.
- Performance changes can move overhead rather than remove it; benchmark evidence is required.

## Pre-Implementation Gate

Status: ready.

Problem / root-cause model:

- Rust Windows typed snapshot over SHM is slow because `receive_win_shm_with_control()` performs deadline and abort-poll work before reaching the fast SHM receive path, even when the response is already available. SOW-0022 diagnostics showed direct WinSHM snapshot send/receive around `1.2M/s`, while typed `call_snapshot()` was around `94k/s`; a temporary wrapper bypass raised typed calls to around `1.14M/s`.
- Go raw batch servers spend material CPU in generic batch item retrieval and builder logic. SOW-0022 `perf` showed `protocol.(*BatchBuilder).Add` at `22.32%` and `protocol.BatchItemGet` at `15.77%` in a production Go raw server benchmark.
- Go lookup method codecs spend material CPU in repeated validated directory, string, label, and builder work. SOW-0022 `perf` showed lookup helper and builder functions dominating the `cgroups-lookup-mixed-16` codec-only benchmark.

Evidence reviewed:

- SOW-0022 root-cause evidence and targeted benchmark/profiling runs.
- Level 2 typed API docs for timeout, abort, and scale behavior.
- Code organization docs for codec/service/transport boundaries.
- Rust raw client and WinSHM receive paths.
- Go raw server batch dispatch and protocol batch helper paths.
- Go apps/cgroups lookup codec builders and common lookup helpers.

Affected contracts and surfaces:

- Rust internal raw client receive path for Windows SHM.
- Go internal raw server batch dispatch.
- Go internal protocol batch helper path.
- Go internal apps/cgroups lookup codec builder/decode helpers.
- Benchmarks and validation evidence.
- No public API, wire format, or protocol status contract should change.

Existing patterns to reuse:

- Keep codec ownership of wire-format details and service ownership of typed orchestration.
- Preserve transport receive validation before service dispatch when relying on validated batch structure.
- Keep Go pure Go without cgo.
- Preserve timeout as an absolute call deadline and abort checks as observable control behavior.
- Add internal helpers rather than exposing public fast-path APIs unless a design decision is required.

Risk and blast radius:

- Medium risk for Rust Windows because timeout/abort behavior is correctness-critical.
- Medium risk for Go raw batch because malformed-message rejection must not regress.
- Medium risk for Go lookup codec because SOW-0021 added scale and corruption edge cases that must remain covered.
- Blast radius is limited to Rust Windows typed SHM receive and Go internal service/protocol hot paths if public contracts remain unchanged.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write secrets, credentials, bearer tokens, private endpoints, customer data, personal data, or proprietary incident details into durable artifacts.
- Record only synthetic benchmark measurements, local command names, and repository file references.

Implementation plan:

1. Rust Windows SHM typed-client receive:
   - Add a fast path that attempts the already-ready SHM receive before deadline/abort bookkeeping where semantics allow.
   - Preserve slow-path timeout and abort behavior for absent or delayed responses.
   - Add or update focused tests for timeout/abort if an existing test does not cover the changed branch.
2. Go raw batch server:
   - Add internal helpers for validated batch item traversal and response building used only after inbound batch validation has succeeded.
   - Keep generic public helpers defensive for untrusted callers.
   - Validate malformed batch tests still fail.
3. Go lookup codec:
   - Add internal hot-path helpers that avoid repeated validation for already-decoded request items and builder-owned data.
   - Preserve external validation behavior for public decode/build APIs.
   - Validate SOW-0021 lookup scale and corruption tests still pass.
4. Re-run focused tests and benchmarks, then broader POSIX and Windows validation.

Validation plan:

- POSIX Go tests for protocol and raw service packages.
- POSIX Rust tests for the touched crate paths.
- Windows Rust focused tests and `snapshot-shm` benchmark rows on `win11`.
- Windows Go focused tests if Go raw/lookup paths are touched in Windows-sensitive tests.
- Focused POSIX Go batch and lookup benchmarks before/after comparison.
- Full benchmark generation when focused fixes are stable.
- `.agents/sow/audit.sh` and `git diff --check`.

Artifact impact plan:

- AGENTS.md: no update expected; workflow rules do not change.
- Runtime project skills: no runtime project skills exist; no update expected.
- Specs: likely no update because public API/wire behavior should not change. Update only if a behavioral contract changes.
- End-user/operator docs: no update expected unless benchmark/performance guarantees become documented behavior.
- End-user/operator skills: no update expected unless docs/specs change.
- SOW lifecycle: SOW-0022 completed; SOW-0023 owns implementation and validation.

Open-source reference evidence:

- Not relevant. The work fixes local hot paths identified by local benchmark/profiling evidence, not an external protocol or third-party integration.

Open decisions:

- User approved SOW-0022 decisions `1A`, `2A`, `3A`, and `4A` on 2026-06-15.
- No remaining implementation-blocking decisions are known.

## Implications And Decisions

User decisions carried from SOW-0022:

1. Rust Windows `snapshot-shm`: A. Implement a typed-client receive fast path while preserving timeout and abort semantics.
2. Go batch/pipeline server: A. Implement internal validated-batch and response-builder fast paths.
3. Go lookup-method codecs: A. Implement internal hot-path improvements without public API changes.
4. SOW execution: A. Keep SOW-0022 analysis-only and track implementation here.

## Plan

1. Inspect current Rust Windows receive-control tests and implementation before editing.
2. Implement Rust Windows SHM fast path and validate focused Rust tests/benchmark.
3. Inspect Go raw batch validation and malformed-batch tests before editing.
4. Implement Go raw batch internal fast path and validate focused Go tests/benchmark.
5. Inspect Go lookup codec corruption and scale tests before editing.
6. Implement Go lookup codec hot-path improvements and validate focused Go tests/benchmark.
7. Run broader POSIX and Windows tests/benchmarks and update validation evidence.

## Execution Log

### 2026-06-15

- Created SOW-0023 from the completed SOW-0022 decisions.
- Implemented a crate-internal Rust Windows SHM ready receive probe in `src/crates/netipc/src/transport/win_shm.rs`.
- Wired the Rust typed Windows SHM receive wrapper in `src/crates/netipc/src/service/raw/client_call.rs` to check abort, try the ready-only SHM receive path, and fall back to the existing timeout/abort-controlled loop.
- Optimized Go batch directory validation, batch item extraction, and batch builder normal-path appends in `src/go/pkg/netipc/protocol/frame.go`.
- Optimized Go lookup common directory validation, payload slicing, string view creation, label layout, and label writing in `src/go/pkg/netipc/protocol/lookup_common.go`.
- Optimized Go cgroups request-backed response building in `src/go/pkg/netipc/protocol/cgroups_lookup.go` so `AddRequestItem()` uses the already-validated decoded request item directly.
- Built and tested the changed source on POSIX and on `win11` from a clean `/tmp/plugin-ipc-sow23` source copy that excluded `.env` and build artifacts.
- External review round 1 found no concrete implementation bug, but raised validation blockers: missing full benchmark regeneration and missing direct focused tests for the new Windows SHM ready receive path and Go fast-path overflow/malformed edges.
- Added Windows Rust tests for `receive_ready()` ready-hit, miss, bad header, and oversized-length behavior in `src/crates/netipc/src/transport/win_shm.rs`.
- Added a Windows Rust wrapper test for `receive_win_shm_with_control()` ready-hit, abort-before-call, timeout fallthrough, and peer-close fallthrough behavior in `src/crates/netipc/src/service/raw/client_call.rs`.
- Added Go 32-bit guard coverage for direct `BatchItemGet()` over-`int` offset/length reads, large `BatchBuilder.Add()` item directory indexes, and malformed cgroups request-backed `itemBytes()` cases in `src/go/pkg/netipc/protocol/lookup_guard_test.go`.
- Adjusted the impossible negative packed-area guard in `validateLookupDir()` to return `ErrOutOfBounds`, preserving the old error class more closely for that unreachable invalid state.
- Re-ran POSIX and Windows focused/full tests after the focused test additions.

## Validation

Acceptance criteria evidence:

- Rust Windows `snapshot-shm` typed-client anomaly fixed:
  - Before SOW-0023, SOW-0022 measured Rust typed `call_snapshot()` around `94k/s` to `105k/s`.
  - After SOW-0023, targeted Windows benchmark from `/tmp/plugin-ipc-sow23` measured `snapshot-shm rust->rust @ max` at `1,216,818/s`.
  - Same Rust server context rows measured `c->rust` at `1,196,797/s` and `go->rust` at `989,398/s`.
- Go raw batch server overhead reduced:
  - Before SOW-0023, SOW-0022 focused profile measured `uds-pipeline-batch-d16 go->go` at `35,327,058 item/s`.
  - After SOW-0023, focused runs measured `41,533,724 item/s` and `41,311,391 item/s`; a profiled run with `perf` active measured `38,736,003 item/s`.
  - After-profile still shows residual cost in `BatchBuilder.Add`, `BatchItemGet`, generic dispatch, and benchmark increment handler. The obvious repeated checked-helper overhead was reduced; the remaining Go gap needs reviewer scrutiny before any further change.
- Go lookup codec overhead reduced:
  - Before SOW-0023, SOW-0022 focused profile measured `cgroups-lookup-mixed-16 go->go` at `448,973/s`.
  - After SOW-0023, focused rows measured `cgroups-lookup-mixed-16` at `536,932/s` and `apps-lookup-mixed-16` at `659,207/s`.
  - Larger focused rows measured `cgroups-lookup-mixed-256` at `41,904/s` and `apps-lookup-mixed-256` at `50,908/s`.
- No public API, wire format, status value, or documented Level 2 behavior was intentionally changed.

Tests or equivalent validation:

- POSIX Go protocol package: `go test ./pkg/netipc/protocol` passed.
- POSIX Go raw service package: `go test ./pkg/netipc/service/raw` passed after the final Go changes.
- POSIX Go full module: `go test ./...` under `src/go` passed.
- Go benchmark driver module: `go test ./...` under `bench/drivers/go` passed.
- POSIX Rust full crate: `cargo test --manifest-path src/crates/netipc/Cargo.toml` passed, including `375` library tests and binary/doc test harnesses.
- CMake build: `cmake --build build` passed.
- POSIX CTest: `/usr/bin/ctest --test-dir build --output-on-failure` passed, `48/48` tests, `0` failed. The plain `ctest` command on PATH is a broken Python wrapper missing the `cmake` module, so `/usr/bin/ctest` was used.
- After focused test additions, POSIX Go protocol package: `go test ./pkg/netipc/protocol -count=1` passed.
- After focused test additions, POSIX Go 32-bit guard coverage: `GOARCH=386 go test ./pkg/netipc/protocol -count=1 -run 'TestLookupThirtyTwoBitGuardCoverage|TestBatch'` passed.
- After focused test additions, POSIX Go full module: `go test ./... -count=1` under `src/go` passed.
- After focused test additions, POSIX Rust full library validation: `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib` passed, `375` tests, `0` failed.
- After focused test additions, CMake build: `cmake --build build` passed.
- After focused test additions, POSIX CTest: `/usr/bin/ctest --test-dir build --output-on-failure` passed, `48/48` tests, `0` failed.
- Windows Rust focused/full library validation from `/tmp/plugin-ipc-sow23`: `cargo.exe test --manifest-path src/crates/netipc/Cargo.toml --lib` passed, `263` tests, `0` failed, including the new Windows SHM ready/wrapper tests.
- Windows Go focused packages from `/tmp/plugin-ipc-sow23/src/go`: `go.exe test ./pkg/netipc/protocol ./pkg/netipc/service/raw -count=1` passed.
- Windows Go 32-bit guard coverage from `/tmp/plugin-ipc-sow23/src/go`: `GOARCH=386 go.exe test ./pkg/netipc/protocol -run TestLookupThirtyTwoBitGuardCoverage -count=1` passed.
- Windows Rust warning check after the final warning fix: `cargo.exe test --manifest-path src/crates/netipc/Cargo.toml --lib transport::win_shm::tests::test_receive_timeout_hybrid_windows` passed, `1` test, `0` failed.
- Focused Windows benchmark: `bash tests/run-windows-bench-targeted.sh --duration 5 --row snapshot-shm,rust,rust,0 --row snapshot-shm,c,rust,0 --row snapshot-shm,go,rust,0` passed.
- Focused POSIX Go lookup and batch benchmark commands passed using `/tmp/netipc-bench-go`.
- Added benchmark-report note support to `tests/generate-benchmarks-posix.sh` and `tests/generate-benchmarks-windows.sh`.
- Added `benchmarks-posix.notes.md` and `benchmarks-windows.notes.md` so benchmark rows replaced by validated targeted reruns are visible in generated reports instead of living only in SOW text.
- Added reviewer-requested safety comments for the Rust Windows SHM malformed/oversized sequence-consumption rule and the Go batch-builder fast path.
- Removed unreachable Go lookup negative-length checks after re-verifying the values are `uint32` lengths widened to `int`.
- External review rounds after the focused fixes and benchmark-regeneration updates found no implementation blockers. One reviewer withheld `PRODUCTION GRADE` only because this SOW lifecycle was still open with pending outcome text; this closure update resolves that lifecycle blocker without changing source behavior.
- Full POSIX benchmark regeneration:
  - `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_posix` passed.
  - `bash tests/run-posix-bench.sh benchmarks-posix.csv 5` completed.
  - `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md` passed row-count validation and reported `All performance floors met`.
  - Final POSIX CSV contains `345` measurement rows.
  - POSIX floor retry evidence: `snapshot-shm go->go @ max` recovered from `520,442/s` to `804,650/s`, retry stable ratio `1.072684`, status `recovered`.
  - POSIX `snapshot-shm @ max` after the full run: `c->rust 1,457,148/s`, `rust->rust 1,476,776/s`, `go->rust 1,184,967/s`, `go->go 804,650/s`.
  - POSIX Go lookup after the full run: `cgroups-lookup-mixed-16 go 546,195/s`, `apps-lookup-mixed-16 go 673,592/s`, `cgroups-lookup-mixed-256 go 41,188/s`, `apps-lookup-mixed-256 go 50,817/s`.
  - POSIX `uds-pipeline-batch-d16 go->go @ max` after the full run: `37,662,172 item/s`.
  - POSIX `uds-pipeline-batch-d16 go->rust @ max` produced one suspicious full-suite outlier at `13,133,928 item/s`; isolated reruns of the same server/client command produced `65,534,043`, `70,190,250`, and `65,409,370 item/s`. A fourth isolated rerun using the same server CPU accounting as the official script produced `66,261,510 item/s`, `p50 99us`, `p95 205us`, `p99 279us`, client CPU `84.1%`, server CPU `89.797%`, total CPU `173.897%`. The final generated POSIX report uses that validated targeted row and this SOW records the replacement explicitly.
- Full Windows benchmark regeneration:
  - From `/tmp/plugin-ipc-sow23`, `bash tests/run-windows-bench.sh benchmarks-windows.csv 5` completed the full matrix under `MSYSTEM=MINGW64` with `/mingw64/bin:/c/Users/[USER]/.cargo/bin` prepended to `PATH`.
  - The full Windows run produced `200` of `201` rows because `snapshot-baseline rust->c @ max` failed stability twice: first attempt stable ratio `2.572475`, diagnostic stable ratio `1.689472`, limit `1.35`.
  - The failed Windows row showed warm-up/ramp evidence rather than a crash: first attempt samples ranged from `8,628/s` to `40,872/s`; diagnostic samples ramped from `10,613/s` to `61,032/s`.
  - Targeted rerun with `bash tests/run-windows-bench-targeted.sh --out-dir /tmp/netipc-bench-targeted-sow23 --duration 5 --attempts 3 --row snapshot-baseline,rust,c,0` passed on the first attempt with `71,467/s`, `p50 14.600us`, `p95 29.500us`, `p99 74.200us`, stable ratio `1.006983`.
  - The final Windows CSV combines the full matrix with that validated targeted row.
  - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md` passed row-count validation and reported `All performance floors met`.
  - Final Windows CSV contains `201` measurement rows.
  - Windows `snapshot-shm @ max` after the full run: `c->rust 1,050,196/s`, `rust->rust 1,114,147/s`, `go->rust 908,892/s`, `go->go 676,802/s`.
  - Windows `shm-batch-ping-pong @ max` after the full run: `go->rust 47,631,572 item/s`, `rust->go 41,766,050 item/s`, `go->go 38,294,916 item/s`.
  - Windows local lookup after the full run: `c 121,925,499/s`, `rust 177,615,463/s`, `go 104,161,146/s`.

Real-use evidence:

- Windows benchmark rows exercised real Rust typed snapshot client/server SHM calls through the normal Level 2 benchmark driver, not a diagnostic bypass.
- POSIX Go batch benchmark exercised the production Go raw server path.
- POSIX Go lookup benchmark exercised codec/dispatch loops used by the typed lookup stack.

Reviewer findings:

- External review round 1:
  - `glm`: `PRODUCTION GRADE`; noted a non-blocking recommendation for direct Windows `receive_ready()` / wrapper tests.
  - `mimo`: `PRODUCTION GRADE`; noted direct Windows wrapper tests and full benchmark regeneration as useful before close.
  - `kimi`: `NOT PRODUCTION GRADE`; blockers were missing full benchmark regeneration and missing focused Windows SHM / Go fast-path edge tests; also noted the tracked benchmark binary diff should not be committed and the negative packed-area error-class drift should be recorded or removed.
  - `minimax`: technical failure; it timed out before producing a final vote after running local validation commands. It observed a CTest fuzz failure from an interrupted/timeout-limited run, then reran `go_FuzzDecodeAppsLookupResponse` successfully. The authoritative full CTest rerun after focused test additions passed `48/48`.
  - `deepseek`: first output was lost due tool-output truncation; rerun required.
  - `qwen`: first output was lost due tool-output truncation; rerun required.
- Round 1 blocker handling:
  - Direct Windows `receive_ready()` tests added.
  - Direct Windows `receive_win_shm_with_control()` wrapper tests added.
  - Go 32-bit batch and cgroups request-backed fast-path guard tests added.
  - Negative packed-area error-class drift changed to `ErrOutOfBounds`.
  - Full benchmark regeneration started.
  - Tracked `bench/drivers/go/go` binary diff is acknowledged as a build artifact and must not be included in the final SOW commit unless the user explicitly approves committing rebuilt benchmark executables.
- External review round 2 after benchmark regeneration:
  - `mimo`: `PRODUCTION GRADE`; no blockers.
  - `deepseek`: `PRODUCTION GRADE`; no blockers.
  - `minimax`: `PRODUCTION GRADE`; no implementation blockers, with minor documentation/artifact clarity notes.
  - `qwen`: `PRODUCTION GRADE`; no blockers.
  - `glm`: `PRODUCTION GRADE`; no blockers, recommended making targeted benchmark-row replacements visible in generated reports.
  - `kimi`: `PRODUCTION GRADE`; no blockers after focused validation, with minor clarity suggestions.
- Round 2 non-blocking handling:
  - Benchmark-row replacement notes were added to generated reports through sidecar note files.
  - Rust Windows SHM sequence-consumption intent was documented in code.
  - Go batch fast-path intent was documented in code.
  - Go lookup unreachable negative-length checks were removed.
- External review round 3 after the clarity fixes:
  - `mimo`: `PRODUCTION GRADE`; no blockers.
  - `deepseek`: `PRODUCTION GRADE`; no blockers.
  - `qwen`: `PRODUCTION GRADE`; no blockers.
  - `glm`: `PRODUCTION GRADE`; no blockers.
  - `kimi`: `PRODUCTION GRADE`; no blockers.
  - `minimax`: `NOT PRODUCTION GRADE` only because the SOW lifecycle still said `Status: in-progress`, `Outcome: Pending`, and `Lessons Extracted: Pending`. It identified no implementation blocker and explicitly called the remaining work mechanical SOW closure.
- Round 3 lifecycle handling:
  - Status changed to `completed`.
  - Outcome, lessons, artifact maintenance, and follow-up mapping were filled.
  - The SOW is moved to `.agents/sow/done/` as part of the same closure work.
- External closure review after lifecycle update:
  - `glm`: `PRODUCTION GRADE`; no blockers, independently reran Go protocol/raw-service checks, Go 32-bit guard coverage, Rust build, Rust library tests, SOW audit, and a sensitive-data scan over benchmark/report diffs.
  - `minimax`: `PRODUCTION GRADE`; no blockers, noted only that `bench/drivers/go/go` must not be committed.
  - `kimi`: `PRODUCTION GRADE`; no blockers.
  - `mimo`: `PRODUCTION GRADE`; no blockers.
  - `deepseek`: `PRODUCTION GRADE`; no blockers.
  - `qwen`: `PRODUCTION GRADE`; no blockers, noted only that `bench/drivers/go/go` must not be committed.

Same-failure scan:

- Rust Windows receive-control same-failure class searched in SOW-0022. The implemented fast path is limited to Windows SHM because the measured order-of-magnitude anomaly was Windows-specific.
- Go batch helper changes are in shared protocol helpers used by the raw server batch path, not benchmark-only code.
- Go lookup helper changes are in shared cgroups/common lookup protocol helpers. Apps lookup benefits through common helpers; cgroups additionally benefits from request-backed item reuse.

Sensitive data gate:

- `.env` was not read.
- The Windows temp source copy explicitly excluded `.env`.
- Durable artifacts record only synthetic benchmark numbers, source paths, and command names.

Artifact maintenance gate:

- AGENTS.md: no update needed; workflow and project guardrails did not change.
- Runtime project skills: no runtime project skills exist for this repository.
- Specs: no update needed so far; public API, wire format, protocol status values, and documented Level 2 semantics did not change.
- End-user/operator docs: no update needed so far; this is an internal performance implementation change.
- End-user/operator skills: no update needed so far; no exported operator workflow changed.
- SOW lifecycle: SOW-0022 completed and moved to `done`; SOW-0023 completed and moved to `done` after implementation, benchmark regeneration, validation, and reviewer lifecycle findings were handled.

Specs update:

- No spec update needed because behavior, public APIs, and wire contracts are unchanged.

Project skills update:

- No project skill update needed because the work did not change repository operating procedure and no runtime project skills exist.

End-user/operator docs update:

- No end-user/operator docs update needed because the work is an internal performance fix and benchmark evidence update, not a user-facing API or workflow change.

End-user/operator skills update:

- No end-user/operator skill update needed because no exported operator workflow changed.

Lessons:

- The Rust Windows anomaly was removable without changing the WinSHM data plane: avoiding wrapper-level deadline work on the already-ready path restored typed-client parity.
- Go helper hot paths need profiles after each pass; some changes move only a few percent, while common label/layout and request-backed item reuse produced larger lookup wins.
- Benchmark outlier replacements need durable, generated-report-visible notes so the performance record is reviewable without rereading SOW history.
- A tracked benchmark executable can become dirty during validation; it remains a local build artifact for this SOW and is excluded from the intended commit.

Follow-up mapping:

- Full benchmark regeneration completed for POSIX and Windows; generated reports pass row-count validation and performance floors.
- Benchmark-row replacement transparency was implemented through report note sidecars and regenerated reports.
- Reviewer implementation findings were handled or explicitly judged non-blocking with evidence above.
- No remaining tracked work belongs to this SOW.

## Outcome

Completed.

SOW-0023 implemented the SOW-0022 accepted hot-path fixes:

- Rust Windows typed SHM receive now checks abort first, tries an already-ready SHM receive path, and falls back to the existing timeout/abort-controlled loop.
- Go protocol batch and lookup helpers avoid repeated checked-helper work in proven hot paths while preserving malformed-input rejection and 32-bit guard coverage.
- Benchmark reports now include sidecar notes for validated targeted rerun replacements.

Validation passed on POSIX and Windows for the touched Go and Rust areas, CMake/CTest passed, full POSIX and Windows benchmark reports regenerated, and all performance floors pass. After SOW lifecycle closure, all six required external reviewers voted `PRODUCTION GRADE`.

## Lessons Extracted

- Fast-path work must preserve the same control semantics as the slow path. The Rust Windows change is safe because abort is checked before the ready probe, malformed/oversized observations consume the same sequence as `receive()`, and slow/no-response cases still use the old deadline loop.
- Go hot-path optimization is worth doing only when profiles identify repeated production helper overhead and tests prove equivalent bounds. The added 32-bit guard tests protect the main narrowing-risk paths touched here.
- Benchmark replacements are acceptable only when the replacement command and measurement are recorded. The generated reports now carry those notes next to the benchmark tables.
- Validation can dirty tracked build artifacts. The source and report changes are commit material; `bench/drivers/go/go` is not part of this SOW's intended source change.

## Followup

None. No remaining tracked work belongs to this SOW.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
