# SOW-0052 - Transport Hygiene

## Status

Status: open

Sub-state: split from SOW-0043 on 2026-10-31 (transport-level items); starts after SOW-0051 completes.

## Requirements

### Purpose

Fix the remaining transport-level defects found by SOW-0040, SOW-0046 and SOW-0047: EINTR handling, the stale-socket recovery race, failure classification and SHM resource timing.

### User Request

From SOW-0040 decisions (2026-10-29): fix all reproduced and review-found defects under the zero-known-bug policy. SOW-0043 was split on 2026-10-31 (assistant process decision, recorded in SOW-0043); this SOW carries the transport-level items.

### Assistant Understanding

Facts (line numbers verified 2026-10-31 against commit `b450b40`):

- N9: EINTR is treated as a disconnect. C `netipc_uds.c:45-47` (send), `:74-76` (sendmsg), `:83` (recv), with handshake callers returning `ERR_RECV` (`netipc_uds_handshake.c:162-164`, `:214-216`); Rust `posix.rs:908-910` (`raw_send_iov`), `:917-927` (`raw_send`), `:932-937` (`raw_recv`); only the poll at `posix.rs:999` handles EINTR.
- Stale-socket recovery can unlink a live server's endpoint (SOW-0046 working theory; user decision: reproduction test first). A server's socket file exists from `bind()` until `listen()`, and connects in that window are refused. C `nipc_uds_check_and_recover_stale` (`netipc_uds_lifecycle.c:181-212`, unlink at `:205-208`; bind `:252`, listen `:258`) and Rust (`posix.rs:1104-1144`, unlink `:1123-1136`) unlink on any non-ENOENT connect error after one probe; Go (`uds_stale.go:27-44`, unlink `:68`) retries ECONNREFUSED 3 times with 50 ms delays, which narrows but does not close the window, and is a cross-language difference.
- Failure classification: after SOW-0044, a send/recv failure reports DISCONNECTED when a peer probe confirms the peer is gone, but SHM receive errors other than timeout still return TRUNCATED (C `netipc_service_posix_client_call.c:120`; Rust `client_call.rs:475`, `:500`, `:517`; Go `client_unix.go:164`, `:232`), and without a probe hit all three fall back to TRUNCATED ("buffer too short") for what is a transport failure.
- SHM regions are created before HELLO is read for every accepted connection (C POSIX `netipc_service_posix_server.c:206-207`, C Windows `netipc_service_win_server.c:107-111`, Rust `server_unix.rs:43`/`:137-143`, `server_windows.rs:28`/`:133`, Go `server_unix.go:123`), so every connection, including rejected and silent ones, costs a region create and destroy. SOW-0043 moves this work into the session worker but keeps it before HELLO.

Inferences:

- EINTR during a handshake on a plugin that receives signals (Netdata plugins do) turns a transient signal into a failed connect.

Unknowns:

- Whether the stale-socket race is reachable in practice; the reproduction test decides.

### Acceptance Criteria

- Tests prove EINTR is retried in C and Rust send/recv paths.
- A reproduction test for the stale-socket race exists; if it reproduces, all three languages close the window with one shared rule.
- Transport failures are not reported as TRUNCATED in any language.
- SHM regions are created only for sessions that negotiate SHM, or the cost is justified with evidence.

## Analysis

Sources checked:

- SOW-0040, SOW-0046, SOW-0047 evidence; SOW-0043 facts (verified 2026-10-31).

Current state:

- None of these items has a reproduction test yet.

Risks:

- Error-code reclassification is visible to callers; Netdata treats any non-OK as failure, so the impact is in logs and diagnostics.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- See Facts; completed at activation after SOW-0051, which changes the same client call paths.

Evidence reviewed:

- The verified locations above; the full gate is completed at activation.

Affected contracts and surfaces:

- L1 send/recv, listener stale recovery, L2 error mapping, server SHM setup, docs, tests in all languages.

Existing patterns to reuse:

- SOW-0044's send/recv-failure classification and peer probes.

Risk and blast radius:

- Every transport path; recorded fully at activation.

Sensitive data handling plan:

- No secrets involved. Applies to this SOW, docs, the integrator skill and code comments.

Implementation plan:

1. Completed at activation.

Validation plan:

- New tests per item on Linux, the Netdata Linux targets and Windows; interop suites.

Artifact impact plan:

- AGENTS.md: assess at activation.
- Runtime project skills: assess at activation.
- Specs: transport docs under `docs/` as affected.
- End-user/operator docs: same.
- End-user/operator skills: `docs/netipc-integrator-skill.md` if error codes change.
- SOW lifecycle: split from SOW-0043.

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
