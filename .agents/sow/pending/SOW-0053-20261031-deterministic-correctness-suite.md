# SOW-0053 - Deterministic Correctness Suite

## Status

Status: open

Sub-state: created 2026-10-31 from the user's test policy; starts right after SOW-0043, before SOW-0051.

## Requirements

### Purpose

Make the correctness suites of C, Rust and Go fast and deterministic, so every fix is validated in seconds, and keep fuzz, stress and benchmark suites out of everyday validation.

### User Request

User policy (2026-10-31):

1. Correctness tests should take seconds to complete.
2. Any test that requires synchronizing threads and processes must be done without long sleeps.
3. Fuzz, stress, benchmarks, etc. must be run only before vendoring and only if needed.

Earlier in the same discussion: correctness means one go, no flakiness, no sleeps, no retries; correctness tests may include multi-threaded and interop tests, but nothing that is not purely validation that the code works. The user does not want to handle the details; the assistant applies the policy.

### Assistant Understanding

Facts (measured 2026-10-31 on the Linux host, branch `sow-0040-robustness-repro-tests`):

- Linux ctest: 51 entries, 337 s wall with `-j4`. Non-correctness entries: 12 Go fuzz targets at `-fuzztime=20s` (~21 s each, `CMakeLists.txt:246-265`), `fuzz_protocol_30s` (`CMakeLists.txt:241`), `test_stress` (61 s; "Long-running stability (60 seconds)", `tests/fixtures/c/test_stress.c:712`), `test_stress_go` (62 s, `CMakeLists.txt:694`), `test_stress_rust` (31 s, `CMakeLists.txt:709`), `test_lookup_scale_*_interop` (load test, 8192 items), `test_windows_bench_stability_policy` (benchmark tooling).
- Go `service/raw` takes 137 s: `TestStressLongRunning60s` 60 s; lookup tests with 2 s handler sleeps (`TestLookupProactiveRequestSplit` 14.8 s, `TestLookupLogicalLimits` 4.5 s, `TestLookupCallsReconnectForRequestCapacityGrowth` 4.2 s, `TestLookupTimeoutDuringFollowupSubcall` 4.0 s); K4 tests 6 s each; `TestRobustSilentClientReleasedAtHandshakeDeadline` 5.1 s; `TestUnixShmAttachFailureFallsBackToBaseline` 5.0 s (production SHM attach retry).
- Sleep calls in test code: C 208, Rust 138, Go 107. Heaviest: `src/crates/netipc/src/service/raw_unix_tests.rs` (52), `tests/fixtures/c/test_service.c` (41), `src/crates/netipc/src/transport/posix_tests.rs` (31), `tests/fixtures/c/test_chaos.c` (29), `tests/fixtures/c/test_uds.c` (21).
- Sleep patterns: polling for server readiness (`raw_unix_tests.rs:58-78`, `test_service.c:5541`); fixed waits for background completion (`test_service.c:5301`, `raw_unix_tests.rs:3748`, `:4472`); waiting out production timeouts (handshake 5000 ms, SHM attach retry 5000 ms, stop grace 1000 ms, K4 6 s); handlers that sleep to force timeouts (`raw_unix_tests.rs:1396`, `:1454`); retry loops (`servedWithin` / `served_within`); duration-driven stress and chaos loops.
- SOW-0043 already excludes fuzz, stress and scale suites from `tests/run-linux-target.sh` (minimal policy step 3 for its own validation).

Inferences:

- The root causes are missing deterministic synchronization points (no listening signal before `run()` blocks, no completion signal for stop/close), production timeouts that tests cannot shorten, and non-correctness suites registered next to correctness suites.

Unknowns:

- None that investigation cannot resolve.

### Acceptance Criteria

- Correctness suites in every language complete in seconds on the Linux host; Netdata Linux targets under QEMU and Windows stay proportional (no test waits for a fixed duration).
- No correctness test uses a sleep to wait for another thread or process, and no retry loop stands in for an event; every wait is on an explicit signal, with a generous upper bound that only fires on a real bug.
- Timeout behaviour is still proven, with test-only short timeouts instead of production values.
- Fuzz, stress, scale and benchmark suites run only from explicit scripts, before Netdata vendoring when needed; ctest default runs, `run-linux-target.sh` and the Windows validation scripts run correctness only.
- No assertion is weakened: each rewritten test proves the same behaviour.
- AGENTS.md Test Cadence and the vendoring skill state the policy.

## Analysis

Sources checked:

- Linux ctest timing log, Go `-v` per-test timings, sleep counts per file (2026-10-31).

Current state:

- See Facts.

Risks:

- Rewriting hundreds of tests can silently drop coverage; mitigated by keeping each test's assertions and by the coverage scripts.
- Test-only timeout knobs must not change production defaults or the public API.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- See Inferences; completed at activation.

Evidence reviewed:

- See Facts; completed at activation with a per-file classification.

Affected contracts and surfaces:

- Test code in C, Rust and Go; `CMakeLists.txt` test registration; `tests/run-linux-target.sh`; Windows validation scripts; possibly internal test-only hooks in the libraries (no public API change).

Existing patterns to reuse:

- Existing `NIPC_INTERNAL_TESTING` compile definitions and fault-injection hooks.

Risk and blast radius:

- Tests in every language and platform; recorded fully at activation.

Sensitive data handling plan:

- No secrets involved. Applies to this SOW, docs, AGENTS.md, the vendoring skill and code comments.

Implementation plan:

1. Completed at activation.

Validation plan:

- Full correctness suites on Linux, the Netdata Linux targets and Windows, plus a coverage comparison before and after.

Artifact impact plan:

- AGENTS.md: Test Cadence states the policy.
- Runtime project skills: `.agents/skills/project-netdata-vendoring/SKILL.md` runs fuzz/stress/benchmarks before vendoring when needed.
- Specs: none expected (tests only).
- End-user/operator docs: none expected; re-check validation commands in `docs/netipc-integrator-skill.md`.
- End-user/operator skills: `docs/netipc-integrator-skill.md` validation commands if they change.
- SOW lifecycle: created from the user's policy; runs before SOW-0051.

Open-source reference evidence:

- None.

Open decisions:

- None; the user set the policy and delegated the details.

## Implications And Decisions

None yet.

## Plan

Written at activation.

## Execution Log

None yet.

## Validation

Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
