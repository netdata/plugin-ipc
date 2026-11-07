# SOW-0053 - Deterministic Correctness Suite

## Status

Status: completed

Sub-state: completed 2026-10-31; only the K4 tests, owned by SOW-0051, still fail.

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

Status: ready

Problem / root-cause model:

- Tests wait because the code gives them nothing to wait on: Rust and Go `run()` bind the listener internally and signal nothing, so tests poll the socket path or connect in retry loops; C `nipc_server_init` already listens, so its post-start sleeps are pure waste.
- Tests that prove timeout behaviour wait out production values (handshake 5000 ms, SHM attach retry 5000 ms, stop grace 1000 ms, slot wait 200 ms, 100 ms stop-poll slice per teardown) because those values are compile-time constants.
- Handlers sleep to hold a request in flight instead of waiting on a release signal.
- Non-correctness suites (fuzz, stress, chaos, scale, benchmark policy) are registered in the default ctest set and in the Rust/Go default test runs.
- One hidden bug: Go `waitUnixServerReady` (`src/go/pkg/netipc/service/raw/client_test.go:40-53`) probes with a 4096-byte request limit that small-limit servers reject, so it silently spends its full 2 s and returns as if ready.

Evidence reviewed (baseline 2026-10-31, Linux host, commit `2ac2011`):

- Correctness ctest set (`-j8`, fuzz/stress/scale/bench excluded): `test_service_rust` 45.9 s (serial `--test-threads=1`), `test_robustness` 24.8 s, `test_service` 23.0 s, `test_shm_rust` 10.8 s, `test_chaos` 6.5 s, service interop 4.9 s each; everything else under 3 s.
- Go `go test ./...` without stress: `service/raw` 76 s, other packages under 0.5 s. Top tests: `TestLookupProactiveRequestSplit` 14.8 s, K4 tests 6 s each (SOW-0051), `TestRobustSilentClientReleasedAtHandshakeDeadline` 5.1 s, `TestUnixShmAttachFailureFallsBackToBaseline` 5.0 s, `TestLookupLogicalLimits` 4.5 s, `TestLookupCallsReconnectForRequestCapacityGrowth` 4.2 s, `TestLookupTimeoutDuringFollowupSubcall` 4.0 s; about 200 tests at 0.2-0.3 s each (100 ms stop-poll slices at teardown).
- Per-language inventories (sleep sites and causes) summarized in Facts; C: 260 sleep calls in 27 fixtures and 11 scripts; Rust: 140; Go: about 110.
- Existing patterns: `NIPC_INTERNAL_TESTING` `*_for_tests` entrypoints (`netipc_service.h:418-440`), Windows C tests' `ready_event` (`test_win_service.c` `start_server_named`), `tests/run-extended-fuzz.sh` for long fuzz runs.

Affected contracts and surfaces:

- No wire change. One public addition: a listening signal on Go and Rust managed servers (decision 4). Internal per-instance timing values on servers and clients (defaults equal today's constants), settable only through test entrypoints: C `*_for_tests` functions under `NIPC_INTERNAL_TESTING`, Rust `pub(crate)`/`#[doc(hidden)]` setters, Go unexported fields.
- Internal ready signal in Rust and Go managed servers (listener bound and accept loop running).
- Test code in C, Rust, Go (POSIX and Windows); interop scripts and fixtures (READY printed after bind); `CMakeLists.txt` registration; `tests/run-linux-target.sh`; Windows validation scripts; new `tests/run-extended-stress.sh`.

Existing patterns to reuse:

- `NIPC_INTERNAL_TESTING` entrypoints; Windows `ready_event` and entered/release events in `test_win_service_guards.c`; `tests/run-extended-fuzz.sh` structure and `run-low-priority.sh`.

Risk and blast radius:

- Rewriting many tests can drop coverage silently. Mitigation: each rewrite keeps its assertions; line coverage per language is compared before and after (`tests/run-coverage-{c,rust,go}.sh`).
- Per-instance timing fields touch production code paths (read where the constants were read). Defaults are the existing constants, so behaviour is unchanged; interop and all suites rerun.
- Shorter test timeouts can make tests flaky under load. Mitigation: shortened values stay well above scheduling noise (handshake deadline, attach window, grace: 200-300 ms; poll slice 10 ms), and every wait that only fires on a bug keeps a generous upper bound.
- Performance assessment: the per-session receive loop reads its poll slice from the server instance instead of a constant (C `netipc_service_posix_server_session.c`, Rust `server_session_unix.rs:42,65`, Go `server_unix.go:257,279`, Windows equivalents). That is one field load per poll wakeup, not per message encode/decode, and the poll timeout value is unchanged in production. No benchmark run needed; recorded again at close with the final diff.

Sensitive data handling plan:

- No secrets involved. Applies to this SOW, AGENTS.md, the vendoring skill, docs and code comments.

Implementation plan:

1. Separation: CMake option `NETIPC_EXTENDED_TESTS` (default OFF) registers fuzz, stress, chaos, scale and benchmark-policy ctest entries; executables still build. Rust stress/scale tests `#[ignore]` with a reason naming the extended script; Go stress/scale tests behind build tag `netipc_extended`. New `tests/run-extended-stress.sh` runs them; `tests/run-extended-fuzz.sh` keeps fuzz. Drop the exclusion lists from `tests/run-linux-target.sh` and the Windows scripts.
2. Go: per-instance server timings (handshake, stop grace, slot wait, poll slice) and client SHM attach window; internal listening channel; test helpers wait on it and fail loudly; single connects; handler release channels; fix `waitUnixServerReady`.
3. Rust: same mechanisms; parallel-safe tests so ctest entries drop `--test-threads=1` where paths are unique.
4. C POSIX and Windows: `nipc_server_set_timings_for_tests` / `nipc_client_set_timings_for_tests`; delete post-start/post-join sleeps; handler entered/release; blocking EOF waits instead of sleeps.
5. Interop: fixtures print READY after bind; scripts block on READY without sleeps.
6. AGENTS.md Test Cadence and the vendoring skill state the policy; integrator skill validation commands checked.

Validation plan:

- Per milestone: the touched language's suites on Linux (Windows for Windows parts).
- At close: full correctness ctest, Rust, Go (`-race`), interop on Linux; all Netdata Linux targets; Windows C/Rust/Go; coverage before/after per language; each suite run twice to show stable results; extended scripts run once to prove they still work.

Artifact impact plan:

- AGENTS.md: Test Cadence gains the correctness/extended split and the commands.
- Runtime project skills: `project-netdata-vendoring` preflight runs `tests/run-extended-fuzz.sh` and `tests/run-extended-stress.sh` when the vendored change can affect them.
- Specs: none (tests and internal hooks only); confirmed at close.
- End-user/operator docs: none expected; checked at close.
- End-user/operator skills: `docs/netipc-integrator-skill.md` validation commands checked.
- SOW lifecycle: runs before SOW-0051; K4 tests stay at their current duration until SOW-0051 gives clients deadlines.

Open-source reference evidence:

- None.

Open decisions:

- None; the user set the policy and delegated the details. Mechanism choices are recorded under Implications And Decisions.

## Implications And Decisions

Assistant-applied decisions (2026-10-31, under the user's delegation of test details):

1. Timing overrides are per instance, not global, so tests running in parallel cannot affect each other, and production defaults stay the existing constants. Classification: long-term-best.
2. Non-correctness suites stay in the repository and run from explicit extended scripts (`tests/run-extended-fuzz.sh`, new `tests/run-extended-stress.sh`); default ctest, cargo and go test runs contain correctness only. Classification: long-term-best.
3. Fuzz is not correctness (user): Go fuzz files build only with the `netipc_extended` tag and Rust proptests are `#[ignore]`d; `tests/run-extended-fuzz.sh` runs them. Go stress tests move behind the same tag, Rust stress tests are `#[ignore]`d, C `test_chaos` runs its deterministic malformed-input cases by default and its randomized attacks with `--extended`.
4. Ready signal (revises the gate's "internal" plan): the Go wrapper packages and the interop fixtures (a separate Go module and separate Rust binaries) cannot reach unexported state, so the Go and Rust managed servers gain a small public listening signal (Go `Server.Listening() <-chan struct{}` on raw and the typed wrappers; Rust an equivalent on `ManagedServer` and wrappers). Rationale: C already guarantees this, because `nipc_server_init` listens before it returns; Rust and Go had no way to know when clients can connect, which also affects real callers that start a server and then spawn clients. Alternatives rejected: polling the socket from fixtures (a retry loop, the pattern this SOW removes); a public bind/run split (larger API change). Timing overrides stay internal (decision 1). Classification: long-term-best. Docs (`docs/level2-typed-api.md`) document the signal.

5. Busy named pipe (bug found while removing client retry loops): a Windows server has no free pipe instance between accepting one client and creating the next, and connects in that window failed at once with `ERROR_PIPE_BUSY` in all three clients (C `nipc_np_connect`, Rust `NpSession::connect`, Go `Connect`). The test retry loops hid it. Fix: on `ERROR_PIPE_BUSY`, wait once with `WaitNamedPipeW` for at most 1000 ms (`NIPC_NP_CONNECT_BUSY_WAIT_MS`, Rust `CONNECT_BUSY_WAIT_MS`, Go `ConnectBusyWaitMs`), then try once more. A bounded single wait keeps a refresh from blocking indefinitely; client call deadlines in general belong to SOW-0051. Reproduction tests drive the window deterministically through a test-only busy hook (C `nipc_np_set_connect_busy_hook_for_tests` under `NIPC_INTERNAL_TESTING`, Rust `#[cfg(test)] CONNECT_BUSY_HOOK`, Go unexported `connectBusyHook`): the first client holds the only instance, the server accepts only after the second client reported busy. Classification: long-term-best.
6. C reconnect drain removed: the C-only 150 ms `reconnect_drain_ms` sleep before a reconnect (ops field and its uses) is gone. Go and Rust never had it; it was a downstream port that SOW-0006 flagged as possibly hiding a race, and SOW-0043's two-poll-slice worker-slot wait now covers that race. Classification: long-term-best.
7. C listening signal: none needed. `nipc_server_init` binds the listener (POSIX `nipc_uds_listen`; Windows `nipc_np_listen` creates the first instance), so a C caller may connect as soon as init returns. Go and Rust bind inside `Run`, hence decision 4.
8. MSYS test threads (bug found while removing retries): under load, a thread that a C fixture started with `CreateThread` on the MSYS2 runtime sometimes never ran, and the process then died silently with exit status 0 (no `atexit`, no crash event). The interop retry loops had masked it as a slow server. Evidence (costa-win11, `ctest -j8` background load): with `CreateThread`, 1-5 silent server deaths per 120 starts across about 8 runs; with that one thread started by `pthread_create`, 0 in 240. Cygwin does not support threads it did not start (cygwin mailing list, 2009-10, "Thread related crash"). Fix here: `test_create_thread()` in `tests/fixtures/c/test_sync.h` (same arguments as `CreateThread`) starts test threads with `pthread_create` under `__MSYS__` and returns an event that is signaled when the thread function returns, so callers keep `WaitForSingleObject`/`CloseHandle`; every C fixture uses it. The library's own `CreateThread` use under `__MSYS__` and the C benchmark driver are tracked by SOW-0054, because the library needs real Win32 thread handles for `CancelSynchronousIo`.
9. Interop server stop is identity-checked: `tests/interop-lib.sh` used to send `taskkill //T //F` to the server's Windows PID even after the server had exited; a recorded run showed kills aimed at PIDs that no longer existed, and a reused PID would have killed an unrelated process (and `/T` its children). `interop_kill` now kills only when `/proc/PID/exename` (MSYS) or `/proc/PID/exe` (Linux) still names the started binary, and no longer uses `/T`. Found in validation: under QEMU binfmt (ARM Netdata targets) `/proc/PID/exe` names the interpreter, so the check never matched, the server was never killed and the script blocked in `wait` (armv7-gnu stalled 30 min after the first pair). The check now also accepts `argv[1]` from `/proc/PID/cmdline`, where binfmt puts the program path.

## Plan

Written at activation.

## Execution Log

2026-10-31:

- Separation (milestone 1): `NETIPC_EXTENDED_TESTS` CMake option (default OFF) gates fuzz, stress, randomized chaos, lookup-scale and benchmark-policy ctest entries; `test_chaos` runs its deterministic cases by default and its randomized attacks with `--extended`; Go fuzz files and stress tests build only with the `netipc_extended` tag (stress helpers used elsewhere moved to `large_helpers_unix_test.go`); Rust proptests and `test_stress_*` are `#[ignore]`d; `tests/run-extended-fuzz.sh` passes the tag and `--ignored`; new `tests/run-extended-stress.sh`; `tests/run-linux-target.sh` drops its exclusion lists.
- Go (milestone 2): per-instance `serverTimings` (poll slice, stop grace, handshake deadline; slot wait derived) and `Client.shmAttachTimeout`, defaults equal to the old constants; public `Listening()` on `raw.Server` and the typed wrappers (decision 4); nil-by-default test hooks `sessionExited` and `handshakeStarted`. Tests: `runServerForTest` (test timings, waits for `Listening`, fails loudly), single connects instead of retry helpers, release channels instead of handler sleeps, session-exit waits instead of post-close sleeps, SHM attach waits on a created signal, the malformed-batch SHM tests read the server's error reply, the robustness helper process reports when its handler holds a request. Hidden test defects fixed: `waitUnixServerReady` probe rejected by small-limit servers (silent 2 s waits); `TestClientRefreshFromBroken` always took its silent early return (a failed reconnect leaves NOT_FOUND, not BROKEN), so its refresh-from-BROKEN half never ran; it now reaches BROKEN through a call timeout.
- Go results (Linux host): `service/raw` 76 s -> 17 s (12 s are the two K4 tests owned by SOW-0051); `-race` clean apart from K4. Windows (`costa-win11`): `service/raw` 52 s -> 12 s (6 s K4).
- Rust (milestone 3): `ServerTimings` on `ManagedServer` (defaults equal the old constants), `RawClient::shm_attach_timeout`, public `ListeningSignal` from `listening_signal()` on the raw and typed servers (re-exported as `netipc::service::ListeningSignal`), `#[cfg(test)]` hooks for session exit and handshake start. A detached `StopHandle` backs `stop()` so a test can stop a server whose `run()` owns it (Windows needs the listener wake). Test helpers `spawn_for_test` / `RunningTestServer` / `TestEvents` replace ready-flag polling, `/proc/net/unix` polling, fixed post-start sleeps and the manual Windows wake connection. Raw listeners bind before their thread starts. Handler sleeps became release channels; post-close sleeps became session-exit waits; retry loops became single attempts. The attach-refused robustness tests keep the production 5 s attach budget, which is what they guard.
- Rust CTest gap found and fixed: the `test_service_typed_rust` filter `service::cgroups::tests::` matched no test, so the typed facade tests never ran under CTest; filters are now disjoint (`protocol::`, `transport::posix::` (all of `transport::` on Windows), `transport::shm::`, the three facade modules, `service::raw::`) and run in parallel without `--test-threads=1`.
- Rust results: Linux CTest Rust entries 46 s serial -> 6.3 s parallel, twice, only K4 failing; `cargo test --lib` 6.0 s; Windows `cargo test --lib` 6.0 s, only Windows K4 failing; Rust 1.91.0 builds; clippy CI gates clean on Linux and Windows targets.
- C (milestone 4): per-instance server `poll_ms`, `stop_grace_ms`, `handshake_ms` and client `shm_attach_timeout_ms` (defaults equal the old constants, set through `nipc_server_set_timings_for_tests`, `nipc_client_set_shm_attach_timeout_for_tests` under `NIPC_INTERNAL_TESTING`), a server test event hook (handshake started, session exited), and `tests/fixtures/c/test_sync.h` (condition-variable flags and counters, server event waits, test timings). POSIX suites converted: `test_service` 23 s -> 3.4 s, `test_robustness` 24.8 s -> 1.5 s plus K4, `test_chaos` 4.6 s -> 0.1 s, `test_multi_server` 2.0 s -> 0.22 s; the rest were already fast. Linux ctest 13.5 s total, only K4 failing. Windows suites converted the same way; Windows ctest about 75 s wall at `-j8`, only K4 failing, three runs in a row after a clean rebuild.
- Interop (milestone 4): new `tests/interop-lib.sh` (`interop_start_server` blocks on the server's READY line through process substitution; `interop_stop_server`/`interop_wait_server`); all POSIX and Windows interop scripts use it, with single client attempts. C fixtures print READY after init (decision 7); Go fixtures after `Listening()`; Rust fixtures after `listening_signal().wait()`. Windows interop costs about 0.5 s per pair, all of it process creation (per-pair timing recorded on costa-win11).
- Hidden test defects fixed in milestone 4: `test_win_service_extra.c` still referenced the removed drain field; the interop retry loops masked decisions 5 and 8.
- Accepted stimulus sleeps (they set up the condition under test, the outcome does not depend on their length): C `test_robustness` N1 300 ms handler, `test_shm_timeout` 50 ms child delay and its 1100 ms timeout measurement (it measures timeouts), `test_win_shm` 25 ms delayed send, `test_cache` 100 us reader contention, interop `HANDLER_DELAY_MS=300` slow pass (it must outlast the 100 ms client abort-poll slice), the 10 s/120 s fixture self-stop timers (safety bound only, never reached in a passing run); randomized `test_chaos --extended` keeps its sleeps (stress, not correctness); Rust N1 300 ms handler (Linux, Windows), `test_poll_fd_eintr_returns_timeout` 50 ms signal delay, transport SHM/WinSHM 20-50 ms sender delays, `test_receive_abort_after_poll_slice_reports_aborted` 300 ms and `send_response_after` delays (they must outlast the 100 ms production abort poll slice), the 2 ms cache epoch sleep, 100 us reader contention; `transport::shm::tests::test_receive_timeout` keeps its 1.1 s case because it covers the timespec seconds field.

## Validation

Acceptance criteria evidence:

- Correctness suites in seconds (Linux host): ctest 13.5-16 s (was 337 s), `cargo test --lib` 6.0 s, Go `service/raw` 17 s (was 137 s), other Go packages about 1 s each. Most of what is left is the K4 tests (6 s each, owned by SOW-0051). Netdata Linux targets: ctest about 20 s natively (x86_64-musl, i386-gnu, i386-musl), 23.5 s on armv7-gnu under QEMU. Windows: ctest about 75 s wall at `-j8` (process creation), `cargo test --lib` 6 s, Go `service/raw` 11 s.
- No sleep-based synchronization or retry loop in correctness tests: same-failure search over C, Rust and Go test code (`usleep|nanosleep|sleep|Sleep(`, `thread::sleep`, `time.Sleep(`, `servedWithin|served_within|retry`). What remains is the accepted stimulus sleeps listed in the Execution Log, sleeps inside `#[ignore]`d or `netipc_extended` stress tests, and these Go Windows stimulus sleeps: `transport/windows/shm_test.go:498,653` and `pipe_edge_test.go:201` (let a receive/accept block before close; the result is the same if close comes first), `shm_test.go:759` (spurious-wake stimulus), `pipe_integration_test.go:1061` (connect delay; the listener already exists), `service/raw/shm_windows_test.go:109` (stays idle across server poll slices), `cache_test.go:32`, `more_windows_test.go:911`, `uds_more_edge_test.go:250` (clock advance). Accepted exception: `shm_test.go:745-753` polls the receiver's shared-memory waiting flag, which no event exposes; the bound only expires on a bug.
- Timeout behaviour still proven with per-instance test timings (decision 1); assertions were kept per rewritten test.
- Non-correctness suites run only from `tests/run-extended-fuzz.sh` and `tests/run-extended-stress.sh` (decisions 2-3). The fuzz script ran once with a 2 s fuzz time: 15 passed, 0 failed. It was not run at full length, because this SOW changes no decoder; per Test Cadence it runs before vendoring when the vendored changes touch decoding.
- `tests/run-extended-stress.sh` ran once to prove the extended split: 7/7 passed (`test_stress`, `test_stress_go`, `test_stress_rust`, `test_chaos_extended`, `test_lookup_scale_interop`, `test_lookup_scale_shm_interop`, `test_windows_bench_stability_policy`), 158 s.
- AGENTS.md Test Cadence and the vendoring skill state the policy.

Tests:

- Linux host: ctest twice, only K4 failing (`test_robustness`, `test_service_rust`); `cargo test --lib` twice, 376 passed, 2 K4 failed, 20 ignored (fuzz/stress); Go twice, only the 2 K4 tests failing; POSIX interop ctests pass; `test_shm_full_backing_store` passed with `NIPC_REQUIRE_TMPFS_TEST=1` under sudo (local runs skip it without privileged namespaces; CI enforces it).
- Netdata Linux targets (`tests/run-linux-target.sh`, all 9: x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl): C, Rust, Go (gnu targets; musl targets skip pure Go by design) and interop; only the K4 tests fail. ctest 20-24 s per target. The ARM targets ran after the QEMU identity-check fix (decision 9).
- Windows (costa-win11): ctest 3 runs after a clean rebuild, each 19/20 with only K4 (`test_win_robustness`); `cargo test --lib` twice, 261 passed, 1 K4 failed, 14 ignored; Go twice, only `TestWinRobustClientBoundedWhenServerNeverSendsHelloAck` (K4).
- Busy-pipe reproduction tests (decision 5) pass in C, Rust and Go on Windows.
- Rust 1.91.0 builds on Linux and Windows (msvc); `cargo fmt --check` and the clippy CI gates are clean; gofmt and go vet clean apart from the existing `unsafe.Pointer` notes in `shm.go`.
- Coverage: Go 89.7% statements (baseline 89.8%; the uncovered new lines are the accept-retry sleep and the slot-wait timer branch, both error paths); Rust 92.21% lines (baseline 92.46% included the 20 fuzz/stress tests now `#[ignore]`d); C 91.8% lines (baseline 91.8%), every file at or above the 90% gate.

Real-use evidence: the interop scripts start real C, Rust and Go servers and clients across UDS, SHM, named pipes and WinSHM through `tests/interop-lib.sh`, on the Linux host, Netdata Linux targets and Windows.

Reviewer findings: none beyond the issues found during validation, all fixed here (decisions 5, 8, 9 and the QEMU identity check).

Performance assessment: no benchmark run needed. The only per-request change is the server idle poll timeout read from a per-instance field instead of a constant, production value unchanged (100 ms): C `netipc_service_posix_server_session.c:124,144`, `netipc_service_win_server_session.c:46,72`; Rust `server_session_unix.rs:43,66,83`, `server_session_windows.rs:38,61`; Go `server_unix.go:264,286`, `server_windows.go:374,396`. The busy-pipe wait, drain removal and listening signal run only on connect, reconnect or startup.

Artifact maintenance gate:

- `AGENTS.md`: Test Cadence rewritten into three classes (deterministic correctness, extended, performance), plus the extended scripts and `interop-lib.sh` command lines.
- Runtime project skills: `.agents/skills/project-netdata-vendoring/SKILL.md` gained the extended-suites preflight step and record line.
- Specs: no `.agents/sow/specs/` change; the readiness contract and busy-pipe wait are public behaviour and are specified in `docs/level2-typed-api.md` and `docs/level1-windows-np.md`, which are authoritative.
- End-user/operator docs: `docs/level2-typed-api.md` (Readiness section, deterministic-testing requirement), `docs/level1-windows-np.md` (busy-pipe wait).
- End-user/operator skills: `docs/netipc-integrator-skill.md` (readiness, busy wait, test guidance).
- CI: `.github/workflows/runtime-safety.yml` no longer names `test_win_stress` in the Windows ctest regex (its entry is extended-only now).
- SOW lifecycle: SOW-0054 created for the library and benchmark MSYS threads; SOW-0051 facts updated (drained-server reconnect hang in the HELLO_ACK read; the busy wait must sit inside the call deadline).

Same-failure search: see the sleep/retry search above; identity checks other than `interop-lib.sh` (`/proc/.*exe` in `tests/*.sh`): none.

## Outcome

Correctness suites in C, Rust and Go are event-synchronized and run in seconds. Fuzz, stress, randomized chaos, lookup-scale and benchmark-policy suites run only from the extended scripts. Removing the retry loops exposed and fixed three product or fixture bugs: Windows clients failing at once on a busy pipe, MSYS fixture threads started by `CreateThread` dying silently, and an interop stop that could kill an unrelated reused PID. The only correctness failures left are the K4 client-deadline tests, owned by SOW-0051.

## Lessons Extracted

- Retry loops in tests hide product bugs: the busy-pipe failure and the MSYS thread deaths were both masked as "slow server". Recorded as the Test Cadence rule against retries.
- An identity check must account for how the program is launched: under QEMU binfmt `/proc/PID/exe` names the interpreter. Recorded in the `interop_pid_runs` comment.
- A long validation run needs a stall check, not a passive wait; a 2-minute target sat hung for 30 minutes.

## Followup

- SOW-0051 (pending): client call deadlines, fixes the K4 tests.
- SOW-0054 (open): MSYS-safe thread creation in the Windows C library and the C benchmark driver.
- Deferred-items search (`defer|later|follow-up|future|TODO|pending`): every hit maps to SOW-0051 or SOW-0054 above, or to "before vendoring" extended runs, which the vendoring skill now requires.

## Regression Log

None yet.

## Regression - 2026-11-07

What broke:

- The default-CTest suites `test_service_payload_limits` and `test_service_method_limits` still synchronized with sleeps: `tests/fixtures/c/test_service_limit_helpers.h` polled server readiness with `usleep(500)` up to 2000 times and connected clients through `refresh_until_ready` (up to 200 refreshes with 10 ms sleeps). This contradicts the acceptance criterion that correctness tests synchronize on events (reported on netdata/plugin-ipc#18, review comment 4496633289).

Why validation missed it:

- The same-failure search listed the sleep sites of the files the SOW rewrote; this shared helper header was not in that set.

Repair:

- The server thread publishes `ready`, `init_ok` and `done` under the `test_sync.h` lock and broadcasts; `start_server_named` blocks on the condition variable with the `TEST_WAIT_LIMIT_MS` bound that only expires on a bug.
- `refresh_until_ready` is replaced by `refresh_ready`: the server listens before `start_server_named` returns, so one refresh connects (the handshake waits until the run loop accepts).

Validation:

- `ctest -R 'service_(payload|method)_limits'`: 2/2 passed in 0.83 s.
- Same-failure search over `tests/fixtures/c` for `usleep|Sleep(`: the remaining sites are the accepted stimulus sleeps listed in this SOW's Execution Log (`interop_service*.c` handler delay, `test_win_shm.c` delayed send, `test_shm_timeout`, `test_cache`, `test_robustness`), `Sleep(INFINITE)` in the Windows robustness child processes (killed by the test), and the stress and chaos suites.

Artifact updates:

- None beyond this section: the rule is already in AGENTS.md (Test Cadence) and `tests/fixtures/c/test_sync.h`.
