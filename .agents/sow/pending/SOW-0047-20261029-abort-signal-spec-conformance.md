# SOW-0047 - Abort Signal Spec Conformance

## Status

Status: open

Sub-state: created 2026-10-29 from the SOW-0041 analysis round; awaiting its turn in the one-issue-at-a-time decision sequence.

## Requirements

### Purpose

The documented client abort mechanism and the Rust/Go implementations must agree.

### User Request

2026-10-29: "there are no known bugs. All bugs must be fixed now. Zero known bug policy." and "I want one issue at a time, with proper explanation, risks, proposed solutions, risk of the solutions recommended and explicit recommendations with reasoning."

### Assistant Understanding

Facts:

- The specs say a client abort is a file descriptor (POSIX) or event (Windows) waited on together with the socket or pipe (`docs/level1-posix-uds.md:107-110`, `docs/level1-windows-np.md:104-106`).
- C implements that (`netipc_uds_receive.c`, abort pipe fd polled with the socket; `netipc_named_pipe.c:396-397`, abort event checked about every 1 ms).
- Rust (`Option<&AtomicBool>`, `transport/posix.rs`, `transport/windows.rs`) and Go (abort channel, `uds_receive.go`, `pipe_receive.go`) instead poll a flag every 100 ms, so an abort can take up to 100 ms to be noticed.
- SOW-0041 kept the 100 ms flag polling (user decision 1A) and did not change this.

Inferences:

- Downstream Netdata consumers may be affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions, presented one issue at a time.

### Acceptance Criteria

- Spec and implementation agree in all three languages (either the spec documents bounded flag polling, or Rust and Go implement real abort descriptors/events), decided by the user.

## Analysis

Sources checked:

- Read-only analysis round recorded in SOW-0041 (`.agents/sow/done/SOW-0041-20261029-rust-receive-timeout-slice.md`).

Current state:

- Bug confirmed; no fix.

Risks:

- Changing Rust/Go to real abort descriptors changes public client APIs; amending the spec changes a documented guarantee.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- See Facts.

Evidence reviewed:

- See Facts.

Affected contracts and surfaces:

- To be completed when this SOW starts, after the user decision.

Existing patterns to reuse:

- To be completed when this SOW starts.

Risk and blast radius:

- To be completed when this SOW starts.

Sensitive data handling plan:

- No secrets or customer data involved; evidence is code paths and command output only.

Implementation plan:

1. To be completed after the user decision.

Validation plan:

- Spec text and the existing abort tests in C, Rust and Go agree with the decided behavior.

Artifact impact plan:

- AGENTS.md: to be assessed.
- Runtime project skills: to be assessed.
- Specs: to be assessed.
- End-user/operator docs: to be assessed.
- End-user/operator skills: to be assessed (`docs/netipc-integrator-skill.md`).
- SOW lifecycle: created from the SOW-0041 analysis round.

Open-source reference evidence:

- None.

Open decisions:

- Presented to the user one issue at a time.

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
