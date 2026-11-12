# SOW-0062 - Pipeline+Batch Benchmark Deadlock On Small Socket Buffers

## Status

Status: completed

Sub-state: completed 2026-11-03; implemented and validated on Linux, the 9 Netdata Linux targets, FreeBSD and macOS.

## Requirements

### Purpose

Every benchmark row must run on every POSIX platform that runs the benchmark suite, so FreeBSD (and macOS) benchmark runs are complete evidence.

### User Request

Standing rules: zero known bugs; a Netdata-supported configuration that cannot be tested is a blocker. SOW-0059 decision 10A asked for a FreeBSD benchmark run.

### Assistant Understanding

Facts:

- On the FreeBSD 14.3 test VM, `tests/run-posix-bench.sh` row `uds-pipeline-batch` (depth 16) fails for all 9 C/Rust/Go pairs; every other row completes, including `uds-pipeline-d16` (single-item pipelining).
- Reproduction (C client, C batch server): the client blocks for the server's whole lifetime (34 s with a 30 s server) and then fails with `EPIPE` when the server exits. Rust and Go clients fail the same way.
- `truss` of the server: it receives request 1 (8192 + 1440 bytes), sends the first 8192-byte chunk of its response, then blocks sending the rest. The client is still sending requests 2..16 and reads nothing, so both sides block in send: a write-write deadlock.
- The client sends `depth` batches of 2-1000 random items before reading any response (`bench/drivers/c/bench_posix.c` `uds-pipeline-batch-client`, Rust `bench/drivers/rust/src/main.rs` `run_pipeline_batch_client`, Go `bench/drivers/go/main.go` `runPipelineBatchClient`).
- FreeBSD's default SEQPACKET `SO_SNDBUF` is 8192 bytes (SOW-0048); Linux's is about 208 KiB, which holds the bytes in flight, so the row passes there.
- The spec already tells callers about this on macOS (`docs/level1-posix-uds.md`, macOS stream baseline, "Socket buffers": read responses while sending, or keep the bytes in flight small). FreeBSD has no such note.
- No session send timeout exists (`SO_SNDTIMEO` is set only while connecting, `netipc_uds_lifecycle.c`), so the blocking is unbounded until a peer exits.

Inferences:

- The library behaves as specified (blocking sends, ordinary socket backpressure); the defect is the benchmark client's send-everything-then-read pattern on small-buffer platforms.
- macOS (8 KiB stream buffers) most likely deadlocks the same way; not yet run there.

Unknowns:

- None that investigation cannot resolve; the fix direction is a user decision.

### Acceptance Criteria

- `uds-pipeline-batch` completes for all 9 pairs on Linux, FreeBSD and macOS.
- Pipelining a client over the POSIX baseline cannot deadlock on small buffers in C, Rust or Go (reproduction tests on shrunken socket buffers pass); one-in-flight calls keep today's blocking send.
- Docs describe the POSIX behavior and the interim Windows caller rule.

## Analysis

Fix options (presented 2026-11-01; decided, see Implications And Decisions):

1. Benchmark clients read responses while sending (send one, drain whatever responses are ready, keep at most `depth` in flight). Same scenario on every platform; Linux numbers may shift, so the Linux floor for this row needs re-evidence.
2. Benchmark clients cap the bytes in flight at what the socket buffers hold (reduce the effective depth on small-buffer platforms, record the effective depth in the row). Linux unchanged; FreeBSD/macOS rows measure a smaller depth.
3. Library: raise session `SO_SNDBUF`/`SO_RCVBUF` on FreeBSD/macOS toward Linux defaults, in C, Rust and Go. On FreeBSD this also raises the SEQPACKET packet size (fewer chunks). Moves the threshold; does not remove the caller constraint; affects packet-size negotiation and needs cross-language interop validation.

Production session concurrency (checked 2026-11-01, part of decision 3a):

- Sessions are single-threaded in every language, so a caller cannot read on one thread while sending on another:
  - C: send adds to the in-flight message-id set, growing it with `realloc` (`netipc_uds_send.c:42`, `netipc_uds_inflight.c`); receive removes from it (`netipc_uds_receive.c:153`). No lock: concurrent use is a data race.
  - Rust: `UdsSession::send(&mut self)` and `receive(&mut self)` (`transport/posix.rs:353`) cannot run at the same time; the compiler enforces it.
  - Go: `Session.Send` and `Session.Receive` both use the plain map `inflightIDs` (`uds_session.go:56`) with no mutex; concurrent use risks a "concurrent map write" panic.
- A chunked message is sent by one blocking call (the `sendmsg` loop in the trace above). Once inside it, the caller cannot read until the whole message is out. So single-thread "read while sending" (option 1 as decided) cannot be built on the current API: with more than one message in flight and the bytes beyond the buffers, client and server can still both block in send.
- Deadlock-free pipelining therefore needs one of: a sender thread and a receiver thread on the same session (library change in C, Rust and Go), or the caller keeping unread bytes in flight within the real buffer capacity (no library change). This reopens the fix decision.

Netdata consumers (checked 2026-11-02, `netdata/netdata` master @ 5f67470fc2, read-only):

- 30 files outside the vendored copy use NetIPC, all C (apps.plugin, cgroups.plugin, ebpf.plugin, network-viewer.plugin, common-cgroups and their tests); no Go or Rust consumers.
- None calls the transport directly (`nipc_uds_send`/`receive`, `nipc_shm_*`); all use the typed API (`nipc_client_call_cgroups_lookup`, `nipc_client_call_apps_lookup`, `nipc_client_call_cgroups_snapshot_timeout`, `nipc_server_*`).
- A typed client call is one send then one receive (`netipc_service_common.c:365-370`), so at most one request is in flight; the server session loop is receive, handle, send. Neither can hit this deadlock, so no Netdata caller changes are needed. Netdata receives the library change through normal vendoring (SOW-0050 preflight).
- Design point for the gate: the deadlock needs both peers blocked in send; progress on the client side (draining responses while its send waits) is enough. A server that queued requests during a stalled send would need its own bound, so server-side progress must be justified before adding it.

Windows named pipes (checked 2026-11-02):

- Same deadlock class. All three languages use synchronous (non-overlapped), message-mode pipes with 64 KiB buffers per direction (C `netipc_named_pipe.c:218-222` `CreateNamedPipeW(... PIPE_WAIT ..., buf_size, buf_size ...)`, `buf_size = max(packet_size, 64 KiB)` at `:58-63`; client `CreateFileW` at `:986-990` without `FILE_FLAG_OVERLAPPED`; Rust `windows.rs:1164-1176`; Go `pipe_listener.go:268-276`). Sends are blocking `WriteFile` per packet with no timeout; a blocked write cannot stop to read.
- The Windows pipeline-batch benchmark passes only because its clients already keep a byte budget: `inflight_budget = packet_size * 2` (`bench_windows.c:2007`), each slot costed as request plus response (`:2052`), responses drained before a send that would exceed it (`:2054-2063`); Go and Rust Windows drivers mirror it. This came from the pre-SOW rewrite, where the full-duplex pipelining deadlock was recorded (`.agents/sow/todo-history/TODO-pending-from-rewrite.md:2713`).
- `docs/level1-windows-np.md:159-167` says nothing about pipelining limits.
- The POSIX mechanism does not port directly: a synchronous pipe handle has no per-call "don't wait" flag. Windows progress needs overlapped I/O (`FILE_FLAG_OVERLAPPED` on both ends, every read and write in the C, Rust and Go Windows transports) or `PIPE_NOWAIT`, which Microsoft keeps only for LAN Manager compatibility.
- Netdata on Windows uses only typed one-in-flight calls (same consumer set as above), so it cannot hit this.

POSIX design under decision 5C (no further forks found):

- Where: only the POSIX transport's packet hooks change (C `nipc_uds_raw_send_iov` and the receive packet reads in `netipc_uds_receive.c`; Rust `transport/posix.rs` and `posix_stream.rs` write/read closures passed to `framing::send_message`/`receive_message`; Go `sendPacket`/`Recv` closures given to `framing.SessionSend`/`SessionReceive`). The shared framing code and the Windows transports stay unchanged.
- When: client sessions only, and only while other requests are in flight (the in-flight set holds more than the request being sent). A one-in-flight call, which is every typed call and every Netdata caller, keeps today's blocking send exactly.
- How: send the packet with `MSG_DONTWAIT`; on `EAGAIN`, `poll` for readable and writable; read what is readable without blocking into a per-session raw packet queue (SEQPACKET: whole datagrams; macOS stream: raw bytes); resume sending when writable. Receive takes packets from the queue before the socket, so reassembly and validation stay as they are.
- Bound: queued bytes may not exceed what the in-flight responses can occupy (in-flight count x (maximum response payload + header + chunk headers)); more is a protocol failure of the session.
- No new timeout: a stalled send waits as long as today's blocking send; abort semantics unchanged.
- Server sessions unchanged: the deadlock needs both peers blocked in send, and client progress alone breaks it.
- Benchmark clients unchanged on POSIX: their send-everything-then-read pattern is the reproduction, and with the fix it completes.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- A POSIX client with more than one request in flight sends each packet with a blocking call. When the unread responses fill its receive buffer, the server blocks sending and stops reading, and the client blocks sending: both peers block forever. Buffer size only moves the threshold (Linux about 208 KiB, FreeBSD and macOS 8 KiB). The current API cannot read during a send (Analysis).

Evidence reviewed:

- FreeBSD reproduction and `truss` traces (Assistant Understanding); send paths C `netipc_uds_send.c` (all packets through `nipc_uds_raw_send_iov`, `netipc_uds.c:69`; stream `nipc_uds_stream_send_iov`), Rust `transport/posix.rs:353-365` (`send_packet` closure into `framing::send_message`) and `posix_stream.rs` (`send_iov`), Go `uds_send.go:16-40` (`sendPacket` closures into `framing.SessionSend`) and `uds_stream.go`; receive paths C `netipc_uds_receive.c` (`receive_first_packet`, `receive_one_chunk`, stream `nipc_uds_stream_read_exact`, the stream wait at the top of `nipc_uds_receive_timeout`), Rust `posix.rs:369-430` (`raw_recv_control`, stream `recv_session_packet`/`read_exact`), Go `uds_receive.go`; session state C `netipc_uds.h:92-115`, Rust `posix.rs:255-270`, Go `uds_session.go`; Netdata consumers and Windows (Analysis).

Affected contracts and surfaces:

- POSIX transport client send and receive in C, Rust and Go, both SEQPACKET (Linux, FreeBSD) and the macOS stream; session state gains a receive queue; session close frees it.
- Wire format, handshake, server sessions, SHM, shared framing modules, Windows transports: unchanged.
- Specs and docs: `docs/level1-transport.md` (pipelining), `docs/level1-posix-uds.md` (send progress; the macOS "Socket buffers" caller rule becomes a library guarantee), `docs/level1-windows-np.md` (interim caller rule, decision 6A), `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- `poll`-based waits (`nipc_uds_wait_for_readable`, Rust `wait_fd_readable`, Go `internal/syspoll`); the stream partial-write loops; the in-flight set as the source of the bound; per-instance test hooks for timing.

Risk and blast radius:

- Only client sessions with more than one request in flight take the new path. Typed calls and every Netdata caller have one in flight and keep the blocking send unchanged.
- Receive gains one "queue empty?" check per packet; the queue is drained before the socket, so reassembly, limits and in-flight validation are unchanged.
- macOS stream: partial writes under `MSG_DONTWAIT` resume after progress; covered by the macOS host suites.
- Peer close during a stalled send: the read side stops at EOF and the next send fails with the existing send error; no busy loop.

Sensitive data handling plan:

- Source paths, command names and counts only, in this SOW, specs, docs, the integrator skill, AGENTS.md and code comments; hosts named by role.

Implementation plan:

1. C: session receive queue (SEQPACKET datagram records, stream raw bytes) and its bound; a session-level packet send that uses `MSG_DONTWAIT` plus `poll` progress when the client has other requests in flight; the receive packet reads and the stream wait take from the queue first; free on close.
2. Rust: the same in `transport/posix.rs` and `posix_stream.rs`, through the existing closures.
3. Go: the same in `transport/posix` (`uds.go`, `uds_stream.go`, `uds_receive.go`, `uds_session.go`).
4. Reproduction tests in each language: a client pipelines 16 messages of about 16 KB over a session with forced small packet size and shrunken socket buffers, against a plain receive-then-send server thread; plus peer-close during a stalled send, and the queue bound.
5. Docs and integrator skill.

Validation plan:

- Reproduction tests fail on the current code (recorded) and pass after the fix, in C, Rust and Go.
- Linux: CTest, `cargo test` stable and 1.91, fmt, clippy gate, `go test`, all interop scripts; all 9 Netdata Linux targets.
- FreeBSD 14.3: full suite, plus the `uds-pipeline-batch` benchmark scenario for all 9 language pairs (the original failure) as real-use evidence.
- macOS host: full suite (stream variant).
- Windows: the shared framing code is untouched; Windows Rust clippy and the Windows suites confirm.

Artifact impact plan:

- Specs/docs listed under Affected contracts; AGENTS.md unaffected unless commands change; the project vendoring skill unaffected.

Performance assessment:

- Per-request established-session code touched: the client packet send (one branch on role and in-flight count before the existing `sendmsg`; pipelining clients use `MSG_DONTWAIT` on the same call) and each packet read (one queue-empty check). Single-in-flight rows keep the same syscalls. This touches the measured send/receive and pipelining paths, so the POSIX benchmark rows run once in the consolidated pre-vendoring run after SOW-0063 (Test Cadence batching); the FreeBSD pipeline-batch rows run here as acceptance.

Open decisions:

- None (decisions 3a/5C and 6A recorded).

## Implications And Decisions

- Depth (2026-11-01, user question): the 16 is the benchmark's `PIPELINE_DEPTH` (`tests/run-posix-bench.sh:1107`), not a production limit; NetIPC has no in-flight cap on the baseline transport, and no production POSIX code pipelines. In-flight capacity is set by socket buffer bytes and the peer's reading, so the deadlock comes from send-everything-then-read, not from the number (with 8 KiB buffers and ~16 KB batches even 2 in flight can deadlock).
- Fix (2026-11-01, user: "3a"): option 1 — benchmark clients read responses while sending; also verify whether production sessions support concurrent send and receive, so a production pipelining caller can avoid the same deadlock. Option 2 (byte cap) dropped: it is another arbitrary limit.
- Fix revised (2026-11-02, user: "ok C"): the current session API cannot read while sending (see Analysis), so decision 3a is replaced by option C, non-blocking send with progress, in the library for C, Rust and Go: send each chunk with `MSG_DONTWAIT`; only on `EAGAIN`, poll for readable/writable, receive replies into a per-session queue bounded by the in-flight set, then resume; the next receive returns queued replies first. No extra syscall on the fast path. Rejected: full-duplex sessions (lock on every send/receive, threads required in callers) and a benchmark-only capacity window (production callers would carry a documented rule). Windows named pipes: checked in the gate; widening scope needs the user.
- Decision 1 (2026-11-03, user: "ok, A"): queued responses vs fd polling. Chosen: A, a public has-pending check in C (`nipc_uds_session_has_pending`), Rust (`UdsSession::has_pending`) and Go (`Session.HasPending`), and fd-accessor docs telling fd-polling callers to check it before waiting. Rejected: B, docs only (a caller cannot follow the rule without a check); C, an extra signalling fd per session (an fd and syscalls per session for a path no caller uses yet). No internal caller is affected: server sessions never queue (only a pipelining client fills the queue), and the service client paths call receive, which reads the queue first.
- Windows (2026-11-02, user: "A"): Windows gets the same guarantee with overlapped I/O in its own SOW (SOW-0064). This SOW stays POSIX-only, and adds the interim caller rule (keep bytes in flight under the pipe buffer) to `docs/level1-windows-np.md` and the integrator skill so the docs are accurate until SOW-0064 lands.

## Plan

The gate's implementation plan: C, then Rust, then Go, then docs, then validation.

## Execution Log

### 2026-11-01

Created from the SOW-0059 FreeBSD benchmark run.

### 2026-11-02

- Reproduction test first (C `test_pipeline_exceeds_socket_buffers` in `tests/fixtures/c/test_uds.c`): the first version failed in about 40 ms for the wrong reason. Its 256-byte caller buffer was smaller than one 1024-byte packet, so the server's receive truncated packets (`NIPC_UDS_ERR_CHUNK`). With the buffer sized to the packet, the test fails on the unfixed code through the real deadlock: the client's send blocks until the test's 2 s `SO_SNDTIMEO` bound fires.
- C (`netipc_uds_send.c`, `netipc_uds_receive.c`, `netipc_uds.c`, `netipc_uds_internal.h`, `netipc_uds_lifecycle.c`, public session struct in `netipc_uds.h`): `session_send_packet` routes a client packet sent while another request is in flight to `send_with_progress` (`MSG_DONTWAIT`, `poll` for in/out, `nipc_uds_pending_drain` into the session queue, retry); everything else keeps `nipc_uds_raw_send_iov`. Receive reads the queue first (SEQPACKET `session_recv_packet`, stream `session_read_exact`/`session_recv_packet`), and the stream's pre-receive wait is skipped while the queue holds bytes. A bound overflow, or a failed queue allocation, shuts the socket down and fails all in-flight requests. `nipc_uds_close_session` frees the queue. No new source file, so Netdata's build lists are unchanged.
- Rust (`transport/posix.rs`, `posix_stream.rs`): `Pending` queue, `pending_bound`, `send_with_progress`; `UdsSession::send` computes the in-flight count before `framing::send_message` (the closure holds the in-flight set), and clears the set after a protocol failure. The SEQPACKET receive closure and `stream::recv_session_packet` take from the queue first. `framing.rs` unchanged.
- Go (`uds_send.go`, `uds_session.go`, `uds_receive.go`, `uds_stream.go`, `internal/syspoll` gains `Out`): `pendingQueue`, `pendingBound`, `sendWithProgress`; the send closures call `s.sendSessionPacket`, which uses `s.progressBound` set before `framing.SessionSend`, so no new closure per send. Shared `transport/internal/framing` unchanged.
- Tests in each language: pipelining 16 x 16000-byte requests with 1024-byte packets and 4096-byte socket buffers against a receive-then-send server (C `test_pipeline_exceeds_socket_buffers`, Rust `test_pipeline_exceeds_socket_buffers`, Go `TestPipelineExceedsSocketBuffers`); the server closing during a stalled send (`*_peer_close_during_send`, expects the send error and a cleared in-flight set); a server flooding responses beyond the bound (`*_pending_bound`, expects the protocol error and a cleared in-flight set). The 2 s `SO_SNDTIMEO` in the tests only bounds a failing run; it is not a synchronization sleep, and the passing runs never reach it.
- macOS: the first macOS run failed the progress tests in all three languages after the 2 s bound. A probe on the macOS host showed that macOS ignores `MSG_DONTWAIT` on a stream `send` that lacks buffer space (it blocked until `SO_SNDTIMEO`), while `O_NONBLOCK` returns `EAGAIN` at once and `recv(MSG_DONTWAIT)` works. The macOS progress send now sets `O_NONBLOCK` for the send and restores it (C `send_with_progress`, Rust `send_with_progress`, Go `sendSessionPacket`); Linux and FreeBSD keep `MSG_DONTWAIT`. The decided design (client-only, only while pipelining, no new timeout) is unchanged; only the macOS non-blocking mechanism differs.
- Acceptance criterion correction: the benchmark drivers build only with POSIX SHM (`CMakeLists.txt:1182`, `:1214`, `if(NETIPC_POSIX_SHM)`), so macOS runs no benchmark rows. macOS evidence for the stream path is the reproduction tests in C, Rust and Go.
- FreeBSD 14.3 VM: `uds-pipeline-batch` depth 16 completes for all 9 pairs (5 s each, 17.7M-31.2M items/s; before the fix all 9 deadlocked).
- Independent review (one reviewer, read-only) findings and handling:
  1. Queued responses are invisible to callers that wait on the session fd (`nipc_uds_session_fd`, Rust `UdsSession::fd`, Go `Session.Fd`): a design decision, open decision 1 below.
  2. macOS stream: a failed receive shut the read side but left peer bytes in the queue, and the next receive delivered them (a forged response for an in-flight id was accepted). Fixed: the queue is cleared at that shutdown in all three languages. Reproduction tests `test_stream_failed_receive_drops_pending` (C, Rust) and `TestStreamFailedReceiveDropsPending` (Go, darwin build tag) fail without the clear and pass with it on the macOS host.
  3. Go `pendingBound` could overflow `int` on 32-bit targets (a bound wrapping to 0 would bring back the blocking send). Fixed: saturates at `math.MaxInt`, as C and Rust saturate.
  4. A failed restore of the macOS descriptor flag failed the session in C only. Fixed: all three shut the socket down and report a send error, which fails every in-flight request.
  5. Tests: the 2 s `SO_SNDTIMEO` was also set on the server, so a stall over 2 s could fail a passing run. Fixed: the hang bound is client-only and 10 s. The reviewer's "C peer-close test lacks the in-flight check" was wrong (the check exists). The peer-close tests assert the contract (the send fails, no hang) whichever path reports the close (EPIPE directly, or EOF then EPIPE); which one runs depends on timing, and both satisfy the contract.
- Removing the server-side timeout exposed a Go test bug on macOS: `TestPipelinePendingBound` waited for the flooding server goroutine before closing the client, and on macOS a peer's `shutdown` does not wake a sender blocked on a full unix stream, only closing does. The Go tests now close the client before waiting, as the C and Rust tests already did.
- Process note: a `tar` re-sync to the macOS host kept old file mtimes, so cargo reused a binary built from a temporary revert and reported a false failure. Remote syncs now extract with `tar -m`.
- Docs: `docs/level1-transport.md` (pipelining guarantee), `docs/level1-posix-uds.md` ("Pipelined sends"; the macOS "Socket buffers" caller rule now points to it), `docs/level1-windows-np.md` ("Pipelining": interim caller rule until SOW-0064), `docs/netipc-integrator-skill.md` (L1 ownership list).

### 2026-11-03

- Decision 1 (option A): `nipc_uds_session_has_pending` (public inline in `netipc_uds.h`; the internal `nipc_uds_pending_queued` now calls it), Rust `UdsSession::has_pending`, Go `Session.HasPending`. The fd accessors' docs point to it. `docs/level1-posix-uds.md` ("Pipelined sends", "Polling the fd") and `docs/netipc-integrator-skill.md` state the caller rule. The `*_exceeds_socket_buffers` tests assert it in all three languages: false before sending, true after the sends (the 4096-byte buffers hold a fraction of the 16 responses, so finishing the sends required queueing responses), false after every response is received.
- Follow-up mapping: SOW-0064 gains the matching Windows acceptance criterion; SOW-0050's performance assessment lists this SOW's per-request changes for the pre-vendoring benchmark run.

## Validation

Acceptance criteria evidence:

- `uds-pipeline-batch` (depth 16, all 9 C/Rust/Go pairs): FreeBSD 14.3 VM, all 9 complete (5 s each, 17.7M-31.2M items/s; before the fix all 9 deadlocked). Linux workstation (final tree, 2026-11-03): all 9 complete (5 s each, 27.5M-47.7M items/s). macOS: the criterion was corrected (Execution Log 2026-11-02): the benchmark drivers build only with POSIX SHM (`CMakeLists.txt:1182`, `:1214`), so macOS runs no benchmark rows; its evidence is the reproduction tests on the stream path below.
- No deadlock on small buffers: `test_pipeline_exceeds_socket_buffers` (C, Rust) and `TestPipelineExceedsSocketBuffers` (Go) fail on the unfixed code after the client's 2 s hang bound (recorded 2026-11-02) and pass on Linux, FreeBSD and macOS. One-in-flight calls keep the blocking send: `session_send_packet` takes the progress path only for a client with more than one request in flight (`netipc_uds_send.c:236`; Rust `UdsSession::send`; Go `uds_send.go:34-36`, `:80`).
- Docs: `docs/level1-transport.md` ("Pipelining"), `docs/level1-posix-uds.md` ("Pipelined sends", including "Polling the fd"), `docs/level1-windows-np.md` ("Pipelining": interim caller rule), `docs/netipc-integrator-skill.md`.

Tests:

- Reproduction tests in all three languages: pipelining beyond the buffers, peer close during a stalled send (send error, in-flight set cleared), the queue bound (protocol error, in-flight set cleared), macOS stream failed receive drops the queue (`test_stream_failed_receive_drops_pending`, Go `TestStreamFailedReceiveDropsPending`), and the has-pending check. Each bug reproduction test failed without its fix.
- Linux workstation (final tree, 2026-11-03): CTest 32/32; `cargo test` stable and 1.91.0, 398 passed; clippy gate (`-D clippy::correctness -D clippy::suspicious`) on Linux, plus Windows-gnu and darwin on 2026-11-02 (no Windows- or darwin-specific change since); `cargo fmt --check` on both toolchains; `go test ./...`; `go vet` linux/darwin/freebsd; gofmt clean. The 8 interop suites, valgrind on `test_uds` and the 386 Go transport tests ran on 2026-11-02; the 2026-11-03 change adds a read-only accessor and test assertions, with no wire or interop effect.
- Netdata Linux targets (x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl): all 9 rc=0 on the final tree (2026-11-03).
- FreeBSD 14.3 VM (final tree): CTest 31/31, `cargo test` 397 passed, `go test ./...`, `GOARCH=386 go test` of the POSIX transport, all 8 interop suites including SHM.
- macOS test host (final tree): CTest 24/24, `cargo test` 330 passed, `go test ./...`, C `test_uds` 271 passed, `test_uds_interop`, `test_service_interop`, `test_cache_interop`, `test_lookup_scale_interop`, `interop_codec`. `test_shm_interop` exits early by design (no SHM on macOS).
- Windows: no Windows-compiled file changed (`netipc_service.h` includes `netipc_uds.h` only in its non-Windows branch; `internal/syspoll` is unix-only; the Rust POSIX transport is `cfg(unix)`); Rust clippy for `x86_64-pc-windows-gnu` clean and `GOOS=windows go vet` shows only the existing `windows/shm.go` warnings.
- Timing: the tests synchronize on the server thread and socket state; the 10 s client `SO_SNDTIMEO` only bounds a failing run (Execution Log 2026-11-02).

Real-use evidence: the original failure, the FreeBSD `uds-pipeline-batch` benchmark scenario, now completes for all 9 pairs with the production benchmark drivers.

Reviewer findings: one independent read-only review (2026-11-02), five findings; handling recorded in the Execution Log (1 became decision 1, now implemented; 2-4 fixed with reproduction tests where testable; 5 fixed in the tests, with one reviewer claim rejected as wrong with evidence).

Same-failure search:

- Other pipelining paths: `uds-pipeline-d16` (single-item pipelining) passed before and after; POSIX SHM allows a single request in flight (`docs/level1-posix-shm.md`, "Single in-flight constraint"), so it cannot pipeline; Windows Named Pipes have the same class, tracked in SOW-0064 (their benchmark clients already keep a byte budget).
- Production callers: no Netdata caller pipelines (Analysis, "Netdata consumers"); all use typed one-in-flight calls.
- fd polling: every internal caller that waits on a session fd waits on a server session, which never queues (C `netipc_service_posix_server_session.c:143`, Rust `server_session_unix.rs:69`, `:86`, Go `server_unix.go:292`); the other fd uses check for peer disconnection after an error or pass the fd to the SHM wait, and the client service paths call receive, which reads the queue first.

Performance assessment: per-request established-session changes are one branch per client packet send (`netipc_uds_send.c:236`), one queue-empty check per packet receive (`netipc_uds_internal.h:115-118`, `netipc_uds_receive.c:202`, `:477`), and for pipelining clients a `MSG_DONTWAIT` send (macOS: plus three `fcntl` calls per packet, `netipc_uds_send.c:213-220`). The has-pending check runs only when a caller asks. These are measured paths, so the POSIX benchmark rows run once in the consolidated pre-vendoring run (SOW-0050, after SOW-0063), as Test Cadence batching requires. macOS has no benchmark rows (no SHM build), so its `fcntl` cost stays unmeasured; no macOS caller pipelines.

Sensitive data gate: this SOW, the docs, the integrator skill and the code comments hold source paths, line numbers, test names, counts and command names only; hosts are named by role; the Netdata evidence cites `netdata/netdata` at a commit with repo-relative paths.

Artifact maintenance gate:

- AGENTS.md: no change; no command, workflow or guardrail changed.
- Runtime project skills: `project-netdata-vendoring` unchanged; its preflight already covers performance assessment, and the SOW-0050 assessment now lists this SOW's changes.
- Specs: the public specs under `docs/` were updated (`level1-transport.md`, `level1-posix-uds.md`, `level1-windows-np.md`); `.agents/sow/specs/` holds no transport rules.
- End-user/operator docs: README lists the pipeline benchmark rows only and makes no buffer or deadlock claim; no change.
- End-user/operator skills: `docs/netipc-integrator-skill.md` updated (pipelining limits, has-pending rule, interim Windows rule).
- SOW lifecycle: completed and moved to `done/`; SOW-0064 (Windows) created from decision 6A and given the has-pending parity criterion; SOW-0050 records the benchmark coverage.

Spec update: see the artifact gate.

Project skill update: none needed (see above).

Lessons: see Lessons Extracted.

Follow-up mapping (search for defer/later/follow-up/future/TODO/pending):

- Windows pipelining progress: SOW-0064.
- Benchmark rows for the changed paths: the consolidated run in SOW-0050.
- macOS `fcntl` cost per pipelined packet: rejected for now with evidence: no macOS caller pipelines and macOS has no benchmark rows; a whole-session non-blocking mode would be a design change brought to the user if a pipelining macOS caller appears.
- "Pending" sections of this file: filled.

## Outcome

A POSIX client may pipeline any number of requests, whatever the socket buffer sizes, in C, Rust and Go: a send that waits for buffer space reads the arriving responses into a bounded per-session queue, and receive returns them first. The FreeBSD `uds-pipeline-batch` benchmark row, which deadlocked for all 9 pairs, completes. Callers that poll the session fd check `has_pending` first. One-in-flight calls, which is every typed call and every Netdata caller, are unchanged.

## Lessons Extracted

- macOS ignores `MSG_DONTWAIT` on a unix stream send that lacks buffer space and blocks until `SO_SNDTIMEO`; only `O_NONBLOCK` returns `EAGAIN`. A platform's honoring of per-call flags needs a probe on that platform.
- On macOS a peer's `shutdown` does not wake a sender blocked on a full unix stream; closing does. Tests that stop a blocked peer must close, not only shut down.
- A session-local queue in front of a socket hides data from fd readiness; any such queue needs a public "has pending" check next to the fd accessor.
- Remote tree syncs with `tar` must reset mtimes (`-m`), or incremental builds reuse stale binaries and report false results.

## Followup

- SOW-0064: Windows Named Pipe pipelining progress (overlapped I/O), including has-pending parity.
- SOW-0050: the consolidated pre-vendoring benchmark run covers this SOW's rows.

## Regression Log

None yet.
