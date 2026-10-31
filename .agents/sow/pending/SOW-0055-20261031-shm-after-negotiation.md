# SOW-0055 - Create SHM Regions Only For Sessions That Negotiate SHM

## Status

Status: open

Sub-state: split from SOW-0052 on 2026-10-31; starts after SOW-0052 completes.

## Requirements

### Purpose

Stop managed servers from creating and destroying a per-session SHM region for every accepted connection, including rejected, silent and baseline-only clients.

### User Request

Zero-known-bug policy (AGENTS.md); the SOW-0040 decisions to fix every review-found defect. Split from SOW-0052 (assistant process decision, recorded there) because the fix needs a new step inside the L1 server handshake in C, Rust and Go on POSIX and Windows, and lands best as its own reviewable commit.

### Assistant Understanding

Facts (from SOW-0052, verified 2026-10-31 against `6609157`):

- C POSIX creates the region before HELLO is read (`netipc_service_posix_server_session.c` `server_prepare_accept_config`, called from `server_session_handshake` before `nipc_uds_accept_handshake`); it already waits for HELLO to be readable first, so silent clients no longer cost a region, but rejected and baseline-only clients still do.
- C Windows, Rust (`server_unix.rs`, `server_windows.rs`) and Go (`server_unix.go`, Windows server) follow the same create-before-negotiation order; exact lines to re-verify at activation.
- The SHM profile guarantee (`docs/level1-posix-uds.md`, `docs/level1-windows-np.md`): a successful HELLO_ACK that selects SHM means the region already exists, so creation must happen after negotiation but before HELLO_ACK is sent.

Inferences:

- Creating after negotiation also lets the region use the agreed payload sizes instead of the server maximums.

Unknowns:

- None that investigation cannot resolve.

### Acceptance Criteria

- Servers in C, Rust and Go, POSIX and Windows, create a region only when negotiation selects an SHM profile, before HELLO_ACK; a failed creation falls back to baseline in the same handshake when the client supports it.
- Tests prove a rejected or baseline-only client creates no region, and the SHM interop suites pass.

## Analysis

Sources checked:

- SOW-0052 facts.

Current state:

- No test covers region creation for non-SHM sessions.

Risks:

- Changes the L1 server handshake API shape in three languages; interop must stay identical on the wire.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Region creation is decided before the client's profiles are known.

Evidence reviewed:

- To be completed at activation.

Affected contracts and surfaces:

- L1 server handshake (C, Rust, Go; POSIX and Windows), managed servers, SHM docs, tests.

Existing patterns to reuse:

- The split accept (`accept_pending` / `accept_handshake`) added by SOW-0043.

Risk and blast radius:

- Every SHM session setup; connection setup only, not established-session request handling.

Sensitive data handling plan:

- No secrets involved. Applies to this SOW, docs and code comments.

Implementation plan:

1. Completed at activation.

Validation plan:

- New tests, full ctest including SHM interop on Linux and Windows, the Netdata Linux targets.

Artifact impact plan:

- AGENTS.md: assess at activation.
- Runtime project skills: assess at activation.
- Specs: `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, SHM docs as affected.
- End-user/operator docs: same.
- End-user/operator skills: `docs/netipc-integrator-skill.md` if the server contract text changes.
- SOW lifecycle: split from SOW-0052.

Open-source reference evidence:

- None.

Open decisions:

- Made at activation (assistant-applied under the user's autonomy instruction, recorded with reasoning).

## Implications And Decisions

None yet.

## Plan

Written at activation.

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
