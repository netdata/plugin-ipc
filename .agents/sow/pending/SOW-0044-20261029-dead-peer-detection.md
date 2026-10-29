# SOW-0044 - Dead Peer Detection

## Status

Status: open

Sub-state: created from SOW-0040 reproduction results; awaiting prioritization and design decisions.

## Requirements

### Purpose

Servers must release sessions whose client left (cleanly or by crashing), and clients must notice a crashed server, on both SHM transports.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- K1: Rust and Go POSIX SHM server sessions ignore client departure on SHM receive timeout (`src/crates/netipc/src/service/raw/server_session_unix.rs:54`, `src/go/pkg/netipc/service/raw/server_unix.go:208-209`); C checks the session socket (`netipc_service_posix_server_session.c:121-125`). Worker slots and `.ipcshm` files leak. Reproduced in Rust and Go; C guard passes (SOW-0040).
- K2: Windows SHM server sessions detect only the clean-close flag (`netipc_win_shm.c:671-680`, `:893-907`; C `netipc_service_win_server_session.c:47-48`, Rust `server_session_windows.rs:49`, Go `server_windows.go:250-251`). A crashed client holds a worker until server stop. Reproduced in all three languages. The spec (`docs/level1-windows-shm.md:189-211`) has the same gap.
- N5: SHM clients never notice a crashed server and wait the full call timeout (POSIX: `nipc_shm_owner_alive` exists at `netipc_shm.c:874` but is unused in the call loop; Windows: only `resp_server_closed`, `netipc_win_shm.c:893-906`). Reproduced in all languages on Linux and Windows (10 s with a 10 s timeout).
- Amplifier: a client whose SHM attach fails falls back to a new baseline session (`netipc_service_posix_client_connect.c:138-150`, Go `client_unix.go:104-115`), leaking one slot per fallback on Rust/Go servers.
- Related: refused or broken SHM sessions surface as `Truncated` / `buffer too short` instead of a disconnect error in Rust and Go.
- Analysis 2026-10-29: no POSIX transport sets `SOCK_CLOEXEC`/`O_CLOEXEC` (C `netipc_uds_lifecycle.c:202,244,275`, Rust `posix.rs:266,646,712`, Go `uds_listener.go:34,98`, `uds_session.go:95`); a forking process (Netdata spawning plugins) leaks session sockets into children, which keeps the peer from ever seeing EOF and defeats socket-based dead-peer detection.
- Analysis 2026-10-29: the `nipc_shm_owner_alive` generation check can never fail, because the cached generation is read from the same mapped header it is later compared against (`netipc_shm.c:621` vs `:887`); its `kill(pid, 0)` check breaks across PID namespaces.
- Correction 2026-10-29: the "fallback leak" is not a separate client bug (C and Go close the session socket before the baseline retry); it leaks only because of K1. The error-code issue is cross-language: C also returns `NIPC_ERR_TRUNCATED` for every SHM/socket transport failure (`netipc_service_posix_client_call.c:101,124`), and no language has a "disconnected" error.
- Sibling bug: the client's SHM attach retry waits its full 5 s even when the server already closed the session socket (refusal at capacity).

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- SOW-0040 K1, K2, N5 tests pass; C guards keep passing.
- Windows spec revised with a crash-detection rule (for example named-pipe break or process liveness).
- Error codes for refused/broken SHM sessions reviewed for cross-language consistency.

## Analysis

Sources checked:

- See SOW-0040 (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`) for evidence, test files and commands.

Current state:

- Reproduction tests exist and run ungated (the `NETIPC_REPRO_KNOWN_BUGS` gate was removed in SOW-0041 under the zero-known-bug policy); they fail until this SOW lands.

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

- The covered SOW-0040 reproduction tests must pass on Linux and `costa-win11`.

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
