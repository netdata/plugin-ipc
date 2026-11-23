# SOW-0049 - macOS Support

## Status

Status: completed

Sub-state: delivered; validated on macOS, Linux (native and all 9 Netdata targets), FreeBSD 14.3 and Windows.

## Requirements

### Purpose

NetIPC must build and run on macOS in C, Rust and Go, with cross-language interoperability.

### User Request

2026-10-29: "netipc should build and run on all these platforms" (Linux on all architectures, Windows, FreeBSD and macOS). Fix designs are presented to the user one issue at a time.

### Assistant Understanding

Facts:

- macOS has no AF_UNIX `SOCK_SEQPACKET` (apple-oss-distributions/xnu `bsd/kern/uipc_proto.c` registers only stream and datagram unix protocols), so the documented UDS SEQPACKET baseline (`docs/level1-transport.md:466`) cannot work as specified.
- Go: darwin lacks `syscall.MSG_NOSIGNAL` (`transport/posix/uds.go`; macOS uses `SO_NOSIGPIPE`); `service/raw/client_unix.go` and `server_unix.go` reference Linux-only SHM types.
- Rust: same Linux-only symbols as FreeBSD (`server_session_unix.rs:299` `__errno_location`, `server_unix.rs:43`).
- A macOS host is reachable for testing (macOS 26.5, arm64; has clang, cargo/rustc and bash 3.2; no cmake, ninja or go; checked 2026-10-31).
- Measured on that host (2026-10-31): `socket(AF_UNIX, SOCK_SEQPACKET, 0)` fails with "Protocol not supported"; `fchmod()` on an unbound AF_UNIX stream socket fails with EINVAL, and the socket file is created `0777 & ~umask`. The SOW-0032 owner-only mechanism (`fchmod` before `bind`) therefore fails closed on macOS, so the macOS listener needs its own owner-only mechanism.

Inferences:

- A macOS baseline transport needs a design: stream socket with message framing, or datagram socket; either changes the transport spec for macOS.

Unknowns:

- The transport design choice (user decision).

### Acceptance Criteria

- C, Rust and Go build and pass their test suites on macOS; interop across all language pairs passes there.
- The transport spec documents the macOS baseline.

## Analysis

Sources checked:

- SOW-0046 investigation (`.agents/sow/done/SOW-0046-20261029-go-cross-platform-build.md`).
- Build and test probe on the macOS host (2026-11-01, tree synced from commit `7b73adc`, cmake 4.4.5, ninja 1.13.2, Go 1.27.3 installed under the host user's `~/opt`):
  - `CMakeLists.txt:39-44` keeps the POSIX service stack off on macOS (`NOT APPLE`); with that gate lifted on the host copy only, the C library, C services and Rust build; the only C failure is `tests/fixtures/c/test_robustness.c` calling the Linux-only `pthread_timedjoin_np`.
  - Go fails only at `src/go/pkg/netipc/transport/posix/uds.go:170` (`syscall.MSG_NOSIGNAL` undefined on darwin).
  - With the default gate, CTest runs 10 tests; the UDS tests in C, Rust and Go fail at socket creation (SEQPACKET).
- Socket probes on the macOS host (2026-11-01):
  - socket file mode right after `bind()`: `0755` (umask 022);
  - `chmod()` by path after bind, then `rename()` to another path in the same directory: both succeed; the renamed socket is `0600` and connectable;
  - `connect()` to a mode-0000 socket owned by the same user: `EACCES`, so macOS enforces socket file permissions on connect;
  - non-blocking connects to a full backlog: `ECONNREFUSED` at once;
  - `MSG_NOSIGNAL` (0x80000) is defined in the SDK and makes `send()` to a closed peer return `EPIPE` without a signal; `SO_NOSIGPIPE` is accepted;
  - default AF_UNIX stream `SO_SNDBUF` and `SO_RCVBUF`: 8192;
  - no `pipe2()` or `accept4()` declarations; `SOCK_CLOEXEC` undefined.
- Receive paths: every language reads the first packet and decides "complete or first chunk" from the received datagram length:
  - C `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c:171-189`, `:357-366`, continuation reads `:242-266`;
  - Rust `src/crates/netipc/src/transport/framing.rs` `receive_message` over the `raw_recv_control` closure (`src/crates/netipc/src/transport/posix.rs:360-383`);
  - Go `src/go/pkg/netipc/transport/internal/framing/receive.go` over the `Recv` hook (`src/go/pkg/netipc/transport/posix/uds_receive.go:111-150`).
- Senders fill the first chunk exactly to `packet_size - 32` payload bytes (C `netipc_uds_send.c:175-177`, Rust `framing.rs:157-162`, Go `transport/internal/framing/send.go`), and every continuation header carries `chunk_payload_len` (`docs/level1-wire-envelope.md:125-157`).
- Higher layers already treat a timeout or abort as a broken session (C `src/libnetdata/netipc/src/service/netipc_service_common.c:422-426`).
- No macOS CI job exists (`.github/workflows/*` run on `ubuntu-latest` and one `windows-latest`); the repository is public.

Current state:

- macOS builds only the protocol and UDS transport (C), Rust and the Go codec; no transport works at runtime.

Risks:

- A framing layer for stream sockets is new protocol surface that must stay identical across three languages.
- Stream sockets have no message boundaries: a partially consumed message leaves the byte stream desynchronized, so receive failures after partial consumption must end the session.
- Robustness tests that craft raw SEQPACKET datagrams (short packets, truncated chunks) behave differently on a stream; their macOS expectations must be derived, not weakened.
- macOS ships bash 3.2; shell interop scripts may use newer bash features.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The baseline transport relies on a socket type macOS lacks; the owner-only socket mechanism relies on `fchmod()` of an unbound socket, which macOS rejects; Go uses a send flag darwin's `syscall` package lacks. See Facts and Analysis.

Evidence reviewed:

- See Analysis (source paths, host build probe, socket probes).

Affected contracts and surfaces:

- Specs: `docs/level1-posix-uds.md` (new macOS stream section: socket type, framing, packet size, owner-only bind, partial-receive rule), `docs/level1-transport.md` (baseline transport table, SHM platform note).
- C: `src/libnetdata/netipc/src/transport/posix/` (socket creation, raw send/receive, handshake receive, listener bind, close cleanup), `CMakeLists.txt` (POSIX runtime on macOS).
- Rust: `src/crates/netipc/src/transport/posix.rs` (same points), a new stream helper module.
- Go: `src/go/pkg/netipc/transport/posix/` (same points, darwin build files).
- Tests: C `tests/fixtures/c/test_uds.c`, `test_robustness.c`, `test_service.c`; Rust `transport/posix_tests.rs`, `service/raw_unix_tests.rs`, `service/raw_robustness_unix_tests.rs`; Go `transport/posix/*_test.go`, `service/raw/*_unix_test.go`; shell interop scripts.
- CI: a new macOS workflow job.
- Wire format: unchanged (decision 1A).
- Profile bit 0 keeps its value; on macOS it denotes the stream baseline (peers on one host always share the platform).

Existing patterns to reuse:

- The per-language "receive one packet" hook (C `nipc_uds_raw_recv`, Rust `raw_recv_control` closure, Go `Recv` hook): the macOS stream reader plugs in there, so the shared framing code is unchanged.
- FreeBSD-style platform splits: C `#if` blocks in `netipc_uds_lifecycle.c`, Rust `#[cfg(target_os = ...)]` helpers in `posix.rs`, Go build-tagged files (`uds_cloexec_*.go`, `uds_sndbuf_*.go`).
- Existing EINTR injection (C `--wrap`, Rust `eintr_inject`, Go fault hooks) for the stream helper tests on Linux.
- Endpoint lock (`{service}.lock`) to guard the new bind directory.

Risk and blast radius:

- Linux, FreeBSD and Windows keep their current code paths: the stream reader, stream sender and rename-bind are selected at compile time for macOS only; the stream helpers are compiled on every POSIX system for unit tests, but Linux and FreeBSD sessions do not call them.
- Interop risk: all three languages must read the same packet lengths from the same headers; covered by the macOS interop suites (C/Rust/Go pairs).
- Security: the owner-only guarantee on macOS moves from `fchmod` to a private-directory bind plus rename; a mistake would expose the endpoint to other users. Covered by a test that checks the final mode and the absence of a wider window, and by the existing peer-permission tests.

Sensitive data handling plan:

- No secrets, customer data or private host names; the macOS host is named by role ("macOS test host") in SOWs, specs, docs, project skills, agent instructions and code comments. Evidence is code paths and command output only.

Implementation plan:

1. Spec: write the macOS stream section in `docs/level1-posix-uds.md` and update `docs/level1-transport.md`.
2. C: stream helpers (`read exactly`, `send all` over iovecs, packet reader that derives the packet length from the header) compiled on all POSIX; macOS sessions use them in data and handshake receives and sends; a receive that consumed bytes without returning a complete message shuts the socket down; `SO_NOSIGPIPE` on every session socket on macOS; fixed 256 KiB packet-size limit on macOS; rename-bind through `{run_dir}/{service}.d/s` under the endpoint lock, with stale-directory cleanup; enable the POSIX runtime in CMake on macOS; portable replacement for `pthread_timedjoin_np` in the robustness test.
3. Rust: the same in `posix.rs` plus a stream helper module, with unit tests on Linux.
4. Go: the same in darwin build files plus shared stream helpers, with unit tests on Linux.
5. Tests: give each raw-datagram robustness test a macOS-correct expectation (stream semantics), keep every test running on every platform, no skips.
6. CI: add a macOS job (CTest, `cargo test`, `go test`) with pinned actions, matching the existing workflows' pinning.
7. Docs and skill: integrator skill, AGENTS.md validation commands for macOS.

Validation plan:

- Linux: CTest, `cargo test` (stable and 1.91), `go test`, all four interop scripts, clippy and fmt gates; the new stream helper tests run here with EINTR and partial-transfer injection.
- macOS host: CTest, `cargo test`, `go test`, interop scripts across all language pairs.
- FreeBSD 14.3: CTest and `cargo test` (shared POSIX code changed).
- Netdata Linux targets for the C and Rust POSIX files touched (all 9, C/Rust/interop).
- Windows: `cargo test` and clippy for the Windows target (shared Rust crate), plus CTest when C shared files change.
- Performance assessment: no established-session code on Linux, FreeBSD or Windows changes; the macOS data path is new and not benchmarked (no macOS benchmark floors exist). Evidence recorded per file at close.

Artifact impact plan:

- AGENTS.md: add macOS validation commands (project-wide platform list).
- Runtime project skills: assess at close (vendoring skill unaffected unless Netdata vendors macOS files).
- Specs: `docs/level1-transport.md`, `docs/level1-posix-uds.md`.
- End-user/operator docs: same, plus `docs/getting-started.md` if it lists platforms.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: split from SOW-0046.

Open-source reference evidence:

- apple-oss-distributions/xnu `bsd/kern/uipc_proto.c` (no local checkout; the claim is backed by the direct measurement on the macOS host recorded in Analysis).

Open decisions:

- None; see Implications And Decisions.

## Implications And Decisions

User decisions (2026-10-31, user):

1. macOS baseline transport: AF_UNIX `SOCK_STREAM` with explicit message framing for the NetIPC envelope (new spec section; C, Rust and Go).
2. cmake, ninja and Go (latest stable) may be installed on the macOS test host.
3. Scheduling: after the SOW-0050 Netdata vendoring (Netdata does not build NetIPC on macOS).
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

User decisions (2026-11-01, user: "long-term-best" for every item):

1. Stream framing (1A): no new bytes on the wire. A packet is the 32-byte header plus the bytes it declares: an outer header (`NIPC` magic) declares `min(payload_len, packet_size - 32)` bytes for its first packet, a continuation header (`NCHK` magic) declares `chunk_payload_len`. Senders already fill the first chunk exactly, so the bytes are identical to the SEQPACKET transport.
2. Packet size on macOS (2A): fixed 256 KiB limit, or the configured size when smaller; `SO_SNDBUF` is not consulted (a stream has no per-message cap). Chunking code unchanged.
3. Owner-only socket on macOS (3A): bind inside a private `0700` directory `{run_dir}/{service}.d/`, `chmod 0600` the socket file by path, then `rename()` it to `{run_dir}/{service}.sock` and remove the directory. The endpoint lock guards the directory; a server that finds a leftover directory under the lock removes it. The umask is not changed and the socket is never reachable with a wider mode.
4. Partial receive on a stream (4A, refined 2026-11-01 by user choice A): a receive that consumed any byte of a message but did not return it complete (timeout, abort, limit, protocol or chunk error) shuts down the socket's receive side (`shutdown(SHUT_RD)`), so every later receive on that session fails. Sends stay valid: outgoing messages are always whole, so a partial read cannot misalign them, and the server's LIMIT_EXCEEDED reply after an oversized request header (`netipc_service_posix_server_session.c:150-167`, same in Rust and Go) must still reach the client for its capacity learning. Shutting down both directions (the first wording) would have turned that reply into a plain disconnect on macOS.
5. Coverage (5A): the stream helpers compile on every POSIX system and are unit-tested on Linux with stream socket pairs (including EINTR and partial-transfer injection); a macOS GitHub Actions job runs CTest, `cargo test` and `go test`; sessions use the stream helpers only on macOS.
6. SHM on macOS (6A): baseline only, as the spec states. Long-term-best because Netdata does not build NetIPC on macOS (user decision 3 above); a macOS SHM would also need `os_sync_wait_on_address`, which pure Go cannot call without cgo.

## Plan

See Pre-Implementation Gate, Implementation plan.

## Execution Log

### 2026-10-29

Created from the SOW-0046 platform-scope split.

### 2026-11-01

- Installed cmake 4.4.5, ninja 1.13.2 and Go 1.27.3 on the macOS test host under the host user's `~/opt` (approved, decision 2 of 2026-10-31); checksums verified for Go and CMake (ninja publishes none).
- Probed the build and socket semantics (Analysis); user decisions recorded; gate filled; moved to `current/`.
- C, Rust and Go implemented as decided:
  - C `netipc_uds.c` (stream helpers), `netipc_uds_receive.c`, `netipc_uds_handshake.c`, `netipc_uds_lifecycle.c`, `netipc_uds_internal.h`;
  - Rust `transport/posix.rs` and a new `transport/posix_stream.rs`;
  - Go `transport/posix/uds_stream.go` and darwin/other platform and bind files.
- Rules found while implementing, applied in all three languages:
  - a stream's first header is checked (full decode plus limits) before its body is read;
  - EOF before a packet is a disconnect, inside a packet a protocol error, inside a continuation a chunk error;
  - bytes beyond the receive buffer are not read.
- Go's stale-endpoint probe dialed `unixpacket` on darwin, which fails on every socket and would have unlinked a live server's endpoint. It now dials the platform's socket type (`uds_platform_*.go` `staleDialNetwork`).
- Test adjustments with stream-correct expectations (no skips): a shared `endStreamWrites`/`end_stream_writes` helper for raw short or over-long packets, full first packets in raw chunk tests, and macOS variants of the packet-size and EINTR-count tests.
- Problems found by the macOS runs, outside the transport:
  - `tests/interop-lib.sh` used `exec {fd}<` (bash 4.1+). Under macOS bash 3.2 with an EXIT trap, the scripts then exited 0 after the failure (false pass, reproduced). Rewritten with a fixed fd and a `sh -c 'echo $$; exec "$@"'` wrapper, since bash 3.2 sets no `$!` for a process substitution.
  - macOS `$TMPDIR` pushes `t.TempDir()` and interop run-directory socket paths past the 104-byte `sun_path`. Go tests got `shortTempDir`; the interop scripts got `interop_mktemp_run_dir` (`/tmp` on Darwin).
  - Shared test servers used `backlog 4` with bursts of 5 to 50 clients. macOS and FreeBSD refuse a connect to a full backlog at once, so those tests were nondeterministic there. The Go and Rust shared test configs now use 64; C already sized its burst tests.
  - `test_hardening` "session table growth" shrank the session table to 1 slot below `worker_count`, but the server has had no table growth since commit `8e2e43af`. The test itself wrote past the table (heap overflow under ASan on Linux and macOS; a Bus error on macOS). It now shrinks the table only to `worker_count`, the smallest valid size.
- Go receive: the stream branch runs before the SEQPACKET closures are built, and `PropagateRecvError` uses top-level functions, so the Linux receive allocates nothing new.

## Validation

Acceptance criteria evidence:

- macOS test host (arm64, macOS 26.5): CTest 24/24 (`test_uds` 243/243), `cargo test` 324 passed, `go test ./...` ok with `-count=3`; interop `test_uds_interop.sh`, `test_service_interop.sh`, `test_cache_interop.sh`, `test_lookup_scale_interop.sh` and `interop_codec.sh` pass across all C/Rust/Go pairs, with no processes left behind. An ASan build of the C suites (`build-asan` on the host) is clean after the `test_hardening` fix.
- The macOS stream baseline is specified in `docs/level1-posix-uds.md` ("macOS stream baseline") and in the baseline table of `docs/level1-transport.md`.
- Linux x86_64 (after the review fixes): build, CTest 32/32 (1 skipped: `test_shm_full_backing_store`, root-only, unaffected), every POSIX interop script (`interop_codec`, uds, shm, service, service_shm, cache, cache_shm, lookup_scale), `cargo test` with Rust 1.91.0 (393 passed), `cargo fmt --check` with both toolchains, the CI clippy gate (`-D clippy::correctness -D clippy::suspicious`) for Linux and `x86_64-pc-windows-gnu`, `go vet` and `go test ./...`.
- `cargo test` stable on Linux: 392 passed, 1 failed, `test_listen_excludes_starting_server` ("endpoint free after close: AddrInUse"), intermittent. Root cause confirmed with a standalone flock probe: a child forked by a concurrent test inherits a copy of the endpoint lock fd, and the flock stays held until that child exits. This is a regression of SOW-0052's claim, not caused by this SOW (no lock code changed here); it is reopened as a SOW-0052 regression (Followup).
- FreeBSD 14.3: CTest 24/24, `cargo test` 323 passed, `go test ./...` ok, the same five interop scripts pass.
- Windows (after the review fixes): CTest 20/20 (including the interop scripts on the rewritten `interop-lib.sh` under MSYS2), `cargo test` 273 passed, `go test ./...` ok.
- Netdata Linux targets (`tests/run-linux-target.sh <target>`, C, Rust, Go and interop): see the Linux targets entry below.

Linux targets:

- All 9 pass with the final code (C, Rust, Go and interop): x86_64-musl, i386-gnu, i386-musl, armv7-gnu-time64 and armv7-musl in the first run; armv7-gnu, armv6-musl, aarch64-gnu and aarch64-musl on a rerun, each started after memory PSI full avg10 stayed under 5% for a minute (peak during each run: 0).
- In the first run those four failed only on readiness and deadline timeouts while memory PSI full avg10 was 50-80% and runs were several times slower than normal (aarch64-gnu 1077 s, against 35 s on the rerun). Which targets failed changed between runs, and the same code then passed at no pressure, so those results were treated as invalid, not as bugs.

Tests added or changed (all run on every platform; no skips):

- Stream helpers unit-tested on Linux with stream socket pairs: C `test_uds.c` stream cases, Rust `posix_stream.rs` tests, Go `uds_stream_test.go`. They cover packet length from the header, EOF mapping, first-header checks (over-limit, undecodable, continuation first), and one-byte transfers with EINTR injection.
- macOS-only: partial receive ends reads (Go `uds_stream_darwin_test.go`, the same in C and Rust), rename-bind (`test_listen_bind_dir` in C, Rust and Go: final mode 0600, leftover directory reclaimed, foreign content and a directory at the socket name refused), fixed packet-size limit.

Reviewer findings (independent review of the full diff, static checks only):

1. Rust and Go checked less of the first header than C before reading its body, so a bad version could block until timeout: fixed, full decode in both, plus a bad-version test in each.
2. Rust created the bind directory with mode `0777 & ~umask` before the chmod: fixed with `DirBuilder::mode(0o700)`.
3. Go removed an empty directory named `s` inside a leftover bind directory, where C and Rust refuse: fixed with `syscall.Unlink`; foreign-subdirectory case added to all three `test_listen_bind_dir` tests.
4. C did not fail pending requests when the first packet's body read hit a receive error: fixed (`nipc_uds_inflight_fail_all`).
5. Go `streamBodyLen` converted 32-bit header fields to `int` unbounded (32-bit Go panic): fixed, bounded in `uint32` first.
6. Test scripts: `interop_stop_server` did not wait for a server that never printed READY (fixed: the drain process always starts and the wait covers server exit); `test_lookup_scale_interop.sh` expanded an empty array under `set -u` on bash 3.2 (fixed).
7. Small macOS stream buffers (8 KiB default) can deadlock a client that writes many pipelined requests before reading: confirmed as a property of the platform, documented in `docs/level1-posix-uds.md` ("Socket buffers") with pipelining guidance. Not a NetIPC defect: the same client pattern deadlocks on any stream transport once both buffers fill.
- Noted by the reviewer, not new: the Go server handshake maps a mid-HELLO timeout to `ErrRecv` where C and Rust return TIMEOUT (`transport/internal/framing/handshake.go`), on every platform. Confirmed by a probe test; tracked as SOW-0061.

Same-failure search:

- `exec {fd}` and other bash 4+ forms in the scripts macOS runs: only `interop-lib.sh` used them; fixed. The remaining scripts with bash 4 features (`run-linux-target.sh`, the Windows scripts) do not run on macOS.
- `backlog` values in shared test servers: Go `service/raw/client_test.go` and Rust `raw_unix_tests.rs` raised to 64; C tests already size their bursts.
- `t.TempDir()` used for socket run directories: replaced with `shortTempDir` in the Go transport, raw and lookup tests; the interop scripts use `interop_mktemp_run_dir`.
- `unixpacket` dials: only the stale probe; fixed.

Performance assessment:

- Linux, FreeBSD and Windows keep the same syscalls on the established-session path.
- C: `nipc_uds_wait_for_readable` (`netipc_uds.c`) lost `static` so the stream reader can share it; no call is added on SEQPACKET. Stream code is behind `NIPC_UDS_STREAM` (macOS only).
- Rust: `send_packet`/`send_control` (`transport/posix.rs:942`, `:950`) are cfg-selected one-line dispatchers that inline to `raw_send_iov`/`raw_send` off macOS.
- Go: `sendPacket` (`transport/posix/uds.go:149`) branches on the constant `streamTransport`, which folds away off darwin; the receive closures are built only after the stream dispatch, and `PropagateRecvError` uses top-level functions, so no closure allocation is added.
- No benchmark run for this SOW: the full benchmark run before vendoring (user ordering) covers these files. The macOS data path is new and has no benchmark floors.

Sensitive data gate: hosts named by role (macOS test host, FreeBSD VM, Windows host); evidence is code paths, test names and command output; no secrets, customer data or private endpoints in the SOW, specs, docs, skills, AGENTS.md or code comments.

Artifact maintenance gate:

- AGENTS.md: macOS validation bullet added (CI workflow, commands, bash 3.2 rule for the script paths macOS runs).
- Runtime project skills: `project-netdata-vendoring` unaffected: its preflight is platform-neutral, and `vendor-to-netdata.sh`/`diff-netdata-vendor.sh` copy whole directories, so new files are picked up.
- Specs: `docs/level1-posix-uds.md` and `docs/level1-transport.md` updated; `.agents/sow/specs/` holds no transport rules, so no change there.
- End-user/operator docs: `README.md` platform table (Linux, FreeBSD, macOS rows).
- End-user/operator skills: `docs/netipc-integrator-skill.md` macOS platform note.
- SOW lifecycle: completed, moved to `done/`; follow-ups mapped below.

Spec update: `docs/level1-posix-uds.md` ("macOS stream baseline", rename-bind under Socket permissions, EINTR note) and `docs/level1-transport.md` (baseline table).

Project skill update: none needed (see the artifact gate).

Lessons: see Lessons Extracted.

Follow-up mapping: see Followup.

## Outcome

NetIPC builds and runs on macOS in C, Rust and Go over an AF_UNIX stream baseline with explicit packet framing (same bytes as SEQPACKET), owner-only rename-bind, and the partial-receive rule. All suites and cross-language interop pass on macOS, and a macOS CI workflow runs them. Linux, FreeBSD and Windows paths are unchanged.

## Lessons Extracted

- A shell feature missing on an older bash can turn into a false pass when an EXIT trap overrides the status: interop scripts must fail closed, and scripts that run on macOS must stick to bash 3.2.
- macOS `$TMPDIR` is long enough to push socket paths past `sun_path`; socket tests need short run directories there.
- macOS and FreeBSD refuse a connect to a full listen backlog at once, where Linux queues it; burst tests must size the backlog.
- ASan on a second platform found a heap overflow that a test itself caused (`test_hardening`); run the sanitizer build when porting.
- Timing failures on the qemu Linux targets tracked host memory stalls (PSI memory full up to 80%) and moved between targets from run to run; sample PSI before triaging them, as AGENTS.md requires.

## Followup

- Fork-held endpoint lock (`test_listen_excludes_starting_server` intermittent): reopen SOW-0052 as a regression; fix design goes to the user.
- Go server handshake maps a HELLO timeout to `ErrRecv` (C and Rust: TIMEOUT), confirmed by a probe on Linux: SOW-0061 (pending).
- FreeBSD SHM: SOW-0059 (pending). FreeBSD 15.1: SOW-0056 (pending).
## Regression Log

None yet.
