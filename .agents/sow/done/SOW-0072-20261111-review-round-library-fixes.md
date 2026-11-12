# SOW-0072 - Review round: library fixes from netdata#24633 and plugin-ipc#18

## Status

Status: completed

Sub-state: implemented, validated and re-vendored. Library commits `04f71b1`, `142f289` (regression 2026-11-11, macOS C test) and `792eb3c` (regression 2026-11-12, review round), plus a docs and record commit for the second review round of 2026-11-12. macOS CI passed on `142f289` (run 43123299102) and `792eb3c` (run 43125452973); extended fuzz 15/15 on the `04f71b1` tree and stress 7/7 on `04f71b1` and again on `5e68489` (after the attach-loop change) (validation 2026-11-11 and 2026-11-12, below). Re-vendored into netdata/netdata#24633 as `b41982210f` (`142f289`) and `6aaaeea23a` (`792eb3c`); the Netdata-side record is SOW-0050.

## Requirements

### Purpose

The vendored NetIPC copy in Netdata must carry no known bug. Review findings on netdata/netdata#24633 (vendored code) and netdata/plugin-ipc#18 (SOW and skill text) are validated, the confirmed ones are fixed upstream in all three languages, and the fix is re-vendored (the re-vendoring is recorded in SOW-0050).

### User Request

The user asked to "check for issues/comments" on both PRs. After the triage the user accepted the recommended options for decisions 6-9 ("I like your picks", 2026-11-11) and the plan: fix upstream, push, re-vendor, reply in each thread and resolve it.

### Assistant Understanding

Facts:

- 24 unresolved threads: 15 on netdata#24633 (13 library findings, 1 Netdata test finding raised by two bots), 9 on plugin-ipc#18 (SOW and skill text).
- Confirmed library defects, with evidence in the Analysis: Windows handshake profile mask (#1), socket mode under a umask without owner write (#3), persistent call-bound SHM attach failure (#4, plus a Rust/Go retry-class parity bug), receive-side failure leaves in-flight IDs (#7), no `chunk_index < chunk_count` check (#9), C POSIX server write after `acceptor_started` is cleared (#11), two inaccurate comments (#13, #14), and Go test waits without a bound after a watchdog fires (#10).
- Rejected with evidence: #2, #5 (decision 8), #6, #8, and the plugin-ipc#18 request to fill SOW-0050's regression log.

Inferences:

- None beyond the decisions below.

Unknowns:

- None.

### Acceptance Criteria

- Each confirmed defect has a test that fails without the fix and passes with it, where the defect is observable (Validation lists each one), in every language and platform the defect exists in.
- The specs (`docs/`) state the corrected behavior: Windows profile mask, socket mode rule, SHM attach fallback rule, chunk index rule.
- Linux CTest, `cargo test`, `go test`, POSIX interop, FreeBSD 14.3 and 15.1, the Windows host suites and macOS CI pass.
- The doc findings on plugin-ipc#18 are fixed in the cited SOW and skill files.

## Analysis

Sources checked:

- Review threads of netdata/netdata#24633 and netdata/plugin-ipc#18 (unresolved on 2026-11-11).
- Code and specs cited per finding below.

Current state (per finding, netdata#24633 numbering):

- #1: the Windows named pipe handshake passes the configured profile bits through. C `netipc_named_pipe.c:879-881` (client HELLO) and `:1023-1025` (server offer); Rust `windows.rs` `client_handshake` and `server_handshake`; Go `pipe_handshake.go` leaves `PlatformProfiles` unset. POSIX masks with `NIPC_UDS_PLATFORM_PROFILES` (`netipc_uds_internal.h:44-50`). A configured `0x08` (`NIPC_PROFILE_SHM_WAITADDR`, no Windows implementation) can be selected, and the service layer then runs the session over the pipe.
- #3: the socket file is `0600 & ~umask` (Linux and FreeBSD: `fchmod` before `bind`, `netipc_uds_lifecycle.c:185-197`; macOS keeps the umask narrowing on purpose, `:156-159`). Connecting needs write permission on the socket file, so a umask without owner write (for example `0277`, file `0400`) makes the endpoint unreachable for its own user while `listen` reports success. Netdata uses umask `022` or `007` and is not affected.
- #4: `call_bound = attach_ms <= shm_attach_timeout_ms` (C `netipc_service_posix_client_connect.c:133`, Windows `netipc_service_win_client_connect.c:136`, Rust `client_unix.rs:45`, `client_windows.rs:36`, Go `client_unix.go:118`, `client_windows.go:111`). After connect the remaining call time is always below the call timeout, so with a call timeout at or below the 5000 ms attach timeout (network-viewer uses 5000 ms) every attach failure is call-bound and SHM is never removed: a region that never becomes attachable makes every call end at its deadline, against `docs/level2-typed-api.md:332-336`. Parity: C retries only `NOT_READY`, `OPEN`, `BAD_MAGIC` (Windows `OPEN_MAPPING`, `OPEN_EVENT`, `BAD_MAGIC`); Rust and Go retry every error, so a permanent error is never treated as an SHM failure.
- #7: on the stream baseline (FreeBSD, macOS) a failure after the first byte sets `recv_failed` (C `netipc_uds_receive.c:502-506`, Rust `posix.rs:533-537`, Go `uds_receive.go:184-188`) without failing the in-flight IDs, against `docs/level1-transport.md:148-152`. Only `NIPC_UDS_ERR_RECV` failed them (`netipc_uds_receive.c:232-247`); protocol, chunk and limit errors did not. The in-flight set is bounded, so the claimed unbounded growth does not exist.
- #9: every receiver checks `chunk_index` against its running counter and `chunk_count` against the expected count, but none rejects `chunk_index >= chunk_count` (`docs/level1-wire-envelope.md:152`): C POSIX `validate_chunk_header` (`netipc_uds_receive.c:296-307`), C Windows (`netipc_named_pipe.c:1903-1905`), Rust `framing.rs:315-325`, Go `framing/receive.go:304-310`. A peer that sends smaller chunks than the budget sends more chunks than it declared. Reassembly stops on the byte count, so memory safety is unaffected.
- #10: Go robustness tests block on channel receives without a bound after their watchdog fired (`robustness_windows_test.go`, `robustness_unix_test.go`); each wait is bounded by a second mechanism, but the file header claims more than that.
- #11: `server_accept_loop_enter` clears `acceptor_started` when it sees a stop, and `nipc_server_run` then stores `running = false` (`netipc_service_posix_server.c:36,162`). Destroy waits only for `acceptor_started`, so it can free the server before that store. The Windows server stores `running = 0` before `accept_loop_active = 0` (`netipc_service_win_server.c:166-167`) and is not affected.
- #13: C comments say the remaining time is rounded up (`netipc_service_posix_client_connect.c:131-132`, `netipc_service_win_client_connect.c:133-134`); `nipc_service_common_client_op_remaining_ms` (`netipc_service_common.c:207-214`) returns the exact integer difference. Both deadlines are whole milliseconds on the same clock, so the tie rule (`<=`) is still right. SOW-0070 line 250 records the same inaccuracy.
- #12 and #15 (one finding raised by two bots): Netdata's `run_overflow_status_test()` (`src/collectors/network-viewer.plugin/tests/test_network_viewer_apps_lookup_client.c`, Netdata worktree of netdata/netdata#24633) applies a lookup response, which locks `apps_lookup_cache_mutex`, without `nv_apps_lookup_init()`, which owns that mutex: locking an uninitialized mutex works only on zeroed storage. Fixed in the Netdata test (init before, destroy after), not in the library.
- #14: "waiting costs nothing" (C POSIX and Windows server, Rust `common.rs:83-84`, Go `server.go:141-145`): the accept loop waits up to two poll slices, so clients queued behind the refused one wait too; they would be refused as well while every worker is busy.

Rejected (evidence for the replies):

- #2: the server sets the directional limits and the client checks the selected profile and packet size only (`docs/level1-transport.md:259`).
- #5 (decision 8): the zeroing happens only on the drain path, while a send is blocked and responses are queued, off the established request/response path.
- #6: protocol failures end the session (`docs/level1-transport.md:450-452`).
- #8: documented by `docs/level1-posix-uds.md:74-80`; SOW-0070 D3 corrected that wording.
- plugin-ipc#18 "SOW-0050 Regression Log says None yet": the regressions found after SOW-0050 belong to SOW-0070 and are recorded there.

plugin-ipc#18 doc findings (all confirmed):

- `.agents/skills/project-netdata-vendoring/SKILL.md`: the Go import rewrite uses paths relative to the current directory (run from plugin-ipc it rewrites plugin-ipc's own tree); the warning check `grep` exits 0 when it finds a warning and the pipe hides a failed build; the fortified check passes `-O2` in `CMAKE_C_FLAGS`, which the Release `-O3` overrides.
- SOW-0050: workstation paths at lines 129, 136 and 158, against its own plan; line 111 should name the accepted SonarCloud exception.
- SOW-0070: the status line should cite the re-vendor commit; line 425 cites `beginOp` where the deadline starts in `newLookupDeadline` (`lookup_common.go:110`).
- SOW-0071 line 24: the `starttime` field exists on every platform; only Linux has a usable nonzero value for it.

Risks:

- #3 changes `listen` behavior on Linux and FreeBSD under a umask without owner write: it now fails instead of creating an unreachable endpoint. Existing umask tests under `0277` (SOW-0067) expected success and change with it.
- #4 adds client state; a wrong reset rule could disable SHM after transient slowness.
- #9 adds one comparison per continuation chunk on the receive path (decision 9).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Per finding, in Analysis. Common root: rules that exist on one platform or language were not applied to its siblings (#1, #4 parity, #7), and two rules were stated without being enforced (#9, #3).

Evidence reviewed:

- The files and lines in Analysis; `docs/level1-transport.md`, `docs/level1-wire-envelope.md`, `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level1-windows-shm.md`, `docs/level1-posix-shm.md`, `docs/level2-typed-api.md`; SOW-0051 decision 7 (call-bound rule), SOW-0066 and SOW-0067 (umask handling), SOW-0070 F9.

Affected contracts and surfaces:

- Windows handshake (C, Rust, Go): offered and accepted profiles.
- POSIX listener (C, Rust, Go): socket mode rule and `listen` result.
- Level 2 clients (C, Rust, Go; POSIX and Windows): SHM attach fallback rule.
- Level 1 receive (C, Rust, Go; POSIX and Windows): chunk index rule; stream baseline in-flight failure.
- C POSIX managed server start/stop.
- Specs listed above; SOW-0050, SOW-0070, SOW-0071 text; the vendoring skill.

Existing patterns to reuse:

- `NIPC_UDS_PLATFORM_PROFILES` / Rust `PLATFORM_PROFILES` / Go `framing` `PlatformProfiles` for #1.
- `nipc_uds_inflight_fail_all`, Rust `inflight_ids.clear()`, Go `failAllInflight` for #7.
- The attach-fallback tests (C `test_client_shm_attach_failure_falls_back_to_baseline`, Rust `test_refresh_shm_attach_failure_falls_back_to_baseline` and the WinSHM twin, Go `TestUnixShmAttachFailureFallsBackToBaseline` and `TestWinShmAttachFailureFallsBackToBaseline`) for #4.
- The SOW-0067 child-process umask tests for #3.

Risk and blast radius:

- Compatibility: wire format unchanged. #1 only removes bits that no Windows peer implements. #9 rejects only messages a conforming sender never produces.
- Performance: per-request work changes only in #9 (decision 9) and #7 (failure path only).
- Security: #11 removes a possible write after free.

Sensitive data handling plan:

- Evidence is code paths, spec lines and public PR thread IDs; no credentials, customer data or private endpoints. The SOW-0050 fix removes workstation paths.

Implementation plan:

1. #11 reorder; #13 and #14 comments.
2. #9 chunk index check in the four receivers, with tests.
3. #1 Windows profile mask in C, Rust and Go, with tests; spec.
4. #7 fail in-flight IDs on the stream `recv_failed` transition, with tests.
5. #4 call-bound counter and retry-class parity in C, Rust and Go, POSIX and Windows, with tests; specs.
6. #3 socket mode rule in C, Rust and Go, with tests; spec.
7. #10 bounded waits in the Go robustness tests.
8. Doc fixes in the vendoring skill and SOW-0050, SOW-0070, SOW-0071.

Validation plan:

- Linux: CTest, `cargo test`, `go test`, POSIX interop scripts, coverage scripts for the umask child tests.
- FreeBSD 14.3 and 15.1 (stream baseline for #7, SHM for #4, umask for #3).
- Windows host: C, Rust, Go Windows suites (#1, #4, #9).
- macOS CI (#3 macOS mode, #7 stream baseline).
- Extended fuzz before vendoring (#9 touches the decoders' receive path).

Artifact impact plan:

- AGENTS.md: no workflow change expected.
- Runtime project skills: `project-netdata-vendoring` fixes (plugin-ipc#18).
- Specs: the `docs/` files listed above.
- End-user/operator docs: `docs/netipc-integrator-skill.md` SHM attach section if its fallback text changes.
- End-user/operator skills: same file.
- SOW lifecycle: this SOW closes with the work; done SOWs 0050, 0070, 0071 get text corrections only.

Open-source reference evidence:

- None; all evidence is in this repository.

Open decisions:

- Resolved: decisions 6-11 below (6-9 user, 10 assistant, 11 user).

## Implications And Decisions

6. SHM attach that keeps failing on a live connection (#4), user 2026-11-11: option A. The client counts call-bound attach failures that end with the session still connected; the second in a row removes SHM from its proposals, as an attach failure does. A successful attach resets the count. A failure ended by abort or by a disconnected peer neither counts nor resets. Rejected: B, removing SHM on the first call-bound failure, which a single slow attach under a short call timeout would trigger.
7. Socket mode under a umask without owner write (#3), user 2026-11-11: option A. macOS sets the socket exactly `0600` (it sits in a private 0700 directory until the rename). Linux and FreeBSD check the bound file after `bind` and fail `listen` with a socket error (`EACCES`) when owner write is missing. Assistant clarification within A: the check is for owner write only, the permission `connect` needs on Linux, FreeBSD and macOS; requiring owner read too would break umask `0477` (file `0200`), which works today and is tested by SOW-0067. Rejected: B, path-based `chmod` after bind (SOW-0032); C, a private bind directory on every POSIX platform (endpoint layout change in every language and in Netdata).
8. Drain-path zeroing (#5), user 2026-11-11: option B, rejected with evidence; no change.
9. Benchmarks for #9, user 2026-11-11: option A, performance assessment only, no benchmark run.
10. Windows C guard executables (assistant decision, 2026-11-11, recorded for user review): validation found that `test_win_service_guards` and `test_win_service_guards_extra` fail on this tree and identically on `dce436c` (1 and 3 assertions). They were never in CTest (`CMakeLists.txt`: "coverage-only", kept out for CTest timing) and Windows CI only built them for CodeQL, so their expectations went stale unnoticed. This SOW's acceptance requires the Windows suites to pass, so they are fixed here: stale expectations follow the spec, the racy drain test is made deterministic, and both executables are registered in CTest, the Windows CI job (`runtime-safety.yml`) and `tests/run-windows-msys-validation.sh`. They run in 5.6 s and 0.2 s. Alternative not taken: keeping them coverage-only, which is how they rotted.
11. SHM attach retry classes (user 2026-11-12, option B, surgical): the code keeps retrying every open failure and every magic mismatch until the deadline; the specs (`docs/level2-typed-api.md`, `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`) now say so and why. The server creates the region in place (`netipc_shm.c:486-560`: create 0600, `fchmod`, zero-fill, header; Windows zero-fills the view, `netipc_win_shm.c:401-405`), so a missing file, `EACCES` before the `fchmod`, an undersized file, a zero magic, and ring offsets and capacities not written yet (a ring offset inside the header or a zero capacity) are all transient, and a permanent error in those classes costs at most the bounded attach wait (decision 6). Rejected: A, retrying only `ENOENT`/`EACCES`, size 0 and zero magic, which needs errno and magic detail in every language's error types.

## Plan

See the implementation plan in the gate.

## Execution Log

### 2026-11-11

- Created after the triage; decisions recorded.
- #11: `server_accept_loop_enter` stores `running = false` before it clears `acceptor_started`; `nipc_server_run` no longer writes `running` after a failed enter.
- #13, #14: comments corrected in C (POSIX and Windows client connect and server), Rust `common.rs` and Go `server.go`.
- #9: `chunk_index >= chunk_count` rejected in C `validate_chunk_header` (`netipc_uds_receive.c`), C `netipc_named_pipe.c`, Rust `framing.rs` and Go `framing/receive.go`; spec `docs/level1-wire-envelope.md`.
- #1: Windows platform profile mask (`BASELINE | WIN_SHM_HYBRID | WIN_SHM_BUSYWAIT`) applied to the client HELLO and to the server's supported and preferred sets: C `NIPC_NP_PLATFORM_PROFILES`, Rust `PLATFORM_PROFILES` (`windows.rs`), Go `platformProfiles` (`pipe_handshake.go`); spec `docs/level1-windows-np.md`.
- #7: the stream baseline `recv_failed` transition fails every in-flight ID (C `nipc_uds_inflight_fail_all`, Rust `inflight_ids.clear()`, Go `failAllInflight`); spec `docs/level1-posix-uds.md` "Partial receive".
- #4 (decision 6): `shm_call_bound_failures` counter and `CLIENT_SHM_CALL_BOUND_FAILURE_LIMIT` (2) in C, Rust and Go, POSIX and Windows. Rust and Go now retry only the C retry classes (POSIX `NotReady`, `Open`, `BadMagic`; Windows `OpenMapping`, `OpenEvent`, `BadMagic`); any other attach error removes SHM at once. Specs `docs/level2-typed-api.md`, `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`; integrator skill `docs/netipc-integrator-skill.md`.
- #3 (decision 7): Linux and FreeBSD `lstat` the socket after `bind` and fail `listen` with `EACCES` (unlinking the file) when owner write is missing; macOS `fchmodat`s the socket to exactly `0600`. C `netipc_uds_lifecycle.c`, Rust `posix.rs` `bind_owner_only`, Go `uds_bind_other.go` and `uds_bind_darwin.go`; spec `docs/level1-posix-uds.md` "Socket permissions".
- #10: generic `recvWithin` helper (`server_helpers_test.go`) bounds every channel receive after a watchdog in `robustness_unix_test.go` and `robustness_windows_test.go`; header comments corrected.
- plugin-ipc#18 text: vendoring skill (Go import rewrite runs in a subshell from the Netdata checkout; warning check fails on a match and keeps the build status; Release flags set through `CMAKE_C_FLAGS_RELEASE`); SOW-0050 (workstation paths removed, SonarCloud exception named); SOW-0070 (re-vendor commit `b53818fab6`, `newLookupDeadline`, the F9 rounding claim corrected for C); SOW-0071 (`starttime` wording).

- Windows guard tests (decision 10):
  - `test_win_service_guards.c`, typed "missing SHM response after request": the server exits after the request, so the typed call reports `NIPC_ERR_DISCONNECTED` (`docs/level2-typed-api.md`, retry rules), not `TRUNCATED`. The raw twin passes `TRUNCATED` through a test-local retry helper (`test_win_raw_client_helpers.h`), not the library.
  - `test_win_service_guards_extra.c`, worker limit and thread-create fault: the worker runs the handshake, so a refused connection (no free worker, or no worker thread, `netipc_service_win_server.c:222-229`) is closed before HELLO_ACK and `nipc_np_connect` fails (`docs/level2-typed-api.md`, worker limit). The tests expected the pre-SOW connect success.
  - Drain timeout test, in both files: the test released the blocked handler right after starting a 1 ms drain, so the result depended on scheduling (it failed every run in `_extra` and passed in the other file). The handler now blocks on a synchronous `ReadFile` of a pipe nobody writes, which only the drain's cancel phase ends (`CancelSynchronousIo`, `netipc_service_win_server_session.c:409-410`), after the deadline; `drained == false` is deterministic.
  - Registered in CTest, `runtime-safety.yml` (build targets and the CTest regex) and `tests/run-windows-msys-validation.sh`.

## Validation

Acceptance criteria evidence:

- Tests that fail without the fix and pass with it (POSIX C, Rust and Go verified against the old code): #9 C `test_receive_extra_continuation` (`test_uds.c`), Rust `test_receive_more_chunks_than_declared`, Go `TestUdsReceiveRejectsMoreChunksThanDeclared`; Windows C `FAKE_ACK_GOOD_THEN_EXTRA_CHUNK` (`test_named_pipe.c`), Go `TestSessionReceiveRejectsMoreChunksThanDeclared`. #4 C `test_client_shm_persistent_call_bound_failure_falls_back`, Rust `test_refresh_persistent_call_bound_attach_failure_falls_back` and `test_refresh_permanent_attach_error_falls_back_at_once`, Go `TestUnixShmPersistentCallBoundAttachFailureFallsBack` and `TestUnixShmPermanentAttachErrorFallsBackAtOnce`; Windows C `test_hybrid_persistent_call_bound_failure_falls_back`, Rust `test_refresh_persistent_call_bound_winshm_attach_failure_falls_back`, Go `TestWinShmPersistentCallBoundAttachFailureFallsBack`. #1 C `test_unassigned_profile_bit`, Rust `test_unassigned_profile_bit_not_selected`, Go `TestPipeUnassignedProfileBitNotSelected` (profile bit `0x08`). #3 C `test_listen_socket_restrictive_umask` (masks `0077`, `0477`, `0277`, `0777`), Rust `lock_umask_helper_process` and Go `TestLockUmaskHelperProcess` (unreachable socket now fails `listen` with `EACCES`). #7 assertions added to C, Rust and Go `*stream_failed_receive_drops_pending` (stream baseline, FreeBSD and macOS only).
- Not testable deterministically: #11 (a race between destroy and the run loop's last store; fixed by ordering), #13 and #14 (comments), #10 (bounds a hang only after a regression).
- Windows permanent-attach-error parity has no Windows twin of the POSIX `PermanentAttachError` tests: a WinSHM region that opens with a bad version needs a hand-built mapping the Windows fixtures do not have; the retry classes are the same code shape as POSIX and are covered by the call-bound tests.
- Specs: `docs/level1-wire-envelope.md` (chunk index), `docs/level1-windows-np.md` (profile mask), `docs/level1-posix-uds.md` (socket permissions, partial receive), `docs/level2-typed-api.md`, `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md` (attach fallback).

Tests or equivalent validation (2026-11-11, low priority):

- Linux: build clean; CTest 32/32; `cargo test` 417 passed; `cargo +1.91.0 test`, `cargo fmt --check` (1.91.0 and stable) and `cargo clippy --all-targets` pass (pre-existing warnings only); `go test ./...` and `go test -race` on `service/...` and `transport/...` pass; `test_uds_interop` 18, `test_shm_interop` 9, `test_service_interop` 18, `test_cache_interop` 9, `interop_codec` all pass.
- Coverage scripts: `run-coverage-rust.sh` passes; `run-coverage-c.sh` (per-file 90%: `netipc_uds.c` 84.8%, `netipc_shm.c` 89.8%) and `run-coverage-go.sh` (88.1%) fail, with the same C numbers and Go 88.0% on `dce436c` measured from a `git archive` export: pre-existing, not caused by this SOW, tracked by SOW-0068. The umask child tests pass under coverage.
- FreeBSD 14.3 and 15.1: CTest 31/31, `cargo test` 415 passed, `go test ./...` and `GOARCH=386 go test ./...` pass on both; the stream (#7), chunk (#9) and umask (#3) tests ran.
- Windows host: Rust 282 passed (including the #1 and #4 tests); Go all packages pass (the #1, #4 and #9 tests confirmed with `-v`); C CTest 22/22 including both guard executables, which also pass 3 of 3 direct runs.
- macOS: `.github/workflows/macos.yml` failed on `04f71b1` (regression 2026-11-11, below) and passed on `142f289` (run 43123299102) and `792eb3c` (run 43125452973), CTest, Rust, Go and interop.
- Extended fuzz (#9 touches the receive path) and stress (transports): `tests/run-extended-fuzz.sh` 15/15 and `tests/run-extended-stress.sh` 7/7 on the `04f71b1` tree (2026-11-12). Later commits change no codec or decoder, so the fuzz result carries; `792eb3c` changes the SHM attach loops, so stress ran again on `5e68489` (which contains it): 7/7.

Real-use evidence:

- The library's real use is Netdata; the re-vendored Netdata build and consumer tests run under SOW-0050.

Reviewer findings:

- The 24 PR threads are the review. Fixed: netdata#24633 #1, #3, #4, #7, #9, #10, #11, #13, #14, #12 and #15 (Netdata test, fixed in the Netdata worktree); plugin-ipc#18 skill and SOW text. Rejected with evidence: #2, #5, #6, #8 and the SOW-0050 regression-log request (Analysis).

Same-failure search:

- #1: every Windows handshake entry (C client and server, Rust both, Go both) masks; POSIX already did.
- #9: all four receivers (C POSIX, C Windows, Rust shared framing, Go shared framing).
- #4: both C connect files, Rust and Go POSIX and Windows clients.
- #7: every stream `recv_failed` transition in C, Rust and Go.
- #3: C, Rust and Go, Linux/FreeBSD and macOS bind paths.
- Stale guard tests: the same drain race existed in both guard files; both fixed. Every other `test_*` executable in `CMakeLists.txt` has a CTest entry.

Performance assessment:

- Per request on an established session: only #9 adds one integer comparison per continuation chunk on receive (`netipc_uds_receive.c` `validate_chunk_header`, `netipc_named_pipe.c` chunk loop, Rust `framing.rs`, Go `framing/receive.go`); messages within one packet never reach it. #1 (handshake), #3 (listen), #4 (connect and attach), #7 and #9 rejections (failure paths), #11 (start/stop) are outside the measured paths. No benchmark run (decision 9).

Artifact maintenance gate:

- `AGENTS.md`: no workflow change; the Windows validation commands it names are unchanged.
- Runtime project skills: `project-netdata-vendoring` fixed (Go import rewrite location, warning check status, Release flags).
- Specs: the `docs/` files above. `.agents/sow/specs/`: no durable decision beyond the public specs.
- End-user/operator docs: `docs/netipc-integrator-skill.md` SHM attach section updated for the call-bound limit.
- End-user/operator skills: same file.
- SOW lifecycle: this SOW moves to `done/` as `completed`; SOW-0050, SOW-0070 and SOW-0071 get text corrections only; SOW-0068 records the C coverage gap.

## Outcome

All confirmed findings from both PRs are fixed in C, Rust and Go on POSIX and Windows, with tests and spec updates; the rejected ones have evidence for the replies. Two Windows C guard executables that had silently rotted outside CTest are fixed and now run in CTest and CI.

## Lessons Extracted

- A test executable outside CTest and CI rots: its expectations stopped matching the spec months ago and nothing noticed. Every correctness executable is registered in CTest; a coverage script may run more, never fewer.
- Rules stated once and implemented per platform drift (#1, #4 parity, #7): when one platform or language gets a rule, the same change checks its siblings.
- "CTest passed" proves only what CTest runs; validation checks that the new tests are in the run.

## Followup

- C and Go coverage thresholds: SOW-0068 (pending).
- Duplicate tests between `test_win_service_guards.c` and `test_win_service_guards_extra.c` (four functions with the same name): rejected as not worth a SOW; both copies now pass and cost under 6 s.
- Re-vendoring, extended fuzz on the candidate, macOS CI result and thread replies: SOW-0050 (reopened for the re-vendoring, as for `d3c94a5` and `ce6834e`).

## Regression Log

Regressions 2026-11-11 (macOS C test) and 2026-11-12 (review round) are appended at the end of this file.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.

## Regression - 2026-11-11

What broke:

- macOS CI on `04f71b1` (`macos.yml` run 43122445371): CTest `test_uds` failed 2 checks, "socket mode narrowed by the umask" and "socket mode narrowed by umask 0477". The macOS-only block of `test_listen_socket_mode_macos` in `tests/fixtures/c/test_uds.c` still expected the umask to narrow the socket (`0400`, `0200`), the rule decision 7 replaced with exactly `0600`. The job stops at CTest, so its Rust, Go and interop steps were skipped.

Why validation missed it:

- The block is under `#ifdef __APPLE__`, so the Linux and FreeBSD builds never compiled it, and the same-failure search for #3 looked at the code paths, not at macOS-only test expectations. Rust and Go already expected `0600` on macOS.

Repair:

- The two checks expect `0600` under umasks `0277` and `0477`, and the comment states the rule.

Validation:

- macOS CI on the repair commit (CTest, Rust, Go, interop); its result is recorded with the re-vendoring in SOW-0050. Linux and FreeBSD do not compile the changed block, and `GOOS=darwin go vet` on the transport package is clean.

Artifact updates:

- None beyond this test: the spec and the Rust and Go tests already state `0600`.
- Lesson: a rule change on one platform includes a search of that platform's conditional test code (`#ifdef __APPLE__`, `cfg(target_os = "macos")`, `_darwin` files), which the local builds do not compile.

## Regression - 2026-11-12

What broke (review of `04f71b1` and `142f289` on plugin-ipc#18, 7 threads; all validated against the code):

- SHM attach loop (C POSIX and Windows connect, Rust and Go Unix and Windows clients): the deadline was checked before peer liveness and abort, and only the call-bound branch probed the peer. A server that died in the last retry slot before the SHM attach deadline was taken for an SHM failure and SHM was removed for the client's lifetime, against decision 6 and the loop's own rule. Present before this SOW (`keep_shm = call_bound` on `dce436c`); this SOW's counter kept the order.
- `test_win_service_guards` and `test_win_service_guards_extra` were registered in CTest without the `TIMEOUT` and `RUN_SERIAL` every other Windows service test sets.
- The Go SHM tests' server helpers (`serveSessionsUntilClosed` and the older attach-failure and raw-server waits, 8 receives in `shm_posix_test.go` and `shm_windows_test.go`) and the Rust twins (`serve_sessions_until_closed`, the WinSHM call-bound test) waited without a bound.
- The vendoring skill's import-rewrite check and fortified-build check read a `grep` error (status 2) as success through `! grep` and `xargs -r`.
- The new profile-mask test comments (C, Rust, Go) said `0x08` is assigned on POSIX; it is SHM_WAITADDR, reserved, implemented by no transport (`docs/level1-windows-shm.md`, profile table).
- This SOW's Analysis had no entry for #12 and #15.

Rejected with evidence:

- "The Rust permanent-attach-error test's premise is wrong because the server unlinks the stale region": the test serves sessions from a bare `UdsListener` (`serve_sessions_until_closed`), which never creates or reclaims an SHM region, so the pre-written bad-version region stays; the test fails on the code before this SOW.

Why validation missed it:

- The attach-loop order was reviewed for the call-bound branch only, the branch the counter changed. The test-helper bound and the CTest properties followed the files' older patterns, not the bounded-wait rule (#10) or the neighbouring Windows tests.

Repair:

- All six attach loops check abort, then peer liveness, then the deadline; the deadline branch only applies the call-bound counter. No deterministic test: the window is the server closing between the last liveness probe and the deadline, with no hook in between.
- `TIMEOUT 120` and `RUN_SERIAL TRUE` for both guard tests.
- Go waits through `recvWithin`; Rust helpers report through a channel read with `recv_timeout(TEST_WAIT_LIMIT)`.
- Skill: each `grep` status is checked on its own (1 = no match passes; 2 fails); both blocks run in a subshell. Checked on a scratch tree: rewrite, already clean, unreadable directory (fails with 2).
- Comments name `0x08` as reserved and unimplemented.
- Analysis entry for #12 and #15.

Validation (2026-11-12, low priority):

- Linux: CTest 32/32; `cargo test` 417; `cargo fmt --check` with 1.91.0 and stable; `go test ./...`, `go test -race` on `service/...` and `transport/...`; UDS, SHM, service, cache and codec interop all pass.
- FreeBSD 14.3 and 15.1: CTest 31/31, `cargo test`, `go test ./...` and `GOARCH=386 go test ./...` pass on both.
- Windows host: Rust 282 passed, Go all packages, CTest 22/22 (both guard tests with the new properties).
- macOS: CI passed on `792eb3c` (run 43125452973).
- Re-vendored as Netdata `6aaaeea23a`: Linux build with no NetIPC warning, the five lookup tests, `go test`, `go vet`, `cargo test -p netipc` 416, `cargo fmt --check`, `cargo doc` 0 warnings, fortified `-O2` library build clean.
- Performance: only the connect-time attach loop changed; nothing runs per request on an established session. No benchmark.
- Artifacts: vendoring skill (both checks); specs `docs/level2-typed-api.md` (attach-failure paragraph), `docs/level1-posix-shm.md` and `docs/level1-windows-shm.md` (liveness probe before each deadline check) and the integrator skill `docs/netipc-integrator-skill.md` (SHM attach failure) now state that a peer gone during the attach wait is not an attach failure; `AGENTS.md` unaffected.

Second review round on `792eb3c` (2026-11-12, 13 threads, validated against the code):

- Fixed, records: this SOW's status now names every library commit, the macOS runs, the extended-suite results and the Netdata commits; the Regression Log points at the appended sections; SOW-0070's F9 entry keeps its original sentence with a dated correction, as its R1-R12 records do.
- Fixed, specs: `docs/level1-posix-uds.md` limits the `fchmod`-before-`bind` procedure to Linux and FreeBSD; `docs/level1-posix-shm.md` and `docs/level1-windows-shm.md` state the tie rule at the attach-timeout boundary; the retry classes follow decision 11.
- Decision 11 (user, option B) answers the four code threads on the retry classes (Rust and Go, Unix and Windows) and the two spec threads on the same rule.
- No vendored source changed in this round, so Netdata `6aaaeea23a` stays the vendored state.

