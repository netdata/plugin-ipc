# SOW-0059 - FreeBSD SHM Transport

## Status

Status: completed

Sub-state: delivered (2026-11-01).

## Requirements

### Purpose

Give FreeBSD the shared-memory fast path that Linux and Windows have, so FreeBSD sessions are not limited to UDS throughput.

### User Request

2026-10-31: "I want SHM on freebsd, not high priority."

### Assistant Understanding

Facts:

- FreeBSD sessions use the UDS SEQPACKET baseline only. Since SOW-0048 the POSIX handshake masks profiles to baseline off Linux (`NIPC_UDS_PLATFORM_PROFILES` in `netipc_uds_internal.h`, Rust `PLATFORM_PROFILES` in `transport/posix.rs`, Go `platformProfiles` in `transport/posix/shm_other.go`).
- The POSIX SHM transport waits and wakes with Linux futexes (`netipc_shm.c`, `<linux/futex.h>`); its region layout and synchronization protocol are specified in `docs/level1-posix-shm.md`.
- Region files are created with Linux `fallocate` (`docs/level1-posix-shm.md`, region creation).
- FreeBSD's default SEQPACKET buffer is 8192 bytes (SOW-0048), so large messages chunk often there; SHM avoids that.

Inferences:

- FreeBSD's `_umtx_op(2)` with `UMTX_OP_WAIT_UINT` / `UMTX_OP_WAKE` (or the `_PRIVATE` variants for process-private use; shared regions need the non-private ones) is the likely futex equivalent. Unverified for cross-process use on a shared file mapping.
- `posix_fallocate(2)` is the likely portable replacement for Linux `fallocate`. Unverified on the filesystems Netdata uses on FreeBSD.

Unknowns:

- Whether the existing region layout and wait/wake protocol map onto `_umtx_op` without a wire or layout change (if the layout must change, both peers must agree, which affects the SHM spec).
- Go: pure Go must call `_umtx_op` through `syscall.Syscall6`; whether Go's `syscall` package exposes the needed constant numbers on FreeBSD.

### Acceptance Criteria

- C, Rust and Go negotiate and use SHM on FreeBSD, interoperate in every language pair, and fall back to baseline when SHM preparation fails, exactly as on Linux.
- Linux behavior and the SHM spec stay compatible (or the spec change is decided by the user first).
- Benchmarks on FreeBSD show the SHM rows.

## Analysis

Probes on the FreeBSD 14.3 test VM (amd64, UFS root, 2026-11-01; C programs, no root):

- `_umtx_op(UMTX_OP_WAIT_UINT)` + `UMTX_OP_WAKE` on a `MAP_SHARED` file mapping: a waiter in one process is woken by a store + wake from another process that mapped the same file separately (woke after 106 ms, value 1). The `_PRIVATE` variants do not cross processes (the waiter timed out at 5000 ms). So the shared variants are the futex equivalent.
- Timeout: `struct _umtx_time {timespec _timeout; u32 _flags; u32 _clockid}` passed as `uaddr2` with its size in `uaddr1`; relative CLOCK_MONOTONIC timeout returns `ETIMEDOUT` after 50.1 ms for 50 ms.
- Value mismatch: `WAIT_UINT` with a stale expected value returns 0 at once (Linux futex returns `EAGAIN`). Callers already re-check the sequence after every wake, so the difference is harmless but must be handled as "re-check", not as an error.
- Constants (`/usr/include/sys/umtx.h`): `UMTX_OP_WAKE` 3, `UMTX_OP_WAIT_UINT` 11, `_PRIVATE` variants 15 and 16. Go's `syscall` package defines `SYS__UMTX_OP` (454) for every FreeBSD architecture, so pure Go can call it through `syscall.Syscall6`; the Go runtime itself uses `_umtx_op`.
- `posix_fallocate` on a UFS file: ok.
- Named POSIX shm objects (`shm_open`, mode 0600 honored): `posix_fallocate` returns ok even for 4 TiB on a host with 16 GiB RAM and 1 GiB swap, so on FreeBSD it reserves nothing for shm objects. A shm object therefore does not meet the spec's reservation guarantee (`docs/level1-posix-shm.md`, region creation step 3).

Where Netdata puts the run directory (Netdata `src/libnetdata/os/run_dir.c` `detect_run_dir`): `$NETDATA_RUN_DIR`, else `/var/run/netdata` on FreeBSD, else `/tmp/netdata`. On a FreeBSD install with a ZFS root, `/var/run` is on ZFS.

Root probes (2026-11-01, approved; temporary file-backed ZFS pool and a 64 MiB tmpfs under `/tmp/costa-*`, all removed and the zfs module unloaded afterwards; ZFS 2.2.7 on FreeBSD 14.3):

| Filesystem | 8 MiB `posix_fallocate` | 64 GiB (over capacity) | Free space after the 8 MiB call |
|---|---|---|---|
| ZFS | `EINVAL` | `EINVAL` | unchanged |
| tmpfs | ok | `ENOSPC` | dropped 8 MiB |
| UFS | ok | ok (the disk had the room) | dropped 8 MiB (64 GiB call: dropped 64 GiB) |

- ZFS rejects `posix_fallocate`, so with the current spec (step 3: no sparse fallback) a FreeBSD server whose run directory is on ZFS never offers SHM and serves baseline. That includes Netdata's default `/var/run/netdata` on a ZFS-root install.
- tmpfs and UFS really reserve the space and report exhaustion as an error, which is the spec's guarantee.
- ZFS is copy-on-write, so no allocation call can promise that later writes to a mapped region find space; a sparse or zero-filled region on ZFS can still fail a write when the pool fills, which shows up as `SIGBUS` in the mapped access the spec rules out.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Feature gap: the POSIX SHM data plane is Linux-only by construction. Only two primitives are OS-specific: the futex wait/wake and the reserving allocation (see Analysis); the region layout, sequence protocol and handshake are portable.

Evidence reviewed:

- Probes and code paths in Analysis; SOW-0048 platform split; `docs/level1-posix-shm.md`; Netdata `src/libnetdata/os/run_dir.c`; Netdata `.github/data/distros.yml:504-508` (FreeBSD: community support, no architecture list); Rust `libc` 0.2.186 (`_umtx_op`, `_umtx_time`, `UMTX_OP_*`, `posix_fallocate`, `CLOCK_MONOTONIC` = 4 on FreeBSD); Go `syscall` (`SYS__UMTX_OP` 454, `SYS_POSIX_FALLOCATE` 530 on every FreeBSD architecture, no wrappers).

Affected contracts and surfaces:

- C `src/libnetdata/netipc/src/transport/posix/netipc_shm.c` (platform guard, wait/wake, allocation), `netipc_uds_internal.h` (`NIPC_UDS_PLATFORM_PROFILES`), `CMakeLists.txt` (`NETIPC_POSIX_SHM` and SHM test targets), `tests/fixtures/c/shm_fallocate_wrap.c` (FreeBSD wraps `posix_fallocate`), C tests with SHM gates.
- Rust `transport/shm.rs`, `transport/mod.rs`, `transport/posix.rs` (`PLATFORM_PROFILES`) and the `target_os = "linux"` SHM gates in `service/raw/*` and the tests.
- Go `transport/posix/shm_linux.go` (made portable to `linux || (freebsd && (amd64 || 386))`, with the wait/wake, allocation and clock id moved to per-OS files), `shm_other.go` (the complement), SHM tests.
- Wire format and region layout: unchanged.
- Specs: `docs/level1-posix-shm.md` (FreeBSD wait/wake and allocation, ZFS note), `docs/level1-transport.md` (platform table), `docs/netipc-integrator-skill.md`, README platform table, AGENTS.md FreeBSD command line (the SHM suites now build there).

Existing patterns to reuse:

- C platform `#if` blocks (`netipc_uds_lifecycle.c`), Rust `cfg(any(target_os = "linux", target_os = "freebsd"))` as at `posix.rs:742` (decision 8A), Go build-tagged files (`uds_cloexec_*.go`).
- Existing SHM tests and fault injection (`shm_fallocate_wrap.c`, Rust and Go allocation hooks) run unchanged on FreeBSD.

Risk and blast radius:

- Linux: the wait/wake and allocation calls move behind per-OS helpers; Linux keeps the same syscalls and arguments. Covered by the Linux suites and all 9 Netdata Linux targets.
- FreeBSD: new SHM path. `UMTX_OP_WAIT_UINT` returns 0 on a value mismatch (Linux: `EAGAIN`); the wait loops already re-check the sequence after every return, so 0 is handled as "re-check". `posix_fallocate` returns the error number instead of setting `errno`; the C helper converts it.
- Go raw syscalls are hand-written per architecture; limited to amd64 and 386, both testable on the FreeBSD VM (decision 9A).
- Region files on ZFS: allocation fails with `EINVAL`, SHM preparation fails, baseline is used (decision 7A); covered by the existing allocation-failure tests.

Sensitive data handling plan:

- Source paths, man-page citations and command output only, in the SOW, specs, docs, skills, AGENTS.md and code comments; hosts named by role.

Implementation plan:

1. C: per-OS `shm_wait`/`shm_wake`/`shm_allocate` helpers; FreeBSD guard; CMake builds SHM and its tests on FreeBSD; the test wrapper wraps `posix_fallocate` there; C test gates per the classification.
2. Lift the FreeBSD profile mask in C, Rust and Go (Go only on amd64 and 386).
3. Rust: the same helpers in `shm.rs` (`libc::_umtx_op`, `libc::posix_fallocate`), cfg gates extended in place (8A).
4. Go: portable `shm_posix.go` plus `shm_futex_linux.go` and `shm_umtx_freebsd.go` (wait, wake, allocation, clock id) and per-architecture `posix_fallocate` argument layout for amd64 and 386.
5. Tests: the existing SHM suites run on FreeBSD; add a FreeBSD check that a ZFS-style `EINVAL` from the allocation falls back to baseline through the existing fault hooks.
6. Docs and specs; FreeBSD benchmark run (decision 10A).

Validation plan:

- FreeBSD 14.3 (amd64): CTest including the SHM suites, `cargo test`, `go test ./...`, Go with `GOARCH=386`, all POSIX interop scripts including the SHM ones.
- Linux: CTest, `cargo test` (stable and 1.91), `go test`, all interop scripts, clippy and fmt; all 9 Netdata Linux targets.
- macOS: build and suites (SHM stays off; stubs must still build).
- Windows: Rust clippy for the Windows target (shared crate).

Artifact impact plan:

- See Affected contracts and surfaces (specs, docs, integrator skill, README, AGENTS.md).

Performance assessment:

- Linux established-session path: the wait/wake and allocation move behind static helpers with the same syscalls (C inlines them; Rust `#[inline]`; Go per-OS functions), so no Linux change is expected. The full pre-vendoring benchmark run measures it.
- FreeBSD: new path; one benchmark run on the VM, SHM against baseline, recorded here without floors (decision 10A).

Open decisions:

- None (decisions 7A, 8A, 9A, 10A recorded).

## Implications And Decisions

- Priority (2026-10-31, user): low; after SOW-0050, SOW-0049 and SOW-0057.
- Backing store (2026-11-01, user: "7A"): keep the spec's reservation rule unchanged. FreeBSD regions use `posix_fallocate` in the run directory; where it fails (ZFS returns `EINVAL`), SHM preparation fails as specified and the session uses baseline. Document that a FreeBSD operator can point the run directory at tmpfs or UFS to get SHM. Rejected: a sparse or zero-filled region on ZFS (option B, `SIGBUS` on a full pool) and `shm_open` memory objects (option C, no reservation on FreeBSD either, OOM kill instead of an error).
- Rust platform gates (2026-11-01, user: agreed to the recommendations, decision 8A): extend each `target_os = "linux"` SHM gate in place to `any(target_os = "linux", target_os = "freebsd")`; no build script.
- FreeBSD architectures (decision 9A): C and Rust SHM on every FreeBSD architecture (they go through libc); Go SHM only on FreeBSD amd64 and 386, whose raw-syscall argument layouts can be tested on the amd64 VM (386 through FreeBSD's 32-bit compatibility); other Go FreeBSD builds negotiate baseline.
- FreeBSD benchmarks (decision 10A): one benchmark run on the FreeBSD VM comparing SHM with baseline, recorded in this SOW as evidence, no floors.
- Root probes on the FreeBSD test VM (2026-11-01, user: "a"): temporary ZFS file-backed pool and tmpfs under `/tmp/costa-*`, removed after the probe.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

See the gate.

## Execution Log

### 2026-10-31

Created at the user's request.

### 2026-11-01

- C: FreeBSD `futex_wake`/`futex_wait` over `_umtx_op(UMTX_OP_WAKE / UMTX_OP_WAIT_UINT)` with a relative `struct _umtx_time` on `CLOCK_MONOTONIC`; `shm_allocate()` (Linux `fallocate`, FreeBSD `posix_fallocate` with its return value moved to `errno`); `NIPC_UDS_PLATFORM_PROFILES` covers FreeBSD. CMake builds SHM and its tests on FreeBSD (`NETIPC_POSIX_SHM`), wraps `posix_fallocate` there for fault injection, and gates the Go SHM tests on `NETIPC_GO_POSIX_SHM` (Linux, FreeBSD amd64/i386). The C test SHM gates cover FreeBSD; `test_shm_full_backing_store` stays Linux-only (it needs a Linux tmpfs mount).
- Rust: the same split in `transport/shm.rs` (`libc::_umtx_op`, `libc::posix_fallocate`); `PLATFORM_PROFILES` covers FreeBSD; the SHM `target_os = "linux"` gates in `service/raw/*`, the tests, the interop fixture and the bench driver extended in place (8A). The POLLRDHUP gates stay Linux-only (FreeBSD has no POLLRDHUP).
- Go: `shm_linux.go` became `shm_posix.go` (`linux || (freebsd && (amd64 || 386))`); per-OS `shm_futex_linux.go` and `shm_umtx_freebsd.go` (wait, wake, clock id, allocation); `posix_fallocate` raw syscall per architecture (`shm_fallocate_freebsd_amd64.go`, `shm_fallocate_freebsd_386.go`, 64-bit offsets split into two words on 386; the error comes back in the return value); `shm_other.go` and the pause files retagged as complements; SHM tests renamed `*_posix_test.go` and retagged.
- FreeBSD found two `EISDIR`-only directory checks. FreeBSD and macOS fail `unlink` on a directory with `EPERM`:
  - SHM stale region at a directory (C `unlink_stale_name`, Rust `unlink_stale_path`): found by `test_shm` "create reclaims endpoint directory" on FreeBSD; both now accept `EISDIR` or `EPERM`.
  - UDS stale endpoint at an empty directory (C `netipc_uds_lifecycle.c` `unlink_stale_socket_path`, Rust `transport/posix.rs` stale check): found by the same-failure search. New tests in C (`test_stale_recovery_reclaims_empty_directory`), Rust (`test_empty_directory_at_socket_path_is_reclaimed`) and Go (`TestStaleRecoveryReclaimsEmptyDirectory`) failed for C and Rust on macOS before the fix (address-in-use) and pass after; Go (`os.Remove`) already reclaimed it. Spec note in `docs/level1-posix-uds.md`, "Socket file".
- New tests: ZFS-style `EINVAL` allocation in C (`test_shm.c`), Rust (`test_server_create_does_not_fallback_when_fallocate_is_unsupported`, now over `EOPNOTSUPP` and `EINVAL`) and Go (`TestShmServerCreatePreallocation`, same two causes); Go FreeBSD raw-syscall checks `TestPosixFallocateRawSyscall` (offset above 4 GiB so both 386 words matter; a pipe returns `ESPIPE`) and `TestUmtxWaitStaleValueAndTimeout`.

## Validation

Acceptance criteria evidence:

- C, Rust and Go negotiate and use SHM on FreeBSD and interoperate in every pair: FreeBSD 14.3 `test_shm_interop` 9/9, `test_service_shm_interop` 18/18, `test_cache_shm_interop` 9/9, `test_lookup_scale_interop` 36/36 (0 skipped).
- Fallback to baseline on SHM preparation failure, exactly as on Linux: the existing allocation-failure tests run on FreeBSD (C wraps `posix_fallocate`; Rust and Go hooks), plus the new `EINVAL` cases.
- Linux behavior and the SHM spec stay compatible: no layout or wire change; Linux keeps the same syscalls behind the helpers; all Linux suites and the 9 Netdata Linux targets pass.
- Benchmarks on FreeBSD show the SHM rows: see Performance below.

Tests (final code, 2026-11-01):

- FreeBSD 14.3 amd64 (UFS): build, CTest 31/31, `cargo test` 394, `go test ./...`, `GOARCH=386 go test ./...`, all 8 interop scripts with 0 skipped.
- Linux: build, CTest, `cargo test` stable and 1.91, `cargo fmt --check`, `go vet` and `go test`, all 8 interop scripts; CI clippy gate for Linux, `x86_64-pc-windows-gnu` and `x86_64-unknown-freebsd`; `go vet` for linux/amd64, linux/386, freebsd/amd64, freebsd/386, freebsd/arm64, darwin/arm64, windows/amd64.
- Netdata Linux targets: all 9 (x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl) pass, peak PSI full avg10 at most 4.59%.
- macOS: CTest 24/24, `cargo test` 326, `go test`, and the macOS interop list (SHM stays off; the stubs build).
- Windows: `cargo test` 273, `go test`, CTest 20/20.

Real-use evidence: the SHM interop scripts and the benchmark run real C, Rust and Go servers and clients over SHM on FreeBSD.

Reviewer findings: no external review was requested for this SOW.

Same-failure search:

- `EISDIR` across `src/` and `tests/`: four sites. Two were SHM (fixed), two were UDS endpoint recovery in C and Rust (fixed, see Execution Log). Go uses `os.Remove`, which handles both errors.
- Other Linux-only SHM gates: `grep target_os = "linux"` in the crate leaves only the POLLRDHUP gates and `test_poll_fd_eintr`, which are not SHM.

Performance assessment:

- Established-session hot path: SHM wait/wake (C `netipc_shm.c` `futex_wait`/`futex_wake`, Rust `transport/shm.rs`, Go `shm_posix.go:424` and `:537` calling `futexWakeCall`/`futexWaitCall`). On Linux these keep the same syscalls and arguments behind per-OS helpers; allocation runs only at session setup. The full pre-vendoring benchmark run (with SOW-0050) measures Linux; no Linux run in this SOW.
- FreeBSD (decision 10A, no floors): one `tests/run-posix-bench.sh` run on the FreeBSD 14.3 VM (8 vCPU, shared; `NIPC_BENCH_FLOOR_RETRY_SAMPLES=0`), 336 rows. Max-rate throughput, baseline -> SHM, over the 9 language pairs:
  - ping-pong: 89k-136k -> 526k-1.81M req/s (5.4x-15.2x; Go clients 5.4x-6.8x, C and Rust clients 10.7x-15.2x); p50 5-8 us -> under 1 us.
  - batch ping-pong: 12.9M-18.3M -> 15.6M-23.1M items/s (1.19x-1.51x); p50 22-30 us -> 15-23 us.
  - snapshot refresh: 66k-106k -> 260k-727k/s (3.6x-7.4x).
  - At fixed 1000/s rates SHM lowers p50 (ping-pong 44-69 us -> 40-55 us; batch 98-124 us -> 56-75 us) at equal or lower CPU.
  - `uds-pipeline-batch` (baseline only, no SHM) failed for all 9 pairs: a write-write deadlock in the benchmark client on FreeBSD's 8 KiB SEQPACKET buffers (see Followup). Every other row completed.

Artifact maintenance gate:

- `AGENTS.md`: FreeBSD command line updated (SHM runs there; Go 386 run; tests need UFS or tmpfs).
- Runtime project skills: `project-netdata-vendoring` unaffected (no vendoring workflow change; Netdata does not build NetIPC on FreeBSD yet).
- Specs: `docs/level1-posix-shm.md` (FreeBSD syscalls table, umtx wait/wake and stale-value return, `posix_fallocate`, ZFS `EINVAL` and run-directory guidance), `docs/level1-transport.md` (SHM platform table and fallback text), `docs/level1-posix-uds.md` (empty directory at the endpoint). No `.agents/sow/specs/` file states FreeBSD behavior.
- End-user/operator docs: README platform table; `docs/netipc-integrator-skill.md` platform notes.
- End-user/operator skills: `docs/netipc-integrator-skill.md` updated as above.
- SOW lifecycle: SOW-0056 acceptance criteria now include SHM on FreeBSD 15.1 (filesystem type recorded); this SOW moves to `done/` as completed in its commit.

Sensitive data gate: hosts named by role; only source paths, command names and result counts recorded in the SOW, specs, docs, AGENTS.md and code comments.

## Outcome

FreeBSD has the POSIX SHM fast path in C and Rust on every architecture and in Go on amd64 and 386, interoperating across all language pairs. Region files need a run directory on tmpfs or UFS; on ZFS SHM preparation fails and sessions use baseline, as the spec requires. Directory leftovers at SHM and UDS endpoint paths are now reclaimed on FreeBSD and macOS as on Linux.

## Lessons Extracted

- `unlink` on a directory fails with `EISDIR` on Linux but `EPERM` on FreeBSD and macOS (POSIX allows both). Any "remove a directory if unlink says it is one" path must accept both.
- FreeBSD `posix_fallocate` returns its error number and does not set `errno`, and ZFS rejects it with `EINVAL`; reservation-based designs must treat ZFS as "no reservation".
- Recorded in the specs and code comments above; no project skill change needed.

## Followup

- FreeBSD 15.1 SHM validation: tracked in SOW-0056 (acceptance criteria updated).
- FreeBSD `uds-pipeline-batch` benchmark deadlock (found by this SOW's benchmark run, baseline transport, predates this SOW): tracked in SOW-0062, which needs a user decision on the fix.
- Linux benchmark coverage of the moved wait/wake helpers: the full pre-vendoring benchmark run with SOW-0050.
- Go SHM on FreeBSD architectures other than amd64 and 386: rejected for now by decision 9A (raw-syscall layouts cannot be tested here); those builds negotiate baseline.

## Regression Log

None yet.
