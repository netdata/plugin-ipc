# SOW-0059 - FreeBSD SHM Transport

## Status

Status: open

Sub-state: scheduled after SOW-0049, before benchmarks and vendoring (user ordering 2026-10-31).

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

Pending: research `_umtx_op` cross-process semantics and `posix_fallocate` before the gate.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Feature gap: the POSIX SHM backend is Linux-only by construction.

Evidence reviewed:

- SOW-0048 code and measurements.

Affected contracts and surfaces:

- POSIX SHM transport (C, Rust, Go), the platform profile mask, CMake `NETIPC_POSIX_SHM` gating, `docs/level1-posix-shm.md`, `docs/level1-transport.md`.

Existing patterns to reuse:

- The Linux SHM implementation and tests; SOW-0048 per-OS file split (Go `shm_linux.go` / `shm_other.go`, C `#if defined(__linux__)`).

Risk and blast radius:

- New per-request synchronization code on FreeBSD; Linux must stay untouched in behavior.

Performance assessment:

- Adds an established-session fast path on FreeBSD; needs FreeBSD benchmark rows (Test Cadence: performance-purpose SOW).

Sensitive data handling plan:

- Source paths, man-page citations and command output only; hosts named by role.

Implementation plan:

1. Research and record `_umtx_op` and `posix_fallocate` behavior with probes on the FreeBSD test VM.
2. Present any layout/spec decision to the user.
3. Implement C, Rust, Go; lift the FreeBSD profile mask; tests and interop.

Validation plan:

- FreeBSD procedure (AGENTS.md) plus SHM interop; Linux suites and targets unchanged; FreeBSD benchmark rows.

Artifact impact plan:

- `docs/level1-posix-shm.md`, `docs/level1-transport.md`, `docs/netipc-integrator-skill.md` platform notes, AGENTS.md FreeBSD command (SHM suites then build there).

Open decisions:

- Possible layout/spec change, after research.

## Implications And Decisions

- Priority (2026-10-31, user): low; after SOW-0050, SOW-0049 and SOW-0057.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

See the gate.

## Execution Log

### 2026-10-31

Created at the user's request.

## Validation

Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
