# SOW-0070 - PR review findings before the Netdata vendoring

## Status

Status: completed

Sub-state: implemented and validated (2026-11-08), including the regressions at the end of this file (2026-11-08, 2026-11-09 and 2026-11-11). The 2026-11-11 fix (`ce6834e`) is re-vendored into netdata/netdata#24633 by Netdata commit `b53818fab6`.

## Requirements

### Purpose

Every confirmed defect reported by the automated reviewers on the source PR (netdata/plugin-ipc#18) and the vendoring PR (netdata/netdata#24633) is fixed upstream before the vendoring merges, so Netdata receives a library with zero known bugs.

### User Request

The user pointed out that both PRs carry many AI review comments and asked them to be handled. After the triage the user chose (2026-11-07): "1-6: as you recommend, 7.B, 8.B, 9.A" (decisions below).

### Assistant Understanding

Facts:

- About 170 inline comments (cubic-dev-ai, CodeRabbit) plus CodeRabbit review-body nitpicks were triaged against `9568f4e` (library sources equal to the vendored `4261919`), each against code and specs, with C/Rust/Go parity checks.
- About 12 are behavioural bugs, about 30 are valid minor defects (hardening, docs, tests, records), the rest are false positives or duplicates. None is triggered by Netdata's current call patterns.
- Two findings are regressions of completed SOWs and are handled there per AGENTS.md: SOW-0053 (default-CTest helpers still poll with sleeps, `tests/fixtures/c/test_service_limit_helpers.h:199-202,227-236`) and SOW-0067 (Rust and Go lack lock-creation tests under a restrictive umask, an acceptance criterion at SOW-0067:42).

Inferences:

- F5 and F6 change the per-request send path, and F19/F20 the receive path, so the affected benchmark rows run once after all fixes (Test Cadence).

Unknowns:

- FreeBSD 13 behaviour of `fchmod` on an unbound AF_UNIX socket (decision 7B): out of scope by decision 11 (FreeBSD 13 is end of life).

### Acceptance Criteria

- Every finding in the Findings list is fixed with a test where behaviour changes, or answered with evidence as a false positive.
- C, Rust and Go stay interoperable; the affected specs under `docs/` describe the new behaviour.
- Correctness suites pass on Linux, FreeBSD (14.3, 15.1; 13 out of scope per decision 11), macOS and Windows; extended fuzz and stress pass with the fixed `run()` helper; affected benchmark rows show no unaccepted regression.
- Every PR thread gets an individual reply and is resolved (decision 9A).

## Analysis

Sources checked:

- Review comment dumps of both PRs; code at `9568f4e`; `docs/level1-*.md`, `docs/level2-typed-api.md`, `docs/codec*.md`, `docs/netipc-integrator-skill.md`; prior SOWs named below.

Current state: see Findings.

Risks:

- F6 (deadline-aware send) and F7 (listener close order) change shutdown and send semantics in three languages; interop and robustness suites must cover them.
- F19 (MSG_TRUNC on the pending queue) and F6 touch the steady-state path; benchmark evidence required.

## Findings

IDs are GitHub review-comment ids (`pi` = netdata/plugin-ipc#18, `nd` = netdata/netdata#24633).

Service layer:

- F1 C managed server rejects every connection when `supported_profiles == 0`, the documented default (`netipc_service_posix_server_session.c:305`, `netipc_service_win_server_session.c:234`). Fix: normalize 0 to baseline at init. pi 4496633058, 4496633108.
- F2 C POSIX `nipc_server_run`/`nipc_server_drain` never close the listener, contrary to `netipc_service.h:543-547` and `docs/level2-typed-api.md:628`. nd 4496664519, 4496693511.
- F3 C POSIX run/destroy start race: run publishes `acceptor_started` after its second stop check, so destroy can tear down the mutex under a starting run (`netipc_service_posix_server.c:160-165`). pi 4496523675.
- F4 C Windows run/destroy race: both can close the listener (`netipc_service_win_server.c:150-156,235-240`). nd 4496693533 (the id first recorded, 4496693516, was deleted).
- F5 The receive budget is computed before a send that can block (C `netipc_service_common.c:351-371`, Rust `client_call.rs:122-138`, Go `client.go:302-327`). pi 4496523808, 4496523698, 4496523689.
- F6 The typed-call send is not bounded by the call deadline or abort, contrary to `docs/level2-typed-api.md:276-299`; Level 1 specs define it as blocking (`level1-posix-uds.md:316-318`, `level1-windows-np.md:194-195`). nd 4496664569, 4496664475.
- F7 Go and Rust keep the listening socket open until workers exit; C releases the endpoint lock before workers finish. pi 4496633236, nd 4496693428.
- F8 Go lookups drop the capacity-reconnect error (`apps_lookup.go:88`, `cgroups_lookup.go:159`); C and Rust propagate it. pi 4496523683, nd 4496664546, 4496664500.
- F9 SHM attach boundary: rounding plus a strict `<` blames SHM when the call deadline ended the wait (C `netipc_service_posix_client_connect.c:128-131`, Rust `client_unix.rs:41-45`, `client_windows.rs:33-36`, Go `client_unix.go:115-117`). pi 4496633368, 4496524165.
- F10 Go Windows worker runs uncancellable when `OpenThread` fails (`server_windows.go:262`). nd 4496693441.
- F11 C Windows reads `learned_*_payload_bytes` with plain loads (`netipc_service_win_server_session.c:218-219`). nd 4496664678.
- F12 C POSIX HELLO wait uses `poll_ms` instead of the remaining handshake deadline and ignores EINTR time (`netipc_service_posix_server_session.c:362,395`). pi 4496523940, 4496523948, nd 4496664669.

Transport:

- F13 Windows connect: `WaitNamedPipeW` does not reserve the instance; a second `ERROR_PIPE_BUSY` fails the connect (C `netipc_named_pipe.c:1303-1304`, Rust `windows.rs:1543-1548`, Go `pipe_session.go:240-246`; spec `level1-windows-np.md:88-93`). pi 4496633422, 4496633407, 4496523972, nd 4496664610, 4496664553, 4496664506.
- F14 Windows listener after a failed replacement instance: C closes the listener handle without clearing it (`netipc_named_pipe.c:1168-1173`); all three stop accepting. pi 4496523774.
- F15 Windows clients accept an agreed packet size above the proposal (C `netipc_named_pipe.c:924`, Rust `windows.rs:1767`, Go `framing/handshake.go:162-207`). pi 4496633131.
- F16 POSIX advertises and accepts unimplemented profile bits (`netipc_uds_internal.h:45`, Go `shm_posix.go:59`). pi 4496523816.
- F17 A mode-000 lock file left by a crash between create and `fchmod` blocks every start (C `netipc_uds_lifecycle.c:352-358`, Rust `posix.rs:557-587`, Go `uds_stale.go:45-59`). pi 4496633657, 4496633634, 4496523757, nd 4496664484.
- F18 SHM region `fchmod` result ignored (C `netipc_shm.c:503`, Rust `shm.rs:245`, Go `shm_posix.go:151`). pi 4496524069, nd 4496664801.
- F19 Pending queue stores a truncated oversized datagram that the direct path rejects (C `netipc_uds.c:276-280`, Go `uds_session.go:108`, Rust equivalent). pi 4496524110, nd 4496664629.
- F20 A receive that consumed no byte (undersized buffer, timeout or abort between the two waits) marks the session failed (C `netipc_uds_receive.c:479-492`, Rust `posix.rs:480-484`, `posix_stream.rs:50-51`, Go `uds_receive.go:166-181`). pi 4496524158, 4496633567, 4496524150, 4496633357, nd 4496664710, 4496664532.
- F21 Rust clears in-flight ids on a clean timeout or abort (`framing.rs:216-221,289`); C and Go only on disconnect. nd 4496693410.
- F22 HELLO and HELLO_ACK lengths are not checked exactly (C `netipc_uds_handshake.c:168,197,243,269`, Rust `protocol/mod.rs:547,619`, Go `frame.go:582,651`). pi 4496524189, 4496523720, nd 4496664644.
- F23 `accept` does not retry EINTR (C `netipc_uds_lifecycle.c:69-71`, Rust `posix.rs:721`, Go `uds_cloexec_accept4.go:16`). nd 4496664573.
- F24 C connect and listen dereference NULL `config`/`out` (`netipc_uds_lifecycle.c:387,608`, `netipc_uds_handshake.c:130`). pi 4496523797.
- F25 Windows connect opens the pipe before checking abort/deadline (C `netipc_named_pipe.c:1279`, Rust `windows.rs:1514`, Go `pipe_session.go:212`). pi 4496524288.
- F26 Go macOS accept: fork window between accept and close-on-exec (`uds_cloexec_other.go:25`). pi 4496523784, nd 4496664595.
- F27 Empty `run_dir`: Go builds `x.sock` but `/x.lock`; C and Rust build `/x.*` (`uds.go:99`, `uds_stale.go:42`). pi 4496633348, nd 4496664580.
- F28 Go SHM receive returns a zero-length message instead of a protocol error (`shm.go:807`; spec `level1-posix-shm.md:144-146`). nd 4496664481.
- F29 Go handler types do not state that request and builder are valid only during the call (`apps_lookup.go:9`, `cgroups_lookup.go:10`, `cgroups_snapshot.go:9`). nd CodeRabbit nitpick.

Docs:

- D1 `netipc_shm.h:2,57` say Linux only; SHM also runs on FreeBSD. nd 4496664784.
- D2 Packet-size comments describe SEQPACKET only; the stream transport uses a fixed 256 KiB (C `netipc_uds.h:69`, Rust `posix.rs:196,225`, Go `uds_session.go:27,39`). nd 4496664791.
- D3 `level1-posix-uds.md:71` and `netipc_uds_lifecycle.c:102` overclaim descriptor-relative steps. pi 4496524021.
- D4 `netipc-integrator-skill.md:1313` packet-size default and cap wording. pi 4496524080.
- D5 `netipc-integrator-skill.md:45` omits that Go runs once per architecture on musl targets. pi 4496524341.
- D6 `level1-transport.md:474` lacks the POSIX `has_pending` cross-reference. pi 4496524316.
- D7 `level1-windows-shm.md:304` omits the live-pipe recovery case. pi 4496633308.
- D8 Lookup-scale interop moves to the extended suites (decision 8B). pi 4496633517.

Tests, scripts, build:

- T1 `run()` in `tests/run-extended-fuzz.sh:44-52` (and the asan, tsan, valgrind and go-race scripts) reads `$?` after `!` and always returns 0; rerun extended fuzz. pi 4496633181.
- T2 Stale `GoVersionOverride = "1.26.4"` in three `osv-scanner.toml`. pi 4496523872.
- T3 POSIX CTest registration skips `transport::framing::tests` (`CMakeLists.txt:492`). pi 4496633664.
- T4 Go bench SHM rows silently run baseline where Go SHM is a stub (`bench/drivers/go/main.go:471-488`). pi 4496633389.
- T5 Bench drivers accept a non-positive duration (Go `bench/drivers/go/main.go:508`, C driver). pi 4496524013.
- T6 Rust Windows robustness test calls `ConnectNamedPipe` outside the watchdog (`raw_robustness_windows_tests.rs:420`); check C and Go fixtures. pi 4496633193.
- T7 Rust stream test fixtures assume one read returns a whole HELLO (`posix_tests.rs:99,3337`, `raw_unix_tests.rs:569`). pi 4496633700, 4496633400, nd 4496664698.
- T8 Go `monotime_test.go:25` is timing-sensitive. nd 4496664493.
- T9 Go `shm_more_edge_test.go:487` unreadable-file case fails as root. nd 4496664513.
- T10 Go `uds_more_edge_test.go:1128` uses `info` before checking `err`. nd 4496664767.
- T11 Go `uds_bind_darwin_test.go:24` leaks the listener on early failure. nd 4496664776.

Records (corrections only where a record is factually wrong; history is not rewritten):

- R1 SOW-0040:234 Outcome omits the N8 exception (mapped to SOW-0043). pi 4496524039.
- R2 SOW-0046:7 omits the Go-once-per-architecture note. pi 4496524094.
- R3 SOW-0063:88 gate line superseded by decision 6C. pi 4496524126.
- R4 SOW-0060 (pending) duplication figure differs from SOW-0057:140; re-measure at activation. pi 4496633499.
- R5 SOW-0015:1095 Outcome compares a with-tests baseline to a production-only result. pi 4496633534.
- R6 SOW-0037:7 links `current/SOW-0046`, now in `done/`. pi 4496633437.
- R7 SOW-0027:5 records no vendor-parity result. pi 4496633375.
- R8 SOW-0032:201 claim later disproven by SOW-0067; SOW-0032:222 wording. pi 4496633599, 4496633607.
- R9 SOW-0021:1362 closure condition omits the adversarial-matrix gate. pi 4496633243.
- R10 SOW-0069 (pending) plan lacks the Windows host and durable host metadata. pi 4496633213, 4496633223.
- R11 SOW-0065 (pending) facts mix languages. pi 4496633320.
- R12 SOW-0050:68,72,110 candidate commit, gate evidence and open-decision lines are stale. pi 4496633173, 4496633279, 4496633462.

Regressions handled in their own SOWs: SOW-0053 (pi 4496633289) and SOW-0067 (pi 4496524331).

FreeBSD 13 (decision 7B): CodeRabbit claims listener creation fails on FreeBSD 13 (nd 4496693466, review summary).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Each finding above names its root cause with file:line evidence; the triage read the code, the spec and the other two languages for every comment.

Evidence reviewed:

- Code at `9568f4e`; specs listed in Analysis; SOW-0043, SOW-0044, SOW-0051, SOW-0053, SOW-0062, SOW-0064, SOW-0066, SOW-0067 for the designs these fixes touch.

Affected contracts and surfaces:

- Level 1 POSIX and Windows transports (send/receive, handshake, connect, accept, listener, lock, SHM region); Level 2 managed server lifecycle and client call deadline in C, Rust and Go; specs `level1-posix-uds.md`, `level1-windows-np.md`, `level1-transport.md`, `level1-windows-shm.md`, `level2-typed-api.md`; `docs/netipc-integrator-skill.md`; test scripts, CTest registration, OSV config, benchmark drivers, `AGENTS.md` and `.github/workflows/macos.yml` (D8).

Existing patterns to reuse:

- SOW-0062 progress send (`send_with_progress_loop`, POLLOUT wait) for the POSIX deadline-aware send; SOW-0064 overlapped Named Pipe client handle for the Windows send; the Windows server's interlocked accept-loop state as the model for F3/F4; Rust `ensure_lookup_request_capacity` error handling as the model for F8; the SOW-0067 child-process umask helpers for the lock tests.

Risk and blast radius:

- Shutdown ordering (F2, F3, F4, F7) and send deadlines (F6) affect every server and client; covered by the robustness, interop and service suites on every platform. Wire format is unchanged except stricter handshake validation (F15, F16, F22), which rejects only non-conforming peers.

Sensitive data handling plan:

- Evidence is file:line, comment ids and test names; no hosts beyond role names, no tokens, no workstation paths in SOWs, specs, docs, skills, instructions or code comments.

Implementation plan:

1. Regressions first, each in its own commit: SOW-0053 (event-based readiness in the limit helpers), SOW-0067 (Rust and Go lock-creation umask tests).
2. Test scripts and build: T1 to T5 (rerun extended fuzz after T1).
3. POSIX transport in C, Rust, Go: F16, F17, F18, F19, F20, F21, F22, F23, F24, F26, F27, F28, D1 to D3.
4. Windows transport in C, Rust, Go: F13, F14, F15, F25, D7.
5. Service layer: F1, F2, F3, F4, F7, F8, F9, F10, F11, F12, F29.
6. Deadline-aware send (F5, F6) in all languages and both platforms, with spec revisions.
7. Tests T6 to T11; docs D4 to D6, D8; records R1 to R12.
8. FreeBSD 13 validation: dropped by decision 11 (end of life).
9. Full correctness matrix, extended suites, benchmarks for the send/receive paths.
10. Re-vendor under SOW-0050, push both PRs, reply to and resolve every thread (decision 9A).

Validation plan:

- A failing test before each behavioural fix where a deterministic test is possible; CTest, `cargo test`, `go test` on Linux; FreeBSD 14.3/15.1 (13 dropped, decision 11), macOS and Windows suites; all interop scripts; extended fuzz and stress; POSIX and Windows benchmark rows for the send/receive paths compared with `4261919`.

Artifact impact plan:

- AGENTS.md: D8 moves lookup-scale to the extended suites and updates the macOS command list.
- Runtime project skills: none expected; re-check at close.
- Specs: the `docs/` files named under Affected contracts.
- End-user/operator docs: `docs/netipc-integrator-skill.md` (D4, D5, and any API-visible change such as F27).
- End-user/operator skills: same file.
- SOW lifecycle: SOW-0050 paused; SOW-0053 and SOW-0067 reopened as regressions; R4, R10, R11 edit pending SOWs.

Open-source reference evidence:

- None; all evidence is in this repository.

Open decisions:

- Resolved (2026-11-07), see Implications And Decisions. Decision 7B was replaced by decision 11.

## Implications And Decisions

User decisions, 2026-11-07 ("1-6: as you recommend, 7.B, 8.B, 9.A"):

1. A: one SOW fixes every confirmed finding (bugs, hardening, docs, tests, record corrections); completed SOW records are corrected only where factually wrong.
2. A: the typed-call send honours the call deadline and abort in C, Rust and Go on POSIX and Windows; the Level 1 specs are revised. Benchmarks run once after all fixes.
3. A: the managed server closes the listening socket when the accept loop exits and keeps the endpoint lock until workers finish, in all three languages.
4. A: a Windows client keeps retrying `ERROR_PIPE_BUSY` until the busy deadline, in all three languages; `level1-windows-np.md` changes.
5. A: after a failed replacement pipe instance, the Windows server disconnects that client and keeps the instance as its listener, in all three languages.
6. A: an empty `run_dir` is rejected in all three languages.
7. B: test FreeBSD 13 on a FreeBSD 13 host and add a fallback if listener creation fails there.
8. B: lookup-scale interop moves to the extended suites, including on macOS.
9. A: after the fixes land, every PR thread gets an individual reply (fix or false-positive evidence) and is resolved.

Follow-up decisions, 2026-11-07:

10. T2 (user: "we need to be aligned with netdata on this"): `GoVersionOverride` follows the Go patch release Netdata ships. Netdata keeps `go 1.27.0` in `src/go/go.mod` and builds with go1.27.3 (netdata/netdata @ 42313ad410, `packaging/check-for-go-toolchain.sh:136-184`); Netdata runs no Go stdlib vulnerability scanner. The three `osv-scanner.toml` files now set `1.27.3` and name that pin as their source; the vendoring preflight skill checks the alignment. Deleting them was considered and rejected: OSV would then scan the 1.27.0 stdlib, which neither CI nor Netdata ships, and `supply-chain-security.yml` fails on any finding.
11. Decision 7B revised to out of scope (user: "freebsd 13.x is EOL since April. We don't need to support that", then "A"). Netdata's `.github/data/distros.yml:517-521` (netdata/netdata @ 42313ad410) still lists `freebsd 13-STABLE` as Community support, its only FreeBSD entry, so the list looks stale; FreeBSD is validated on 14.3 and 15.1. The CodeRabbit thread gets this evidence in its reply.
12. F17 revised after the first CI run on `6b14803` (see Regression - 2026-11-08): flawfinder and Semgrep flag the path-based `chmod` of the lock repair. Options were to annotate the line, to prevent the mode-000 file, or to remove the repair. User chose prevention ("B"): a missing lock is created under a temporary name, made 0600 and hard-linked into place. A file already at mode 000 is no longer repaired.

## Plan

See Implementation plan.

## Execution Log

### 2026-11-07

- Triage of both PRs' review comments; decisions recorded.
- T1 (`run()` exit code in five scripts), T3 (Rust transport CTest filter), T4/T5 (bench driver SHM gate and duration parsing) done.
- F16 (explicit profile mask), F17 (mode-000 lock repair), F18 (checked SHM `fchmod`), F24/F27 (NULL params, empty `run_dir`) done in C, Rust and Go, each with a reproduction test where one is possible (F18 is hardening: no portable way to make `fchmod` fail on an owned file).
- F19: the SEQPACKET pending-queue drain now peeks each datagram's full length (`MSG_PEEK|MSG_TRUNC`, Linux 3.4+) and stores it whole, so a queued oversized datagram reaches the receive path as a direct receive sees it and is rejected the same way. Only the drain runs the extra peek; the drain runs only while a send waits for buffer space. Tests: C `test_pending_oversized_datagram_rejected`, Rust `test_pending_oversized_datagram_rejected`, Go `TestPendingOversizedDatagramRejected`; each failed before the fix (pending receive returned OK).
- F20: a receive buffer smaller than a header is `BAD_PARAM` before any byte is read (both transports). On the stream baseline the receive tracks whether any byte of the message was taken (C `session_read_exact` `consumed`, Rust `read_exact_pending` `consumed`, Go `streamConsumed`); a failure that took none (a timeout or abort firing between the outer and inner waits) no longer marks the session failed. Tests: C `test_receive_undersized_buffer_keeps_session`, Rust `test_receive_undersized_buffer_keeps_session`, Go `TestReceiveUndersizedBufferKeepsSession`; each failed before the fix. The between-waits timing variant (deadline expiring or abort firing after the outer wait succeeded and before the first read) has no deterministic trigger without a wait-injection hook, so the `consumed` tracking is covered by the existing stream receive suites (any regression that loses `consumed` on a taken byte fails the mid-message failure tests) but its no-byte branch has no dedicated test.
- Linux runs after F19/F20: CTest 32/32, `cargo test` 408 passed, `go test ./...` ok; Rust `cargo check --tests --target x86_64-unknown-freebsd` and Go `vet` for freebsd/darwin clean.
- F21: Rust framing clears the client's in-flight ids only when the failed receive means the peer is gone (`FramingError::is_recv_failure`: POSIX `Recv`, Windows `Recv`/`Disconnected`), as C and Go do; a timeout or abort keeps them, so a late response is still accepted. Test: Rust `test_receive_timeout_keeps_inflight` (failed before: late response rejected with an unknown id).
- F22: HELLO must carry exactly 44 payload bytes and HELLO_ACK exactly 48, by both `payload_len` and packet length, in C POSIX, C Windows, Rust (shared framing, POSIX and Windows) and Go (shared framing, POSIX and Windows). The checks run after the layout decode, so a future layout is still reported as incompatible. Tests: C `test_connect_malformed_hello_ack` (two new cases) and `test_accept_malformed_hello_payload` (two new cases), Rust `test_handshake_payload_length_exact`, Go `TestHandshakePayloadLengthExact`; each failed before the fix. C Windows `test_named_pipe.c` gains the two HELLO_ACK cases (run on the Windows host).
- F23: C and Rust retry `accept` on EINTR (C `accept_cloexec`, Rust `accept_pending` through `retry_eintr`). Go needs no change: its runtime installs every signal handler with `SA_RESTART` (go1.27.2 `runtime/os_linux.go:480`, `os_darwin.go:397`, `os_freebsd_amd64.go:15`), so the kernel restarts `accept`. Spec `level1-posix-uds.md` "Interrupted system calls" now covers accept and the Go behaviour. Tests: C `test_eintr_retried` injects EINTR into the server's first `accept4` through a new `--wrap=accept4` (failed before: connect failed); Rust `test_eintr_retried` injects through `eintr_inject::ACCEPT` (the injection exists only on the new retry path, so the C test is the reproduction).
- F26: Go on platforms without `accept4` (macOS) makes the listener non-blocking and runs `accept` plus close-on-exec under `syscall.ForkLock`, waiting for clients with `poll` outside the lock, as Go's `net` package does; the accepted socket is set back to blocking (BSD sockets inherit `O_NONBLOCK`). `TestListenerAndSessionSocketsAreCloexec` also requires a blocking accepted socket. The fork race itself has no deterministic test; the macOS CI run executes this path.
- F28: Go SHM receive (POSIX and Windows) reports a zero-length slot as `ErrShmBadHeader`/`ErrWinShmBadHeader`, as C and Rust do; the POSIX Go client maps it to `ErrTruncated`, as C and the Windows Go client already did, so the caller-visible error is unchanged. Tests: `TestShmChaosForgedLength` asserted the old behaviour and now requires the error (failed before the fix); Windows `TestWinShmReceiveRejectsZeroLength` (run on the Windows host).
- D1 `netipc_shm.h` says Linux and FreeBSD. D2 packet-size comments in C, Rust and Go name both transports' limits. D3 the macOS bind text in `level1-posix-uds.md` and the C, Rust and Go comments now say `fchmodat` and `open` resolve the path (no symlink following, owner check after open) and only later steps are descriptor-relative.
- Linux full run after F19-F28: CTest 32/32, `cargo test` 411 passed, `go test ./...` ok.
- F13 + F25 (decision 4A): Windows connect checks abort and deadline before opening the pipe (F25), and after each successful `WaitNamedPipeW` retries `CreateFileW`, waiting again on `ERROR_PIPE_BUSY` until the busy bound ends (F13), in C, Rust and Go. New test hook (C `nipc_np_set_connect_waited_hook_for_tests`, Rust `CONNECT_WAITED_HOOK`, Go `connectWaitedHook`) runs after a successful wait so a test client takes the freed instance first. Tests: C `test_connect_retries_stolen_instance`, Rust `test_connect_retries_stolen_instance`, Go `TestPipeConnectRetriesStolenInstance` (run on the Windows host). Spec `level1-windows-np.md` busy-connect and bounded-connect paragraphs updated.
- F14 (decision 5A): when creating the next pipe instance fails, accept disconnects the client and keeps its instance as the listener's (C, Rust, Go), so the next accept listens on it again; the service accept loops already retry. New test hook fails replacement creation (C `nipc_np_fail_next_instances_for_tests`, Rust `FAIL_NEXT_INSTANCES`, Go `failNextInstances`). Tests: C `test_accept_survives_failed_instance`, Rust `test_accept_survives_failed_instance`, Go `TestPipeAcceptSurvivesFailedInstance` (Windows host). Spec `level1-windows-np.md` accept paragraph updated.
- F15: Windows clients reject an agreed packet size above their proposal or not above the header, as POSIX clients do. C Windows client handshake; Rust Windows `client_handshake` (the too-small case now reports `Protocol`, as C, Go and Rust POSIX do, instead of `Handshake`); Go moves the check from the POSIX wrapper into the shared `framing.ClientHandshake`, covering both transports. Tests: Go `TestClientRejectsAgreedPacketSizeOutOfRange` (failed before the fix), C Windows `test_client_handshake_rejections` two new cases, Rust `test_connect_rejects_agreed_packet_size_out_of_range` (sharing a new `fake_hello_ack_server` helper with the selected-profile test). Spec `level1-wire-envelope.md` states the client rule for every transport.
- D7: `level1-windows-shm.md` attach paragraph now separates the cases: a broken pipe, call deadline or abort keeps SHM enabled; an attach failure or attach timeout with the pipe connected disables SHM and reconnects baseline-only, as C `netipc_service_win_client_connect.c:159-186` and the Rust and Go clients do.
- Windows host run after F13-F15: CTest found two failures, both fixed. C `test_zero_chunk_budget_rejected` relied on a client accepting a 32-byte agreed packet size, which F15 now rejects; it now connects normally and forges the session's packet size to reach the send guard, as the POSIX test does with a socketpair. Rust `test_connect_retries_stolen_instance` failed under CTest only: Rust runs tests in parallel and the hooks were process-wide, so another test's connect or accept could fire or consume them (`FAIL_NEXT_INSTANCES` was global). The connect hooks now get the pipe name and each test ignores other pipes, the three hook tests share a test lock, and the instance-failure counter is a per-listener test field. Rerun: C `test_named_pipe` 326/0, `test_win_service` 123/0, Rust `transport::windows` 5 runs x 34 passed, Go Windows transport and framing ok.
- F1: C POSIX and Windows managed servers normalize `supported_profiles == 0` (the documented default) to baseline at init; the accept path masks profiles and rejected every client. Rust and Go leave the mask to the transport, which already maps 0 to baseline. Tests: C `test_server_zero_profiles_selects_baseline` (POSIX, failed before the fix) and the Windows counterpart in `test_win_service.c` (passed on the Windows host).
- F2 + F7 (decision 3A): a POSIX listener can stop listening (close the socket, unlink its file) while keeping the endpoint lock (new C `nipc_uds_stop_listening()`, Rust `UdsListener::stop_listening()`, Go `Listener.StopListening()`). Every server's run closes the listening socket when its accept loop exits; the lock is released only after the workers are joined (C drain and destroy release it after joining, Rust drops the listener after `stop_workers`, Go's deferred `Close` runs after `stopSessionWorkers`). C drain previously never closed the listener and C destroy released the lock before joining workers; Rust and Go kept the socket open until workers exited, so a client reconnecting during shutdown waited out its call timeout on a socket nobody accepted. Windows already closed the listening instance first, and its pipe name stays owned while session instances exist. Tests: C `test_server_shutdown_order`, Rust `test_robust_shutdown_closes_socket_before_endpoint`, Go `TestRobustShutdownClosesSocketBeforeEndpoint`; all failed before the fix. Specs `level1-posix-uds.md` (endpoint lock) and `level2-typed-api.md` (shutdown) updated.
- F3 + F4: run and drain/destroy now meet through a store-then-load handshake that needs no lock: run publishes the active loop and then checks the stop; a stop publishes itself and then checks the loop (C POSIX sequentially consistent atomics, C Windows interlocked full barriers). Either the stop waits for the loop, or run sees the stop and returns without touching the session lock (POSIX) or the listener (Windows). The race needs destroy to land between run's checks and its first lock, and its effect is undefined behaviour without a reliable symptom, so it has no deterministic test; the fix holds by construction. Rust and Go own the listener inside run and are not affected.
- F8: Go lookups now return the capacity reconnect's own error when the item fits the client's limit, as C and Rust do; only an item above the client's limit falls back (apps: overflow, cgroups: oversized marker). Test `TestLookupCapacityReconnectFailureReported` (a reconnect to a socket that never answers must report the lookup timeout; a reconnect to a vanished endpoint must report overflow, not a later "not ready"); failed before the fix.
- F9: the SHM attach wait treats a tie between the call's remaining time and the attach timeout as call-bound (`<=`) in C, Rust and Go, POSIX and Windows. Rust and Go round the remaining time up, so the tie means the call deadline comes first; C takes both values as whole milliseconds on one clock, so a tie ends both waits together. Either way SHM is no longer disabled for it. The boundary depends on sub-millisecond timing, so there is no deterministic test.
- F10: a Go Windows worker whose thread handle cannot be opened is refused before HELLO_ACK, as at capacity, because stop could not cancel its blocking pipe I/O. `openWorkerThread` is a test seam. Test `TestWinRobustWorkerWithoutThreadHandleRefused` (Windows host).
- F11: C Windows reads the learned payload limits with interlocked access in the accept path and in the growth loops.
- F12: the C POSIX HELLO wait is bounded by one monotonic handshake deadline: time lost to EINTR counts, the last poll slice ends at the deadline, and the handshake read that follows gets the remaining time instead of one poll slice. Rust and Go already pass the whole handshake timeout to one stop-aware call. Timing-only effect; no deterministic test.
- F29: the Go raw and typed handler types state that the request and builder are valid only during the call.
- F5 + F6 (decision 2A): a client send now honours the call deadline and abort, and the response wait gets only what the send left of the budget. New Level 1 entry points: C `nipc_uds_send_timeout` / `nipc_np_send_timeout`, Rust `UdsSession::send_with_control` / `NpSession::send_with_control`, Go `Session.SendTimeout` (POSIX and Windows); the old sends call them with no deadline, so server sessions and other callers are unchanged. POSIX: a bounded client send uses the existing non-blocking progress loop (it also drains the peer's bytes, as a pipelined send does), polling with the time left; C polls the abort pipe, Rust and Go recheck the flag at least every 100 ms. Windows: the posted overlapped write is collected without waiting first (`GetOverlappedResult(..., FALSE)`), then waited for with the time left (C waits on the write and abort events, Rust and Go use slices of at most 100 ms); the progress loop checks the same wait. A send that ends on the deadline or abort breaks the session (POSIX shutdown, Windows cancel and close) and fails every in-flight request, since part of the message may be on the wire; Level 2 already disconnects after a timeout or abort. The SHM sends never wait for the peer and are unchanged. The Go session keeps the wait by value so a send allocates nothing. Tests: C `test_client_send_bounded_by_deadline_and_abort` (POSIX `test_service.c`, Windows `test_win_service.c`), Rust `test_robust_send_bounded_by_deadline_and_abort` / `test_win_robust_send_bounded_by_deadline_and_abort`, Go `TestRobustSendBoundedByDeadlineAndAbort` / `TestWinRobustSendBoundedByDeadlineAndAbort`: a server completes the handshake and never reads, the client sends an 8 MiB apps lookup and must return the timeout (200 ms) or, aborted once the server sees the request start, the abort, within 5 s. POSIX mutation checks (send called without deadline and abort): C, Rust and Go each hung until the server let go (10 s) and failed. Specs `level1-posix-uds.md` (Who, Deadline and abort), `level1-windows-np.md` (Who, Deadline and abort, Failure), `level2-typed-api.md` (deadline and abort paragraphs) and `level1-transport.md` (chunked send) updated.
- Windows host after F1-F12 and F5/F6: C `test_named_pipe` 326/0, `test_win_service` 137/0 (both new bounded-send cases pass), Rust `transport::windows` plus Windows robustness 48 passed, Go Windows transport, framing and `TestWinRobust*` ok. Linux: CTest 32/32, `cargo test` 412 passed, `go test ./...` ok.
- Windows mutation checks (each fix reverted on the host, the matching tests run, the file restored and its checksum compared with the local copy): C F6, F13, F14, F15; Rust F6, F13, F14, F15; Go F6, F13, F14, F10. Every test failed with its fix reverted.
- Test defect found by the F15 mutation: both handshake rejection tables in C `test_named_pipe.c` passed `NIPC_NP_OK` as the "alternate" result and the check accepted it, so every row passed even when the handshake succeeded. `handshake_err_matches` now treats `NIPC_NP_OK` as "no alternate"; all 326 checks still pass, so no handshake bug was hidden, and the F15 and F22 rows now fail when their checks are removed.
- T6: every server-side `ConnectNamedPipe` in the Windows robustness fixtures is bounded: Rust runs it inside the 5 s `within` watchdog and asserts it connected; C runs it on a thread with a 5 s wait and exits the test process with a FATAL line on timeout; Go's two helper `wait()` methods select on `testWaitLimit`. A missing client now fails the test instead of hanging the suite.
- T7: Rust stream fixtures that decode the HELLO read until the whole HELLO is in (`posix_tests.rs` `recv_hello`, eight call sites; the raw HELLO-ACK server in `raw_unix_tests.rs`). The C (`test_service.c`, `test_uds.c`) and Go (`more_unix_test.go`) sites only wait for the HELLO to start and never decode it, so a short read cannot change their outcome; no change there.
- T8: Go `monotime_test.go` brackets `After(d)` between two `Now()` reads, so the check no longer depends on scheduling.
- T9: Go `shm_more_edge_test.go` writes the unreadable region file with no header magic, so both outcomes (open refused for non-root, no magic for root) report "invalid, removed", as the Rust twin does; no euid check needed.
- T10: Go `uds_more_edge_test.go` checks `err` before using `info`. T11: Go `uds_bind_darwin_test.go` closes the listener through `t.Cleanup` (Close is idempotent).
- D4: the integrator skill says the stream packet size defaults to 256 KiB and a smaller configured size still applies (`nipc_uds_packet_size`: a configured value below the limit wins). D5: it states that the Go suites run on the glibc targets and armv6 musl, and the other musl targets skip them (`tests/run-linux-target.sh` `run_go`). D6: `level1-transport.md` POSIX wait-object bullet names the `has_pending` check in each language.
- D8 (decision 8B): the macOS workflow and the AGENTS.md macOS commands no longer run `tests/test_lookup_scale_interop.sh`; it already runs under CTest only with `NETIPC_EXTENDED_TESTS=ON`, so `tests/run-extended-stress.sh` (regex `test_lookup_scale.*`) covers it on Linux, FreeBSD and macOS before vendoring.
- Spec notes for F16, F17, F27 in `level1-posix-uds.md`: the profile set a POSIX endpoint offers and accepts, the mode-000 lock repair, and the empty `run_dir` rejection.
- Linux full run after T6-T11, D4-D8 and the spec notes: CTest 32/32, `cargo test` 412 passed, `go test ./...` ok, interop codec 18, UDS 9, SHM 18, service 9 and cache interop pass.
- R1-R12 record corrections, each a dated "Correction (2026-11-07, SOW-0070)" note that keeps the original text: R1 SOW-0040 Outcome names N8 as the exception; R2 SOW-0046 status states Go ran once per architecture and the CI matrix had not run at close; R3 SOW-0063 gate discloses the 6C public Go API change; R4 jscpd 5.3.1 re-run at `7b73adc` on `posix.rs`, `windows.rs`, `framing.rs` (4289 lines) gives 29 clones, 309 lines (7.2%), so SOW-0057's 338 is corrected and SOW-0060's 309 stands; R5 SOW-0015 Outcome is a production-only rebaseline, not a fall; R6 SOW-0037 links `done/SOW-0046`; R7 SOW-0027 records that no vendor-parity result exists and points at SOW-0050's diffs; R8 SOW-0032 same-failure search and Outcome corrected for the umask (SOW-0067); R9 SOW-0021 closure condition includes the adversarial-matrix gaps; R10 SOW-0069 plan adds durable host metadata in both generators and the Windows host A/B and SHM stability analysis; R11 SOW-0065 facts separate Go's stitched-buffer allocation from Rust and C, which stitch into client-owned buffers (Rust `cgroups_lookup.rs:224-229`, C `netipc_service_cgroups_lookup.c:440-452`); R12 SOW-0050 gate names the vendored commits, the preflight evidence and the resolved push decision.
- Windows host full run after T6 and the other changes: CTest 20/20, Rust `cargo test --lib` 280 passed, Go `./...` ok (`service/raw` 37.6 s).
- Extended suites on Linux: `tests/run-extended-fuzz.sh` 15 passed (including Rust proptest, 100000 cases), `tests/run-extended-stress.sh` 7/7.
- FreeBSD 14.3 and 15.1: `cargo test` 411 passed, Go `./...` and `GOARCH=386` ok; CTest failed `test_uds` on two F22 cases ("HELLO_ACK packet with bytes after its payload rejected", "HELLO packet with bytes after its payload rejected"). Test defect, not a library bug: the stream baseline has no packet boundary, so the handshake reads exactly the header plus `payload_len` and the extra bytes stay in the stream as the next packet. The two cases now run only on SEQPACKET (`#if !TEST_UDS_STREAM`); the Rust and Go F22 tests check the parser on whole packets and need no change. Rerun: `test_uds` 303/0 on both FreeBSD hosts, Linux `test_uds*` 4/4. Spec `level1-wire-envelope.md` now states the exact HELLO/HELLO_ACK payload lengths and the per-transport packet rule.
- T2: the three `osv-scanner.toml` files set `GoVersionOverride = "1.27.3"` (decision 10); `project-netdata-vendoring` preflight step 8 checks the `go` directive and the override against Netdata.
- Overlapped Named Pipe I/O passes no byte-count pointer (pi 4496633078): C `overlapped_recv`/`start_overlapped_send`, Rust `overlapped_read`/`start_overlapped_write`, Go `pipe_overlapped.go` read and write now pass NULL, as the Win32 `ReadFile`/`WriteFile` docs require for asynchronous calls; the count already came from `GetOverlappedResult`, so behaviour is unchanged. The write start returned while the write could be pending, leaving the pointer to a dead local. Cross-checks: Rust Windows clippy (only pre-existing lints), Go `GOOS=windows go vet` (only the pre-existing `shm.go` unsafe.Pointer notes). Not yet built or run on the Windows host (busy benchmarking). nd 4496664636 is a duplicate of F18.
- Handoff (2026-11-07, session restart):
  - Benchmarks (decision 2A) were started on frozen copies of the tree before the overlapped-I/O change: POSIX `tests/run-posix-bench.sh` into the scratch CSV `abfull-s70-posix.csv`, Windows `tests/run-windows-bench.sh` into a scratch CSV on the Windows host, to compare with the `4261919` runs of 2026-11-05 (`abfull-4261919-{posix,windows}.csv`). Both were children of the session; if the restart killed them, rerun both once (the overlapped change does not affect what they measure: it changes one argument).
  - Next: Windows host build and suites for the overlapped change (CTest, `cargo test`, `go test`) after the benchmark; benchmark comparison; replies for all 168 open bot threads (118 on plugin-ipc#18, 50 on netdata#24633; an earlier count of 149 read only the first page of plugin-ipc threads), drafted in the scratch file `thread-replies.json`: 82 fixed, 18 corrected, 3 duplicates of fixed findings, 64 rejected with evidence, 1 out of scope, from the Findings and the triage notes (false positives and duplicates) in the scratch file `triage-notes.md`; fill Validation, Outcome, Lessons, Followup; commit SOW-0070; re-vendor (SOW-0050); ask the user before pushing.
- After the restart: `cargo fmt` rewrapped one call in `transport/posix.rs`; `cargo fmt --check` passes with Rust 1.91.0 and stable, `cargo +1.91.0 check --all-targets` is clean, `cargo clippy --all-targets` reports exactly the warnings of HEAD (no new one), `gofmt -l` is empty for the three Go modules, `go vet` passes for linux, freebsd and darwin. The Windows benchmark survived the restart; the POSIX one did not and was restarted from the same frozen tree.

## Validation

Acceptance criteria evidence:

- Every finding is fixed or answered: F1-F29, D1-D8, T1-T11 and R1-R12 are each recorded in the Execution Log with the change, the test and the spec or record touched; the FreeBSD 13 item is out of scope (decision 11). The 168 open review threads (118 on netdata/plugin-ipc#18, 50 on netdata/netdata#24633) each have an individual reply, posted and resolved on netdata/plugin-ipc#18 at `6b14803`; the 50 on netdata/netdata#24633 wait for the re-vendored code under SOW-0050: 82 fixed, 18 corrected, 3 duplicates of fixed findings, 64 rejected with file or spec evidence, 1 out of scope.
- Interoperability: C/Rust/Go interop suites pass on Linux (codec 18, UDS 9, SHM 18, service 9, cache) and through CTest on FreeBSD and Windows. Specs updated: `level1-posix-uds.md`, `level1-windows-np.md`, `level1-transport.md`, `level1-windows-shm.md`, `level1-wire-envelope.md`, `level2-typed-api.md`.
- Correctness: Linux CTest 32/32, `cargo test` 412, `go test ./...` ok; Windows CTest 20/20, Rust 280, Go ok; FreeBSD 14.3 and 15.1 CTest (after the F22 test fix) with `test_uds` 303/0, `cargo test` 411, Go and Go 386 ok; extended fuzz 15/15 and stress 7/7. macOS: `macOS stream baseline (C, Rust, Go, interop)` passed on `6b14803`.
- Benchmarks (bounded send runs per request on established sessions, so the full suites ran once after all fixes, against the `4261919` runs on the same hosts):
  - POSIX: 345 rows, median ratio 1.00; lookup, snapshot-baseline and pipeline rows within 0.87-1.05. The run exits non-zero on 26 floor rows, the same 26 rows the `4261919` run missed; floor rebaselining for this host is tracked by `pending/SOW-0069`. The nine worst rows (0.45-0.74, all SHM or ping-pong, which also reached 1.65 upward) were rerun as six interleaved A/B rounds of both builds: ratios 0.93-1.07, each row's range overlapping the baseline range. No regression.
  - Windows: 194 rows comparable, median ratio 1.00, worst 0.82 (within the 0.58-1.42 spread earlier sequential runs showed on unchanged rows). The run exits non-zero because five max-rate `shm-ping-pong` rows with C or Rust servers failed the sample-stability check: their samples are bimodal (about 1.0M at 1 us p50 and 2.3-2.9M at 0.4 us p50; the upper mode matches the baseline), and this SOW changes no per-request Windows SHM code (C and Rust untouched, Go adds one length check on receive; bench driver changes are argument parsing only). Three interleaved A/B rounds of both builds on the six `shm-ping-pong` rows with C or Rust servers: each build lost 4 of 18 row-runs to the same stability check, and median ratios were 0.91-1.08 with overlapping ranges. No regression; the instability belongs to the host and is part of the stability analysis in `pending/SOW-0069`.

Tests or equivalent validation:

- A reproduction test that failed before its fix exists for every behavioural finding where a deterministic trigger exists (Execution Log, per finding). Mutation checks on the Windows host and POSIX confirmed the F6, F10, F13, F14 and F15 tests fail with their fix reverted; the F15 mutation exposed and fixed a defect in the C handshake rejection tables.
- No deterministic test, by construction or timing (each recorded with its reason): F3/F4 (undefined behaviour inside a narrow race; fixed by a store-then-load handshake), F9 and F12 (sub-millisecond timing), F18 (`fchmod` cannot be made to fail on an owned file), F20's between-waits branch, F26's fork race (macOS CI runs the path).
- Tooling: `cargo fmt --check` with Rust 1.91.0 and stable, `cargo +1.91.0 check --all-targets`, clippy (no warning beyond HEAD), Rust Windows clippy, `gofmt`, `go vet` for linux, freebsd, darwin and windows.
- Overlapped Named Pipe I/O (NULL byte-count pointer in C, Rust and Go) rebuilt and run on the Windows host with the final tree: CTest 20/20, Rust 280 passed, Go all packages ok (`transport/windows` and `service/raw` included).

Real-use evidence:

- The library has no standalone runnable product; its real use is Netdata. The re-vendored Netdata build and its NetIPC consumer tests run under SOW-0050 after this commit.

Reviewer findings:

- The findings of the two PRs' automated reviewers are this SOW's input; each is fixed or answered with evidence (Findings, Execution Log, drafted replies). Two triage results were wrong and corrected here: T2 (deleting the OSV overrides would have scanned a stdlib no build ships; decision 10) and the FreeBSD `test_uds` F22 cases (stream transport has no packet boundary).

Same-failure search:

- Every finding was checked in the other two languages and on the other platform during triage; parity fixes are recorded per finding. The overlapped byte-count pointer (pi 4496633078) was searched across C, Rust and Go read and write paths and fixed in all six sites. The handshake-table false pass (`NIPC_NP_OK` as alternate) was searched in both C rejection tables.

Artifact maintenance gate:

- `AGENTS.md`: macOS command list no longer runs lookup-scale interop; it is an extended suite (D8).
- Runtime project skills: `project-netdata-vendoring` preflight checks the Go `go` directive and the OSV `GoVersionOverride` against Netdata (decision 10).
- Specs: the `docs/` files listed above; no `.agents/sow/specs/` entry, since the public specs carry every rule changed here.
- End-user/operator docs: `docs/netipc-integrator-skill.md` (D4, D5); `docs/level1-transport.md` (D6).
- End-user/operator skills: `docs/netipc-integrator-skill.md` is that skill; updated as above.
- SOW lifecycle: SOW-0053 and SOW-0067 regressions fixed in their own commits (`616bc81`, `d223d9f`); records corrected in SOW-0015, 0021, 0027, 0032, 0037, 0040, 0046, 0050, 0057, 0063, 0065, 0069; this SOW moves to `done/`; SOW-0050 resumes for the re-vendoring.

SOW status/directory: `completed` in `done/`.

Spec update: yes, listed above.

Project skill update: yes, `project-netdata-vendoring`.

End-user/operator docs update: yes, listed above.

End-user/operator skill update: yes, `docs/netipc-integrator-skill.md`.

## Outcome

Every confirmed defect from the automated reviews of netdata/plugin-ipc#18 and netdata/netdata#24633 is fixed in C, Rust and Go on POSIX and Windows, with reproduction tests where a deterministic trigger exists, and the specs describe the new behaviour. The largest changes: a typed call's send now honours its deadline and abort; managed servers close the listening socket before their workers finish and keep the endpoint lock until they have; Windows connect survives a stolen pipe instance and the listener survives a failed replacement instance; handshakes reject non-conforming lengths, profiles and packet sizes. Correctness suites pass on Linux, Windows, FreeBSD 14.3 and 15.1; macOS is validated by CI after the push.

## Lessons Extracted

- A test table whose "alternate" result can equal success passes every row; table-driven rejection tests need an explicit "no alternate" value and a mutation check.
- Handshake length checks that rely on packet length are transport-specific: a stream has no packet boundary, so tests that add trailing bytes belong to message-oriented transports only.
- A scanner override that looks stale is not stale just because its comment says so; check what it is aligned with (here, the Go patch release Netdata ships) before removing it.
- Paginated API dumps must be checked against their total count; the first thread dump read 100 of 119 threads.
- Long benchmark runs must be detached from the agent session (`setsid nohup`), or a session restart kills them.

## Followup

- macOS validation runs in CI after the push; a failure reopens this SOW as a regression.
- Re-vendoring, PR replies and thread resolution (decision 9A) continue under SOW-0050, which records the push approval.
- Netdata's `.github/data/distros.yml` still lists FreeBSD 13-STABLE; this is a Netdata record, not a plugin-ipc item. It is stated in the CodeRabbit reply and left to Netdata maintainers.
- R4: SOW-0060's 309-line figure is confirmed by the re-measurement; no further work.
- No other deferred item: the pre-close search for defer, later, follow-up, future, TODO and pending finds only finding descriptions, pending-SOW names and the pending queue.

## Regression Log

## Regression - 2026-11-08

What broke:

- `C Static Analysis` (flawfinder level 5, CWE-362) and `Codacy Local Analysis` (Semgrep `c_race_rule-chmod`, code-scanning alert 7768) failed on `6b14803`, both on the `chmod` that F17 added at `netipc_uds_lifecycle.c:376`. They passed on `9568f4e`.
- A new automated review of `6b14803` opened 34 more threads, some on code this SOW added: a bounded send of an untracked message with nothing in flight divided by zero in `pending_bound` (N3 C, N14 Go).

Evidence:

- CI run ids 42460571771 (flawfinder) and 42460571682 (Codacy). Go `TestBoundedControlSendIdle` panics with "integer divide by zero" with the fix reverted.
- `C, Rust, and Go Coverage` also fails, on both `9568f4e` and `6b14803`, at the Codacy upload: "No Write permission on Project". It is a repository token problem, not this code; recorded for SOW-0050.

Why validation missed it:

- flawfinder and Semgrep run only in CI and were not run locally before the push; flawfinder is available locally and now runs before a push.
- The bounded-send tests only sent tracked requests, so `inflight_count` was never zero there.

Repair:

- F17 (decision 12): `publish_endpoint_lock` in C, Rust and Go creates `{service}.lock.{pid}.{seq}.tmp`, `fchmod`s it to 0600, hard-links it to the lock name and unlinks the temporary name. Any failure falls through to the previous creating open, so a filesystem without hard links keeps working. `level1-posix-uds.md` updated. Tests check through a publish hook, under umasks 0477, 0277 and 0777, that the file is 0600 before the lock name exists and that the temporary name is gone.
- Fixed from the new review (24 threads): N1 (above), N3/N14 zero in-flight bound in C, Go and Rust with tests (the C division is undefined behaviour that GCC folds at this build's optimisation level, so only the Go test is a reproduction), N8 lookup-scale interop on macOS through `workflow_dispatch` plus `AGENTS.md` (the full extended suite needs SHM, which macOS lacks), N10 and N27 this SOW's record, N11/N16 fixtures prove the request arrived, N15/N18 bench duration caps, N17 Go `StopListening` serialized (no deterministic test: proving a call stays blocked needs a timing wait), N20, N23 and N34 SOW records, N21, N33 and N35 docs, N22 truncated HELLO in a Rust fixture, N24/N26/N28 unassigned-bit tests use `0x10`, N25/N31 C server duration 0 restored, N32 comment.
- Rejected with evidence (11 threads, each answered in its thread): N2, N4, N5, N6, N7, N9, N12, N13, N19, N29, N30.

Validation:

- Linux CTest 32/32, `cargo test` 413, Go ok; `cargo fmt --check` on 1.91.0 and stable, `cargo +1.91.0 check --all-targets`, `go vet` for linux, freebsd, darwin and windows, flawfinder level 5: 0 hits.
- Windows CTest 20/20, Rust 280, Go ok. FreeBSD 14.3 and 15.1: CTest 31/31, `cargo test` 412, Go and Go 386 ok. macOS: CI on the push.
- Performance: no benchmark rerun. Lock creation runs once per listen, and `pending_bound` only changes its result when nothing is in flight, which no benchmark row does (every row sends tracked requests).

Artifacts:

- Specs: `level1-posix-uds.md` (lock creation, FreeBSD Go SHM architectures). Docs: `netipc-integrator-skill.md` (x86_64 coverage). `AGENTS.md`: macOS extended command. Project skills: `project-netdata-vendoring` unaffected. SOW records: SOW-0050, SOW-0053, SOW-0065.
- A review of `3fb1bef` added six comments, all fixed here: the Go stalled-reader poll retries `EINTR` and requires `POLLIN`, the macOS manual step runs only lookup-scale interop, and three bench comments and usage lines were corrected. Its three CodeQL alerts (`cpp/path-injection` on the caller-supplied `run_dir`, `cpp/stack-address-escape` on two test hooks) are false positives of classes already triaged on this repository (alerts 7633, 7641).
- Lesson: run the locally available static analyzers before pushing, and wait for CI and the automated reviewer before posting replies that describe the pushed code.

## Regression - 2026-11-09

What broke:

- The SOW-0050 re-vendoring of `278125c` into Netdata built with a new warning in NetIPC code: `netipc_uds_lifecycle.c:376`, `ignoring return value of 'link' declared with attribute 'warn_unused_result'` (`-Wunused-result`). `publish_endpoint_lock` (decision 12) discarded the result with a `(void)` cast, which GCC does not accept for this attribute. The earlier vendoring built with no warning in NetIPC sources.

Evidence:

- Netdata compiles NetIPC with `-O2 -D_FORTIFY_SOURCE=3` (from `compile_commands.json` in the Netdata worktree build); glibc marks `link()` `warn_unused_result` only under `_FORTIFY_SOURCE`.
- A plugin-ipc Release build with `-O2 -D_FORTIFY_SOURCE=3 -Wall -Wextra` reproduces it and shows one more library warning: `-Wmisleading-indentation` at `netipc_uds_receive.c:363` and `:383`, from four tab-indented lines in a file indented with spaces (formatting only; the guarded statements are correct).

Why validation missed it:

- plugin-ipc builds and CI configure without `_FORTIFY_SOURCE` (the local build is `Debug`), so the attribute is not active there; the fortified build happens only downstream.

Repair:

- `netipc_uds_lifecycle.c`: keep the `link()` result in a local and discard it, with a comment on why. Rust and Go have no equivalent warning.
- `netipc_uds_receive.c`: the four tab indents replaced with spaces.

Validation:

- Fortified Release build: no warning in library sources. Linux CTest 32/32. FreeBSD 14.3 and 15.1: C build and CTest 31/31 (Rust and Go unchanged).
- Performance: no benchmark run. Lock creation runs once per listen; the receive change is whitespace only.

Artifacts:

- Project skill `project-netdata-vendoring`: step 10 now requires no warning in vendored NetIPC sources and gives the fortified build command to run before pushing. Specs, docs and `AGENTS.md`: unaffected (no behavior change). SOW-0050 records the re-vendoring.
- Lesson: before vendoring, build the C library with Netdata's optimisation and fortify flags, since plugin-ipc's own builds do not enable `_FORTIFY_SOURCE`.


## Regression - 2026-11-11

What broke:

- Netdata CI on netdata/netdata#24633 (head `75b92c216d`, Go Tests workflow, `go test -race`) failed `TestRobustSendBoundedByDeadlineAndAbort/send_deadline`: `robustness_unix_test.go:727: no request bytes arrived: n=0 revents=0x0 errno=errno 0`. The call itself returned the expected `ErrTimeout`; only the fake server's assertion failed. The same commit passed earlier runs.

Evidence:

- The review fix in `3fb1bef` made the Go stalled reader (`startStalledReader`, and `startWinStalledReader` on Windows) fail the test unless request bytes arrive within 10 s, in both variants.
- The call deadline starts before the request is encoded (`newLookupDeadline`, `lookup_common.go:110`, called by `CallAppsLookupWithTimeout`). Under `-race` on one core, encoding the test's 1M-PID request takes 191-229 ms (measured locally, 3 runs), against the variant's 200 ms deadline; without `-race` it takes 2-10 ms. When the encode outlasts the deadline, the client correctly returns `ErrTimeout` before writing, and no bytes ever reach the reader.
- C (`test_service.c` / `test_win_service.c` `test_client_send_bounded_by_deadline_and_abort`) checks that the request reached the server only in the abort variant; Rust ignores the poll result. Go was the only one asserting it in the deadline variant.

Why validation missed it:

- plugin-ipc's Go runs are fast enough that the encode stays well under 200 ms; the race-enabled run on a loaded Netdata CI runner was the first to cross it. The assertion's outcome depended on timing, which the test policy forbids.

Repair:

- Go Unix and Windows stalled readers record whether request bytes arrived instead of failing; the abort variant, which waits for the request to start before aborting, asserts it, as C does. The deadline variant asserts only the result (`ErrTimeout`) and the bound, as C and Rust do.
- Both tests close the client before joining the reader, as Rust does, so a reader still waiting for bytes wakes on the hang-up instead of sitting out its 10 s wait.

Validation:

- Forced the failure condition with a temporary 1 ms deadline (encoding always outlasts it), restored afterwards: Linux `go test -race -count=3` passes in 2.9 s (before the close reorder each run sat out the 10 s wait); Windows (costa-win11, Go 1.27.3, `-count=3`; `-race` needs MinGW cgo, which that host's MSYS shell lacks) passes with the deadline variant at 10 ms.
- Normal runs: Linux `go test -race ./pkg/netipc/service/raw/` and `go test ./pkg/netipc/...` pass; `go vet` for linux and windows clean; Windows `TestWinRobustSendBoundedByDeadlineAndAbort` passes; FreeBSD 14.3 and 15.1, amd64 and `GOARCH=386`, pass.
- Same-failure search: the other timed waits in test peers (`pipe_integration_test.go:357,376,397`, `pipe_test.go:236`, `posix_tests.rs:602`, `raw_robustness_unix_tests.rs:898`) wait for an event that already happened or assert that nothing arrived, so their outcome does not depend on timing. C asserts the request start only in the abort variant already.
- Performance: no benchmark run; test code only.

Artifacts:

- Specs, docs, project skills and `AGENTS.md`: unaffected. Test code only, and AGENTS.md Test Cadence already requires outcomes that do not depend on timing.
- Lesson: in a deadline test, a fake peer must not assert progress that the deadline can legitimately preempt. The deadline covers the whole call, encoding included.
- The fix reaches Netdata through a re-vendoring into netdata/netdata#24633 (Netdata commit `b53818fab6`).
