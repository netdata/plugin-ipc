# SOW-0042 - UDS Default Packet Size

## Status

Status: open

Sub-state: created from SOW-0040 reproduction results; awaiting prioritization and design decisions.

## Requirements

### Purpose

Messages larger than one packet must be deliverable over UDS with the default (auto-detected) packet size on Linux.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- C (`src/libnetdata/netipc/src/transport/posix/netipc_uds.c:24-34`), Rust (`src/crates/netipc/src/transport/posix.rs:792-811`) and Go (`src/go/pkg/netipc/transport/posix/uds.go:105-112`) set the packet size to `SO_SNDBUF`.
- On Linux a SOCK_SEQPACKET datagram of exactly `SO_SNDBUF` bytes fails with EMSGSIZE (socketpair check: 212992 fails, 211968 succeeds), so every chunked message fails with the default configuration.
- Reproduced (SOW-0040) in C, Rust and Go with a 512 KiB message; the same message with an explicit 64 KiB packet size passes.
- Spec: `docs/level1-posix-uds.md:73-86` defines `agreed_packet_size` chunking but not how the local packet size is derived.

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- `test_robust_default_packet_size_carries_large_message` (Rust), `TestRobustDefaultPacketSizeCarriesLargeMessage` (Go) and the C `test_robustness` N11 check pass with the gate removed.
- Packet-size derivation documented in `docs/level1-posix-uds.md`; interop across languages unchanged or migrated consistently.
- Fix design (for example a safety margin below `SO_SNDBUF`, a fixed default, or raising `SO_SNDBUF`) decided by the user with benchmark evidence where throughput is affected.

## Analysis

Sources checked:

- See SOW-0040 (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`) for evidence, test files and commands.

Current state:

- Reproduction tests exist and are gated behind `NETIPC_REPRO_KNOWN_BUGS=1`.

Risks:

- Cross-language behavior change; interop and Netdata vendoring must be validated.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- See Facts.

Evidence reviewed:

- SOW-0040 evidence and tests.

Affected contracts and surfaces:

- To be completed before implementation.

Existing patterns to reuse:

- To be completed before implementation.

Risk and blast radius:

- To be completed before implementation.

Sensitive data handling plan:

- No secrets involved; Windows SIDs in evidence are replaced with `<owner-SID>`.

Implementation plan:

1. To be completed before implementation.

Validation plan:

- Remove the known-bug gate from the covered SOW-0040 tests; they must pass on Linux and `costa-win11`.

Artifact impact plan:

- AGENTS.md: to be assessed.
- Runtime project skills: to be assessed.
- Specs: to be assessed.
- End-user/operator docs: to be assessed.
- End-user/operator skills: to be assessed (`docs/netipc-integrator-skill.md`).
- SOW lifecycle: created from SOW-0040.

Open-source reference evidence:

- None.

Open decisions:

- Gathered when this SOW starts.

## Implications And Decisions

None yet.

## Plan

To be written when this SOW moves to `current/`.

## Execution Log

None yet.

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
