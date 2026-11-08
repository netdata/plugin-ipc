# SOW-0040 - Robustness Reproduction Tests

## Status

Status: completed

Sub-state: reproduction tests landed, gated behind `NETIPC_REPRO_KNOWN_BUGS=1`; fixes tracked in SOW-0041 to SOW-0045.

## Requirements

### Purpose

Prove, with executable tests in C, Rust, and Go on POSIX and Windows, which externally reported robustness problems (and sibling problems found by code review) are real, so the user can decide the fix plan from evidence.

### User Request

An external reporter (could not open issues on the repository) reported three problems: SHM servers never free sessions after clients leave (Rust/Go on Linux; all languages on Windows for crashed clients), a client that never sends hello blocks the server and its shutdown, and client calls hang past their timeout. The user asked: "add tests to reproduce all problems identified and any sibling issues you may find by code reviewing. Once you have verified the issues with tests, we can discuss what is going to happen next." Windows validation host: `costa-win11` (Windows 11 + MSYS2).

### Assistant Understanding

Facts:

- K1: Rust/Go POSIX SHM server sessions ignore client departure on SHM receive timeout (`src/crates/netipc/src/service/raw/server_session_unix.rs:54`, `src/go/pkg/netipc/service/raw/server_unix.go:208-209`); C checks the session socket (`src/libnetdata/netipc/src/service/netipc_service_posix_server_session.c:121-125`). Reproduced on Linux with ad-hoc programs.
- K2: Windows SHM server sessions detect only the clean-close flag (`netipc_service_win_server_session.c:47-48`, Rust `server_session_windows.rs:49`, Go `server_windows.go:250-251`).
- K3: the server hello read runs inline in the accept loop with no timeout, all languages and transports (`netipc_uds_lifecycle.c:244-248`, `netipc_uds_handshake.c:216`, `netipc_named_pipe.c:845-871`). Reproduced on Linux.
- K4: client connect and hello have no timeout, all languages and transports (`netipc_uds_handshake.c:164`, `netipc_named_pipe.c:585`). Reproduced on Linux with Go.
- N1: the Rust UDS/named-pipe receive caps each wait at 100 ms when an abort flag exists and returns `Timeout` on the first empty slice (`src/crates/netipc/src/transport/posix.rs:932-934`, `:956-961`; `transport/windows.rs:541-542`, `:631-637`). The client always passes an abort flag (`service/raw/client_call.rs:329`, `:349`).
- N2: server stop/destroy joins workers blocked in mid-message reads or response sends without unblocking them (C `netipc_service_posix_server.c:418-426`, Rust `server_unix.rs:115-117`, Go `server_unix.go:168`).
- N3: a stop issued before the server loop starts is lost (C `netipc_service_posix_server.c:188`, Rust `server_unix.rs:29`, Go `server_unix.go:108`, Windows equivalents).
- N5: the Windows SHM client never notices a crashed server (`netipc_win_shm.c:893-906`).
- N11: the auto-detected UDS packet size equals `SO_SNDBUF` (C `netipc_uds.c:24-34`, Rust `transport/posix.rs:792-811`, Go `uds.go:105-112`). On Linux a SOCK_SEQPACKET datagram of exactly `SO_SNDBUF` bytes fails with EMSGSIZE (verified with a socketpair: 212992 fails, 211968 succeeds), so every chunked message (larger than `SO_SNDBUF`) fails with default configuration.
- N8: the overflow-resize retry loop grants a fresh full timeout per retry (`netipc_service_common.c:404-436`, Rust `client_call.rs:34-99`, Go `client.go:171-235`), contradicting `docs/level2-typed-api.md:261-264`.

Inferences:

- Tests must assert correct behavior and therefore fail on current code. Assertion bounds are deliberately lenient (seconds, not milliseconds) so they do not pre-decide fix design (for example, a handshake deadline versus a concurrent handshake).

Unknowns:

- None blocking test authoring. Fix design is explicitly deferred to the post-verification discussion.

### Acceptance Criteria

- Each in-scope issue has at least one test per affected language/platform that fails on current `main` for the documented reason, verified by running it (Linux locally, Windows on `costa-win11`).
- C POSIX behavior that is already correct (K1 C, crash detection on Linux) has passing guard tests.
- No test can hang the suite: every blocking scenario runs under a watchdog and cleans up its own processes and files.
- Results recorded in this SOW with commands and outcomes.

## Analysis

Sources checked:

- `docs/level1-posix-shm.md`, `docs/level1-posix-uds.md`, `docs/level1-windows-shm.md`, `docs/level1-windows-np.md`, `docs/level1-transport.md`, `docs/level2-typed-api.md`.
- C service/transport sources under `src/libnetdata/netipc/src/`, Rust under `src/crates/netipc/src/`, Go under `src/go/pkg/netipc/`.
- Existing tests: `tests/fixtures/c/test_chaos.c`, `src/crates/netipc/src/service/raw_unix_tests.rs`, `raw_windows_tests.rs`, Go `service/raw/*_test.go`, CI `.github/workflows/runtime-safety.yml`.

Current state:

- No existing test covers more SHM clients than workers, a crashed client, a silent client concurrent with a legitimate client, a hung-but-listening server, a slow handler beyond 100 ms in Rust, stop-before-run, or a worker blocked during shutdown.

Risks:

- Timing-based tests can flake on loaded CI runners; mitigated by generous bounds and watchdogs.
- Tests that fail today will break CI if committed unguarded; landing strategy is a user decision after verification.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- See Facts K1-K4, N1-N3, N5, N8. Root causes: missing peer-liveness checks on SHM wait timeouts; blocking hello I/O without deadlines on the accept thread and client; Rust receive slice treated as the overall deadline; shutdown paths that cannot unblock workers; non-sticky stop state; per-attempt timeouts in retry loops.

Evidence reviewed:

- Code references in Facts; ad-hoc Linux reproductions (C/Rust/Go silent client, Rust/Go SHM leak, Go hung-handshake); Windows ad-hoc reproductions on `costa-win11` (in progress at gate time).

Affected contracts and surfaces:

- Test sources only: new C fixtures and CMake registrations, new Rust test modules (gated by `#[cfg(test)]` in `service/raw.rs`), new Go test files. No library behavior change.

Existing patterns to reuse:

- C: `tests/fixtures/c/test_chaos.c` server thread helpers, `check()` reporting, CMake `add_test` with `TIMEOUT`.
- Rust: `TestServer`, `unique_service`, `shm_server_config`/`shm_client_config` in `raw_unix_tests.rs`; Windows helpers in `raw_windows_tests.rs`.
- Go: `startTestServer`, `testUnixShmServerConfig`, Windows `helpers_windows_test.go`.
- Crash simulation: re-exec of the test binary as a client helper (C `/proc/self/exe` or `CreateProcess`, Go helper-process pattern, Rust `current_exe()` with an env-gated helper test).

Risk and blast radius:

- Test-only. Main risk is flakiness or hung test processes; every blocking step uses a watchdog and records its own PIDs.

Sensitive data handling plan:

- No secrets or customer data involved. Test auth tokens are the existing constant test token. Durable artifacts cite only repository paths and the host alias `costa-win11` (a private lab VM name, no address).

Implementation plan:

1. Linux C fixture `tests/fixtures/c/test_robustness.c` + CMake: K1 guard (C passes), K3 block + shutdown, K4 client hang, N2, N3, N8.
2. Rust `service/raw_robustness_unix_tests.rs`: K1 (clean + crash), K3, K4, N1, N2, N3, N8.
3. Go `service/raw/robustness_unix_test.go`: K1 (clean + crash), K3, K4, N2, N3, N8.
4. Windows equivalents (C fixture, Rust `raw_robustness_windows_tests.rs`, Go `robustness_windows_test.go`): K2 (crash), K3, K4, N1 (Rust), N2, N3, N5.
5. Run all on Linux and `costa-win11`; record outcomes.

Validation plan:

- Run each new test on current code and confirm it fails for the stated reason (and guard tests pass). Run existing suites to confirm no interference.

Artifact impact plan:

- AGENTS.md: likely unaffected (no workflow change).
- Runtime project skills: likely unaffected.
- Specs: no spec change in this SOW; spec gaps (handshake timeout, Windows crash detection, stop-before-run) are recorded for the fix discussion.
- End-user/operator docs: unaffected (tests only).
- End-user/operator skills: unaffected.
- SOW lifecycle: fix work will be split into follow-up SOWs after the user's decisions.

Open-source reference evidence:

- None; only this repository was examined.

Open decisions:

- Resolved 2026-10-29; see Implications And Decisions.

## Implications And Decisions

1. 2026-10-29: user directed to write reproduction tests for all identified problems and code-review siblings first, then discuss next steps. Assertion bounds are kept lenient so they do not choose a fix design.
2. 2026-10-29, landing strategy: user chose option B - commit the tests now, skipped by default and enabled with `NETIPC_REPRO_KNOWN_BUGS=1`. Implementation detail: only tests reproducing open bugs are gated; guard tests for already-correct behavior (C K1 clean/crash, C and Go N1, N11 64 KiB control) always run. Each fix SOW removes the gate from its tests.
3. 2026-10-29, fix split: user chose option A - separate SOWs: SOW-0041 (N1 Rust timeout slice), SOW-0042 (N11 default packet size), SOW-0043 (handshake/timeout bounds and shutdown: K3, K4, N8, N2, N3 plus code-review siblings N4, N6, N7, N9, N10), SOW-0044 (dead-peer detection: K1, K2, N5).
4. 2026-10-29, Windows pipe DACL: user chose option B - its own security SOW, SOW-0045.

## Plan

See Implementation plan.

## Execution Log

### 2026-10-29

- SOW created; sibling code review produced N1-N10. N4, N6, N7, N9, N10 judged not deterministically testable without test hooks; recorded as code-review findings only. N8 needs a scripted multi-session overflow server; not covered in this pass.
- N11 discovered while writing the N2 test (a 4 MiB send failed with EMSGSIZE at the transport layer).
- Files added: `tests/fixtures/c/test_robustness.c`, `tests/fixtures/c/test_win_robustness.c`, `src/crates/netipc/src/service/raw_robustness_unix_tests.rs`, `src/crates/netipc/src/service/raw_robustness_windows_tests.rs`, `src/go/pkg/netipc/service/raw/robustness_unix_test.go`, `src/go/pkg/netipc/service/raw/robustness_windows_test.go`.
- Files modified: `CMakeLists.txt` (registers `test_robustness`, `test_win_robustness`), `raw_unix_tests.rs` / `raw_windows_tests.rs` (submodule includes).
- After decision 2: added the `NETIPC_REPRO_KNOWN_BUGS` gate to all six test files; wired `test_win_robustness` into `.github/workflows/runtime-safety.yml` (build targets and ctest filter) and `tests/run-windows-msys-validation.sh`. Linux CI has no general CTest job; Rust and Go robustness tests run inside the existing cargo/go test jobs and skip gated cases.
- Updated `AGENTS.md` project commands with the gate convention.
- Created pending SOW-0041 to SOW-0045.
- Windows binaries built with MSYS gcc must exit via `_exit`, not `ExitProcess`, or the MSYS runtime reports status 0.

## Validation

Acceptance criteria evidence (2026-10-29, current `main` 63919ec plus test-only changes). FAIL = bug reproduced; PASS on a guard = behavior already correct.

| ID | Issue | C Linux | Rust Linux | Go Linux | C Win | Rust Win | Go Win |
|---|---|---|---|---|---|---|---|
| K1 | SHM session leak, clean close | PASS (guard) | FAIL | FAIL | n/a | n/a | n/a |
| K1/K2 | SHM session leak, client crash | PASS (guard) | FAIL | FAIL | FAIL | FAIL | FAIL |
| K3 | silent client blocks others | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| K3 | silent client blocks shutdown | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| K4 | client hang, server never accepts | FAIL | FAIL | FAIL | n/a | n/a | n/a |
| K4 | client hang, no HELLO_ACK | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| N1 | timeout > 100 ms honored | PASS (guard) | FAIL (101 ms) | PASS (guard) | n/a | FAIL (97 ms) | PASS (guard) |
| N2 | shutdown blocked by non-reading client | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| N3 | stop before run lost | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| N5 | SHM client waits full timeout after server crash | FAIL (10 s) | FAIL (10 s) | FAIL (10 s) | FAIL (10 s) | FAIL (10 s) | FAIL (10 s) |
| N11 | default packet size, 512 KiB message | FAIL | FAIL | FAIL | n/a | n/a | n/a |
| N11 | control, 64 KiB packet size | PASS | PASS | PASS | n/a | n/a | n/a |

Tests or equivalent validation:

- Linux: `tests/run-low-priority.sh cargo test --manifest-path src/crates/netipc/Cargo.toml --lib robustness -- --test-threads=1`; `go test ./pkg/netipc/service/raw/ -run TestRobust -count=1 -v` (from `src/go`); C `test_robustness` built in a scratch CMake build dir.
- Windows (`costa-win11`, MSYS2): CI-equivalent CMake configure, `ctest -R test_win_robustness`; `cargo test --lib robustness -- --test-threads=1`; `go test ./pkg/netipc/service/raw/ -run TestWinRobust -count=1 -v`.
- Every failure message was checked to name the intended reason (no setup failures). `cargo fmt --check` and `gofmt -l` clean. No helper processes left on either host.

Gate behavior:

- Default (gate off): C Linux `7 passed, 0 failed` with 8 SKIP lines; Rust Linux 13 ok; Go Linux 3 pass + 10 skip; Windows ctest `test_win_robustness` passes, Rust Windows 9 ok, Go Windows 0 failures.
- `NETIPC_REPRO_KNOWN_BUGS=1`: C Linux `13 passed, 8 failed`; Windows ctest fails, Rust Windows 8 failed, Go Windows 7 failed - matching the table above.

Real-use evidence:

- Ad-hoc reproductions before the tests: Linux C/Rust/Go interop binaries with a silent Python client; Rust/Go SHM leak programs; Go hung-handshake client; Windows repro programs in `~/src/plugin-ipc-verify-20261029` on `costa-win11` (session leak, silent client, shutdown, fake server, pipe DACL).

Reviewer findings:

- No external reviewers were run (not requested). Independent read-only sub-reviews produced the K1-K5 verification and N1-N10 sibling findings; every finding used here was confirmed by a failing test or, for untested ones, by code citation only (recorded as such in SOW-0043).

Same-failure scan:

- Sibling review covered all unbounded blocking I/O, peer-death detection, shutdown, and retry-accounting paths across C, Rust and Go on both platforms (N1-N10). N11 found during test authoring; the same derivation exists in all three languages.

Sensitive data gate:

- No secrets, credentials, customer data or personal data in durable artifacts. The Windows DACL owner SID is replaced with `<owner-SID>`. `costa-win11` is a private lab host alias with no address.

Artifact maintenance gate:

- AGENTS.md: updated (project commands: `NETIPC_REPRO_KNOWN_BUGS` gate convention).
- Runtime project skills: no update; `project-netdata-vendoring` is unaffected because no library code changed.
- Specs: no update; this SOW changes tests only. Spec gaps (handshake timeouts, total call deadline, Windows crash detection, packet-size derivation, stop semantics, pipe security) are tracked in SOW-0042 to SOW-0045.
- End-user/operator docs: no update; no public API, behavior, or command changed.
- End-user/operator skills: no update; `docs/netipc-integrator-skill.md` describes public APIs and workflows, which are unchanged.
- SOW lifecycle: status `completed`, moved to `done/` in the same commit as the tests; follow-ups SOW-0041 to SOW-0045 created in `pending/`.

Specs update:

- None needed for test-only changes; spec work tracked in follow-up SOWs.

Project skills update:

- None needed; no new reusable workflow beyond the gate convention recorded in AGENTS.md.

End-user/operator docs update:

- None; tests and CI wiring only.

End-user/operator skills update:

- None; public surfaces unchanged.

Lessons:

- See Lessons Extracted.

Follow-up mapping:

- K3, K4, N2, N3, N8, N4, N6, N7, N9, N10, idle-client and over-limit-ack questions: SOW-0043.
- K1, K2, N5, misleading SHM error codes: SOW-0044.
- N1: SOW-0041. N11: SOW-0042. Windows pipe DACL: SOW-0045.
- N8 test not written here: tracked in SOW-0043 acceptance criteria.

## Outcome

Executable reproductions for every reported problem and for the testable code-review siblings now live in the C, Rust and Go suites on Linux and Windows, gated behind `NETIPC_REPRO_KNOWN_BUGS=1`, with always-on guards for behavior that is already correct.

Correction (2026-11-07, SOW-0070): the exception is N8 (overflow-resize retry budget), which has no reproduction here; its test was tracked in SOW-0043's acceptance criteria (see Followup).

## Lessons Extracted

- Existing suites missed these bugs because SHM tests never exceeded the worker count, timeout tests used sub-100 ms timeouts or aborts, and no test held a silent or hung peer while a second client connected.
- Tests for blocking bugs must run every blocking step under a watchdog and release the blocker before joining, or a regression hangs the suite.
- Windows test binaries built with MSYS gcc must exit with `_exit`; `ExitProcess` reports status 0 to MSYS/CTest.
- Auto-detecting a SOCK_SEQPACKET packet size from `SO_SNDBUF` is not safe on Linux; the maximum datagram is smaller than the buffer.

## Followup

- SOW-0041, SOW-0042, SOW-0043, SOW-0044, SOW-0045 (all in `pending/`).

## Regression Log

None yet.
