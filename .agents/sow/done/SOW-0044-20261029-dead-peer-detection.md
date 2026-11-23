# SOW-0044 - Dead Peer Detection

## Status

Status: completed

Sub-state: completed 2026-10-31. First of the two Netdata bug-fix SOWs (this one, then SOW-0043); vendoring both into Netdata is tracked by SOW-0050.

## Requirements

### Purpose

Servers must release sessions whose client left (cleanly or by crashing), and clients must notice a crashed server, on both SHM transports.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- K1: Rust and Go POSIX SHM server sessions ignore client departure on SHM receive timeout (`src/crates/netipc/src/service/raw/server_session_unix.rs:54`, `src/go/pkg/netipc/service/raw/server_unix.go:208-209`); C checks the session socket (`netipc_service_posix_server_session.c:121-125`). Worker slots and `.ipcshm` files leak. Reproduced in Rust and Go; C guard passes (SOW-0040).
- K2: Windows SHM server sessions detect only the clean-close flag (`netipc_win_shm.c:671-680`, `:893-907`; C `netipc_service_win_server_session.c:47-48`, Rust `server_session_windows.rs:49`, Go `server_windows.go:250-251`). A crashed client holds a worker until server stop. Reproduced in all three languages. The spec (`docs/level1-windows-shm.md:189-211`) has the same gap.
- N5: SHM clients never notice a crashed server and wait the full call timeout (POSIX: `nipc_shm_owner_alive` exists at `netipc_shm.c:874` but is unused in the call loop; Windows: only `resp_server_closed`, `netipc_win_shm.c:893-906`). Reproduced in all languages on Linux and Windows (10 s with a 10 s timeout).
- Amplifier: a client whose SHM attach fails falls back to a new baseline session (`netipc_service_posix_client_connect.c:138-150`, Go `client_unix.go:104-115`), leaking one slot per fallback on Rust/Go servers.
- Related: refused or broken SHM sessions surface as `Truncated` / `buffer too short` instead of a disconnect error in Rust and Go.
- Analysis 2026-10-29: no POSIX transport sets `SOCK_CLOEXEC`/`O_CLOEXEC` (C `netipc_uds_lifecycle.c:202,244,275`, Rust `posix.rs:266,646,712`, Go `uds_listener.go:34,98`, `uds_session.go:95`); a forking process (Netdata spawning plugins) leaks session sockets into children, which keeps the peer from ever seeing EOF and defeats socket-based dead-peer detection.
- Analysis 2026-10-29: the `nipc_shm_owner_alive` generation check can never fail, because the cached generation is read from the same mapped header it is later compared against (`netipc_shm.c:621` vs `:887`); its `kill(pid, 0)` check breaks across PID namespaces.
- Correction 2026-10-29: the "fallback leak" is not a separate client bug (C and Go close the session socket before the baseline retry); it leaks only because of K1. The error-code issue is cross-language: C also returns `NIPC_ERR_TRUNCATED` for every SHM/socket transport failure (`netipc_service_posix_client_call.c:101,124`), and no language has a "disconnected" error.
- Sibling bug: the client's SHM attach retry waits its full 5 s even when the server already closed the session socket (refusal at capacity).

Netdata usage (verified 2026-10-30, `netdata/netdata` fork `ktsaou/netdata @ ae9113f2cc`):

- All consumers are C on Linux; no Go or Rust consumer exists and the Rust crate is not built into any binary (`src/crates/Cargo.toml:61` workspace member only; `.github/workflows/rust-tests.yml:86` excludes it).
- Servers: netdata daemon (`cgroups-snapshot`, `cgroups-lookup`; `src/collectors/cgroups.plugin/cgroup-netipc.c:212-252`, `cgroup-lookup-netipc.c:282-333`) and apps.plugin (`apps-lookup`; `src/collectors/apps.plugin/apps-lookup-netipc.c:310-354`).
- Clients: apps.plugin, ebpf.plugin, network-viewer.plugin; all long-lived, 5000 ms call timeout (`apps-cgroups-lookup-client.c:12`, `ebpf_cgroup.c:13`, `network-viewer-apps-lookup-client.c:21`).
- Every consumer prefers `NIPC_PROFILE_SHM_FUTEX` (e.g. `cgroup-netipc.c:216-217`), so live sessions run on POSIX SHM.
- Impact: N5 hits Netdata today (each client call waits the full 5 s after the daemon or apps.plugin dies). K1 and K2 do not (C POSIX servers already detect departure; no Windows consumer). Missing close-on-exec does not leak today: the spawn server forks before NetIPC fds exist and closes all non-stdio fds (`src/libnetdata/spawn_server/spawn_server_nofork.c:352-362,1577-1595`); it remains defence in depth.
- No Netdata consumer checks `NIPC_ERR_TRUNCATED` or calls `nipc_shm_owner_alive`; consumers check only TIMEOUT, ABORTED, OVERFLOW, BAD_LAYOUT, BAD_ITEM_COUNT.

Unknowns:

- None blocking; design decisions recorded below.

### Acceptance Criteria

- SOW-0040 K1, K2, N5 tests pass; C guards keep passing.
- Windows spec revised with a crash-detection rule (for example named-pipe break or process liveness).
- Error codes for refused/broken SHM sessions reviewed for cross-language consistency.

## Analysis

Sources checked:

- See SOW-0040 (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`) for evidence, test files and commands.

Current state:

- Reproduction tests exist and run ungated (the `NETIPC_REPRO_KNOWN_BUGS` gate was removed in SOW-0041 under the zero-known-bug policy); they fail until this SOW lands.

Risks:

- Cross-language behavior change; interop and Netdata vendoring must be validated.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- SHM sessions carry requests through shared memory, so a peer's death is invisible to the SHM wait: the futex/event wait just times out. The session's control channel (UDS socket on POSIX, named pipe on Windows) stays open for the whole SHM session and is closed by the kernel when the peer process exits; it is the only reliable, namespace-independent liveness signal, but only C POSIX servers consult it.
- Transport failures are reported as `TRUNCATED` (receive) or `NOT_READY` (send) because no "peer gone" service error exists.
- `nipc_shm_owner_alive` cannot detect a replaced region (cached and compared generation come from the same mapping) and `kill(pid, 0)` is wrong across PID namespaces and after PID reuse.
- Sockets are created without close-on-exec, so any fork+exec in a consumer would keep session sockets open in the child and mask EOF.

Evidence reviewed:

- SOW-0040 reproduction tests: C `tests/fixtures/c/test_robustness.c:407-460,700-760`; Go `robustness_unix_test.go:226-272,498-530`, `robustness_windows_test.go:185-200,342+`; Rust `service::raw::tests::robustness`, `service::raw::windows_tests::robustness`.
- C peer check: `src/libnetdata/netipc/src/service/netipc_service_posix_server_session.c:43-86,118-126`.
- Rust/Go servers ignoring departure: `src/crates/netipc/src/service/raw/server_session_unix.rs:54`, `src/go/pkg/netipc/service/raw/server_unix.go:208-209`; Windows: `netipc_service_win_server_session.c:47-48`, `server_session_windows.rs:49`, `server_windows.go:250-251`.
- Client SHM waits already sliced at 100 ms for abort: C `netipc_service_posix_client_call.c:77-102`, `netipc_service_win_client_call.c:72-95`; Rust `client_call.rs:440-488`; Go `client_unix.go:200-240`, `client_windows.go:190-225`.
- Error mapping: C `netipc_service_posix_client_call.c:47,58,102,127`, `netipc_service_win_client_call.c:94,118`; Rust `client_call.rs:215,249,270,456,477,487,498,508`; Go `client_unix.go:157-237`, `client_windows.go:146-224`.
- Retry path: `netipc_service_common.c:383-406` (non-overflow, non-timeout errors reconnect and retry once).
- Windows pipe disconnect probe: `netipc_named_pipe.c:300-345` (`PeekNamedPipe` + `is_disconnect_error`); Rust `transport/windows.rs` `wait_readable`; Go `pipe_session.go:89` `WaitReadable`.
- SHM attach retry: C `netipc_service_posix_client_connect.c:118-150`; Rust `client_unix.rs:20-50`, `client_windows.rs:15-45`; Go `client_unix.go:95-120`, `client_windows.go`.
- `owner_alive`: C `netipc_shm.h:211`, `netipc_shm.c:874-905`; Rust `transport/shm.rs:178`; Go `transport/posix/shm_linux.go:121-140`; tests `test_shm.c:542,1278-1392`, `shm_tests.rs:673-760`, `shm_edge_test.go:62-80`, `shm_more_edge_test.go:177-205`.
- Close-on-exec: C `netipc_uds_lifecycle.c:145,202,244,275`; Rust `transport/posix.rs:266,646,712`; Go `uds_listener.go:34,98`, `uds_session.go:95`.
- Netdata usage: see Facts (Netdata usage).

Affected contracts and surfaces:

- Public error enums: C `nipc_error_t` gains `NIPC_ERR_DISCONNECTED` appended after `NIPC_ERR_ABORTED` (no existing value changes); Rust `NipcError::Disconnected`; Go `protocol.ErrDisconnected`. Error-to-string helpers in all three.
- Public transport API removal: `nipc_shm_owner_alive`, `ShmContext::owner_alive`, `(*ShmContext).OwnerAlive`.
- Behavior: SHM servers (all languages, POSIX and Windows) end a session when its control channel reports the peer gone; SHM clients fail a waiting call with DISCONNECTED within one 100 ms slice of the server's death, then take the existing reconnect-and-retry-once path; SHM attach stops retrying when the server closed the session socket and reports the connect as failed without disabling SHM for the context.
- Sockets are close-on-exec on every platform.
- Wire protocol: unchanged. Interop: unchanged.
- Specs: `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`, the service-level error documentation, `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- C `session_peer_disconnected()` (poll with zero timeout for HUP/ERR/RDHUP, `MSG_PEEK` EOF check): move to a shared POSIX service helper; port to Rust (`poll_fd` neighbourhood in `server_session_unix.rs`) and Go (`poll_unix.go`).
- Windows: `nipc_np_wait_readable(session, 0, ...)` returning `NIPC_NP_ERR_DISCONNECTED`; Rust `wait_readable(0)` returning `NpError::Disconnected`-class error; Go `WaitReadable(0)`.
- Client 100 ms abort slices: the liveness probe runs in the same slice loop after an empty wait.

Risk and blast radius:

- Hot path: no extra work while messages flow; the probe runs only after an empty 100 ms wait (one `poll()`/`PeekNamedPipe` syscall per idle 100 ms per session). Performance assessment (AGENTS.md, Test Cadence): no benchmark run needed. On an established session's success path the only added work is one `Option` field read of the session fd per Rust SHM call (`client_call.rs:286`); every other addition runs only after an empty 100 ms wait (C `netipc_service_posix_client_call.c:115-118`, Rust `client_call.rs:467-472`, Go `client_unix.go:227-230`, servers on SHM receive timeout), on error branches (`socket_failure`, `session_failure`, `socketFailure`), or at connection setup (close-on-exec flags, attach-retry probe). The SHM transport diffs only remove `owner_alive` and add `O_CLOEXEC` at open.
- False positives: a probe can only report "gone" on HUP/ERR/EOF or a broken pipe, which the kernel raises only when the peer end is closed. During SHM sessions no data flows on the control channel, so `POLLIN` with `MSG_PEEK` returning data does not count as disconnect (same as C today).
- Behavior change for callers that matched `TRUNCATED`/`NOT_READY` on disconnects: none in Netdata; recorded in docs.
- API removal of `owner_alive`: no known users (Netdata does not call it).
- Close-on-exec on platforms without `SOCK_CLOEXEC` (macOS): `fcntl(FD_CLOEXEC)` right after creation; a fork in the tiny window between the two calls can still inherit it (documented platform limit; Go uses `syscall.ForkLock` there).

Sensitive data handling plan:

- No secrets, customer data or private endpoints involved. Netdata evidence is cited as `ktsaou/netdata @ ae9113f2cc` with repo-relative paths. Windows SIDs, if any appear in test output, are written as `<owner-SID>`. Applies to this SOW, specs, docs, the integrator skill and code comments.

Implementation plan:

1. Error code: add DISCONNECTED to C/Rust/Go error enums and string helpers.
2. Shared liveness probe per platform/language (POSIX socket probe; Windows pipe probe).
3. Servers: on SHM receive timeout, probe the control channel and end the session if gone (Rust/Go POSIX; C/Rust/Go Windows).
4. Clients: in the SHM wait slices, probe after each empty slice and return DISCONNECTED; classify every client transport send/receive failure (SHM and socket/pipe) as DISCONNECTED when the probe says the peer is gone, otherwise keep the current code.
5. Attach retry: probe in the retry loop; on disconnect close the session and report the connect failed without disabling SHM.
6. Remove `owner_alive` and its tests in all three languages; remove now-unused fields/codes only if nothing else uses them.
7. Close-on-exec for all sockets (C, Rust, Go), with `fcntl` fallback where `SOCK_CLOEXEC`/`accept4` is unavailable.
8. Strengthen the K1/K2/N5 reproduction tests to assert DISCONNECTED where a client error is observed; add a close-on-exec test.
9. Specs/docs/integrator skill updates.

Validation plan:

- SOW-0040 K1, K2, N5 reproduction tests pass in C, Rust and Go on Linux and `costa-win11`; C guards keep passing.
- Full C ctest, Rust and Go suites on the host and Windows; `tests/run-linux-target.sh` on the 9 Netdata Linux targets.
- Interop scripts (`tests/test_shm_interop.sh`, `tests/test_service_interop.sh`, `tests/test_uds_interop.sh`).
- Benchmarks: not needed; the performance assessment under Risk and blast radius finds no change on the established-session per-request path (decision 4).
- Formatting/lint: `cargo fmt --check`, gofmt, go vet, shellcheck, `git diff --check`.

Artifact impact plan:

- AGENTS.md: expected no change (no workflow change); re-check at close.
- Runtime project skills: `project-netdata-vendoring` expected unaffected; re-check at close.
- Specs: `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`, service error documentation.
- End-user/operator docs: same docs (they are the public specs).
- End-user/operator skills: `docs/netipc-integrator-skill.md` (new error code, liveness behavior, `owner_alive` removal).
- SOW lifecycle: moved to `current/` 2026-10-30; SOW-0043 follows; Netdata vendoring after both.

Open-source reference evidence:

- `ktsaou/netdata @ ae9113f2cc` (Netdata consumer survey; paths listed in Facts).

Open decisions:

- None; see Implications And Decisions.

## Implications And Decisions

User decisions (2026-10-30):

1. Delivery order: SOW-0044 first, then SOW-0043, then a single Netdata vendoring PR after both; run a read-only Netdata usage survey first (done, see Facts).
2. Error code (decision 1A): add a real "disconnected" error in C, Rust and Go, appended to the enum, used for every peer-gone case on SHM and socket/pipe transports; it keeps the existing reconnect-and-retry-once path.
3. `owner_alive` (decision 2A): remove it from C, Rust and Go together with its tests.
4. Test cadence (2026-10-30, raised while this SOW's before/after benchmarks were running; refined the same day): correctness tests only, targeted to the affected surface, between fixes and at SOW close. Benchmarks run only when a change can affect established-session steady state, judged by a per-SOW performance assessment with evidence; when needed they run once after every fix touching those paths is in, and all regressions are fixed before a single rerun. Recorded in `AGENTS.md` (Test Cadence) and `.agents/skills/project-netdata-vendoring/SKILL.md` (step 7, performance assessment). The in-flight benchmark runs were stopped.

Assistant-applied (single sound option, stated to the user): control-channel liveness probe as the dead-peer signal on POSIX and Windows; close-on-exec on all sockets; SHM attach retry stops when the server closed the session socket.

## Plan

See Pre-Implementation Gate, Implementation plan.

## Execution Log

2026-10-30:

- Error code: C `NIPC_ERR_DISCONNECTED` appended after `NIPC_ERR_ABORTED` (`netipc_protocol.h`); Rust `NipcError::Disconnected` (+ Display); Go `protocol.ErrDisconnected`.
- Liveness probe: C `nipc_service_posix_peer_disconnected()` (moved out of the static server helper, declared in `netipc_service_posix_internal.h`) and `nipc_service_win_peer_disconnected()` (static inline, `netipc_service_win_internal.h`); Rust `server_session_unix::peer_disconnected`, `server_session_windows::peer_disconnected`; Go `peerDisconnected` (`poll_unix.go`, adds `syspoll.RdHup`) and `pipePeerDisconnected` (`client_windows.go`).
- Servers: probe on idle SHM receive in Rust/Go POSIX and C/Rust/Go Windows.
- Clients: probe after each idle 100 ms SHM wait slice (all languages, both platforms); WinSHM close flag maps to DISCONNECTED; socket/pipe send/recv failures map to DISCONNECTED only when they are the transport's send/recv-failure class (or a broken pipe) and the probe confirms the peer is gone.
- Correction during validation: the first version probed after any transport error, which relabelled a protocol violation (wrong response `message_id`) as DISCONNECTED when the test server closed right after sending it (Go Windows `TestWinCall*RejectsMalformedResponseEnvelope`, `TestWinClientTransportReceiveWinShmError`). Narrowed to the send/recv-failure class, matching decision 1A ("end of stream, EPIPE, ECONNRESET, broken pipe"); a missing session is not treated as a disconnect.
- Attach retry: all languages/platforms stop when the probe reports the server closed the session, return DISCONNECTED, and keep SHM enabled (previously the client waited 5 s and then permanently disabled SHM for the context).
- `owner_alive` removed (C function + `nipc_shm_ctx_t.owner_generation` cache, Rust method + field, Go method + field) with its tests; the on-disk `owner_generation` header field stays (stale-region recovery uses it).
- Close-on-exec: C `SOCK_CLOEXEC` / `accept4` / `pipe2` with `fcntl` fallback; Rust `SOCK_CLOEXEC` / `accept4` with fallback, `O_CLOEXEC` on all SHM opens; Go `SOCK_CLOEXEC` / `Accept4` (linux, freebsd) with `ForkLock` fallback, and `F_DUPFD_CLOEXEC` instead of `syscall.Dup` for SHM descriptors (Go `Dup` leaked the region fd).
- Tests: N5 asserts DISCONNECTED (C/Rust/Go, POSIX and Windows); new baseline-socket disconnect test, SHM attach-stop test (POSIX and Windows), close-on-exec tests (C all fds via `/proc/self/fd`; Rust/Go transport-level listener/accepted/client sockets and SHM fds).
- Proof the new C tests detect the bugs: built against unfixed HEAD, all new checks fail (N5 SHM call returned after 10000 ms; baseline returned TRUNCATED; refused refresh took 5001 ms and lost SHM; 5 socket/pipe fds without close-on-exec).
- Specs: `docs/level1-posix-shm.md` (Peer liveness, attach stop), `docs/level1-windows-shm.md` (Detecting a crashed peer, attach stop), `docs/level1-posix-uds.md` (close-on-exec), `docs/level2-typed-api.md` (disconnected error); integrator skill updated.

## Validation

Acceptance criteria evidence:

- K1, K2, N5 reproduction tests pass in C, Rust and Go on Linux and Windows; the C guards keep passing. N5 now fails the call with DISCONNECTED in about 1 s instead of waiting the 10 s timeout (Windows C: `err=16` after about 1 s).
- Windows spec revised: `docs/level1-windows-shm.md` "Detecting a crashed peer" (named-pipe break probed after an idle wait) and the attach stop rule.
- Error codes: all peer-gone cases on SHM, UDS and named pipes now return the new disconnected error in all three languages (decision 1A); other SHM failures keep their previous codes.
- Proof the new tests detect the bugs: the new C checks built against unfixed HEAD all fail (see Execution Log). Go armv7 A/B on a calm host: HEAD fails `TestRobustShmSessionsReleasedAfterClientCrash` and `TestRobustShmClientNoticesCrashedServer` 8/8 (call waits 10 s), the fixed tree passes 8/8 in 1.3 s.

Tests and equivalent validation:

- Linux host: C ctest, Rust (391 pass), Go: the only failures are the six SOW-0043 reproduction tests per language (`test_robustness` handshake/timeout/shutdown checks; `test_service_rust` runs the same Rust filter).
- Interop: `interop_codec`, `test_uds_interop` (18), `test_shm_interop` (9), `test_service_interop` (18) all pass.
- Netdata Linux targets (`tests/run-linux-target.sh`, C + Rust + Go + interop): i386-gnu, x86_64-musl, aarch64-gnu, aarch64-musl, armv7-gnu-time64, armv6-musl, i386-musl clean apart from the SOW-0043 tests. Failures seen in the first pass on armv7-gnu, armv7-musl, armv6-musl and i386-musl coincided with high memory-stall time (`/proc/pressure/memory` some avg10 70-84%) charged to the test session itself, while over 100 GB of RAM stayed available; kernel counters point to direct reclaim and compaction stalls on fragmented memory (`/proc/vmstat` compact_stall, allocstall), but the counters were not sampled per run, so the cause is not proven. Symptoms: e.g. a 100 ms SHM receive timeout measured 49 s, helpers missing a 10 s ready window. Reruns cleared all of them. The final armv7-musl rerun was sampled during the run: it finished in 192 s (the two failed runs took 1101 s and 1315 s), C and Rust failed only the six SOW-0043 tests, interop passed 6/6, memory-stall time stayed at 0 and `compact_stall`/`allocstall` did not move. Go is skipped on musl targets by design (pure Go does not link libc; covered by the gnu targets).
- Windows (costa-win11, MSYS2): Rust 269 pass, five SOW-0043 failures; Go only the five `TestWinRobust*` SOW-0043 failures. Full unfiltered C ctest (37 tests, including every Windows interop suite): 34 pass; `test_win_robustness` fails only its five SOW-0043 checks; `test_lookup_scale_win_interop` and `test_lookup_scale_win_shm_interop` hit the 1800 s ctest timeout from a test-harness defect, fixed here (next item).
- Windows harness defect found by that run: the interop scripts stopped servers with `kill`, then polled for the 60 s client timeout before `taskkill`. The C fixtures are built with MSYS `gcc` and link `msys-2.0.dll`; the MSYS runtime acts on a signal only when a thread re-enters it, so a server blocked in a Win32 wait never exits on SIGTERM (measured: C server alive 22 s after `kill`, Rust and Go servers gone in about 185 ms). The lookup servers never exit on their own (`tests/fixtures/c/interop_service_win.c:431,455`), so each C-server pair cost about 100 s at idle priority and the 18-pair test overran 30 min. The C client itself completes the 8192-item lookup in 111-133 ms with or without `nice`. Fixed: `stop_server` in `tests/test_service_win_interop.sh`, `tests/test_cache_win_interop.sh` and `tests/test_lookup_scale_win_interop.sh` waits a 2 s grace, then force-stops the Windows PID, and no longer ends in an unbounded `wait`. The first rerun showed the force-stop silently skipped: the scripts looked up `taskkill` on PATH, and a minimal PATH without `System32` (as in the validation wrapper) left every C server running; `taskkill` is now resolved through `SYSTEMROOT` when PATH lacks it. Rerun of every affected Windows interop suite (`test_cache_win_go`, `test_win_shm_interop`, `test_service_win_interop`, `test_service_win_shm_interop`, both lookup-scale tests, `test_cache_win_interop`, `test_cache_win_shm_interop`): 8/8 pass, lookup-scale in 68 s and 66 s, zero interop processes left running.
- Static: `cargo fmt --check`, Rust 1.91.0 build, gofmt, `go vet` (linux and `GOOS=windows`), `git diff --check` clean; clippy warnings only on lines this SOW did not change.
- Benchmarks: not needed (decision 4); see the performance assessment under Risk and blast radius.

Real-use evidence:

- The interop suites run real C, Rust and Go server/client processes across UDS and SHM; the crash tests kill real helper processes mid-session.

Reviewer findings:

- Self-review during validation found the over-broad probe (any transport error relabelled as DISCONNECTED); fixed by narrowing to the send/recv-failure class (Execution Log, correction).

Same-failure search:

- Windows server stop: every Windows test script was checked for `kill` without a forced fallback. `tests/test_win_shm_interop.sh` (cleanup, not-ready path, and the server wait after a client failure) and `tests/run-windows-bench.sh` (cleanup, not-ready path, forced-stop path followed by an unbounded `wait`, reachable with `NIPC_WINDOWS_TOOLCHAIN=msys`) now use a bounded force-stop helper. The other Windows scripts start no background servers.

- Close-on-exec: every remaining `socket`/`accept`/`pipe` call without the flag (C `netipc_uds_lifecycle.c:39,51`, `netipc_service_posix_client.c:102`; Rust `posix.rs:759,780`; Go `uds_cloexec_other.go:15,24`) is the fallback for platforms without `SOCK_CLOEXEC`/`accept4`/`pipe2` and sets the flag immediately (Go under `syscall.ForkLock`). C and Rust SHM opens all use `O_CLOEXEC`; Go uses `os.OpenFile` (close-on-exec by default) and `F_DUPFD_CLOEXEC`.
- `owner_alive`: no code references remain; only the removal note in `docs/netipc-integrator-skill.md`.
- Peer-gone error mapping: all client receive/send paths (C, Rust, Go; POSIX and Windows; baseline and SHM) reviewed and mapped.

Sensitive data gate:

- This SOW, the docs, the integrator skill, `AGENTS.md`, the vendoring skill, the scripts and code comments contain no secrets, customer data, private endpoints or workstation paths; Netdata evidence is cited as `ktsaou/netdata @ ae9113f2cc`. `.agents/sow/audit.sh` sensitive-pattern scan: clean.

Artifact maintenance gate:

- `AGENTS.md`: updated with the Test Cadence section (decision 4), the clarified benchmark command line, and the benchmark item in the vendoring preflight rule.
- Runtime project skills: `project-netdata-vendoring` gains the performance-assessment step (step 7, blocker and report line), per decision 4. No vendoring happens in this SOW.
- Specs: the public specs under `docs/` were updated (level1 POSIX SHM, Windows SHM, POSIX UDS, level2 typed API). No `.agents/sow/specs/` change: the behaviour is fully captured in the public specs.
- End-user/operator docs: same `docs/` updates.
- End-user/operator skills: `docs/netipc-integrator-skill.md` updated (new error code, liveness behaviour, close-on-exec, `owner_alive` removal).
- SOW lifecycle: moved pending -> current at start; completed -> done in the closing commit. New pending SOW-0050 tracks the Netdata vendoring of this SOW and SOW-0043.

## Outcome

Completed. Dead peers are detected in C, Rust and Go on POSIX and Windows: SHM servers end sessions whose control channel the client closed (K1, K2), SHM clients fail a waiting call with the new disconnected error within about one 100 ms slice of the server's death instead of waiting the full timeout (N5), peer-gone failures on UDS, named pipes and SHM report that disconnected error, SHM attach stops retrying once the server closed the session, and every NetIPC socket and SHM fd is close-on-exec. `owner_alive` is removed. The Windows interop scripts now force-stop their servers, so the full Windows ctest no longer loses an hour to two timed-out tests. Remaining SOW-0043 reproduction tests fail as expected and are next.

## Lessons Extracted

- Map a transport failure to "peer disconnected" only for the send/recv-failure class plus a confirming probe. Probing after any error mislabels protocol violations when the peer closes right after replying.
- Go `syscall.Dup` does not set close-on-exec; use `F_DUPFD_CLOEXEC`.
- A liveness check that compares a cached value with the same shared memory it was read from can never fail; liveness must come from an independent channel (the session socket or pipe).
- `/proc/pressure/memory` measures time stalled in the allocator, not free RAM: stalls can be high with most RAM available (reclaim and compaction on fragmented memory). Before blaming a timing failure on the host, sample `/proc/vmstat` (compact_stall, allocstall) and the per-cgroup `memory.pressure` during the run; do not attribute it to another workload without evidence.
- On Windows, MSYS-built test fixtures (`msys-2.0.dll`) do not die on `kill` while blocked in a Win32 wait; scripts must force-stop the Windows PID after a short grace. A test that is "slow" may be a harness waiting out its own timeouts: time the product binary on its own before suspecting it.
- A test with no per-step output under ctest looks hung; stall detection must allow for the longest legitimate step.

## Followup

- SOW-0043 (handshake, timeouts, shutdown): the six POSIX and five Windows reproduction tests still failing belong to it; next.
- The SHM "peer dead" error (C `NIPC_SHM_ERR_PEER_DEAD` `netipc_shm.h:55`, Rust `ShmError::PeerDead` `shm.rs:72`, Go `ErrShmPeerDead` `shm_linux.go:77`) is not returned by any path, and was not at HEAD either (`owner_alive` returned a boolean). Removal rejected: dropping a public enum member renumbers the following C values (an ABI break) for no behavioural gain; the value stays reserved.
- Netdata vendoring: single PR after SOW-0043 (user decision 3a), through the vendoring preflight; tracked by `.agents/sow/pending/SOW-0050-20261031-netdata-vendor-robustness-fixes.md`.

## Regression Log

None yet.
