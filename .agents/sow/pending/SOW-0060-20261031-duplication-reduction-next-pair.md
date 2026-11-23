# SOW-0060 - Production duplication reduction, next file pair

## Status

Status: open

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: waiting; picks up after the pre-vendoring benchmarks and the SOW-0050 vendoring (user ordering 2026-10-31).

## Requirements

### Purpose

Bring Codacy's duplicated-files share on `main` toward the 10% goal, one file pair per SOW (SOW-0057 user decision 1).

### User Request

SOW-0057 decision 1 (2026-10-31, user): the Rust transport pair first; each further pair gets its own SOW, picked by re-measuring.

### Assistant Understanding

Facts:

- SOW-0057 moved the Rust transport framing into `transport/framing.rs`. jscpd on `posix.rs`, `windows.rs` and `framing.rs` (`--min-tokens 50`) fell from 587 duplicated lines (12.8%) to 309 (7.2%); the rest is per-transport config types, session accessors, handshake wrappers and test fixtures.
- Other large contributors measured before SOW-0057 (`jscpd --min-tokens 50` over `src/` without tests, duplicated lines per file): the C apps/cgroups lookup codecs (`netipc_protocol_apps_lookup.c` 417, `netipc_protocol_cgroups_lookup.c` 339), the Rust lookup codecs and services (`service/apps_lookup.rs` 428, `service/cgroups_lookup.rs` 314, `protocol/lookup/cgroups_lookup.rs` 299), the Go lookup codecs (`protocol/apps_lookup.go` 286, `protocol/cgroups_lookup.go` 214), and the C Windows transports (`netipc_named_pipe.c` 346, `netipc_win_shm.c` 326).

Inferences:

- The lookup codecs exist in all three languages, so a shared-helper refactor there repeats per language; that's one candidate pair per language.

Unknowns:

- Which pair the re-measurement ranks first, and whether it touches the per-request path (decides the benchmark need under Test Cadence).

### Acceptance Criteria

- The chosen pair loses its duplicated blocks with no behavior change (existing suites, interop).
- Codacy's duplicated-files percentage drops, measured after push.

## Analysis

Sources checked:

- SOW-0057 Analysis and Validation.

Current state:

- Not started. First step: re-run jscpd over `src/` without tests, then pick the pair with the user.

Risks:

- Lookup codec refactors touch the wire decode path in each language; cross-language interop suites must cover them.

## Pre-Implementation Gate

Status: blocked

Not filled: the target pair is chosen by re-measuring when this SOW starts.

## Implications And Decisions

None yet.

## Plan

1. Re-measure, present the ranked pairs to the user, record the choice.
2. Fill the gate, implement, validate.

## Execution Log

### 2026-10-31

Created at SOW-0057 close as its follow-up.

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
