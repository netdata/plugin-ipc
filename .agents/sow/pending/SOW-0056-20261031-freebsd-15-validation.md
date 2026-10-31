# SOW-0056 - FreeBSD 15 Validation

## Status

Status: open

Sub-state: waiting for a FreeBSD 15 test host from the user.

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

Inferences:

- 14.4/14.5 share the 14.3 unix-socket code; 15.1 is the real gap.

Unknowns:

- Which FreeBSD releases Netdata will support once it enables NetIPC there (a Netdata decision).

### Acceptance Criteria

- On FreeBSD 15.1: configure, build, ctest, `cargo test`, `go test ./...` and `tests/interop_codec.sh` pass (the AGENTS.md FreeBSD procedure).
- The maximum SEQPACKET datagram versus `SO_SNDBUF` is measured on 15.1 and the per-platform overhead in C, Rust and Go matches it.

## Analysis

Pending: needs the test host.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Unvalidated platform release; no known defect.

Evidence reviewed:

- SOW-0048 Validation (FreeBSD 14.3 results and the SEQPACKET measurement).

Affected contracts and surfaces:

- POSIX UDS packet-size derivation (`SNDBUF_OVERHEAD` in Rust `transport/posix.rs`, Go `uds_sndbuf_*.go`, C `netipc_uds_*`), only if 15.1 differs.

Existing patterns to reuse:

- SOW-0048 per-platform overhead constants and the AGENTS.md FreeBSD procedure.

Risk and blast radius:

- Limited to FreeBSD builds; Linux and Windows are unaffected by FreeBSD-only constants.

Performance assessment:

- Validation only; a constant change would affect packet size proposal at connection setup only. No benchmark run.

Sensitive data handling plan:

- Name the host only as "the FreeBSD 15 test VM"; record command results, not host details. Covers this SOW, specs, docs, skills and code comments.

Implementation plan:

1. Run the AGENTS.md FreeBSD procedure on 15.1.
2. Measure the SEQPACKET maximum; fix any mismatch in all three languages with tests.

Validation plan:

- The acceptance criteria commands on 15.1, then the Linux and 14.x suites again if code changed.

Artifact impact plan:

- `docs/level1-posix-uds.md` packet-size notes if 15.1 differs; otherwise none, recorded at close.

Open decisions:

- The user provides a FreeBSD 15.1 host (or decides 14.x is enough).

## Implications And Decisions

None yet.

## Plan

See the gate.

## Execution Log

### 2026-10-31

Created from SOW-0048 follow-up mapping.

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
