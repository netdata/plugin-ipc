# SOW-0064 - Windows Named Pipe Pipelining Without Deadlock

## Status

Status: completed

Sub-state: delivered 2026-11-04; pipeline-batch benchmark cost accepted by the user (decision 8A).

## Requirements

### Purpose

A client that pipelines over the Windows named-pipe baseline must not deadlock when the bytes in flight exceed the pipe buffers, matching the POSIX guarantee from SOW-0062.

### User Request

2026-11-02, decision 6A: give Windows the same guarantee with overlapped I/O, in its own SOW.

### Assistant Understanding

Facts (SOW-0062 Analysis, "Windows named pipes"):

- C, Rust and Go create synchronous, message-mode pipes with 64 KiB buffers per direction (`netipc_named_pipe.c:58-63`, `:218-222`, `:986-990`; Rust `windows.rs:1164-1176`; Go `pipe_listener.go:268-276`); each packet is a blocking `WriteFile` with no timeout.
- With more than about 64 KiB in flight, client and server can both block in `WriteFile`.
- The Windows benchmark clients avoid it with a byte budget (`bench_windows.c:2007`, `:2052-2063`; Go and Rust Windows drivers mirror it).
- A synchronous pipe handle has no per-call non-blocking write; progress needs overlapped I/O (`FILE_FLAG_OVERLAPPED` on both ends) or `PIPE_NOWAIT` (kept by Microsoft only for LAN Manager compatibility).
- Netdata's Windows consumers use typed one-in-flight calls only.

Unknowns:

- The exact overlapped design per language (event per operation, cancellation, interaction with the existing `PeekNamedPipe` receive timeout), to be analyzed.

### Acceptance Criteria

- A Windows client pipelining beyond the pipe buffers completes in C, Rust and Go, with interop.
- One-in-flight calls keep their current behavior and performance (benchmark evidence).
- The interim Windows caller rule added by SOW-0062 is replaced by the real behavior in the docs.
- The Windows benchmark clients' byte budget is removed or justified.
- If responses read during a stalled send are queued in the session, the Windows session exposes the same has-pending check as POSIX (SOW-0062 decision 1: C `nipc_uds_session_has_pending`, Rust `UdsSession::has_pending`, Go `Session.HasPending`), and the handle accessor docs say to check it before waiting on the handle.

## Analysis

Sources checked (2026-11-03): C `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`, Rust `src/crates/netipc/src/transport/windows.rs`, Go `src/go/pkg/netipc/transport/windows/`, the Windows service layer in all three, `bench/drivers/*/bench_windows*`, Windows tests, `docs/level1-windows-np.md`, `docs/level1-transport.md`, `docs/netipc-integrator-skill.md`.

- No language uses overlapped I/O: every `ReadFile`, `WriteFile` and `ConnectNamedPipe` passes a NULL overlapped pointer (C `raw_send` `:242-254`, `raw_recv` `:288-304`, accept `:881`; Rust `raw_write` `:427-450`, `raw_recv` `:470-492`, `:922`; Go `rawWrite` `pipe.go:292-305`, `rawRecv` `:311-334`, `:136-142`).
- Server instances: `CreateNamedPipeW` with `PIPE_ACCESS_DUPLEX`, message mode, `PIPE_WAIT`, buffers `max(packet_size, 64 KiB)` (C `:208-226`, `:58-63`; Rust `:1157-1176`; Go `pipe_listener.go:255-283`). Clients: `CreateFileW` without `FILE_FLAG_OVERLAPPED`, then `PIPE_READMODE_MESSAGE` (C `:985-992`, `:1071`; Rust `:975-990`, `:760`; Go `pipe_session.go:172-175`, `:258`). The overlapped flag is per handle, so a client handle can be overlapped while the server's stays synchronous; the wire does not change.
- Send: one blocking `WriteFile` per packet; C chunks inline (`nipc_np_send` `:1145-1294`), Rust and Go use the shared framing senders (`framing::send_message`, `framing.SessionSend`), the same hooks SOW-0062 used for the POSIX pending queue. In-flight IDs live in the transport session (C `netipc_named_pipe.h:141-144`, Rust `:682`, Go `pipe_session.go:65`). Only Level 1 callers pipeline (benchmarks, tests); Level 2 Windows clients send one request at a time (C `netipc_service_win_client_call.c:69`, `:136`).
- Receive: readability by a `PeekNamedPipe` spin/sleep loop with abort and deadline (C `:396-457`, Rust `:540-633`, Go `pipe_session.go:99-145`, `pipe_receive.go:123-139`), then `ReadFile`; without timeout or abort, a plain blocking `ReadFile` (C `:1352-1357`). No Windows pending queue or has-pending check exists.
- Message larger than the receive buffer (`ERROR_MORE_DATA`): C and Rust fail the receive; Go returns the partial read as a complete message (`pipe.go:315-324`), so the rest of that message would be read as the next one. A cross-language difference.
- The pipe stays open under SHM sessions as the dead-peer probe (zero-wait `PeekNamedPipe`: C `netipc_service_win_internal.h:38-42`; Rust `client_call.rs:500`, `:589`, `:607`; Go `client_windows.go:228-233`).
- Handshake, rejection ack and accept use the same raw functions (C `:584-603`, `:686`, `:100`, `:933`; Rust `:1226-1243`, `:1299-1301`; Go `pipe_handshake.go:45`, `:51`, `:89`, `:140`, `:176`). On an overlapped handle every one of these calls must pass an `OVERLAPPED` (a NULL one can report completion wrongly), so converting a handle converts all of its I/O.
- Raw handle accessors are documented as wait objects (C `netipc_named_pipe.h:125`, `:343-350`; Rust `:707-710`; Go `pipe_session.go:82-85`), while the C code says waiting on the pipe handle is not reliable for readability (`:307-309`). An overlapped handle is also signalled by I/O completion. Only tests and the Rust impersonation check use them.
- Docs: `docs/level1-windows-np.md:80` says the pipe has "overlapped I/O capability", contradicting `:172`; the interim caller rule from SOW-0062 is `level1-windows-np.md:171-184`, `level1-transport.md:99-100`, `netipc-integrator-skill.md:202-204`.
- Benchmark byte budget: C `bench_windows.c:2007`, `:2052-2063`; Go `main_windows.go:1224`, `:1362`, drain `:1259`; Rust `bench_windows.rs:1571`, `:1615`, `:1627`.
- Tests: the largest Windows pipelining test keeps about 2 KB in flight (C `test_named_pipe.c:1482`, Rust `test_pipeline_chunked`, Go `TestPipePipelineChunked`); nothing reproduces the deadlock.
- Go without cgo: `syscall.ReadFile`/`WriteFile` take `*syscall.Overlapped`; `GetOverlappedResult`, `CancelIoEx` and `CreateEvent` exist. The `Overlapped` and buffer must stay alive until completion (per-session allocation; the Go heap does not move objects).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- A Windows client send is a blocking `WriteFile` on a synchronous handle (Analysis). With more than the pipe buffers in flight, the server blocks writing responses the client is not reading, stops reading requests, and the client's write never completes: both block. A synchronous handle cannot stop a write to read.

Evidence reviewed: Analysis; SOW-0062 (POSIX pending queue, `pendingBound`, has-pending) and its code (Go `transport/posix/uds_send.go`, `uds_session.go`); C `netipc_named_pipe.c` (raw I/O `:242-304`, waits `:307-487`, client handshake `:536-640`, connect `:984-1095`, close `:1100-1118`, send `:1145-1294`, receive `:1300-1499`); Rust `transport/windows.rs` (raw I/O `:426-492`, session `:660-800`); Go `transport/windows/pipe.go`, `pipe_session.go`, `pipe_send.go`, `pipe_receive.go`, `pipe_handshake.go`; the C session struct copies in the service layer (`netipc_service_win_client_connect.c:105`, `netipc_service_win_server_session.c:350`, ownership moves after connect/accept, as `recv_buf` already does).

Affected contracts and surfaces:

- Windows Named Pipe transport in C, Rust and Go (client side): handle flags, raw read/write, send progress, receive, close, wait-readable; new public has-pending (C `nipc_np_session_has_pending`, Rust `NpSession::has_pending`, Go `Session.HasPending`); handle accessor docs.
- C `nipc_np_session_t` gains fields (source-level; vendored, no binary ABI).
- Wire format, server side, SHM, Level 2 APIs: unchanged.
- Windows benchmark clients (byte budget removed), Windows tests, docs (`level1-windows-np.md`, `level1-transport.md`, `netipc-integrator-skill.md`).

Existing patterns to reuse: SOW-0062's pending queue (length-prefixed records, `pendingBound` formula, progress only for a client with more than one request in flight, protocol error and in-flight failure when the bound is exceeded); the existing `PeekNamedPipe` spin-then-sleep loop; the shared framing senders/receivers in Rust and Go.

Risk and blast radius:

- Every client read/write changes (handshake included): a NULL overlapped pointer on an overlapped handle would misreport completion, so all client I/O goes through the overlapped helpers; server I/O keeps the synchronous helpers (decision 2A).
- An `OVERLAPPED` and its buffer must stay valid until completion: no I/O is left posted when a call returns; a failure while a write is posted cancels it (`CancelIoEx`) and waits for completion first. C and Rust keep the `OVERLAPPED` on the stack of the call; Go keeps them in a per-session heap object, because goroutine stacks can move while a write is posted.
- One-in-flight calls: one extra completion wait per read/write (data is already present after the peek loop, so reads complete at once). Benchmark evidence required.
- Bound exceeded or a protocol failure while draining: the session is unusable; it is closed for I/O and every in-flight request fails, as on POSIX.

Sensitive data handling plan:

- Evidence is code paths, test output and benchmark numbers only; hosts by role; no workstation paths.

Implementation plan:

1. Go: overlapped client handle and per-session events; overlapped raw read/write; `ERROR_MORE_DATA` fails the receive (decision 6A, test updated); pending queue, progress send, `HasPending`, wait-readable honouring the queue; close releases events.
2. Rust: the same in `transport/windows.rs`.
3. C: the same in `netipc_named_pipe.c` / `.h`.
4. Tests per language: a deadlock reproduction (a client pipelining beyond 2 x 64 KiB before reading, against a server that answers each request), first shown to hang/fail on the old code where feasible; has-pending; partial-message error (Go). Interop: a pipelining-beyond-buffer case in the Windows named-pipe interop.
5. Benchmarks: remove the Windows byte budget in C, Rust and Go drivers (decision 5A).
6. Docs: `level1-windows-np.md` (`:80` correction, Pipelining section, has-pending, handle accessor), `level1-transport.md:99-100`, `netipc-integrator-skill.md:202-204`.

Validation plan:

- Windows host: C CTest, Rust and Go suites, named-pipe interop; Linux: Windows cross-builds (`GOOS=windows go vet`, Rust Windows clippy target), Linux suites unaffected but compiled.
- Benchmark: the Windows suite (`tests/run-windows-bench.sh`) once after all fixes; one-in-flight rows compared with `benchmarks-windows.csv`; pipelining rows run without the byte budget.

Artifact impact plan:

- AGENTS.md: unaffected (no workflow change).
- Runtime project skills: unaffected.
- Specs: `docs/level1-windows-np.md`, `docs/level1-transport.md` updated.
- End-user/operator docs: same files; benchmark report if regenerated.
- End-user/operator skills: `docs/netipc-integrator-skill.md` Windows pipelining rule.
- SOW lifecycle: in-progress in current; one commit with the move to done.

Open-source reference evidence: none; Win32 behaviour from the API contracts, verified by tests on the Windows host.

Performance assessment: per-request code on an established session changes (client read/write on Windows named pipes); the Windows benchmark suite runs once after all fixes.

Open decisions: none (1A, 2A-7A recorded).

## Implications And Decisions

- Mechanism (2026-11-02, user: "A"): overlapped I/O, own SOW, after SOW-0062.
- Overlapped ends (2026-11-03, user: "i like your recomendations", decision 2A): only the client handle (`CreateFileW`) is opened with `FILE_FLAG_OVERLAPPED`; server instances, accept, the server handshake and the server session loop stay synchronous. Rejected: B, both ends (rewrites accept and the server paths without being needed to break the deadlock).
- Stalled-send progress (decision 3A): the client posts an overlapped `WriteFile` and, while it is not complete, waits on its event in short slices; between slices it peeks and reads any arriving message into a per-session pending queue; receive returns queued messages first. No read is ever left posted when a call returns. Rejected: B, a posted overlapped read alongside the write (needs `CancelIoEx`, races with every `PeekNamedPipe` user).
- One-in-flight receive (decision 4A): the peek-then-read loop with its timeout and abort stays; reads on the overlapped client handle pass an `OVERLAPPED` and are waited to completion at once. Rejected: B, a receive redesign around a posted read plus abort event.
- Benchmark byte budget (decision 5A): removed from the C, Rust and Go Windows benchmark clients. Rejected: B, keep it.
- Go `ERROR_MORE_DATA` (decision 6A): Go fails the receive like C and Rust, with a reproduction test. Rejected: B, a separate SOW.
- Stalled-send yield budget (2026-11-03, assistant decision under the autonomous-execution instruction, within 3A, for user review): the yield phase before the wait slices is bounded by time (1 ms since the last progress) instead of 256 yields. Evidence: Execution Log, pipeline-batch investigation (E1 recovers the rows; the slices caused the loss). Rejected: pure yielding (E1; burns a core for as long as a server stalls), a drain before every pipelined write (E2; small pipelines 0.63x), a posted read alongside the write (3B, rejected earlier). Cost: a stalled pipelined client keeps one core busy for up to 1 ms per stall before it sleeps.
- Pipeline-batch benchmark cost (2026-11-04, user: "1a", decision 8A): accepted. np-pipeline-batch-d16 is about 10% lower for C and Rust clients and 15-20% lower for Go clients than HEAD's byte-budgeted clients (Execution Log, residual gap investigation); one-in-flight and small-message pipeline rows are equal or better. Rejected: B, profile further (needs an ETW tool install on the Windows host, uncertain gain); C, overlapped only while pipelining (does not help the pipelined row, adds a second code path in three languages).
- Handle accessors (decision 7A): the accessor docs say the pipe handle is not a readability signal and callers must check the new has-pending first; has-pending is added to the Windows session in C, Rust and Go. Rejected: B, removing the accessors (API break).

## Plan

After analysis.

## Execution Log

### 2026-11-02

Created from SOW-0062.

### 2026-11-03

- Go: `pipe_overlapped.go` (new: overlapped helpers, `pipeIO` heap-held `OVERLAPPED`s with manual-reset events, `writeWithProgress`, `drainPending`, `breakSession`, `pendingQueue`, `pendingBound`); `pipe.go` (`rawRecv` fails on `ERROR_MORE_DATA`); `pipe_session.go` (overlapped client handle, `HasPending`, `WaitReadable` honours the queue, `Close` releases events and queue, handle doc); `pipe_handshake.go`, `pipe_send.go`, `pipe_receive.go` (client I/O through the session helpers, progress bound when more than one request is in flight, receive pops the queue first). `GetOverlappedResult` and `CreateEventW` are called through kernel32 procs (`syscall` lacks them); no cgo.
- Rust: `transport/windows.rs`, the same design (`PipeIo`, `PendingQueue`, `write_packet` with progress, `has_pending`); the `OVERLAPPED` is a stack local of each call, which waits for completion before returning.
- C: `netipc_named_pipe.c` / `.h`, the same design (session events and queue fields, `pipe_send`, `pending_drain`, `break_session`, `nipc_np_session_has_pending`); server sessions are zeroed, so their events stay NULL and their I/O synchronous.
- Tests: Go `TestPipePipelineBeyondPipeBuffers`, `TestPendingQueuePopTooSmall`, `TestPipeClientReceiveMoreDataFails` (and `TestRawRecvMoreData` replaced by `TestRawRecvMoreDataFails`); Rust `test_pipeline_beyond_pipe_buffers`, `test_pending_queue_pop_too_small`; C `test_pipeline_beyond_pipe_buffers` in `test_named_pipe.c`. Each pipelines 32 x 16 KiB requests (512 KiB each way, 8x the pipe buffers) before reading, against a server that answers each with blocking writes; a 30 s watchdog only reports a deadlock. Interop: `pipeline-server` / `pipeline-client` modes in the three `interop_named_pipe` fixtures; `tests/test_named_pipe_interop.sh` runs the 9 pairs once per mode.
- Red check: the Go reproduction (without the new APIs) ran on the Windows host against the old code and hung until Go's 10-minute test timeout (`FAIL ... 601.289s`, goroutine dump in the client send); the temporary test file was removed from the host afterwards.
- Benchmarks (decision 5A): the byte budget and its accounting (`inflight_budget`, per-slot cost, `outstanding`) removed from `bench_windows.c`, Go `main_windows.go` (warmup and measured loop), Rust `bench_windows.rs`; each cycle sends the whole depth, then reads.
- Docs: `level1-windows-np.md` (connection lifecycle states which handle is overlapped; Pipelining section describes the real behaviour, bound, failure, peer close, has-pending and the handle accessor), `level1-transport.md` (Pipelining scope; Windows wait object note), `netipc-integrator-skill.md` (Windows pipelining rule).

- Independent review (read-only agent over the full diff): no confirmed bug; findings and handling under Validation. Fixes applied: Go `pipeIO` holds the posted read/write buffers (`readBuf`, `writeBuf`), so every buffer handed to the kernel escapes to the heap (`go build -gcflags=-m`: `leaking param: buf/data/msg`); a non-disconnect `PeekNamedPipe` error while draining now fails the send (protocol error, cancel, break) in C, Rust and Go instead of stopping the drain; the Rust reproduction test reports a client panic instead of "deadlocked"; doc wording (wait slices, bound, peek failure, oversized message).

- Benchmark round 1 (NP scenarios: `np-ping-pong`, `np-batch-ping-pong`, `snapshot-baseline`, `np-pipeline-d16`, `np-pipeline-batch-d16`; SHM and lookup rows do not run the changed client pipe I/O: SHM data moves through shared memory and its pipe sees only the handshake and the zero-wait `PeekNamedPipe` probe). Against `benchmarks-windows.csv` (2026-06-15) every NP max-rate row was lower, the pipeline rows about 0.4x; a same-host A/B against HEAD (5e6bfb7) showed that drift is the host, not this change (np-pipeline-d16 c->c: HEAD 98.4k, change 109.7k).
- A/B against HEAD, same host, same session, 5 x 20 s samples per row:
  - np-ping-pong max (two runs, both orders): throughput equal or higher (c->c 44.2k/48.2k vs 44.2k; rust->c 44.0k vs 38.4k; go->c 41.9k-45.8k vs 39.0k-41.2k), p50 equal or lower; client CPU 7-11 points higher at max rate, no consistent difference at 10k req/s (82.7/76.7/86.8% vs 85.1/90.1/80.0%).
  - np-batch-ping-pong max: c->c 8.99M vs 8.78M, go->c 8.87M vs 8.26M.
  - np-pipeline-batch-d16: 15-23% lower (c->c 16.6M vs 19.4M; rust->c 15.6M vs 18.7M; go->c 17.0M vs 22.0M), p50 up about 100 us, server CPU down (the server blocked).
- Pipeline-batch regression investigation:
  - Go and Rust zeroed a full packet slot per drained message (`append(make(...))`, `resize(.., 0)`); fixed (the queue storage only grows and is zeroed once; Rust `PendingQueue` keeps `end`, Go `pendingQueue.grow`). Not the main cause: the rows stayed 15-23% lower.
  - C experiment (scratch tree on the Windows host, c->c, same session): current code 16.9M (p50 480 us); E1, yield without the 1 ms slices, 22.8M (p50 323 us), np-pipeline-d16 111k; E2, a drain before every pipelined write, 19.0M but np-pipeline-d16 70k. Cause: after 256 yields (about 100 us) the send slept in `WaitForSingleObject(write_event, 1)`, whose slice lasts a timer tick and does not end when the server's reply arrives, so the server stayed blocked on its reply (review finding 2).
  - Fix: the stalled send yields for up to 1 ms after the last progress (`NP_PROGRESS_YIELD_US`, Rust `PROGRESS_YIELD`, Go `progressYield`; QPC / `Instant` / `monotime`, read only while a write is stalled), then waits in slices as before.
- Benchmark round 2 (all NP scenarios, after the yield fix; 108 rows, rc 0): one-in-flight rows hold (np-ping-pong max c->c 44.7k, rust->c 46.4k, go->c 44.5k; np-batch-ping-pong max c->c 8.85M, go->c 8.63M; np-pipeline-d16 c->c 102.5k vs HEAD 98.4k). np-pipeline-batch-d16 is still below HEAD: c->c 17.6M vs 19.4M, rust->c 17.2M vs 18.7M, go->c 15.6M vs 22.0M. A 1 ms yield does not cover the stalls; investigated next.
- Residual pipeline-batch gap investigation (C variants in the scratch tree, same host, np-pipeline-batch-d16, items/s):
  - HEAD is noisy run to run: c->c 19.4M, 18.2M, 21.4M; go->c 20.9M, 18.6M, 21.3M.
  - Wait variants, c->c: y1 (current, 1 ms yield) 15.6M-16.6M; i1 (y1 instrumented) 15.4M; e1 (pure yield, no slices) 17.6M; y16 (16 ms yield) 18.4M; hr (1 ms yield, then 500 us high-resolution waitable-timer slices) 16.5M. No variant reaches HEAD consistently, so the wait slices are not the main cause in this session.
  - Current transport with HEAD's byte-budgeted drivers: c->c 18.5M, rust->c 20.2M, go->c 18.7M vs HEAD 21.4M, 18.4M, 21.3M in the same session. The budgeted client never stalls a write, so the remaining difference is the per-call cost of the overlapped client handle (client CPU 75% vs 66%; np-pipeline-d16 64% vs 43%), which buys no throughput loss on the small-message rows (np-pipeline-d16 c->c 102.8k vs 94.0k).
  - Conclusion: pipeline-batch rows are about 10% lower for C and Rust clients and 15-20% lower for Go clients, inside a noise band of about 10%; one-in-flight and small-message pipeline rows are equal or better. Decided 2026-11-04 (decision 8A): accepted.

## Validation

Acceptance criteria:

- Pipelining beyond the pipe buffers completes in C, Rust and Go: `test_pipeline_beyond_pipe_buffers` (C, 7 checks), Rust `test_pipeline_beyond_pipe_buffers`, Go `TestPipePipelineBeyondPipeBuffers` pass on the Windows host; the same Go test hung to the 10-minute timeout on the old code (Execution Log, red check). Interop: `tests/test_named_pipe_interop.sh` 18/18 (9 pairs single, 9 pairs pipelining 32 x 16 KiB).
- Has-pending and wait-readable honouring the queue: asserted in all three reproduction tests (queued after the sends, `wait_readable(0)` true, drained at the end).
- Docs: interim caller rule replaced (`level1-windows-np.md` Pipelining, `level1-transport.md`, `netipc-integrator-skill.md`).
- Benchmark byte budget removed in C, Rust and Go Windows drivers.

Tests (Windows host, after the review fixes): Rust `cargo test --lib` 275 passed (stable and 1.91.0-msvc); Go `go test ./...` all packages ok (includes the Windows allocation tests); CTest 20/20 (`test_named_pipe` 301 checks, `test_named_pipe_go`, `test_named_pipe_interop`). Linux: `go vet ./...`, `GOOS=windows go vet ./...` (only the pre-existing `shm.go` unsafe.Pointer warnings), `cargo check --all-targets`, `cargo clippy --target x86_64-pc-windows-gnu --all-targets` (no correctness/suspicious lints; style lints pre-existing or following the FFI naming), `cargo fmt --check`, zig `x86_64-windows-gnu` compiles of the C transport, test, interop fixture and bench driver.

Reviewer findings:

1. Go read buffers could sit on a movable goroutine stack while a read is posted (risk): fixed (buffers pinned through `pipeIO`).
2. The 1 ms wait slice is rounded up by the system timer (perf risk): kept, as decided in 3A and as the existing receive wait loops do (`Sleep(1)`); the slice ends early when the write completes; doc wording corrected; checked by the pipeline rows of the benchmark run.
3. Doc bound omitted the 4-byte queue record (nit): doc fixed (also the same wording in `level1-posix-uds.md`, whose SEQPACKET queue keeps the same record).
4. Any peek error stopped draining, which could bring the deadlock back on a live pipe (risk): fixed in C, Rust and Go.
5. Oversized-message state differs between queue (message dropped) and pipe (rest stays) (nit): documented; behaviour matches C/Rust before this SOW.
6. Rust test reported a client panic as a deadlock (nit): fixed.

Benchmark (Test Cadence): one run of all NP scenarios after all fixes (round 2, 108 rows, rc 0); regressions analysed with same-host A/B variants and HEAD brackets (Execution Log). One-in-flight rows: throughput equal or higher than HEAD; `benchmarks-windows.csv` (2026-06-15) is not comparable (host drift shown by the HEAD A/B). Pipelining rows ran without the byte budget. Floors (`tests/generate-benchmarks-windows.sh` floor rules): the only NP floor, np-ping-pong max > 0, holds on every row. The pipeline-batch cost is accepted (decision 8A). The committed report is regenerated by the full POSIX and Windows run that SOW-0050 schedules before vendoring.

Real-use evidence: the interop suite runs real C, Rust and Go client and server processes over Named Pipes, single and pipelined (18/18); the benchmark drivers pipeline 16 requests without the byte budget across all 9 language pairs (rc 0, no hang).

Same-failure search: no other Windows pipelining workaround (`grep` for `packet_size * 2`, `inflight_budget`, `inflightBudget` in `src/`, `bench/`, `tests/`: none left); every client read/write goes through the overlapped helpers (reviewer confirmed; remaining `raw_*` calls are server handles or test-created synchronous pipes).

## Outcome

Windows Named Pipe clients in C, Rust and Go can keep more requests in flight than the pipe buffers hold: a stalled client write reads arriving responses into a bounded per-session pending queue, so the client and server both keep making progress. The client handle is overlapped; server handles stay synchronous. Callers no longer need a byte budget, and the benchmark drivers no longer carry one. The cost is a higher client CPU per call and about 10-20% on the batched pipeline benchmark row, accepted by the user.

Artifact maintenance gate:

- AGENTS.md: no update; no workflow, command or guardrail changed (the Windows suites and bench commands are the existing ones).
- Runtime project skills: no update; `project-netdata-vendoring` workflow is unchanged, and this SOW's code reaches Netdata through SOW-0050's normal preflight.
- Specs: `docs/level1-windows-np.md` (handle kinds, Pipelining section, has-pending, accessor rule), `docs/level1-transport.md` (Pipelining scope, Windows wait object), `docs/level1-posix-uds.md` (bound wording includes the queue record). `.agents/sow/specs/` holds no Named Pipe rule, so it needs no change.
- End-user/operator docs: the same `docs/` files are the published guides; the benchmark report is regenerated with SOW-0050's full run.
- End-user/operator skills: `docs/netipc-integrator-skill.md` Windows pipelining rule replaced (no byte budget; check has-pending before waiting on the handle).
- SOW lifecycle: completed, moved to `done/` in the same commit; follow-up mapping below.

## Lessons Extracted

- Windows wait slices last a system timer tick (about 15.6 ms), not the requested 1 ms; a progress loop that sleeps early stalls its peer.
- Same-host A/B runs need HEAD runs before and after the variants: HEAD alone moved 18.2M-21.4M items/s between runs on the pipeline-batch row, more than most of the effects measured.
- A benchmark driver change (here the removed byte budget) changes what a row measures; isolate it by running the new transport with the old driver before blaming the transport.
- Processes started by a detached ssh session on the Windows host fail with 0xC0000142 once ssh closes; hold the ssh session open for long runs (recorded in assistant memory, not in the repo).

## Followup

- Regenerated Windows benchmark report: tracked by SOW-0050 (full POSIX and Windows run before vendoring, line "Ordering (2026-10-31, user)").
- Further pipeline-batch profiling: rejected by the user (decision 8A).
- Deferral search (`defer|later|follow-up|future|TODO|pending`): remaining hits are the has-pending API name and the pending queue, not deferred work.

## Regression Log

None yet.
