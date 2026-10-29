# SOW-0045 - Windows Pipe Security

## Status

Status: open

Sub-state: created from SOW-0040 reproduction results; awaiting prioritization and design decisions.

## Requirements

### Purpose

Decide and enforce the Windows named-pipe access policy so that unauthorized local (or remote) principals cannot open NetIPC pipes.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- Pipes are created with default security (`NULL` attributes) in C (`netipc_named_pipe.c:217`), mirrored in Rust and Go.
- Observed DACL on `costa-win11` (user-session server, all three languages): `D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;<owner-SID>)(A;;FR;;;WD)(A;;FR;;;AN)` - Everyone and Anonymous have read access.
- A read-only open is enough to occupy a pipe instance and block the accept loop (observed 7.4 s client delay against the C server), so any local principal can likely cause a denial of service. Not verified with a second user.
- `PIPE_REJECT_REMOTE_CLIENTS` is not set; remote reachability over SMB is untested speculation.
- The default DACL derives from the creating token, so a LocalSystem service may differ from the user-session result.

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- Explicit pipe security descriptor (and remote-client rejection) decided by the user and implemented in C, Rust and Go.
- Verified with a second, non-privileged Windows user and with a service-context server.
- Spec `docs/level1-windows-np.md` documents the access policy.

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
