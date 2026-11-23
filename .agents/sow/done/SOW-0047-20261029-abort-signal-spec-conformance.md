# SOW-0047 - Abort Signal Spec Conformance

## Status

Status: completed

Sub-state: specs amended to document the 100 ms abort-observation bound; no code change.

## Requirements

### Purpose

The documented client abort mechanism and the Rust/Go implementations must agree.

### User Request

2026-10-29: "there are no known bugs. All bugs must be fixed now. Zero known bug policy." and "I want one issue at a time, with proper explanation, risks, proposed solutions, risk of the solutions recommended and explicit recommendations with reasoning."

Issue 2 decision, 2026-10-29: after asking what instant abort means and where abort is used, the user answered "100ms is ok".

### Assistant Understanding

Facts:

- The specs say a client abort is a file descriptor (POSIX) or event (Windows) waited on together with the socket or pipe (`docs/level1-posix-uds.md:107-110`, `docs/level1-windows-np.md:104-106`).
- C implements that (`netipc_uds_receive.c`, abort pipe fd polled with the socket; `netipc_named_pipe.c:396-397`, abort event checked about every 1 ms).
- Rust (`Option<&AtomicBool>`, `transport/posix.rs`, `transport/windows.rs`) and Go (abort channel, `uds_receive.go`, `pipe_receive.go`) instead poll a flag every 100 ms, so an abort can take up to 100 ms to be noticed.
- SOW-0041 kept the 100 ms flag polling (user decision 1A) and did not change this.
- C SHM response waits also use 100 ms abort-check slices (`netipc_service_posix_client_call.c:77-100`); a futex wait cannot also watch a descriptor, so no language can offer instant abort on SHM.
- Abort users: in this repo only API plumbing (Rust typed clients and `raw/cgroups_cache.rs:215`, Go typed clients and `raw/cgroups_cache.go:358-360`). In Netdata (`netdata/netdata @ 57e634eb4f`) three C shutdown paths: `src/collectors/apps.plugin/apps-cgroups-lookup-client.c:682`, `src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.c:594`, `src/collectors/ebpf.plugin/ebpf_cgroup.c:344,355` (called from `ebpf.c:917`). No Rust or Go Netdata code calls abort.
- Abort is observed only while waiting for a response; connect and HELLO_ACK waits ignore it (recorded in SOW-0043 with the K4 fix).

Inferences:

- Downstream Netdata consumers may be affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions, presented one issue at a time.

### Acceptance Criteria

- Spec and implementation agree in all three languages: the specs document that a call blocked waiting for its response observes an abort within 100 ms on every transport.

## Analysis

Sources checked:

- Read-only analysis round recorded in SOW-0041 (`.agents/sow/done/SOW-0041-20261029-rust-receive-timeout-slice.md`).

Current state (before this SOW):

- Specs required descriptor/event-based abort; Rust and Go (all transports) and C (SHM) poll a flag every 100 ms.

Risks:

- Changing Rust/Go to real abort descriptors changes public client APIs; amending the spec changes a documented guarantee.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The L1 specs describe one abort mechanism (descriptor/event waited on with the connection); the implementations use bounded flag polling except C sockets. Abort is used only on shutdown paths, so a 100 ms bound is the practical contract.

Evidence reviewed:

- Specs `docs/level1-posix-uds.md:95-113`, `docs/level1-windows-np.md:92-111`, `docs/level2-typed-api.md:268-272`, `docs/level3-snapshot-api.md:148-155`, `docs/getting-started.md:160-165`, `docs/netipc-integrator-skill.md:396-402`, `:672-685`; code and Netdata call sites listed under Facts.

Affected contracts and surfaces:

- Documentation only: L1 POSIX UDS and Windows named-pipe receive contracts, L2 abort contract, integrator skill. No code, API, wire format, or test change.

Existing patterns to reuse:

- `NIPC_CLIENT_ABORT_POLL_MS` (100) in C, the 100 ms slice in Rust (`transport/posix.rs`, `transport/windows.rs`, `client_call.rs`) and Go (`receiveAbortPollMs`).

Risk and blast radius:

- The documented guarantee changes from immediate to within 100 ms for descriptor-less implementations; all known users abort only at shutdown.

Sensitive data handling plan:

- No secrets or customer data involved; Netdata evidence cites the public upstream repository and commit only.

Implementation plan:

1. L1 UDS and named-pipe specs: abort may be observed through a descriptor/event or through slices of at most 100 ms; an empty slice is never the deadline.
2. L2 spec: a call blocked waiting for its response observes abort within 100 ms on every transport, including SHM.
3. Integrator skill: state the same bound.
4. Record the connect/handshake abort gap in SOW-0043.

Validation plan:

- Re-read every abort statement in `docs/` for consistency with the code in all three languages; no behavior change, so no test change.

Artifact impact plan:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: no update.
- Specs: update `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level2-typed-api.md`.
- End-user/operator docs: `docs/getting-started.md` and `docs/level3-snapshot-api.md` make no latency claim; no update.
- End-user/operator skills: update `docs/netipc-integrator-skill.md`.
- SOW lifecycle: completes; SOW-0043 gains the connect/handshake abort finding.

Open-source reference evidence:

- `netdata/netdata @ 57e634eb4f` paths listed under Facts.

Open decisions:

- None; decision recorded below.

## Implications And Decisions

1. 2026-10-29, Issue 2: user accepted a 100 ms abort-observation bound ("100ms is ok"), i.e. option 2A - amend the specs to match the implementations. Rejected 2B (real abort descriptors in Rust/Go: public L1 API change, new descriptors to manage, and SHM still cannot be instant) and 2C (shorter slices: more idle wake-ups, no consumer needs it).

## Plan

Executed as the implementation plan in the Pre-Implementation Gate.

## Execution Log

### 2026-10-29

- Amended `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level2-typed-api.md`, `docs/netipc-integrator-skill.md`.
- While writing the L2 bound, found that connect/HELLO_ACK waits ignore abort; narrowed the documented bound to "a call blocked waiting for its response" and recorded the gap in SOW-0043.

## Validation

Acceptance criteria evidence:

- C POSIX sockets: abort pipe polled with the socket (within the bound). C Windows pipes: abort event checked about every 1 ms. C SHM, Rust (all), Go (all): 100 ms slices. All satisfy the amended text.

Tests or equivalent validation:

- Documentation-only change; existing abort tests (Rust `test_receive_abort_after_poll_slice_reports_aborted`, `test_client_abort_unblocks_call`, lookup abort tests in C, Rust and Go) already exercise the behavior now documented.

Real-use evidence:

- Netdata's three C abort call sites are shutdown paths; the network-viewer comment already expects abort to cover UDS and SHM.

Reviewer findings:

- None requested.

Same-failure search:

- `grep -n -i abort docs/*.md`: only the L1 UDS, L1 named-pipe and L2 sections made a mechanism or latency claim; L3 and getting-started make none.

Artifact maintenance gate:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: no update; vendoring skill unaffected.
- Specs: updated (L1 UDS, L1 named pipe, L2).
- End-user/operator docs: no update needed; `getting-started.md` and `level3-snapshot-api.md` describe abort without a latency claim.
- End-user/operator skills: updated `docs/netipc-integrator-skill.md`.
- SOW lifecycle: completed and moved to `done/` with the doc change.

Spec update:

- Done (see artifact gate).

Project skill update:

- None needed; no workflow change.

End-user/operator docs update:

- None needed (see artifact gate).

End-user/operator skill update:

- Done (see artifact gate).

Lessons:

- Writing a precise guarantee exposed an unguarded path (connect/handshake ignore abort); spec wording should be checked against every wait in the call, not only the main receive.

## Outcome

The specs now state the abort contract all three languages implement: a call blocked waiting for its response observes an abort within 100 ms on every transport.

## Lessons Extracted

- See Validation -> Lessons.

## Followup

- Connect/HELLO_ACK waits ignore abort: SOW-0043 (with the K4 connect/handshake timeout fix).

## Regression Log

None yet.
