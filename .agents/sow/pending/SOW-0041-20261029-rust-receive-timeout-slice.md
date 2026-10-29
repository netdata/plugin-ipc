# SOW-0041 - Rust Receive Timeout Slice

## Status

Status: open

Sub-state: created from SOW-0040 reproduction results; awaiting prioritization and design decisions.

## Requirements

### Purpose

Rust UDS and named-pipe receive must honor the configured call timeout instead of failing after one 100 ms abort-poll slice.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- Rust caps each wait at 100 ms when an abort flag exists and returns `Timeout` on the first empty slice: `src/crates/netipc/src/transport/posix.rs:932-934`, `:956-961`; `src/crates/netipc/src/transport/windows.rs:541-542`, `:631-637`.
- The client always passes an abort flag (`src/crates/netipc/src/service/raw/client_call.rs:329`, `:349`), so every Rust UDS/pipe call with a server slower than about 100 ms fails with a terminal `Timeout`, including the default 30000 ms.
- C (`netipc_uds_receive.c:48-86`, `netipc_named_pipe.c:379-442`) and Go (`uds_receive.go:103-104`, `pipe_receive.go:118-120`) re-wait until the real deadline.
- Reproduced (SOW-0040): Rust Linux failed after 101 ms, Rust Windows after 97 ms, with a 300 ms handler and a 2000 ms timeout.

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- `test_robust_uds_call_honors_timeout_longer_than_100ms` (Rust Linux) and `test_robust_win_pipe_call_honors_timeout_longer_than_100ms` (Rust Windows) pass with the known-bug gate removed.
- Existing timeout/abort tests still pass; abort still interrupts promptly.
- Cross-language slow-handler coverage considered (sibling review suggested a 300 ms handler across all client/server pairs).

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
