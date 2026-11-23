# SOW-0051 - Client Call Deadlines

## Status

Status: completed

Sub-state: implemented and validated on Linux, the Netdata Linux targets and Windows.

## Requirements

### Purpose

Bound every client connect, handshake and retry by the caller's call deadline and abort signal, and make the C, Rust and Go clients agree on retry and not-ready behaviour.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (handshake/timeout bounds and shutdown among them). SOW-0043 was split again on 2026-10-31 (assistant process decision, recorded in SOW-0043) so each SOW lands as one reviewable, bisectable commit; this SOW carries the client-side items.

### Assistant Understanding

Facts (line numbers verified 2026-10-31 against commit `b450b40`):

- K4: client connect and HELLO_ACK read have no deadline. C `nipc_uds_connect` `connect()` `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:325`, `nipc_uds_client_handshake` recv `netipc_uds_handshake.c:162`; Windows `nipc_np_connect` CreateFileW `netipc_named_pipe.c:903`, `client_handshake` `raw_recv` `:585`; Rust `posix.rs:1155`/`:1082` (connect), `:1214` (recv), `windows.rs:733` (CreateFileW), `:1313` (recv); Go `uds_handshake.go:27` (connect), `:54` (recv), `pipe_session.go:159`, `pipe_handshake.go:40`. `refresh()` takes no timeout (C `netipc_service.h:191`, Rust `client.rs:296`, Go `client.go:84`). Reproduced in all languages and platforms by the SOW-0040 tests (`test_client_bounded_when_server_never_accepts`, `test_client_bounded_when_server_never_sends_hello_ack` and their Rust/Go/Windows equivalents).
- N8: non-timeout failures and overflow-resize retries grant a fresh full timeout per attempt (C `netipc_service_common.c:355-435`, Rust `client_call.rs:14-108`, Go `client.go:158-242`), contrary to the deadline text in `docs/level2-typed-api.md:266-271`. Not yet covered by a test.
- Found by SOW-0053: a client that reconnects to a drained or stopped server whose listener is still open blocks forever in the HELLO_ACK read (seen as a C `test_service` drain hang; the test now holds its sessions open across the drain). Same root cause as K4; the deadline must cover it.
- SOW-0053 added a bounded `ERROR_PIPE_BUSY` wait to the Windows connects (`WaitNamedPipeW`, at most 1000 ms, then one more `CreateFileW`; C `open_client_pipe` in `netipc_named_pipe.c`, Rust `open_client_pipe` in `windows.rs`, Go `openClientPipe` in `pipe_session.go`). The call deadline must also bound that wait.
- Client abort is observed only while waiting for a response; connect and HELLO_ACK waits, including the reconnects a call performs internally, ignore it (C `netipc_uds_handshake.c:162`, `netipc_named_pipe.c:585`; Rust `posix.rs:1214`, `windows.rs:1313`; Go `framing/handshake.go:148`). Abort is checked only before an attempt (`netipc_service_common.c:325`, `client_call.rs:118`).
- The 5 s SHM attach retry ignores the caller's timeout (C `netipc_service_posix_client_connect.c:123`, `netipc_service_win_client_connect.c:126`; Rust `client_unix.rs:27-28`; Go `client.go:11`, `client_unix.go:94`).
- C reconnects up to 20 times inside one call with a drain sleep (`netipc_service_posix_client_connect.c:171-185`, constants `netipc_service_posix_internal.h:13-16`, sleep `netipc_service_common.c:386-387`; Windows `netipc_service_win_client_connect.c:179-187`) while Rust and Go reconnect once as the spec says (`docs/level2-typed-api.md:242-244`; `client_call.rs:57`, `client.go:195`).
- A call on a non-READY client returns `NOT_READY` in C (`netipc_service_common.c:358-361`) but `BadLayout` in Rust (`client_call.rs:21-23`) and Go (`client.go:159-163`).
- Resolved by SOW-0043 (2026-10-31): Rust Windows session close called `FlushFileBuffers` for both roles, so a client close blocked on a server that stopped reading. SOW-0043 removed the flush and `DisconnectNamedPipe` from session close in C, Rust and Go (close now only closes the handle), because the server-side flush blocked shutdown in a way no other thread can release.

Inferences:

- Netdata's C clients call with a 5 s timeout; a server that accepts but never answers HELLO today blocks them indefinitely.

Unknowns:

- None that investigation cannot resolve; design choices are made at activation.

### Acceptance Criteria

- The SOW-0040 K4 tests pass in all languages on Linux and Windows.
- New tests prove the whole call (connect, handshake, attach, retries) is bounded by one deadline and observes abort.
- C, Rust and Go agree on reconnect count and the not-ready error.
- `docs/level2-typed-api.md` states the total call deadline semantics.

## Analysis

Sources checked:

- SOW-0040 evidence and tests; SOW-0043 facts (verified 2026-10-31).

Current state:

- The K4 reproduction tests run ungated and fail until this SOW lands.

Risks:

- Changing retry counts and error codes is a cross-language behaviour change; interop suites and Netdata callers must be checked.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Only the response wait had a deadline and an abort check. Connect, the HELLO_ACK read, the Windows busy-pipe wait and the SHM attach retry ran with their own fixed bounds or none, and every retry inside a call restarted the timeout. A server that accepts but never answers therefore holds a client forever (K4), and a call can run far past its timeout (N8). C additionally reconnected up to 20 times with 5 ms drain sleeps, unlike the spec and Rust/Go.

Evidence reviewed:

- The locations in Facts (re-verified at activation against `2a5888b`), `docs/level2-typed-api.md` section 5, `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, and the SOW-0040 K4 tests in all languages.
- Netdata consumers (`ktsaou/netdata @ ae9113f2cc`): no Rust or Go NetIPC consumers outside the vendored copies; the C callers (apps.plugin, ebpf.plugin, network-viewer, cgroups tests) ignore the `refresh()` return and set `call_timeout_ms` explicitly, so the new not-ready error and the bounded refresh do not change their behaviour beyond ending the K4 hang.

Affected contracts and surfaces:

- L1: new bounded connects (C `nipc_uds_connect_timeout`, `nipc_np_connect_timeout`; Rust `UdsSession::connect_with_timeout`, `NpSession::connect_with_timeout`; Go `posix.ConnectTimeout`, `windows.ConnectTimeout`); existing connects become wrappers with no deadline.
- L2: one deadline per call and per `refresh()`; one reconnect per failed call in C; not-ready error in Rust (`NipcError::NotReady`) and Go (`protocol.ErrNotReady`).
- Windows servers: the reject paths close the pipe without `DisconnectNamedPipe`.
- Docs: `docs/level2-typed-api.md`, `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- The receive-with-deadline-and-abort helpers (C `nipc_uds_receive_timeout`/`nipc_np_receive_timeout`, Rust `ReceiveWait`, Go `receiveWait`) for connect and HELLO_ACK waits.

Risk and blast radius:

- Every client call and refresh in all languages. Error-code change for calls on a non-READY client in Rust and Go. Cross-language interop is unaffected: the wire format does not change.

Performance assessment:

- Per-request code on an established session gains one monotonic clock read when a call starts and one when the attempt computes the time left: C `netipc_service_common.c:351` (`do_raw_call`) and `:407`/`:492` (`begin_op`); Rust `client_call.rs:32` and `:120`; Go `client.go:217` and `:306`, `increment.go:97`. These are vDSO clock reads (tens of nanoseconds) against microsecond-scale round trips, and the Go and Rust receive paths already read the clock to build their deadlines. The change does touch the measured path, so it joins the single benchmark run before vendoring that the user's instruction prescribes (tracked in SOW-0050); no separate run is made here.

Sensitive data handling plan:

- No secrets involved. Netdata evidence cites the repository and commit only. Applies to this SOW, the docs, the integrator skill and code comments.

Implementation plan:

1. C transport bounded connects and HELLO_ACK waits; L2 op deadline, single reconnect, attach clamp.
2. Rust, then Go, the same.
3. Windows server reject paths: close without disconnect, in C, Rust and Go.
4. Tests in every language and platform: reconnect bounded by the call deadline, abort ends a refresh waiting for HELLO_ACK.
5. Docs and integrator skill.

Validation plan:

- Linux full ctest (C, Rust, Go, interop), `cargo test`, `go test`, Windows ctest plus Rust and Go suites, the 9 Netdata Linux targets, rustfmt, CI clippy gate, gofmt, go vet; new tests shown to fail on the previous code.

Artifact impact plan:

- AGENTS.md: no workflow change.
- Runtime project skills: the vendoring skill's workflow is unchanged.
- Specs: `docs/` L1 and L2 specs updated.
- End-user/operator docs: same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: split from SOW-0043; benchmark tracked in SOW-0050.

Open-source reference evidence:

- None.

Open decisions:

- None; the decisions below were assistant-applied under the user's autonomy instruction, as long-term-best options.

## Implications And Decisions

1. One deadline per call or refresh covers connect, handshake, SHM attach, every reconnect and every overflow retry; abort is observed in every wait. `refresh()` uses the resolved context call timeout (no API change) and does not connect while an abort is pending. Reason: the spec already promises a bounded call, and a caller loop must not stall on a silent server.
2. One reconnect per failed call, as the spec says. The C 20-try loop, its 5 ms drain sleeps and the `CLIENT_CALL_RECONNECT_*` constants and ops `sleep_ms`/`reconnect_retry_interval_ms` are removed. Reason: parity with Rust and Go and the spec; the loop only hid server drain races.
3. Not-ready error: Rust `NipcError::NotReady` ("client not ready"), Go `protocol.ErrNotReady`; C keeps `NIPC_ERR_NOT_READY`. Reason: `BadLayout` misreported a lifecycle state as a protocol error; no Rust/Go consumers exist downstream.
4. New L1 bounded connects; the old functions become wrappers with no deadline. Reason: keeps the L1 API compatible.
5. POSIX connect waits in `SO_SNDTIMEO` slices of 100 ms (Linux EAGAIN = full backlog; EINTR/EALREADY retry; EISCONN = success; timeout reset to 0 afterwards); HELLO_ACK uses a readable-wait before recv.
6. Windows busy-pipe wait is bounded by min(1000 ms, deadline) in `WaitNamedPipeW` slices of 100 ms with abort checks; `ERROR_SEM_TIMEOUT` continues the loop. The HELLO_ACK wait treats a disconnected pipe as readable, because a rejection reply may be pending.
7. SHM attach waits at most min(shm attach timeout, time left). When the call deadline ends the wait first, or abort is raised, SHM stays enabled (the connect returns disconnected) instead of being blacklisted. Reason: the deadline, not SHM, failed.
8. Bug found and fixed (zero-known-bug policy): Windows servers called `DisconnectNamedPipe` before `CloseHandle` on handshake and accept failures, which discards a rejection reply the client has not read; the old blocking client read masked the race. Fixed in C (`netipc_named_pipe.c`, three sites), Rust (`windows.rs`: `PendingPipe` drop and the replacement-instance failure path) and Go (`pipe_listener.go`, four sites). A probe on the Windows host showed data written before a server `CloseHandle` stays readable.
9. Remaining time rounds up (Rust `as_micros().div_ceil(1000)`, Go `(µs+999)/1000`), so a transport wait never ends before the call deadline and the call reports the timeout rather than a disconnect.

## Plan

As in the Pre-Implementation Gate implementation plan; executed in that order.

## Execution Log

### 2026-10-31

- C: `netipc_uds_lifecycle.c` (`connect_bounded`, `nipc_uds_connect_timeout`), `netipc_uds_handshake.c` (HELLO_ACK readable-wait), `netipc_named_pipe.c` (bounded busy wait, HELLO_ACK wait, reject without disconnect); service `op_deadline_ms`, `begin_op`, `op_remaining_ms`, single reconnect, attach clamp on POSIX and Windows.
- Rust: `client.rs`, `client_call.rs`, `client_unix.rs`, `client_windows.rs`, `posix.rs`, `windows.rs`; `NipcError::NotReady`.
- Go: `posix/uds_session.go`, `posix/uds_handshake.go`, `windows/pipe.go`, `windows/pipe_session.go`, `windows/pipe_handshake.go`, `windows/pipe_listener.go`, `service/raw/client*.go`, lookups, increment, string reverse, snapshot; `protocol.ErrNotReady`.
- Tests: C `test_robustness.c`, `test_win_robustness.c`, `test_win_service_extra.c`; Rust `raw_robustness_unix_tests.rs`, `raw_robustness_windows_tests.rs`; Go `robustness_unix_test.go`, `robustness_windows_test.go`; not-ready expectations updated across the suites.

## Validation

Acceptance criteria evidence:

- K4 tests pass in all languages on Linux and Windows (C `test_robustness`/`test_win_robustness`, Rust `test_robust_*client_bounded*`, Go `TestRobustClientBounded*`, `TestWinRobustClientBoundedWhenServerNeverSendsHelloAck`).
- New tests: `test_call_reconnect_bounded_by_call_deadline` and `test_abort_ends_refresh_waiting_for_hello_ack` (C POSIX and Windows), Rust `test_robust_call_reconnect_bounded_by_call_deadline`, `test_robust_abort_ends_refresh_waiting_for_hello_ack` and Windows equivalents, Go `TestRobustCallReconnectBoundedByCallDeadline`, `TestRobustAbortEndsRefreshWaitingForHelloAck` and Windows equivalents. The reconnect case returns the timeout error after about 1000 ms; the abort case returns within one 100 ms slice.
- Proof against the old code: the new C, Rust and Go Linux tests, run on the `2a5888b` sources in a scratch copy, all fail: the call blocks past 6 s, and the refresh does not end within 2 s of the abort. With this SOW they pass.
- Reconnect count and not-ready error agree across C, Rust and Go (decisions 2 and 3); `docs/level2-typed-api.md` states the total-call deadline.

Tests or equivalent validation:

- Linux: full ctest 32/32 in 4 s (C, Rust, Go, interop); `cargo test --lib` 380 passed; `go test ./...` all packages ok.
- Windows (costa-win11, MSYS2): ctest 20/20; Rust `cargo test --lib` 264 passed; Go all packages ok.
- Netdata Linux targets: see target results below.
- `cargo fmt --check` clean; CI clippy gate (`-D clippy::correctness -D clippy::suspicious`) passes; the remaining clippy warnings are on lines that predate this SOW. gofmt clean; `go vet` clean on Linux; on Windows only the existing `unsafe.Pointer` notes in `transport/windows/shm*.go`.

Target results:

- `tests/run-linux-target.sh` with C, Rust, Go and interop: x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu and aarch64-musl all pass.
- The first aarch64-gnu run failed time-bounded tests: Rust wedged-peer timeout, abort and lookup-timeout tests; Go crash-detection tests; interop servers not READY in time. While it ran, host memory pressure (`/proc/pressure/memory` some avg60) rose from about 0 to 46-64% and `compact_stall` kept climbing; it fell to 0.23 avg10 afterwards. The single rerun at low pressure (avg60 8.8) passed every suite: Rust 380 passed, Go, C and interop ok. The cause of the pressure was not established, so it is not attributed to any other workload.

Real-use evidence:

- The interop suites in ctest drive real C, Rust and Go servers and clients across all language pairs through the changed connect paths.

Reviewer findings:

- No external review was requested for this SOW; the cross-language comparison against the Rust reference was done per file during implementation.

Same-failure search:

- Searched every client wait for missing deadlines: connect, HELLO_ACK, busy-pipe, SHM attach (all bounded now). Searched every `DisconnectNamedPipe` call in the three languages: the only remaining one is in the Go test helper `pipe_edge_test.go`, which tests the disconnect error path on purpose. Searched every not-ready `BadLayout` expectation in the Rust and Go tests on both platforms.
- Found while validating, outside this SOW's scope: Go `GOOS=darwin` fails on `syscall.MSG_NOSIGNAL` and `GOOS=freebsd` fails on Linux-only SHM types in `service/raw`; both are existing and tracked in SOW-0049 and SOW-0048.

Sensitive data gate:

- This SOW, the docs, the integrator skill and the code comments contain no secrets, credentials, tokens, customer or personal data, or private endpoints. Netdata evidence cites the repository and commit only; the Windows test host is named by its lab hostname only, with no address.

Artifact maintenance gate:

- AGENTS.md: no change; no workflow or guardrail changed.
- Runtime project skills: no change; `project-netdata-vendoring` steps are unaffected.
- Specs: `docs/level2-typed-api.md` (one deadline per call and refresh, abort during connect, not-ready error), `docs/level1-posix-uds.md` (bounded connect), `docs/level1-windows-np.md` (bounded busy wait, HELLO_ACK after disconnect, reject without disconnect). `.agents/sow/specs/` holds no spec on this behaviour.
- End-user/operator docs: the same `docs/` files are the published docs.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (deadline, bounded refresh, not-ready error, busy wait bound).
- SOW lifecycle: moved pending → current → done in this commit; benchmark run tracked in SOW-0050.

## Outcome

Every client connect, handshake, attach and retry is bounded by the caller's call deadline and observes abort, in C, Rust and Go on POSIX and Windows. `refresh()` is bounded by the context call timeout. C reconnects once per failed call like Rust and Go, and all three report the same not-ready error class. A Windows race that discarded server rejection replies is fixed.

## Lessons Extracted

- A blocking read can hide a lost-data race: the Windows reject-path `DisconnectNamedPipe` bug only showed once the client waited with a deadline. When changing a wait, re-check what the peer does on its failure paths.
- Rounding the remaining time down made a wait end a fraction of a millisecond early, and the call reported a disconnect instead of a timeout. Deadline helpers must round up.

## Followup

- Benchmark run for the per-request clock reads: in the single pre-vendoring benchmark run, tracked in SOW-0050.
- Go darwin and freebsd service-layer builds: SOW-0049 and SOW-0048.

## Regression Log

None yet.
