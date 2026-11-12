# SOW-0041 - Rust Receive Timeout Slice

## Status

Status: completed

Sub-state: N1 fixed in Rust UDS and named-pipe receive; validated on Linux and `costa-win11`; known-bug gate removed from all robustness tests.

## Requirements

### Purpose

Rust UDS and named-pipe receive must honor the configured call timeout instead of failing after one 100 ms abort-poll slice.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

2026-10-29, after SOW-0040 landed: "remove NETIPC_REPRO_KNOWN_BUGS - there are no known bugs. All bugs must be fixed now. Zero known bug policy. We created the tests first to prove that they exist and are true bugs. Not to document them, but to fix them." Then: "I want one issue at a time, with proper explanation, risks, proposed solutions, risk of the solutions recommended and explicit recommendations with reasoning."

### Assistant Understanding

Facts:

- Rust caps each wait at 100 ms when an abort flag exists and returns `Timeout` on the first empty slice: `src/crates/netipc/src/transport/posix.rs:932-934`, `:956-961`; `src/crates/netipc/src/transport/windows.rs:541-542`, `:631-637`.
- The client always passes an abort flag (`src/crates/netipc/src/service/raw/client_call.rs:329`, `:349`), so every Rust UDS/pipe call with a server slower than about 100 ms fails with a terminal `Timeout`, including the default 30000 ms.
- C (`netipc_uds_receive.c:48-86`, `netipc_named_pipe.c:379-442`) and Go (`uds_receive.go:103-104`, `pipe_receive.go:118-120`) re-wait until the real deadline.
- Reproduced (SOW-0040): Rust Linux failed after 101 ms, Rust Windows after 97 ms, with a 300 ms handler and a 2000 ms timeout.

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

- Analysis 2026-10-29: with `timeout_ms == 0` ("wait indefinitely", `posix.rs:418`) plus an abort flag, Rust UDS also returned `Timeout` after 100 ms. Rust SHM receive (`client_call.rs:453`, `:484`) and the Rust server session loop already treat an empty slice as "wait again"; send paths have no slice logic; handshake reads have no timeout at all (SOW-0043).

Unknowns:

- None after the user decision below.

### Acceptance Criteria

- `test_robust_uds_call_honors_timeout_longer_than_100ms` (Rust Linux) and `test_robust_win_pipe_call_honors_timeout_longer_than_100ms` (Rust Windows) pass with no skip gate.
- Existing timeout/abort tests still pass; abort still interrupts promptly.
- Cross-language slow-handler coverage considered (sibling review suggested a 300 ms handler across all client/server pairs).

## Analysis

Sources checked:

- SOW-0040 evidence and tests.
- Rust `src/crates/netipc/src/transport/posix.rs` (`ReceiveWait`, `poll_timeout_ms`, `wait_fd_readable`), `transport/windows.rs` (`wait_slice_ms`, `raw_recv_with_wait`, `wait_pipe_readable`), `service/raw/client_call.rs`.
- C `netipc_uds_receive.c:10-87`, `netipc_named_pipe.c:359-442`; Go `transport/posix/uds_receive.go:47-105`, `transport/windows/pipe_receive.go:74-125`.
- Specs `docs/level1-posix-uds.md:101-110`, `docs/level1-windows-np.md:98-106`, `docs/level2-typed-api.md:261-272`.

Current state (before the fix):

- Rust returned `Timeout` on the first empty 100 ms abort-poll slice in both receive functions; C and Go loop until the real deadline.

Risks:

- Calls that used to fail fast by accident now wait their real timeout against a hung server (correct, but a behavior change).
- Abort latency stays up to 100 ms, as in Go (spec drift tracked in SOW-0047).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- `wait_fd_readable` (posix.rs) and `raw_recv_with_wait` (windows.rs) treat the end of one abort-poll slice as the end of the call. The slice exists only so the abort flag is checked about every 100 ms; the deadline is already enforced by `poll_timeout_ms()` / `wait_slice_ms()`, which return `Timeout` once it has passed.

Evidence reviewed:

- Code and specs listed under Analysis; SOW-0040 reproduction results (Rust Linux failed after 101 ms, Rust Windows after 97 ms, with a 300 ms handler and a 2000 ms timeout).

Affected contracts and surfaces:

- Rust transport receive behavior only (`UdsSession::receive_with_control`, `NpSession::receive_with_control`); no public API, wire format, or spec change. Its only callers are `client_call.rs:329`, `:349`.
- Test fixtures: interop service servers in C, Rust and Go on POSIX and Windows gain an optional `NIPC_HANDLER_DELAY_MS` server delay; the two service interop scripts run all 9 pairs a second time with a 300 ms handler.
- Robustness tests in all six files: the `NETIPC_REPRO_KNOWN_BUGS` gate is removed (user decision), so the tests for bugs owned by SOW-0042 to SOW-0045 now run and fail until those SOWs land.

Existing patterns to reuse:

- Go `waitMs()` / `pollReadableForReceive` loop and Rust SHM receive slice loop (`client_call.rs:420-495`, `.as_millis().max(1)`).

Risk and blast radius:

- Small: two `return` statements replaced by `continue`, plus a 1 ms minimum wait so the final millisecond does not busy-spin on `poll(0)`.

Sensitive data handling plan:

- No secrets or customer data involved; evidence is code paths, test names, and timings. Windows SIDs are never recorded.

Implementation plan:

1. Remove the known-bug gate helpers and call sites from the six robustness test files.
2. posix.rs: empty poll slice -> `continue`; remaining wait clamped to at least 1 ms.
3. windows.rs: empty slice -> `continue`; remaining wait at least 1 ms.
4. Add Rust transport tests: delayed response with `timeout_ms` 0 and 2000 plus abort flag; abort after 300 ms reports `Aborted`.
5. Add `NIPC_HANDLER_DELAY_MS` to the six interop service fixtures and a 300 ms pass to `tests/test_service_interop.sh` and `tests/test_service_win_interop.sh`.
6. Record the zero-known-bug policy in `AGENTS.md`; update pending SOWs that referenced the gate.

Validation plan:

- New tests fail without the fix and pass with it; N1 reproduction tests pass on Linux and `costa-win11`; existing timeout/abort tests pass; service interop (baseline and SHM) passes on both platforms; rustfmt, clippy correctness/suspicious gates, gofmt, go vet clean.

Artifact impact plan:

- AGENTS.md: update (zero-known-bug policy; robustness test command note).
- Runtime project skills: none affected (vendoring skill unchanged).
- Specs: none; the fix restores documented behavior.
- End-user/operator docs: none; no documented API or behavior changes.
- End-user/operator skills: none.
- SOW lifecycle: this SOW completes; SOW-0042 to SOW-0045 updated for gate removal and new findings; SOW-0046 and SOW-0047 created.

Open-source reference evidence:

- None.

Open decisions:

- None; decision 1 below recorded before implementation.

## Implications And Decisions

1. 2026-10-29, gate removal: the user ordered removing `NETIPC_REPRO_KNOWN_BUGS` under a zero-known-bug policy; all robustness tests run ungated from this SOW's commit, and the branch merges only when every reproduction test passes.
2. 2026-10-29, N1 fix: user chose option 1A - mirror Go's receive loop (an empty abort-poll slice means "wait again", `Timeout` only at the real deadline, at least 1 ms per wait). Rejected 1B (shared deadline helper across UDS, pipe and SHM: refactors working SHM code without fixing more). Option C (real abort fd/event) moved to SOW-0047.

## Plan

Executed as the implementation plan in the Pre-Implementation Gate.

## Execution Log

### 2026-10-29

- Read-only analysis round for all open robustness bugs (six parallel analyses). New findings were recorded in SOW-0042 to SOW-0045, and in new SOW-0046 (Go linux/arm64 build) and SOW-0047 (abort spec drift).
- Removed the gate from `raw_robustness_unix_tests.rs`, `raw_robustness_windows_tests.rs`, `robustness_unix_test.go`, `robustness_windows_test.go`, `test_robustness.c`, `test_win_robustness.c`.
- Applied the fix in `transport/posix.rs` and `transport/windows.rs`.
- Added `test_receive_with_abort_waits_past_poll_slice` and `test_receive_abort_after_poll_slice_reports_aborted` to `transport/posix_tests.rs`.
- Added the slow-handler interop pass (fixtures and both scripts).

## Validation

Acceptance criteria evidence:

- Linux: `test_robust_uds_call_honors_timeout_longer_than_100ms` ok.
- Windows (`costa-win11`): `test_robust_win_pipe_call_honors_timeout_longer_than_100ms` ok.
- Abort still interrupts promptly: `test_receive_abort_after_poll_slice_reports_aborted` returns `Aborted` between 250 ms and 2000 ms after a 300 ms abort; existing `test_client_abort_unblocks_call` (Windows) and the lookup abort tests pass.
- Cross-language slow handler: added and passing (below).

Tests or equivalent validation:

- Fix reverted temporarily (reverse patch, then reapplied): both new transport tests FAILED; the slow-handler interop pass failed exactly the three Rust-client pairs (`Rust/Rust`, `C/Rust`, `Go/Rust`) with `cgroups call failed: Timeout`. With the fix: all pass.
- Linux `cargo test --lib`: 385 passed, 10 failed. The 10 failures are the ungated reproduction tests owned by open SOWs: K1 x2 and N5 (SOW-0044), K3 x2, K4 x2, N2, N3 (SOW-0043), N11 (SOW-0042). No other failures.
- Windows `cargo test --lib`: 266 passed, 7 failed, all owned by open SOWs: K2, N5 (SOW-0044), K3 x2, K4, N2, N3 (SOW-0043).
- C Linux `test_robustness`: 13 passed, 8 failed; the 8 failures are K3 x2, K4 x2, N2, N3, N5, N11 (open SOWs). The C N1 guard ("300ms handler succeeds with a 2000ms timeout") passes.
- Linux ctest `test_service_interop` and `test_service_shm_interop`: passed (18 pairs each, including 9 with a 300 ms handler).
- Windows ctest `test_service_win_interop` and `test_service_win_shm_interop`: passed.
- `cargo fmt --check` clean; CI clippy gates (`-D clippy::correctness -D clippy::suspicious`, all targets) pass; the only clippy warnings in touched files are pre-existing (`interop_service.rs` `type_complexity`, `needless_update`). `gofmt -l` clean; `go vet` clean for Go fixtures (unix and `GOOS=windows`) and `service/raw` (linux and windows).

Real-use evidence:

- The interop binaries are real client/server processes exchanging the cgroups snapshot service across C, Rust and Go on Linux and Windows with a slow handler.

Reviewer findings:

- No external reviewers were requested. Six read-only analyses informed the design; their findings beyond N1 are mapped under Followup.

Same-failure search:

- Every Rust path using a sliced wait was checked: UDS and named-pipe receive were the only ones returning on an empty slice. Rust SHM receive and the server session loop loop correctly; sends have no slicing; handshakes have no timeout (SOW-0043). C and Go loop correctly.

Artifact maintenance gate:

- AGENTS.md: updated - zero-known-bug policy under project overrides; robustness test command note no longer describes a gate.
- Runtime project skills: no update; `project-netdata-vendoring` is unaffected by a receive-loop fix.
- Specs: no update; the fix makes Rust conform to `docs/level1-posix-uds.md:101-110`, `docs/level1-windows-np.md:98-106`, `docs/level2-typed-api.md:261-272`. The abort-mechanism drift is tracked in SOW-0047.
- End-user/operator docs: no update; no public API, config, or documented behavior changed.
- End-user/operator skills: no update; `docs/netipc-integrator-skill.md` describes call timeouts the fix now honors.
- SOW lifecycle: completed and moved to `done/` in the same commit as the work.

Spec update:

- None needed (see artifact gate).

Project skill update:

- None needed (see artifact gate).

End-user/operator docs update:

- None needed (see artifact gate).

End-user/operator skill update:

- None needed (see artifact gate).

Lessons:

- A reproduction test that "always runs" only in some languages hides that the same bug class may exist elsewhere: the cross-language slow-handler interop pass now covers every client/server pair, not just the language that failed.
- Read-only design analysis across all open bugs before the first fix surfaced several unrelated serious bugs (Go arm64 build, missing `CLOEXEC`, pipe squatting) that per-issue analysis would have found much later.

## Outcome

Rust clients over UDS and named pipes now honor their call timeout like C and Go. Calls with handlers slower than 100 ms succeed; an abort still ends the call within about 100 ms. All robustness reproduction tests run ungated under the zero-known-bug policy.

## Lessons Extracted

- See Validation -> Lessons.

## Followup

- Abort-signal spec drift (Rust/Go poll a flag; spec describes an fd/event): SOW-0047.
- Go linux/arm64 build failure (`syscall.SYS_POLL`): SOW-0046.
- Remaining robustness bugs and the new sibling findings: SOW-0042 (packet size, agreed-size validation, explicit-size cap), SOW-0043 (handshake, timeouts, shutdown, lifecycle races, C retry parity, NotReady parity), SOW-0044 (dead-peer detection, `CLOEXEC`, `nipc_shm_owner_alive`, error codes), SOW-0045 (pipe and SHM object security, squatting, automated test).
- Triage of older pending SOWs (0031, 0032, 0037) under the zero-known-bug policy: pending user decision in the one-issue-at-a-time sequence; no work started.

## Regression Log

None yet.
