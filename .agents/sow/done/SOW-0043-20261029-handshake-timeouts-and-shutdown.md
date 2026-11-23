# SOW-0043 - Server Handshake Bounds And Shutdown

## Status

Status: completed

Sub-state: completed 2026-10-31; K4 client deadlines continue in SOW-0051.

## Requirements

### Purpose

Keep one slow or silent client from blocking other clients or shutdown, bound the server side of every handshake, and make server stop and shutdown reliable in C, Rust and Go on POSIX and Windows.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW. The user's 2026-10-30 instruction: complete all SOWs autonomously, take the recommended option at design forks and record it, run correctness tests per milestone.

### Assistant Understanding

Facts (line numbers verified 2026-10-31 against commit `b450b40`):

- K3: the server reads HELLO inline on the accept thread with no deadline. C POSIX `nipc_uds_accept` (`src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:277-297`) calls `nipc_uds_server_handshake`, whose blocking `recv` is at `netipc_uds_handshake.c:214`; C Windows `nipc_np_accept` (`netipc_named_pipe.c:830-880`) calls `server_handshake`, whose `ReadFile` is reached through `raw_recv` (`:662`, `:282`); Rust `UdsListener::accept_with_config` (`transport/posix.rs:713`, handshake `:1345`) and `NpListener::accept_with_config` (`transport/windows.rs:1160`, handshake `:1437`); Go `Listener.AcceptWithConfig` (`transport/posix/uds_listener.go:98`, handshake `uds_handshake.go:101-116`) and the Windows listener (`transport/windows/pipe_listener.go:78`, `pipe_handshake.go:82-97`). One silent client blocks all accepts and shutdown. Reproduced in all languages on Linux and Windows (SOW-0040).
- N2: shutdown joins workers blocked in mid-message reads or response writes. C POSIX `nipc_server_destroy` joins at `netipc_service_posix_server.c:418-428` without unblocking anything (drain shuts sockets down only after its timeout, `:373-382`); Rust `server_unix.rs:115-117` and Go `server_unix.go:168` join unconditionally; C Windows `CancelSynchronousIo` is sent once (`netipc_service_win_server.c:143-155`) and joins wait `INFINITE` (`:406`, `:440`). Reproduced on all languages and platforms.
- N3: stop before run is lost because run sets the running flag unconditionally (C POSIX `netipc_service_posix_server.c:188`, C Windows `netipc_service_win_server.c:233`, Rust `server_unix.rs:29`/`server_windows.rs:23`, Go `server_unix.go:108`/`server_windows.go:158`). In C, `drain()` before `run()` destroys `sessions_lock` that `run()` then locks (`:405`, `:228`); `destroy()` before `run()` makes `run()` poll fd -1 forever (`:414`, `:191-196`); C Windows `stop()` before `run()` closes the listener and `run()` spins on `Sleep(10)` (`:352-353`, `netipc_named_pipe.c:838-839`). Reproduced everywhere.
- N4: Rust Windows `stop()` closes the stored listener handle (`service/raw/server.rs:103-118`) that `NpListener::drop` closes again (`transport/windows.rs:1201-1211`); after `ConnectNamedPipe` that handle value becomes the live session handle (`windows.rs:1168`), so `stop()` can close a session's pipe or a reused handle value.
- N6: C Windows `sctx->thread` is written after `sessions_lock` is released (`netipc_service_win_server.c:316-320`), and `CancelSynchronousIo` is one-shot, so a worker that enters a blocking call after the cancel is never released.
- N7: C POSIX drain may `shutdown()` a reused fd: it reads `s->session.fd` under `sessions_lock` (`netipc_service_posix_server.c:373-382`) but the session thread closes the fd without that lock (`netipc_service_posix_server_session.c:291`).
- N10: Go `Stop()` closes the listener (`server_unix.go:174-198`) while `Run` polls `listener.Fd()` unlocked (`server_unix.go:115`) and accepts on the fd unlocked (`uds_listener.go:98`).
- C Windows stop races: `server_wake_listener` reads `listener.pipe` unlocked while accept replaces it (`netipc_service_win_server.c:128-138`, `netipc_named_pipe.c:867`), and `stop()` plus the end of `run()` can both close the listener (`:345`, `:353`).
- C POSIX `destroy()` closes the listener before the accept loop exits (`netipc_service_posix_server.c:414-415`).
- `FlushFileBuffers` in the server-side close blocks on a client that stopped reading (`netipc_named_pipe.c:942`, Go `pipe_session.go:140`); it runs on the worker thread, so the stop path must be able to cancel it.
- Rust `run(&mut self)` holds a unique borrow (`server_unix.rs:18`, `server_windows.rs:13`), so `stop()` cannot be called from another thread through the same value; callers use `running_flag()` (42 call sites) and tests stop servers by storing `false` into it. Its meaning ("false until `run()` starts") is asserted by tests (`apps_lookup.rs:286-289`, `apps_lookup_unix_tests.rs:283-286`).

Inferences:

- Netdata's consumers are C on Linux (SOW-0044 survey, `ktsaou/netdata @ ae9113f2cc`); K3 and N2 affect them whenever a plugin connects and stalls or stops reading, and the daemon's shutdown then hangs.

Unknowns:

- None; the design forks below are decided under the user's autonomy instruction.

### Acceptance Criteria

- SOW-0040 K3, N2, N3 reproduction tests pass in C, Rust and Go on Linux and Windows.
- New tests: a silent client's slot is released at the handshake deadline; stop during a pending handshake returns promptly; over-capacity clients are refused without blocking the accept loop.
- No stop path closes a handle or fd that another thread may still use (N4, N6, N7, N10, C Windows stop races), shown by code inspection and the existing suites (Go with `-race`).
- Specs updated: server handshake deadline, worker-slot accounting, stop/shutdown guarantees.

## Analysis

Sources checked:

- SOW-0040 tests and evidence (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`); the code locations above; `docs/level1-transport.md`, `docs/level2-typed-api.md` (Shutdown section says drain/abort mechanics are implementation details).

Current state:

- The reproduction tests run ungated and fail: C POSIX four (K3 ×2, N2, N3) of the six; the two K4 checks move to SOW-0051. Windows: K3 ×2, N2, N3 here; the K4 check moves to SOW-0051.

Risks:

- Moving the handshake to the worker changes when the worker limit applies: over-capacity clients are now refused before HELLO_ACK instead of after it.
- Forced unblocking at shutdown cuts in-flight responses; this is intended for destroy/stop and must not affect drain's graceful window.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The accept loop performs per-client I/O (HELLO read, HELLO_ACK write) with no deadline, so the slowest client sets the pace for every other client and for shutdown (K3).
- Shutdown assumes workers return between messages; a worker blocked in a socket or pipe call never re-checks the flag, and nothing unblocks it (N2, N6).
- Stop is a single flag that `run()` overwrites (N3), and several stop paths close handles that other threads still use (N4, N7, N10, C Windows races, C POSIX destroy ordering).

Evidence reviewed:

- Code locations under Facts (all verified 2026-10-31); SOW-0040 reproduction tests in `tests/fixtures/c/test_robustness.c:465-700`, `test_win_robustness.c`, Rust `service/raw_robustness_unix_tests.rs`, `raw_robustness_windows_tests.rs`, Go `service/raw/robustness_unix_test.go:430-452` and `robustness_windows_test.go`.

Affected contracts and surfaces:

- L1 C API: new `nipc_uds_accept_pending` / `nipc_uds_accept_handshake` and `nipc_np_accept_pending` / `nipc_np_accept_handshake`; `nipc_uds_accept` / `nipc_np_accept` keep their signatures and gain the handshake deadline. Rust and Go L1 listeners gain the same split.
- L2 managed server in all languages: handshake on the worker, worker-slot accounting, stop/drain/destroy semantics, a stop-requested flag next to the existing running flag.
- Docs: `docs/level1-transport.md` (handshake deadline), `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level2-typed-api.md` (Shutdown, worker limit), `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- `nipc_service_posix_poll_with_shutdown` and the Rust/Go 100 ms poll slices for stop-aware waits; `wait_readable` (`netipc_named_pipe.c:300-345`) and its Rust/Go equivalents for Windows readability; `server_wake_listener` for waking `ConnectNamedPipe`; drain's existing `shutdown(SHUT_RDWR)` unblock.

Risk and blast radius:

- Every server accept and shutdown path in six implementations. Mitigation: unchanged wire protocol (handshake bytes identical), per-language reproduction tests, full interop suites, Netdata Linux targets and Windows suites.
- Performance assessment (AGENTS.md Test Cadence): established-session steady state is unchanged. The changes run at accept/handshake (once per connection), at stop/drain/destroy, and in the session thread's exit path (closing its fd under `sessions_lock`). No per-request code changes: C `server_handle_session` (`netipc_service_posix_server_session.c:93-275`) and its Rust/Go/Windows equivalents keep their loops; only their setup moves into the worker. No benchmark run needed.

Sensitive data handling plan:

- No secrets involved; Windows SIDs in evidence are written as `<owner-SID>`. Netdata evidence is cited as `ktsaou/netdata @ ae9113f2cc`. Applies to this SOW, specs, docs, the integrator skill and code comments.

Implementation plan:

1. C POSIX: L1 split (pending accept + handshake with deadline); managed server accepts raw connections, enforces the worker limit before the handshake, and runs config/SHM preparation plus the stop-aware handshake in the session thread; `stop_requested` flag honoured by `run()`; destroy signals, waits for the accept loop, closes the listener, then waits a bounded grace for workers and shuts down the sockets of the rest; session threads close their fd under `sessions_lock`.
2. C Windows: same L1 split; worker handshake; `stop_requested`; stop/drain/destroy only signal and wake (`server_wake_listener` uses the immutable pipe name); `run()` owns the listener close, destroy closes it only when `run()` never ran; `sctx->thread` set under the lock; cancellation repeats until the worker exits.
3. Rust POSIX and Windows: listener split; handshake on the worker; `stop_requested`; run exit unblocks workers (POSIX socket shutdown through a per-session guard, Windows cancellation loop); Windows `stop()` wakes the listener by connecting instead of closing handles.
4. Go POSIX and Windows: listener split; handshake on the worker goroutine; `stopRequested`; `Stop()` only signals and wakes, `Run` owns listener close; run exit unblocks workers.
5. Tests: new deadline/stop-during-handshake/over-capacity tests per language; docs.

Validation plan:

- After each language milestone: that language's unit and robustness suites on Linux (and Windows for the Windows part).
- At SOW close: full C ctest, Rust and Go suites (Go with `-race`), the four POSIX interop suites, `tests/run-linux-target.sh` on the Netdata Linux targets, the full Windows C ctest plus Rust and Go Windows suites. PSI/vmstat sampling during timing-sensitive runs.

Artifact impact plan:

- AGENTS.md: no workflow change expected; re-check at close.
- Runtime project skills: none expected (no vendoring in this SOW).
- Specs: `docs/` transport and typed-API docs as listed above.
- End-user/operator docs: same `docs/` files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (stop/shutdown guarantees, refusal behaviour).
- SOW lifecycle: split into SOW-0043, SOW-0051, SOW-0052 on 2026-10-31.

Open-source reference evidence:

- `ktsaou/netdata @ ae9113f2cc` (consumer survey recorded in SOW-0044).

Open decisions:

- None; decisions 1-5 below are assistant-applied under the user's autonomy instruction.

## Implications And Decisions

Assistant-applied decisions (2026-10-31, under the user's 2026-10-30 autonomy instruction; recommended option taken, recorded for user review):

1. Scope split (process). SOW-0043 held about twenty defects across six implementations. Split into SOW-0043 (server handshake bound and shutdown), SOW-0051 (client deadlines: K4, N8, abort during connect, attach retry, reconnect count, NOT_READY parity; the Rust client-side flush was later resolved here, decision 6) and SOW-0052 (EINTR, stale-socket race, TRUNCATED classification, SHM before HELLO). Reason: each SOW lands as one reviewable, bisectable commit (AGENTS.md: one SOW at a time).
2. K3 design: move the handshake from the accept loop to the session worker, with a fixed server handshake deadline of 5000 ms observed in 100 ms stop-aware slices. Alternative rejected: a deadline on the accept thread, which still serializes clients behind each silent one (a reconnecting silent client throttles every accept by the deadline). Classification: long-term-best. The deadline is a constant, not a config field, to keep the public config structs unchanged; Netdata clients send HELLO immediately on connect, so 5 s only bounds stalled peers.
3. Worker limit: a connection takes a worker slot from accept, including its handshake; over-capacity connections are closed before HELLO_ACK. Today they get HELLO_ACK OK and are then dropped, so the client believes it is READY and fails its first call. Refusing before HELLO_ACK makes the connect itself fail, which the client already handles by retrying on its next refresh.
4. Shutdown: stop/destroy (and Rust/Go run exit) give workers a bounded grace of 1000 ms to finish their current message, then force-unblock the rest (POSIX `shutdown(SHUT_RDWR)` on session sockets, Windows `CancelSynchronousIo` repeated until the worker exits). Drain keeps its caller-supplied graceful window. In-flight responses on stuck sessions are cut, which is what shutdown means; the spec's Shutdown section is updated to state it.
5. Stop state: a stop-requested flag set by stop/drain/destroy and never cleared, checked by `run()` before it starts; the existing running flag keeps its meaning ("the accept loop is running") because Rust exposes it publicly (`running_flag()`) and tests depend on it. Servers are single-use: `run()` after a stop returns at once.

6. Windows session close (2026-10-31, found by validation). The Rust Windows N2 test failed intermittently (1 in about 12 runs): after the stop path cancelled the worker's blocked `WriteFile`, the worker entered `session.close()`, whose `FlushFileBuffers` waits for a client that never reads. Instrumented runs on `costa-win11` showed `CancelSynchronousIo` itself blocking for the 2 s until the test dropped its client, and `DisconnectNamedPipe` from the stopper blocking for the same 2 s (a synchronous pipe handle serializes its I/O). A standalone probe showed that a plain server-side `CloseHandle` returns in 0 ms with the client not reading, and that the client still reads every message written before the close and then gets `ERROR_BROKEN_PIPE` (109), for 3×100 B, 4×8000 B and 1×60000 B messages. The flush therefore protects no data and only adds an unbounded wait. Decision: session close in C, Rust and Go closes the handle only, for both roles (the Rust client-role flush listed in SOW-0051 goes too). Classification: long-term-best; one rule in three implementations instead of a stop-only special case that would still race with a worker already inside the flush.

7. Bounded wait for a worker slot (2026-10-31, found by validation). On the `i386-musl` target, C `test_service` "Test 3k" closed a client of a one-worker server and refreshed a new one at once (`tests/fixtures/c/test_service.c:3496-3510`); the old worker had seen the hangup but its thread had not exited yet, so decision 3 refused the new connection and the client was not READY. Before this SOW the refused client got HELLO_ACK OK and was dropped later, and the C client's internal reconnect hid it. A refresh right after a close is a normal pattern (the client's own reconnect after an error does it). Decision: when all workers are busy, the accept loop waits up to 200 ms (two 100 ms poll slices; a worker whose client left exits within one) for a slot, observing stop, then refuses. Waiting costs nothing because a full server cannot serve any other connection meanwhile. Applied in C POSIX/Windows (`server_wait_for_slot_locked`), Rust (`wait_for_worker_slot` in `service/raw/common.rs`) and Go (`acquireWorkerSlot`). Classification: long-term-best.

8. Test policy (user, 2026-10-31): correctness tests take seconds, synchronize without long sleeps, and fuzz/stress/benchmarks run only before vendoring when needed. Applied to this SOW's close validation: `tests/run-linux-target.sh` now excludes `fuzz_*`, `test_stress*`, `test_lookup_scale*` and benchmark tooling from ctest, Rust skips `test_stress*`, Go skips `^TestStress`; the Windows validation runs use the same exclusions. The test-suite rework itself is SOW-0053.

Rejected (with evidence):

- Idle authenticated clients holding worker slots indefinitely: not a defect. Netdata plugins keep long-lived sessions open by design; an idle timeout would break them.

## Plan

See the implementation plan in the gate; milestones are the per-language steps 1-4, each followed by that language's tests.

## Execution Log

2026-10-31:

- C POSIX (milestone 1): `NIPC_SERVER_HANDSHAKE_TIMEOUT_MS` (`netipc_protocol.h`); L1 split `nipc_uds_accept_pending` / `nipc_uds_accept_handshake` (`netipc_uds.h`, `netipc_uds_lifecycle.c`) with `nipc_uds_wait_readable` for the deadline; `stop_requested` in `netipc_service.h`; `netipc_service_posix_server.c` run/stop/drain/destroy rewritten (stop check before and after marking running, raw accept, capacity check before the handshake, destroy waits for the accept loop before closing the listener); handshake, SHM preparation and session publish moved into `netipc_service_posix_server_session.c`; the session thread closes its fd under `sessions_lock` (N7); `nipc_service_posix_server_wait_sessions` gives a 1000 ms grace, then shuts the remaining sockets down.
- C Windows (milestone 2): `nipc_np_accept_pending` / `nipc_np_accept_handshake` (`netipc_named_pipe.h/.c`); a peer gone before HELLO keeps reporting `NIPC_NP_ERR_RECV` as before the wait was added (found by `test_named_pipe` "peer closes before HELLO"). `netipc_service_win_server.c`: run checks the stop flag, drops the wake connection, enforces capacity before the handshake, creates and stores the worker thread under the lock (N6), and owns the listener close; stop only signals and wakes; `server_wake_listener` uses only the immutable pipe name (stop race); drain/destroy use `nipc_service_win_server_wait_sessions` (grace, then `CancelSynchronousIo` repeated until every worker exits).
- Rust (milestone 3): `SERVER_HANDSHAKE_TIMEOUT_MS`; `UdsListener::accept_pending` -> `PendingConnection::handshake(.., timeout_ms, abort)` and `NpListener::accept_pending` -> `PendingPipe::handshake`; `NpListenerWaker` replaces the stored listener handle, so `stop()` never closes a handle (N4); `server_unix.rs` / `server_windows.rs` run the handshake on the worker, refuse over capacity before HELLO_ACK, and `stop_workers` applies the grace then socket shutdown (POSIX) or the `CancelSynchronousIo` loop (Windows).
- Go (milestone 4): `protocol.ServerHandshakeTimeoutMs`; `Listener.AcceptPending` -> `PendingConn.Handshake(.., timeoutMs, abortCh)` on POSIX and Windows, plus Windows `Listener.Wake`; `server.go` gains `stopChan`/`markStopped` (flag never cleared, stop channel aborts handshakes); POSIX `Stop()` no longer closes the listener (N10), Run owns it; workers run the handshake, POSIX publishes session fds for `shutdown(SHUT_RDWR)` under the lock that also guards their close; Windows workers lock their OS thread and publish a thread handle for the `CancelSynchronousIo` loop; Windows `Stop()` re-wakes the accept every 50 ms until Run unpublishes the listener.
- Tests: the capacity tests in Rust (`raw_unix_tests.rs` `test_server_rejects_session_at_worker_capacity`) and Go (`more_unix_test.go` `TestUnixServerRejectsSessionAtWorkerCapacity`) now assert that the over-capacity connect fails; new "silent client released at the handshake deadline" test in C POSIX/Windows (`test_robustness.c`, `test_win_robustness.c`), Rust POSIX/Windows and Go POSIX/Windows (one worker, a silent client, a legitimate client must be served within deadline + 4 s; POSIX also checks the server closed the silent socket). Stop during a pending handshake is covered by the existing K3 "silent client does not block shutdown" tests, whose silent client now sits in a worker's handshake.
- Validation fixes: bounded worker-slot wait before refusing (decision 7). Windows session close no longer flushes or disconnects (decision 6; `netipc_named_pipe.c` `nipc_np_close_session`, Rust `NpSession::close`, Go `Session.Close`, unused Go `flushFileBuffers` and the Rust `FlushFileBuffers` import removed). Go one-step `AcceptWithConfig` now applies `ServerHandshakeTimeoutMs`, like C `nipc_uds_accept`/`nipc_np_accept` and Rust `accept_with_config`. Go Windows `pipe_edge_test.go`: five tests closed a `rawPipePair` end that the helper's cleanup closed again; on a reused handle value this closed a Go runtime event and crashed the test binary (`runtime: setevent failed; errno=6`, seen once in the full Windows run). New `rawPipePairWithClosers` returns closers that tell the cleanup to skip.
- Docs: `docs/level1-transport.md` (accept split, server handshake deadline), `docs/level1-posix-uds.md` and `docs/level1-windows-np.md` (split API names, Windows wake rule), `docs/level2-typed-api.md` (worker handshake, refusal before HELLO_ACK, Shutdown guarantees), `docs/netipc-integrator-skill.md` (refusal, 5000 ms bound, 1000 ms stop grace); C header and Rust/Go run/stop doc comments.

## Validation

Acceptance criteria evidence:

- K3, N2, N3 reproduction tests (SOW-0040) pass in C, Rust and Go on Linux, on all Netdata Linux targets and on Windows (runs below). The only remaining robustness failures are the two K4 client-deadline tests per language (`test_robustness` / `test_win_robustness` "refresh + call with 1s timeout return within 6s", Rust `test_robust_client_bounded_when_server_never_accepts` / `_never_sends_hello_ack` and the Windows equivalent, Go `TestRobustClientBoundedWhenServerNever{Accepts,SendsHelloAck}` and `TestWinRobustClientBoundedWhenServerNeverSendsHelloAck`); K4 is client-side and is SOW-0051 scope (decision 1).
- New tests pass everywhere: silent client released at the handshake deadline (C POSIX/Windows, Rust POSIX/Windows, Go POSIX/Windows); over-capacity connect refused (Rust `test_server_rejects_session_at_worker_capacity`, Go `TestUnixServerRejectsSessionAtWorkerCapacity`); stop during a pending handshake (K3 "silent client does not block shutdown" tests, whose silent client now sits in a worker handshake).
- Handle/fd ownership (N4, N6, N7, N10, C Windows stop races): by inspection, stop paths only signal and wake; Run owns the listener close in every implementation; Rust Windows `stop()` uses `NpListenerWaker` and closes no handle; C Windows `sctx->thread` is set under `sessions_lock`; POSIX session fds are shut down and closed under the same lock. Go `-race` on Linux reports no race.
- Specs/docs updated (see artifact gate).

Tests (2026-10-31, after all validation fixes):

- Linux host: full ctest (correctness set) fails only K4 entries (`test_robustness`, and `test_service_rust` through its Rust K4 tests); Rust lib (stress skipped) 390 passed, 2 K4 failed; Go `-race` (stress skipped) only the 2 K4 tests fail; POSIX interop `interop_codec.sh`, `test_uds_interop.sh`, `test_shm_interop.sh`, `test_service_interop.sh` pass.
- Netdata Linux targets (`tests/run-linux-target.sh`, C + Rust + Go + interop): x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl fail only K4 (C ctest about 65-68 s, Rust 390 passed / 2 K4 failed, Go K4 only; all interop suites pass). armv7-gnu-time64: the first run failed broadly (23 Rust tests, 2 Go SHM tests, `test_uds_interop` "server not ready after 10s") while host memory PSI `some` avg10 reached 40-75 % (other workloads on the host); the rerun with memory `some` avg10 at most 7 % failed only K4 (Rust 390 passed / 2 K4 failed, C ctest 68 s, all interop suites pass), so the first run was a host stall, not a defect.
- Windows (`costa-win11`, MSYS2): C ctest (20 correctness tests, 85 s), only the `test_win_robustness` K4 check fails; Rust 274 passed, 1 K4 failed; Go only `TestWinRobustClientBoundedWhenServerNeverSendsHelloAck` fails; no leftover processes. The Rust Windows N2 test, intermittent before decision 6, passed 30 of 30 repeated runs.
- Timing triage: PSI and `/proc/vmstat` were sampled every 30 s during the target runs (`compact_stall` and memory `some` recorded per target).

Real-use evidence:

- The interop suites run real C, Rust and Go servers and clients against each other over UDS and SHM on every Linux target; the Windows probe (`CloseHandle` on a non-reading client) confirmed decision 6 against the real kernel behaviour.

Performance assessment:

- No established-session per-request code changed (gate). Decision 6 removes `FlushFileBuffers` from session close only; decision 7 adds a wait in the accept loop only when every worker is busy. Neither runs per request, so no benchmark run is needed.

Reviewer findings:

- No external review was run for this SOW; validation failures found by the suites (Test 3k slot race, `test_named_pipe` "peer closes before HELLO", Rust Windows N2 flush hang, Go Windows double close, Go one-step accept without deadline) were fixed and are recorded in the Execution Log and decisions 6-7.

Same-failure search:

- `FlushFileBuffers`: no remaining call in C, Rust or Go (`grep -rn FlushFileBuffers src tests` finds only comments).
- `DisconnectNamedPipe`: remaining calls are on pre-handshake refusal/error paths owned by the accept or worker thread (`netipc_service_win_server.c:170-209`, `netipc_service_win_server_session.c:334`, `netipc_named_pipe.c:861-900`, Rust `PendingPipe::drop` and listener error path, Go `pipe_listener.go:115-169`), where no other thread has I/O on the handle and no server data is pending.
- Double close of test pipe handles: the pattern exists only in Go `pipe_edge_test.go` (fixed with `rawPipePairWithClosers`); the C and Rust Windows tests have no shared pipe-pair cleanup helper.
- Stop flag overwritten by run: all six servers now check a never-cleared stop-requested flag before and after marking running.

Artifact maintenance gate:

- `AGENTS.md`: no change in this SOW; the user's test policy (decision 8) goes into Test Cadence through SOW-0053, which owns the suite rework.
- Runtime project skills: `project-netdata-vendoring` unaffected (no vendoring); its fuzz/stress pre-vendoring step is added by SOW-0053.
- Specs: `docs/level1-transport.md`, `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level2-typed-api.md` updated (accept split, 5000 ms server handshake deadline, Windows wake and close-only rules, worker handshake, 200 ms slot wait and refusal before HELLO_ACK, Shutdown guarantees). No `.agents/sow/specs/` entry needed: the public docs carry these rules.
- End-user/operator docs: the same `docs/` files plus C header and Rust/Go doc comments.
- End-user/operator skills: `docs/netipc-integrator-skill.md` updated (refusal, 5000 ms bound, 1000 ms stop grace).
- SOW lifecycle: split into SOW-0043, SOW-0051, SOW-0052; SOW-0053 created from the user's policy; SOW-0051 updated (client flush resolved here). This SOW moves to `done/` in its commit.

Lessons: see Lessons Extracted. Follow-up mapping: see Followup.

## Outcome

Completed 2026-10-31. A silent or slow client can no longer block accepts or shutdown in any of the six server implementations: the handshake runs on the worker with a 5000 ms deadline, over-capacity connections are refused before HELLO_ACK after a 200 ms slot wait, stop is never lost, stop paths no longer close handles other threads use, and shutdown force-unblocks workers after a 1000 ms grace. Windows session close no longer flushes. Client-side deadlines (K4) remain open in SOW-0051.

## Lessons Extracted

- Refusing earlier exposes client paths that the late refusal hid (Test 3k): when moving a failure earlier in a protocol, re-check callers that relied on the later one.
- On synchronous Windows pipes, `CancelSynchronousIo` and `DisconnectNamedPipe` from another thread serialize behind a blocked `FlushFileBuffers`; prove such assumptions with a standalone probe before designing a stop path around them.
- Slow suites under memory pressure produce mass timeout failures that look like regressions; sample PSI and rerun before triage (AGENTS.md Test Cadence already says so; it applied here).

## Followup

- SOW-0051 (client call deadlines) and SOW-0052 (transport hygiene): split from this SOW on 2026-10-31.
- SOW-0053 (deterministic correctness suite): created 2026-10-31 from the user's test policy; runs next.

## Regression Log

None yet.
