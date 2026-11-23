# SOW-0065 - Lookup clients stitch into reusable client storage

## Status

Status: open

Sub-state: created from SOW-0063; not analysed beyond the evidence below; needs a design decision before implementation.

## Requirements

### Purpose

Typed lookup client calls on an established session should not allocate per item, matching the Level 2 rule that variable-size responses borrow reusable client storage.

### User Request

Found during SOW-0063 (Go per-request allocations): the Go lookup clients allocate several buffers per call. The user approved tracking it in its own SOW rather than redesigning it inside SOW-0063.

### Assistant Understanding

Facts:

- `docs/level2-typed-api.md:192-193`: "To keep the hot path allocation-free where the typed message shape allows it, Level 2 owns and reuses its internal wire buffers"; `:443-444`: "Variable-size structured responses should return ephemeral typed views that borrow internal reusable client storage".
- Every CGROUPS_LOOKUP and APPS_LOOKUP client call copies each accepted response item into its own heap buffer, even when one subresponse answered every item. The stitched response differs by language (corrected 2026-11-07, SOW-0070):
  - Go also encodes the stitched response into a new buffer: `service/raw/cgroups_lookup.go:264-265`; `service/raw/apps_lookup.go:184-185`; item clone `service/raw/lookup_common.go:173` (escape analysis: `go build -gcflags=-m`);
  - Rust encodes the stitched response into the client's reusable `transport_buf` (`service/raw/cgroups_lookup.rs:224-229`); its per-call allocations are the item copies (`cgroups_lookup.rs:123`, `:190`; `apps_lookup.rs:85`, `:143`) and the temporary slice list (`cgroups_lookup.rs:225`);
  - C encodes the stitched response into the client's reusable `response_buf` (`netipc_service_cgroups_lookup.c:440-452`); its per-call allocations are the item copies (`netipc_service_cgroups_lookup.c:237`, `:392`; `netipc_service_apps_lookup.c:295`) and, for an oversized item only, a one-item marker response (`netipc_service_cgroups_lookup.c:208`).
- The returned view's documented lifetime is already "until the next typed call on this client" in all three languages, so client-owned storage fits the contract.

Inferences:

- Reusing client-owned item and stitch buffers (and returning the single subresponse view directly when no split happened) removes the per-call allocations without a public API change. Speculation until analysed: the split/retry/stitch code may need restructuring in all three languages.

Unknowns:

- Whether the oversized-item path (`cgroups_lookup.c:208`) can use scratch storage too, or stays a rare-path allocation.

### Acceptance Criteria

- An allocation test per language fails before the change and passes after, on single-subresponse and split lookups (Go `testing.AllocsPerRun`; Rust and C equivalents to be designed).
- Lookup interop and lookup-scale suites pass on every platform that runs them.
- The lookup benchmark rows show no regression.

## Analysis

Sources checked:

- The files and lines listed under Facts (SOW-0063 investigation, 2026-11-03).

Current state:

- Lookups allocate per item in C, Rust and Go clients; servers do not (SOW-0063 fixed the Go servers).

Risks:

- Split/retry/stitch is a cross-language contract (`docs/codec-cgroups-lookup.md:119-122`, `docs/codec-apps-lookup.md:135-138`); a reuse bug would corrupt stitched results across calls.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- See Facts. The stitch code collects owned item copies because the subresponse buffer is reused by the next subcall.

Evidence reviewed:

- See Facts.

Affected contracts and surfaces:

- C, Rust and Go lookup clients; Level 2 spec wording on lookup ownership if it changes.

Existing patterns to reuse:

- Client scratch buffers (`ensureClientScratch` in Go, `request_scratch` in Rust); the SOW-0063 Go allocation tests (`service/raw/alloc_calls_test.go`).

Risk and blast radius:

- Lookup correctness across subcalls; all three languages.

Sensitive data handling plan:

- Evidence is code paths and test output only; no sensitive data is involved.

Implementation plan:

1. To be written after analysis and the user's design decision.

Validation plan:

- Allocation tests, lookup unit and interop suites, lookup-scale interop, the lookup benchmark rows.

Artifact impact plan:

- AGENTS.md: likely unaffected.
- Runtime project skills: likely unaffected.
- Specs: `docs/level2-typed-api.md` only if the ownership wording changes.
- End-user/operator docs: likely unaffected (no API change intended).
- End-user/operator skills: `docs/netipc-integrator-skill.md` only if lookup lifetimes change.
- SOW lifecycle: open in pending; created from SOW-0063.

Open-source reference evidence:

- None; this is project code only.

Open decisions:

- Design of the reusable stitch storage, to be presented with options after analysis.

## Implications And Decisions

None yet.

## Plan

Pending analysis.

## Execution Log

### 2026-11-03

Created from SOW-0063.

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

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
