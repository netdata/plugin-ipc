# SOW-0052 - Transport Hygiene

## Status

Status: completed

Sub-state: completed 2026-10-31; the SHM-timing item moved to SOW-0055 (decision 4).

## Requirements

### Purpose

Fix the remaining transport-level defects found by SOW-0040, SOW-0046 and SOW-0047: EINTR handling, the stale-socket recovery race, failure classification and SHM resource timing.

### User Request

From SOW-0040 decisions (2026-10-29): fix all reproduced and review-found defects under the zero-known-bug policy. SOW-0043 was split on 2026-10-31 (assistant process decision, recorded in SOW-0043); this SOW carries the transport-level items.

### Assistant Understanding

Facts (line numbers verified 2026-10-31 against commit `b450b40`):

- N9: EINTR is treated as a disconnect. C `netipc_uds.c:45-47` (send), `:74-76` (sendmsg), `:83` (recv), with handshake callers returning `ERR_RECV` (`netipc_uds_handshake.c:162-164`, `:214-216`); Rust `posix.rs:908-910` (`raw_send_iov`), `:917-927` (`raw_send`), `:932-937` (`raw_recv`); only the poll at `posix.rs:999` handles EINTR.
- Stale-socket recovery can unlink a live server's endpoint (SOW-0046 working theory; user decision: reproduction test first). A server's socket file exists from `bind()` until `listen()`, and connects in that window are refused. C `nipc_uds_check_and_recover_stale` (`netipc_uds_lifecycle.c:181-212`, unlink at `:205-208`; bind `:252`, listen `:258`) and Rust (`posix.rs:1104-1144`, unlink `:1123-1136`) unlink on any non-ENOENT connect error after one probe; Go (`uds_stale.go:27-44`, unlink `:68`) retries ECONNREFUSED 3 times with 50 ms delays, which narrows but does not close the window, and is a cross-language difference.
- Failure classification: after SOW-0044, a send/recv failure reports DISCONNECTED when a peer probe confirms the peer is gone, but SHM receive errors other than timeout still return TRUNCATED (C `netipc_service_posix_client_call.c:120`; Rust `client_call.rs:475`, `:500`, `:517`; Go `client_unix.go:164`, `:232`), and without a probe hit all three fall back to TRUNCATED ("buffer too short") for what is a transport failure.
- SHM regions are created before HELLO is read for every accepted connection (C POSIX `netipc_service_posix_server.c:206-207`, C Windows `netipc_service_win_server.c:107-111`, Rust `server_unix.rs:43`/`:137-143`, `server_windows.rs:28`/`:133`, Go `server_unix.go:123`), so every connection, including rejected and silent ones, costs a region create and destroy. SOW-0043 moves this work into the session worker but keeps it before HELLO.

Inferences:

- EINTR during a handshake on a plugin that receives signals (Netdata plugins do) turns a transient signal into a failed connect.

Unknowns:

- Whether the stale-socket race is reachable in practice; the reproduction test decides.

### Acceptance Criteria

- Tests prove EINTR is retried in C and Rust send/recv paths.
- A reproduction test for the stale-socket race exists; if it reproduces, all three languages close the window with one shared rule.
- Transport failures are not reported as TRUNCATED in any language.
- SHM regions are created only for sessions that negotiate SHM, or the cost is justified with evidence. (Moved to SOW-0055, decision 4.)

## Analysis

Sources checked:

- SOW-0040, SOW-0046, SOW-0047 evidence; SOW-0043 facts (verified 2026-10-31).

Current state:

- None of these items has a reproduction test yet.

Risks:

- Error-code reclassification is visible to callers; Netdata treats any non-OK as failure, so the impact is in logs and diagnostics.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- EINTR: the raw send/recv helpers return the first failure. A signal that arrives before any data moves makes the call fail, and the session is torn down. SOCK_SEQPACKET sends whole messages or nothing, so a retry is safe.
- Stale socket: a server's socket file exists from `bind()` until `listen()`, and connects in that window are refused, exactly like a dead server's leftover file. A connect probe cannot tell them apart, so a second starter can unlink a live server's endpoint, and both servers then report success while the first is unreachable.
- Classification: the L2 clients have no error for a transport I/O failure, so they borrow `TRUNCATED` ("buffer too short"), and C also borrows `NOT_READY` for send failures.

Evidence reviewed (re-verified 2026-10-31 against `6609157`):

- C `netipc_uds.c:43-48` (`nipc_uds_raw_send`), `:50-78` (`nipc_uds_raw_send_iov`), `:80-83` (`nipc_uds_raw_recv`); callers `netipc_uds_handshake.c:77,159,168,220,307`, `netipc_uds_receive.c:174,242`, `netipc_uds_send.c:109,124,155`. Rust `posix.rs` `raw_send_iov`, `raw_send`, `raw_recv` (only the poll at `posix.rs:1050` handles EINTR). Go needs no change: the Go runtime installs its signal handlers with SA_RESTART, and the Go poll loops retry EINTR (`uds_receive.go:79`, `uds_handshake.go:53`).
- Stale recovery: C `netipc_uds_lifecycle.c:188-219`, bind `:259`, listen `:265`; Rust `posix.rs:1155-1195`; Go `uds_stale.go` (3 dials, 50 ms apart).
- Classification: C `netipc_service_posix_client_call.c:58` (SHM send -> `NOT_READY`), `:68` (UDS send -> `NOT_READY`), `:120` (SHM receive -> `TRUNCATED`), `:145` (UDS receive -> `TRUNCATED`); Windows `netipc_service_win_client_call.c:115,139`; Rust `client_call.rs:218,252,258,294,320,334,386-402,478,503,520,536,548,567`; Go `client_unix.go:186,193,213,254,257,270`, `client_windows.go:175,181,204,256,259,271`. `nipc_shm_receive` fails, apart from timeouts, only with `MSG_TOO_LARGE`, `BAD_HEADER` (zero-length message) and `BAD_PARAM`.

Affected contracts and surfaces:

- L1 POSIX send/recv (C, Rust); POSIX listener startup (C, Rust, Go) and a new per-service lock file; L2 error codes (new transport error in C, Rust, Go); `docs/level1-posix-uds.md`, `docs/level2-typed-api.md`, `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- C link-time syscall wrappers (`tests/fixtures/c/shm_fallocate_wrap.c`, `--wrap=fallocate`) for deterministic EINTR injection with no production change.
- Test hooks: C `*_for_tests` setters, Rust `#[cfg(test)]` statics (`CONNECT_BUSY_HOOK`), Go package hooks (`connectBusyHook`).
- SOW-0044's peer probe for `DISCONNECTED`.

Risk and blast radius:

- EINTR retry: every UDS send/recv; behaviour changes only when a signal interrupts a call.
- Lock file: every POSIX server start; adds `{service}.lock` in the run directory. A server of an older version takes no lock, so the connect probe stays as a second guard.
- Error codes: callers that log the error see the transport error instead of "buffer too short" or "not ready"; retry behaviour is unchanged, because the new error follows the same reconnect-once path. The C enum gains a value at the end; existing values keep their numbers.

Performance assessment:

- EINTR: the raw send/recv helpers run per request on established UDS sessions (`netipc_uds.c:43-83`, Rust `raw_send_iov`/`raw_recv`). The added work is one `errno` comparison, made only after a failed syscall; the success path is unchanged. Not measurable, so no benchmark run.
- Lock file: listener startup only.
- Classification: error paths only.

Sensitive data handling plan:

- No secrets involved. Applies to this SOW, the docs, the integrator skill and code comments.

Implementation plan:

1. EINTR: retry in C `nipc_uds_raw_send`, `nipc_uds_raw_send_iov` and `nipc_uds_raw_recv`, and in Rust `raw_send_iov`, `raw_send` and `raw_recv`. Tests: a C link wrapper injects one EINTR into `send`/`sendmsg`/`recv`; a Rust `cfg(test)` hook does the same.
2. Stale socket: per-service lock (decision 2) in C, Rust and Go; remove Go's dial retries. Reproduction test in each language: a listener paused between bind and listen (test hook), and a second listener on the same service, which must fail with address-in-use while the first stays reachable.
3. Classification: a new transport error and the mapping in decision 3, in C, Rust and Go, POSIX and Windows. Update the tests that expect the old codes, and add tests for the new mapping.
4. Docs and integrator skill.

Validation plan:

- New tests fail on the previous code and pass now; Linux full ctest including interop, `cargo test`, `go test`; Windows ctest, Rust and Go; the 9 Netdata Linux targets; rustfmt, CI clippy gate, gofmt, go vet.

Artifact impact plan:

- AGENTS.md: no workflow change expected.
- Runtime project skills: none expected.
- Specs: `docs/level1-posix-uds.md` (EINTR, lock file, stale rule), `docs/level2-typed-api.md` (transport error).
- End-user/operator docs: same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (transport error, lock file in the run directory).
- SOW lifecycle: SHM timing moved to SOW-0055.

Open-source reference evidence:

- None.

Open decisions:

- None; the decisions below are assistant-applied under the user's autonomy instruction (long-term-best).

## Implications And Decisions

1. EINTR is retried inside the raw send/recv helpers in C and Rust. Reason: SOCK_SEQPACKET moves whole messages or nothing, so a retry cannot duplicate or split data; fixing it once at the lowest layer covers handshakes and sessions.
2. Stale-socket rule shared by C, Rust and Go: a listener first takes an exclusive, non-blocking `flock` on `{run_dir}/{service}.lock`, created owner-only (0600) with `O_CLOEXEC | O_NOFOLLOW`, and holds it until the listener closes. If the lock is held, it fails with address-in-use. Once it holds the lock, a refused probe means a dead server, so it unlinks the file, binds and listens. The connect probe stays, for servers of older versions that take no lock. The lock file is not deleted at close: deleting it would reopen the race for a starter that already opened it. Go's three dials 50 ms apart are removed. Reason: the lock closes the bind-listen window completely; no probe timing can. `flock` locks are per open file description, so they work between processes and between listeners in one process, and the kernel releases them when the process dies.
3. New L2 error for transport I/O failures: C `NIPC_ERR_TRANSPORT` (appended to `nipc_error_t`), Rust `NipcError::Transport`, Go `protocol.ErrTransport`. Mapping, identical in all three:
   - socket or pipe send/recv failure, peer gone per probe -> disconnected (unchanged);
   - any other socket or pipe send/recv failure -> transport;
   - SHM send failure other than message-too-large -> transport (disconnected when the probe finds the peer gone);
   - SHM receive `MSG_TOO_LARGE` -> overflow, as a UDS receive over the limit already is;
   - SHM receive of a message shorter than a header, including zero length -> truncated (it is a truncated message);
   - SHM receive `BAD_PARAM`, or a READY client with no session -> transport.
   Reason: callers and logs see what failed. The retry path is unchanged.
4. The SHM-timing item moves to SOW-0055. Reason: it needs a new step inside the L1 server handshake, in three languages on two platforms, and is best reviewed and bisected on its own. It touches connection setup only.

## Plan

As in the Pre-Implementation Gate implementation plan, in that order.

## Execution Log

- 2026-10-31, item 1 (EINTR):
  - C `netipc_uds.c` `nipc_uds_raw_send`, `nipc_uds_raw_send_iov` and `nipc_uds_raw_recv` loop on EINTR.
  - Rust `posix.rs` gains `retry_eintr` with a `#[cfg(test)]` thread-local injector (`eintr_inject`); `raw_send_iov`, `raw_send` and `raw_recv` use it.
  - Tests:
    - C `test_eintr_retried` (`tests/fixtures/c/test_uds.c`) uses a new link wrapper, `tests/fixtures/c/uds_eintr_wrap.c/.h` (`--wrap=send,sendmsg,recv`, non-Apple builds only), and injects 2, 2 and 4 EINTRs;
    - Rust `test_eintr_retried` (`posix_tests.rs`).
  - Both fail on the previous code.
- 2026-10-31, item 2 (endpoint lock):
  - C `netipc_uds_lifecycle.c` `acquire_endpoint_lock` takes the lock before the stale check; every error path closes it, and `nipc_uds_close_listener` releases it after the unlink. The listener struct gains `has_lock`/`lock_fd`, and the test hook `nipc_uds_set_listen_hook_for_tests` runs between bind and listen.
  - Rust: `UdsListener._lock`, `acquire_endpoint_lock`, `#[cfg(test)] LISTEN_HOOK`.
  - Go: `acquireEndpointLock` in `uds_stale.go`, `Listener.lock` released last in `Close`, and a `listenHook`. `dialStaleCandidate` dials once (the three dials 50 ms apart are removed).
  - Reproduction tests: C `test_listen_excludes_starting_server`, Rust `test_listen_excludes_starting_server`, Go `TestListenExcludesStartingServer`.
    - Each pauses a listener between bind and listen and starts a second one. The second must fail with address-in-use while the first stays reachable.
    - All three reproduce the race (the second listener unlinks the live endpoint) on the previous code, and pass now.
  - `TestUdsListenFailsWhenRunDirMissing` now expects the lock-open failure detail, because the lock is the first file the listener creates.
- 2026-10-31, item 3 (classification):
  - New error appended in all three languages: C `NIPC_ERR_TRANSPORT`, Rust `NipcError::Transport` ("transport I/O failed"), Go `protocol.ErrTransport`.
  - Mapping applied in C (`netipc_service_posix_client_call.c`, `netipc_service_win_client_call.c`), Rust (`client_call.rs`: `socket_failure`, `session_failure`, `shm_send_failure`, `pipe_failure`, `map_uds_receive_error`, `map_np_receive_error`) and Go (`client_unix.go`, `client_windows.go`: `socketFailure`, `pipeFailure`, `shmSendFailure`):
    - socket/pipe send/recv failure: disconnected when the probe finds the peer gone, transport otherwise;
    - SHM send failure other than too-large: disconnected when the probe finds the peer gone, transport otherwise;
    - SHM receive: too-large is overflow, `BAD_HEADER` (zero-length or corrupt slot) is truncated, anything else is transport;
    - no session is transport.
  - Other protocol errors (protocol violation, chunk mismatch, unknown message id) keep their previous mapping.
  - Parity fix found on the way: Go mapped a baseline receive over the limit to truncated, while C and Rust mapped it to overflow. Go now maps it to overflow, on POSIX and Windows.
  - Tests:
    - New: C `test_send_failure_with_live_peer_is_transport` (`test_service.c`), Rust `test_send_failure_with_live_peer_is_transport_unix` and `test_transport_without_session_unix`, Go `TestUnixSendFailureWithLivePeerIsTransport`. In each, the client shuts down its own write side while the server keeps the session open and has already removed the endpoint, so the send fails with EPIPE, the probe finds the peer alive, and the reconnect fails.
    - Updated to the new codes: C oversize SHM response (overflow); Rust `test_transport_without_session_windows` and `nipc_error_display_all_variants`; Go `TestUnixClientTransportWithoutSession`, `TestUnixClientTransportReceiveShmError`, `TestWinClientTransportWithoutSession` and `TestWinClientTransportReceiveWinShmError`.
    - Mutation check: with the previous mapping restored, the C, Rust and Go tests fail (C gets `NOT_READY`, Rust and Go get truncated / "buffer too short").
- 2026-10-31, item 4 (docs):
  - `docs/level1-posix-uds.md`: endpoint lock, interrupted system calls, lifecycle step 1.
  - `docs/level2-typed-api.md`: transport error and SHM mapping.
  - `docs/netipc-integrator-skill.md`: transport error, lock file.

## Validation

Acceptance criteria evidence:

- EINTR is retried in C and Rust send/recv paths:
  - C `test_eintr_retried` injects EINTR into `send`, `sendmsg` and `recv` through the link wrapper and passes;
  - Rust `test_eintr_retried` passes;
  - both fail on the previous code.
- Stale-socket race:
  - The reproduction tests in C, Rust and Go reproduced it on the previous code: the second listener unlinked the paused first listener's endpoint.
  - With the shared lock rule, all three pass. The second listener fails with address-in-use and the first stays reachable.
- Transport failures are no longer reported as TRUNCATED:
  - the mapping in decision 3 is applied in C, Rust and Go, on POSIX and Windows;
  - the new live-peer tests in C, Rust and Go get the transport error, and they fail with the previous mapping (C `NOT_READY`, Rust/Go truncated);
  - the remaining truncated returns are genuine short-buffer or short-message cases (same-failure search below).
- SHM resource timing: moved to SOW-0055 (decision 4).

Tests or equivalent validation:

- Linux:
  - full ctest 32/32 (1 skipped by design: `test_shm_full_backing_store`), including the interop suites;
  - `cargo test`: 382 lib tests plus 1 integration test passed;
  - `go test ./...` ok;
  - `go test -race` on the new Go tests ok.
- Windows (costa-win11, MSYS2): ctest 20/20; Rust lib 264 passed; Go all packages ok.
- Netdata Linux targets: all 9 pass (C, Rust, Go, interop).
  - First run: x86_64-musl, i386-musl, armv7-musl, armv6-musl and aarch64-gnu passed. Four failed:
    - armv7-gnu-time64 C `test_eintr_retried`: a test-wrapper gap. With `_TIME_BITS=64`, 32-bit glibc redirects `sendmsg` to `__sendmsg64` (`sys/socket.h`), so `--wrap=sendmsg` missed it. Fixed by wrapping `__sendmsg64` too (guarded by `__USE_TIME_BITS64`, the same approach as the existing `fallocate64` wrapper). The library's retry loop was correct.
    - armv7-gnu, armv7-gnu-time64 and aarch64-musl Rust (qemu): about 30 tests timed out on the 10 s test-server listening wait (`test server is not listening`), and aarch64-musl C failed one helper start. Memory pressure (`some avg10`) was 20-60% during these runs; the cause was not attributed. The same aarch64-musl Rust binary passed 384/384 in 8 s once pressure dropped to about 0%.
    - i386-gnu C live-peer test: two native `test_service` runs (mutation check) overlapped that target's `test_service`, which uses the same fixed `/tmp` fixture paths. This broke the rule against parallel C fixtures.
  - Rerun, one target at a time with pressure sampling (max memory `some avg10` under 5%): i386-gnu, armv7-gnu-time64, armv7-gnu and aarch64-musl pass, with C 14/14 and Rust 384 passed on each.
- Linux native after the wrapper fix: ctest 32/32.
- Static checks:
  - `cargo fmt --check` clean;
  - CI clippy gate (`-D clippy::correctness -D clippy::suspicious`) clean for Linux and `x86_64-pc-windows-gnu`;
  - `gofmt -l` clean;
  - `go vet` clean on Linux. On `GOOS=windows`, `go vet` reports only the existing `unsafe.Pointer` notes in `transport/windows/shm.go`, a file this SOW does not touch.
- Windows C has no live-peer send-failure test: named pipes have no half-close. The Windows mapping is covered by the Rust and Go no-session tests and the invalid-WinSHM-context tests, and by the shared `pipe_failure` code path.

Real-use evidence:

- The interop suites (`test_uds_interop`, `test_shm_interop`, `test_service_interop`, cache interop) run C, Rust and Go servers and clients against each other, so servers that take the endpoint lock still serve clients of every language.
- Exclusion between servers of different languages is not tested directly. It follows from the identical lock path (`{run_dir}/{service}.lock`) and `flock` semantics: one lock per open file description, regardless of the language that opened it. Each language's own test proves the exclusion.

Reviewer findings:

- No external review requested; each change carries a reproduction or mutation-checked test.

Same-failure search:

- EINTR: `grep -rn "send(\|sendmsg(\|recv("` over the POSIX transports. Every raw call goes through the three C helpers or the three Rust helpers. Go is covered by the runtime's SA_RESTART handlers and its poll loops.
- Stale unlink: the C, Rust and Go stale-check functions are the only unlink sites for the socket path.
- Classification:
  - `grep -rn "ErrTruncated\|NipcError::Truncated\|NIPC_ERR_TRUNCATED\|NIPC_ERR_NOT_READY"` over the service layers.
  - The remaining truncated returns are encode-buffer checks (`increment`, `string_reverse`, `cgroups_snapshot`), messages shorter than a header, SHM `BAD_HEADER`, receive-side protocol errors, and Rust `response_payload` bounds checks on an already received message.
  - The remaining `NOT_READY` returns are calls on a client that is not READY. All of these keep their meaning.

Performance assessment:

- Unchanged from the gate. The EINTR check runs only after a failed syscall, the lock is taken at listener start, and the classification changes run only on error paths. No established-session hot path changed (`netipc_uds.c` success paths and Rust `raw_send_iov`/`raw_recv` success returns are unchanged), so no benchmark run.

Sensitive data gate:

- This SOW, the docs, the integrator skill and the code comments contain no secrets, credentials, customer or personal data, or private endpoints. The Windows host is named by lab hostname only.

Artifact maintenance gate:

- AGENTS.md: no change; no workflow changed.
- Runtime project skills: no change; vendoring steps unaffected.
- Specs:
  - `docs/level1-posix-uds.md`: endpoint lock, interrupted system calls, lifecycle step 1;
  - `docs/level2-typed-api.md`: transport error and SHM mapping.
- End-user/operator docs: the same `docs/` files; there is no other operator-facing surface.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (transport error, lock file).
- SOW lifecycle:
  - pending -> current -> done in this commit;
  - SHM timing is split to SOW-0055 (pending), committed together with this SOW.

## Outcome

- Signals no longer break UDS sessions or handshakes in C and Rust.
- Two POSIX servers that start at once for one service can no longer both claim the endpoint.
- L2 clients report a distinct transport error instead of "buffer too short" or "not ready" for I/O failures. SHM responses over capacity are reported as overflow in all three languages.

## Lessons Extracted

- A connect probe cannot tell a dead server's socket file from a live server between `bind()` and `listen()`. Only an exclusive lock held across both closes that window. A lock file must outlive its holder, or two starters can lock different inodes.
- Self-inflicted `shutdown(SHUT_WR)` on the client fd plus a server that removed its endpoint gives a deterministic "send fails, peer alive, reconnect fails" case, with no fault injection in production code.
- glibc's 64-bit `time_t` mode renames socket calls that carry time-bearing control messages (`sendmsg` -> `__sendmsg64`). Link-time wrappers must cover the redirected names, as `fallocate64` already showed for 64-bit file offsets.
- Never run a native C fixture while a Linux-target run is active: the fixed `/tmp` paths collide and produce false failures.
- Classification work exposes parity drift: Go mapped baseline receive-over-limit to truncated while C and Rust mapped it to overflow. Compare all three mappings side by side when touching one.

## Followup

- SHM regions for every accepted connection: SOW-0055.

## Regression Log

None yet.
