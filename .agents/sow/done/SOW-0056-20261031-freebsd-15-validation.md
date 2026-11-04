# SOW-0056 - FreeBSD 15 Validation

## Status

Status: completed

Sub-state: delivered 2026-11-04; FreeBSD (14 and 15) runs the stream baseline.

## Requirements

### Purpose

Every Netdata-supported FreeBSD release must build and pass the NetIPC C, Rust and Go suites, with interop.

### User Request

2026-10-29: "netipc should build and run on all these platforms" (Linux on all architectures, Windows, FreeBSD and macOS). Standing rule: a Netdata-supported configuration that cannot be tested is a blocker.

### Assistant Understanding

Facts:

- SOW-0048 validated FreeBSD on a 14.3 test VM only.
- FreeBSD 13 reached end of life on 2026-04-30; the supported releases are 14.4, 14.5 and 15.1 (FreeBSD security information page, `https://www.freebsd.org/security/`, read 2026-10-31).
- Netdata's distro matrix lists only FreeBSD 13-STABLE (`netdata/netdata`, `.github/data/distros.yml:504`, as read during SOW-0048).
- FreeBSD 15 reworked AF_UNIX sockets (review D44151). NetIPC's packet-size rule depends on SEQPACKET semantics (`docs/level1-posix-uds.md`, "Packet size"), so 14.x results do not carry over.
- Netdata builds NetIPC only on Linux and Windows today, so this does not block vendoring the current Linux/Windows consumers.

- FreeBSD 15.1 test VM provided by the user on 2026-10-31 (named here by role only): 15.1-RELEASE-p6 amd64, 8 CPUs, 16 GiB; has `cmake`, `bash`, `git`; lacks `ninja` and Rust; Go 1.27.3 at `/usr/local/go`. Root and `/tmp` are UFS. `net.local.seqpacket.maxseqpacket = 65536` (14.3: 8192).

Inferences:

- 14.4/14.5 share the 14.3 unix-socket code; 15.1 is the real gap.

Unknowns:

- Which FreeBSD releases Netdata will support once it enables NetIPC there (a Netdata decision).

### Acceptance Criteria

- On FreeBSD 15.1: configure, build, ctest, `cargo test`, `go test ./...` and `tests/interop_codec.sh` pass (the AGENTS.md FreeBSD procedure).
- The maximum SEQPACKET datagram versus `SO_SNDBUF` is measured on 15.1 and the per-platform overhead in C, Rust and Go matches it.
- SHM (SOW-0059) on 15.1: the SHM suites and SHM interop scripts pass, Go also with `GOARCH=386`; the run directory and `/tmp` used by the tests are on UFS or tmpfs (ZFS rejects the region reservation), and the filesystem type is recorded.

## Analysis

Baseline run on the FreeBSD 15 test VM (HEAD 0a6aed3): configure and build pass; CTest 26/31, failing `test_uds` (chunking and pipelining checks), `test_uds_rust` (timeout), `test_service_rust` (chunked response), `test_uds_go` (timeout), `test_uds_interop`. The run was stopped there; the cause below explains every failure.

Root cause: FreeBSD 15 `AF_UNIX` `SOCK_SEQPACKET` keeps record boundaries only where the sender sets `MSG_EOR` (FreeBSD 15.1 `socket(2)`, "SEQUENCED PACKET SOCKET TYPE": "Record boundaries are set by the sender with the MSG_EOR flag of send(2) or sendmsg(2) functions"; man page refactor `freebsd/freebsd-src @ 86d17239233e`). NetIPC never sets `MSG_EOR`, so consecutive packets merge into one record and a receive returns several packets.

Probe evidence (small C programs on the 15.1 VM, the 14.3 VM and Linux; socketpair and bind/connect sockets give the same results):

| Behaviour | FreeBSD 14.3 | FreeBSD 15.1 | Linux |
|---|---|---|---|
| 3 sends without `MSG_EOR`, one receive | one record each | all merged (`recv` returns 150 or 216 bytes) | one record each |
| sends with `MSG_EOR` | one record each | one record each | one record each (flag accepted) |
| receive into a buffer shorter than the record | rest dropped, `MSG_TRUNC` | rest delivered by the next receives; `MSG_EOR` only on the last part | rest dropped, `MSG_TRUNC` |
| largest single send | `SO_SNDBUF` (any size set) | the receiver's free space (default `recvspace` 65536), whatever `SO_SNDBUF` or `maxseqpacket` is | about `SO_SNDBUF` |
| non-blocking send of 10000 bytes into a receiver holding 60000 | `EMSGSIZE` (buffer 8192) | partial: returns 5536, then `EAGAIN` | whole |

Consequences on 15: packets merge (the observed failures); a non-blocking pipelined send can write part of a packet (SEQPACKET code treats a short send as an error); a packet can arrive in parts; the packet-size rule `min(SO_SNDBUF, 262144)` does not bound what one send accepts. FreeBSD 15 `SOCK_SEQPACKET` is a byte stream with record marks, close to the macOS stream baseline (`docs/level1-posix-uds.md`, "macOS stream baseline"), which already handles partial writes and finds packet boundaries from the headers. SHM passes no descriptors over the socket (no `SCM_RIGHTS` in `src/`), so SHM does not depend on the socket type.

Scope: Netdata builds NetIPC only on Linux and Windows today, so no shipped consumer is affected; under the zero-known-bug policy the FreeBSD defect is fixed here.

## Pre-Implementation Gate

Status: ready (2026-11-04, decisions 4A and 5A)

Problem / root-cause model:

- Unvalidated platform release; no known defect.

Evidence reviewed:

- SOW-0048 Validation (FreeBSD 14.3 results and the SEQPACKET measurement).
- SOW-0059 (SHM on FreeBSD, UFS/tmpfs requirement) and the AGENTS.md FreeBSD procedure.
- The 15.1 VM: `net.local.seqpacket.maxseqpacket = 65536`, `recvspace = 65536`; root and `/tmp` on UFS.

Affected contracts and surfaces:

- POSIX UDS packet-size derivation (`SNDBUF_OVERHEAD` in Rust `transport/posix.rs`, Go `uds_sndbuf_*.go`, C `netipc_uds_*`), only if 15.1 differs.
- After the Analysis (decision 4A): the FreeBSD socket type and every stream-baseline gate in C, Rust and Go; the stream partial-receive rule (session flag instead of `shutdown(SHUT_RD)`, which also changes macOS); the session struct (`recv_failed`, C public header); has-pending; specs `docs/level1-posix-uds.md`, `docs/level1-transport.md`; README, AGENTS.md, `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- SOW-0048 per-platform overhead constants and the AGENTS.md FreeBSD procedure.

Risk and blast radius:

- Limited to FreeBSD builds; Linux and Windows are unaffected by FreeBSD-only constants.

Performance assessment:

- Validation only; a constant change would affect packet size proposal at connection setup only. No benchmark run.
- After decision 4A: FreeBSD sessions move to the stream code (per-request send and receive), but no benchmark suite runs on FreeBSD (`tests/run-posix-bench.sh` is the Linux suite). On Linux the established-session path gains one boolean read: `nipc_uds_session_has_pending` (`netipc_uds.h`), Rust `has_pending`, Go `HasPending`, and the initial `eof` of the pipelined send (C `netipc_uds_send.c` `send_with_progress`, Rust `send_with_progress_loop`, Go `sendWithProgress`); the receive flag check sits in stream-only code. Assessment: no measurable effect on the Linux rows; no benchmark run.

Sensitive data handling plan:

- Name the host only as "the FreeBSD 15 test VM"; record command results, not host details. Covers this SOW, specs, docs, skills and code comments.

Implementation plan:

1. Run the AGENTS.md FreeBSD procedure on 15.1.
2. Measure the SEQPACKET maximum; fix any mismatch in all three languages with tests.

Validation plan:

- The acceptance criteria commands on 15.1, then the Linux and 14.x suites again if code changed.

Artifact impact plan:

- `docs/level1-posix-uds.md` packet-size notes if 15.1 differs; otherwise none, recorded at close.
- After decision 4A: `docs/level1-posix-uds.md` ("Transport type", "Packet size", "Stream baseline", "Pipelined sends", EINTR note), `docs/level1-transport.md` (baseline table), README platform table, AGENTS.md macOS line, `docs/netipc-integrator-skill.md` platform notes.

Open decisions:

- None (decisions 4A, 5A below).

## Implications And Decisions

User decision (2026-10-31, user): the user will provide a FreeBSD 15.1 VM; validate there when it is available.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

- Toolchains (2026-11-04, user: "3a"): install `rust` and `ninja` from FreeBSD packages on the 15.1 VM, like the 14.3 VM (rust 1.98.1, ninja 1.13.2). Rejected: B, rustup with 1.91 (MSRV stays validated on Linux and Windows).

- FreeBSD baseline transport (2026-11-04, user: "4a"): FreeBSD, every version, uses the stream baseline (`SOCK_STREAM`, packet boundaries from the headers) already used on macOS, in C, Rust and Go; SHM stays available on FreeBSD. Rejected: B, SEQPACKET with `MSG_EOR`, partial-send resume and record reassembly (a third mode re-implementing the stream logic, tied to semantics FreeBSD already changed once); C, runtime choice by kernel version (two modes on one OS).
- Scope (2026-11-04, user: "5a"): the fix is part of this SOW. Rejected: B, a separate transport SOW.
- Stream partial-receive rule (2026-11-04, assistant decision under the autonomous-execution instruction, within 4A, for user review): a session flag replaces `shutdown(SHUT_RD)` on every stream platform, macOS included; later receives fail without reading the socket, a pipelined send no longer queues inbound bytes, and has-pending is false. Evidence: Execution Log (FreeBSD 15.1 `EPIPE` on the peer's sends, FreeBSD 14.3 still returning queued bytes). Rejected: per-OS `shutdown` handling.

## Plan

See the gate.

## Execution Log

### 2026-10-31

Created from SOW-0048 follow-up mapping.

### 2026-11-04

- Toolchains installed on the 15.1 VM (decision above); HEAD synced; baseline run and probes as recorded under Analysis. Probe processes and the stopped run were cleaned up by PID.
- A temporary `net.local.seqpacket.maxseqpacket=8192` on the test VM (restored to 65536 in the same command) lowered the default `SO_SNDBUF` to 8192 while a 65536-byte send still succeeded.
- Implementation (decision 4A):
  - C: `NIPC_UDS_STREAM` for `__APPLE__ || __FreeBSD__` (`netipc_uds_internal.h`); the FreeBSD `SO_SNDBUF` overhead branch removed (stream ignores `SO_SNDBUF`). Tests: `uds_test_socket.h` stream for FreeBSD; `test_stream_failed_receive_drops_pending` runs on every stream platform (`TEST_UDS_STREAM`); the bind-directory test stays macOS.
  - Rust: every stream gate in `transport/posix.rs` and `posix_tests.rs` is `any(target_os = "macos", target_os = "freebsd")`; `set_nosigpipe`, `bind_owner_only` and `test_listen_bind_dir` stay macOS; `SNDBUF_OVERHEAD` is Linux-only.
  - Go: `uds_transport_stream.go` (`darwin || freebsd`) and `uds_transport_seqpacket.go` (Linux and other SEQPACKET systems) hold the socket type and packet limit; `uds_platform_darwin.go` / `uds_platform_other.go` keep only the SIGPIPE handling; `uds_sndbuf_freebsd.go` removed; tests renamed out of `_darwin` (`uds_stream_baseline_test.go`, `socktype_stream_test.go`; the bind-directory test moved to `uds_bind_darwin_test.go`), because a `_darwin` filename restricts a file to macOS whatever its build tag.
- First run of the stream baseline on both FreeBSD VMs: every interop suite passed; one test failed in all three languages, `test_stream_partial_receive_ends_reads`: after `shutdown(SHUT_RD)` FreeBSD 15.1 failed the peer's sends with `EPIPE`, and FreeBSD 14.3 still returned the queued bytes (a truncated packet instead of a receive failure). The stream baseline relied on macOS `SHUT_RD` semantics.
  - Fix (within 4A, assistant decision under the autonomous-execution instruction, for user review): the session records the failed receive (C `recv_failed` in `nipc_uds_session_t`, Rust `recv_failed`, Go `recvFailed`) and every later receive returns the receive error (C `NIPC_UDS_ERR_RECV`, Rust `UdsError::Recv(0)`, Go `ErrRecv`) without reading the socket; `shutdown(SHUT_RD)` is no longer called. Same behaviour on macOS, FreeBSD 14 and FreeBSD 15. Rejected: per-OS shutdown handling (still differs by release).
- Second run: one C failure on both VMs, `test_eintr_retried` ("every injected EINTR was hit"): on a stream the handshake packets go through `sendmsg`, not `send`. The test now injects (send 0, sendmsg 4, recv 4) on stream platforms, as the Rust test already did.

## Validation

Acceptance criteria:

- FreeBSD 15.1 (UFS `/tmp` and build directory): configure, build, CTest 31/31, `cargo test` (lib 398 passed), `go test ./...` (all packages ok) and `GOARCH=386 go test ./...` (all ok), `tests/interop_codec.sh`, UDS, SHM, service, service SHM, cache, cache SHM and lookup-scale interop: all pass (final run, 2026-11-04).
- SEQPACKET maximum versus `SO_SNDBUF` on 15.1: measured (Analysis table). The result is that 15.1 SEQPACKET does not keep packets apart, so the per-platform overhead question is replaced by decision 4A; FreeBSD no longer consults `SO_SNDBUF` (stream rule: configured size or 262144).
- SHM on 15.1: the SHM suites in CTest and the SHM interop scripts pass, Go also with `GOARCH=386`; filesystem UFS, recorded by the run script.

Tests or equivalent validation:

- Before the fix (HEAD 0a6aed3) on 15.1: CTest 26/31 (Analysis). After: as above.
- FreeBSD 14.3: the same procedure, all pass (CTest 31/31, cargo 398, Go and Go 386 ok, all interop).
- macOS test host: CTest 24/24, `cargo test` 330, `go test ./...`, UDS, service, service SHM, cache, cache SHM, lookup-scale and codec interop pass (`test_shm_interop.sh` does not apply on macOS, as in SOW-0049).
- Linux: CTest 32/32, `cargo test`, `go test ./...`, UDS, service and SHM interop; `cargo fmt --check` with stable and 1.91.0; `cargo check --all-targets` for `x86_64-unknown-linux-gnu`, `x86_64-unknown-freebsd`, `aarch64-apple-darwin` without warnings; `go vet` for linux (amd64, 386, arm, arm64), freebsd (amd64, 386) and darwin; `gofmt -l` empty. The Netdata Linux target suites were not rerun: the Linux C and Rust code is unchanged (all edits are inside FreeBSD/macOS gates except the one-boolean `recv_failed` reads), and the Go Linux build is the same constants moved between files, checked by `go vet` on 386, arm and arm64.

Real-use evidence: the interop scripts run real C, Rust and Go servers and clients across all 9 pairs over the FreeBSD stream baseline, including SHM sessions negotiated over it.

Reviewer findings (independent read-only review of the diff) and handling:

1. Gates, parity, constructors, SHM gating, macOS-only items: no defect found.
2. After a failed stream receive, a pipelined send still drained the abandoned stream into the pending queue, so has-pending could report data that the next receive refuses (the has-pending contract says the next receive can return it): fixed in C, Rust and Go; the send starts as if the peer had closed (`eof` from the flag) and has-pending is false once the flag is set (on macOS this matches what `SHUT_RD` gave before: EOF).
3. Docs: `SO_NOSIGPIPE` described as set on every stream session (macOS only); "Pipelined sends" and the EINTR note said "macOS stream": fixed.
4. Stale comments (stream code "only macOS", `O_NONBLOCK` reason, `SHUT_RD` in test comments, FreeBSD 8 KiB defaults): fixed. The pipeline tests set 4096-byte buffers themselves, so they exceed the buffers on FreeBSD 15 too.
5. Risk: FreeBSD binaries before and after this change cannot connect to each other (`EPROTOTYPE`), and a new server's stale-socket probe treats an old, lockless server as stale. Accepted as no-impact: Netdata does not build NetIPC on FreeBSD today (Analysis, Scope), and when it does, the C, Rust and Go copies ship together from one vendoring.

Same-failure search: every `send`/`sendmsg`/`recv` site and every `__APPLE__`, `__FreeBSD__`, `target_os = "macos"`/`"freebsd"`, darwin/freebsd build tag and `_darwin`/`_freebsd` filename in `src/`, `tests/fixtures/` and `bench/drivers/` reviewed; no other code assumes SEQPACKET on FreeBSD; no other `shutdown(SHUT_RD)` exists (the remaining `SHUT_RDWR` calls end a session on purpose).

SOW status/directory: completed, moved to `done/` in the same commit.

## Outcome

FreeBSD 15.1 is validated. It exposed a real defect: FreeBSD 15 `SOCK_SEQPACKET` keeps record boundaries only with `MSG_EOR` and may write or deliver part of a record, so NetIPC packets merged. FreeBSD (every release) now runs the stream baseline already used on macOS, in C, Rust and Go, with SHM kept. The stream baseline's partial-receive rule no longer depends on `shutdown(SHUT_RD)` semantics, which differ between macOS, FreeBSD 14 and FreeBSD 15. All suites pass on FreeBSD 15.1, FreeBSD 14.3, macOS and Linux.

Artifact maintenance gate:

- AGENTS.md: macOS line names the renamed spec section and that FreeBSD uses the stream baseline; the FreeBSD procedure itself is unchanged.
- Runtime project skills: no update; the vendoring workflow is unchanged (Netdata does not build NetIPC on FreeBSD).
- Specs: `docs/level1-posix-uds.md`, `docs/level1-transport.md` updated (the public specs are the authoritative spec surface); `.agents/sow/specs/` holds no transport rule, so it needs no change.
- End-user/operator docs: README platform table; the `docs/` specs above.
- End-user/operator skills: `docs/netipc-integrator-skill.md` platform notes (FreeBSD stream baseline, SHM kept, packet size, backlog note).
- SOW lifecycle: completed in `done/`; follow-ups mapped below.

## Lessons Extracted

- A socket type's semantics can change between OS releases (FreeBSD 15 `SOCK_SEQPACKET`): validate every supported release, and probe the primitive directly (small C programs) before blaming the library.
- Kernel side effects such as `shutdown(SHUT_RD)` differ by OS; state that a transport needs (a failed receive) belongs in the session, not in the socket.
- A Go `_darwin` / `_freebsd` filename suffix restricts a file to that OS regardless of its build tag; renaming is required when sharing code across OSes.

## Followup

- FreeBSD binaries from before this change cannot talk to newer ones: no work; nothing ships on FreeBSD yet (Validation, reviewer finding 5).
- Deferral search (`defer|later|follow-up|future|TODO|pending`): remaining hits are the pending queue, has-pending API names and historical status lines, not deferred work.

## Regression Log

None yet.
