# SOW-0057 - Production Duplication Reduction

## Status

Status: open

Sub-state: scheduled after SOW-0050 (user decision 2026-10-31).

## Requirements

### Purpose

Continue SOW-0015's file-by-file maintainability work on production sources: reduce real duplication without hiding scanner rules.

### User Request

SOW-0015 (approved 2026-06-05): exclude tests and benchmarks from Codacy metrics, then "continue file-by-file on remaining production files".

### Assistant Understanding

Facts:

- Codacy Cloud on `main` `63919ecb7d` (2026-09-27, public API): 32% complex files, 32% duplicated files, goals 10% / 10% (`fileDuplicationBlockThreshold 1`: a file counts as duplicated with any one duplicated block).
- Local `jscpd --min-tokens 50` over `src/` without tests (2026-10-31, branch tip `dbfa64a`): 4653 of 48542 production lines duplicated (9.6%).
- Largest contributors (duplicated lines per file): Rust `transport/windows.rs` 745 and `transport/posix.rs` 577 (handshake and chunking logic repeated per platform); apps/cgroups lookup codecs in C (`netipc_protocol_apps_lookup.c` 417, `netipc_protocol_cgroups_lookup.c` 339), Rust (`service/apps_lookup.rs` 428, `service/cgroups_lookup.rs` 314, `protocol/lookup/cgroups_lookup.rs` 299) and Go (`protocol/apps_lookup.go` 286, `protocol/cgroups_lookup.go` 214); C Windows transports (`netipc_named_pipe.c` 346, `netipc_win_shm.c` 326).

Inferences:

- The first target is the Rust POSIX/Windows transport pair: Go already shares this logic in `transport/internal/framing`, so a Rust shared module follows an existing project pattern.

Unknowns:

- Whether the user wants this before the next Netdata vendoring (it touches the per-request chunking path, so it needs the benchmark run that precedes vendoring).

### Acceptance Criteria

- Each refactored file pair loses its duplicated blocks with no behavior change (existing suites, interop).
- Codacy duplicated-files percentage drops, measured after push.

## Analysis

See facts above.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- Platform and codec variants were written as copies; shared logic lives twice per language.

Evidence reviewed:

- The measurements above.

Affected contracts and surfaces:

- Internal code organization only; no wire or API change intended.

Existing patterns to reuse:

- Go `transport/internal/framing`; SOW-0014 split patterns.

Risk and blast radius:

- Transport chunking/handshake code runs per request; regressions would hit every session.

Performance assessment:

- Touches per-request chunking and send/receive paths; the change must be in before the pre-vendoring benchmark run, or wait for the next one.

Sensitive data handling plan:

- Source paths and tool output only.

Implementation plan:

1. Rust shared transport module for handshake validation and chunking used by `posix.rs` and `windows.rs`.
2. Re-measure; pick the next pair by evidence.

Validation plan:

- Linux, FreeBSD, Windows suites; interop; Linux targets; benchmark run per Test Cadence.

Artifact impact plan:

- `docs/code-organization.md` if module boundaries change; otherwise none.

Open decisions:

- Priority relative to Netdata vendoring (user).

## Implications And Decisions

User decision (2026-10-31, user): do this after the SOW-0050 Netdata vendoring; benchmark it in the following cycle.

## Plan

See the gate.

## Execution Log

### 2026-10-31

Created from SOW-0015.

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
