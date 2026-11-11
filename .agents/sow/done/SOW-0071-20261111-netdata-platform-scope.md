# SOW-0071 - Netdata NetIPC platform scope decision

## Status

Status: completed

Sub-state: decision recorded in `.agents/sow/specs/netdata-platform-scope.md`; no code change.

## Requirements

### Purpose

Every Netdata-supported platform must be a deliberate, evidence-backed state for NetIPC: either built and exercised, or recorded as out of scope with the reason.

### User Request

The user asked whether NetIPC is enabled in Netdata on all platforms and architectures. After the analysis below the user chose "4c 5a" (2026-11-11): no follow-up work; keep NetIPC Linux-only in Netdata and record why; record the Windows profile constraint for any future Windows consumer.

### Assistant Understanding

Facts:

- Netdata builds the netipc library on Linux and Windows only, and every consumer is Linux-only (evidence in the spec).
- An apps lookup link on FreeBSD, macOS or Windows would display nothing: network-viewer uses only cgroup fields, and lookups need a start time that the protocol defines only on Linux.

Inferences:

- Building the library on FreeBSD and macOS without a consumer was offered (option 4A) and declined; plugin-ipc's own CI and the manual FreeBSD runs remain the build coverage for those platforms.

Unknowns:

- None. Any future off-Linux consumer starts with a product decision on what network-viewer should show there.

### Acceptance Criteria

- The platform scope, its reasons and the Windows profile constraint are recorded in a spec with file evidence: `.agents/sow/specs/netdata-platform-scope.md`.

## Analysis

Sources checked:

- Netdata worktree of netdata/netdata#24633 (head `75b92c216d`): `src/libnetdata/CMakeLists.txt`, `packaging/cmake/Modules/NetdataPluginApps.cmake`, `NetdataPluginNetworkViewer.cmake`, `NetdataOptions.cmake`, `NetdataTests.cmake`, `src/collectors/apps.plugin/apps-lookup-netipc.c`, `src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.{c,h}`, `network-viewer-topology.c`, `network-viewer-windows.c`, `.github/workflows/`.
- plugin-ipc: `docs/codec-apps-lookup.md` (starttime semantics), `docs/level1-windows-shm.md` (profile bits).

Current state:

- `nv_cache_lookup_pid` returns no data when `expected_starttime == 0` (`network-viewer-apps-lookup-client.c:862`); the topology path skips the lookup when the start time is 0 (`network-viewer-topology.c:1038`); the start time is 0 on FreeBSD and Windows (`network-viewer-topology.c:712-720,747-751`).
- `docs/codec-apps-lookup.md:153,197-198`: `starttime` is Linux jiffies; non-Linux encoders send 0.
- The Windows `network-connections` table has no enrichment columns and its `main` never starts the lookup client (`network-viewer-windows.c:1150-1171,1204-1363`).
- Netdata has no FreeBSD CI; the apps lookup tests run on ubuntu only (`.github/workflows/topology-container-tests.yml`).

Risks:

- A future Windows consumer that keeps the current `0x04` preference would busy-spin; recorded as a constraint in the spec.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The user asked about platform coverage. NetIPC is Linux-only in Netdata because its consumers depend on cgroups and on a Linux-defined start time; no code defect is involved.

Evidence reviewed:

- Listed under Analysis.

Affected contracts and surfaces:

- Documentation only: a new spec under `.agents/sow/specs/`.

Existing patterns to reuse:

- SOW decision recording; specs describe current reality.

Risk and blast radius:

- None at runtime; no code, build or wire change.

Sensitive data handling plan:

- Only repository paths, file names and public protocol facts are recorded.

Implementation plan:

1. Write `.agents/sow/specs/netdata-platform-scope.md`.

Validation plan:

- Check every cited path and line against the Netdata worktree and plugin-ipc docs; run `.agents/sow/audit.sh`.

Artifact impact plan:

- AGENTS.md: unaffected; no workflow change.
- Runtime project skills: unaffected; the vendoring workflow does not change.
- Specs: new `netdata-platform-scope.md`.
- End-user/operator docs: unaffected; NetIPC behaviour and APIs do not change.
- End-user/operator skills: unaffected; `docs/netipc-integrator-skill.md` describes the library, which is unchanged.
- SOW lifecycle: created and completed in one commit; no follow-up.

Open-source reference evidence:

- Netdata paths are from the user's own vendoring worktree of netdata/netdata (PR #24633 head `75b92c216d`).

Open decisions:

- Resolved: 4C, 5A.

## Implications And Decisions

4. What follow-up should make NetIPC cover Netdata's non-Linux platforms?
   - A: build the library on FreeBSD and macOS, no consumer.
   - B: A plus a useful apps lookup off Linux (protocol change, portability work, a product decision first).
   - C: no follow-up; keep NetIPC Linux-only in Netdata and record why.
   - Selected: C (user, 2026-11-11).
5. The Windows `0x04` profile preference in the Netdata wrappers.
   - A: record it and fix it within any future Windows consumer.
   - B: change the wrapper defaults now.
   - Selected: A (user, 2026-11-11).

## Plan

1. Record the scope and constraint in a spec.

## Execution Log

### 2026-11-11

- Read-only analysis of the Netdata worktree and plugin-ipc docs; key claims re-checked by hand (`network-viewer-apps-lookup-client.c:862`, `network-viewer-topology.c:712-720,1038`, `docs/codec-apps-lookup.md:153,197-198`, `apps-lookup-netipc.c:316-317`).
- Added `.agents/sow/specs/netdata-platform-scope.md`.

## Validation

Acceptance criteria evidence:

- `.agents/sow/specs/netdata-platform-scope.md` records the table, reasons and Windows constraint with file references.

Tests or equivalent validation:

- Cited paths re-read in the Netdata worktree and plugin-ipc docs; `.agents/sow/audit.sh` clean.

Real-use evidence:

- On the Linux production install of the vendored tree (2026-11-09) all three Linux links connected and served data; no non-Linux runtime exists to observe.

Reviewer findings:

- None; decision record only.

Same-failure scan:

- Not applicable; no defect.

Sensitive data gate:

- Only repository paths and public protocol facts; no hosts, credentials or personal data.

Artifact maintenance gate:

- AGENTS.md: no update; workflow unchanged.
- Runtime project skills: no update; vendoring steps unchanged.
- Specs: added `.agents/sow/specs/netdata-platform-scope.md`.
- End-user/operator docs: none affected; library behaviour unchanged.
- End-user/operator skills: none affected; `docs/netipc-integrator-skill.md` covers the library, unchanged.
- SOW lifecycle: `Status: completed`, file in `done/`, committed with the spec.

Specs update:

- Added `netdata-platform-scope.md`.

Project skills update:

- None needed; the vendoring workflow is unchanged.

End-user/operator docs update:

- None; no behaviour change.

End-user/operator skills update:

- None; no docs or spec of the library changed.

Lessons:

- Check what a consumer actually displays before proposing to enable a link on a new platform: transport availability alone does not make the link useful.

Follow-up mapping:

- Off-Linux consumers: rejected by the user (4C), reasons in the spec.
- Windows profile preference: tracked as a constraint in the spec (5A); it applies only if a Windows consumer is ever built.

## Outcome

NetIPC stays Linux-only in Netdata by decision; the reasons and the Windows constraint are recorded in `.agents/sow/specs/netdata-platform-scope.md`.

## Lessons Extracted

- Check what a consumer displays before proposing to enable it on a new platform.

## Followup

None.

## Regression Log

None yet.
