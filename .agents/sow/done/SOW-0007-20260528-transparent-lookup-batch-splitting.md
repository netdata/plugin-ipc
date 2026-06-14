# SOW-0007 - Transparent Lookup Batch Splitting

## Status

Status: closed

Sub-state: superseded before implementation by `SOW-0021 - NetIPC At Scale`.

## Requirements

### Purpose

Make typed lookup APIs own oversized semantic batching end to end, so callers can pass large cgroups/apps lookup requests without knowing the encoded byte layout or transport request payload caps.

### User Request

The user clarified during SOW-0006 that Level 2+ consumers cannot reasonably know request payload byte sizes. Typed APIs are specific to their message layout and should own batching behavior.

### Assistant Understanding

Facts:

- SOW-0006 keeps request payload byte sizing out of public typed config.
- C lookup calls already compute exact encoded request size before sending.
- SOW-0006 fixes normal below-hard-cap lookup requests by growing internal request capacity and reconnecting.
- A single semantic lookup request above `NIPC_MAX_PAYLOAD_CAP` can still return `NIPC_ERR_OVERFLOW`.

Inferences:

- Transparent split/stitch is the correct long-term Level 2+ behavior for semantic lookup batches above the hard cap.
- Split/stitch changes response assembly and borrowed-view lifetime semantics, so it deserves separate fail-first tests and cross-language parity review.

Unknowns:

- Whether C, Rust, and Go should all expose identical oversized semantic batch behavior in the same release boundary.
- Whether response stitching should use one assembled internal response buffer or a multi-view cursor abstraction.

### Acceptance Criteria

- C cgroups/apps lookup clients transparently split semantic requests that exceed the hard request payload cap but whose individual items fit.
- Caller-visible response order is preserved.
- Borrowed response views remain valid for the documented lifetime after stitched responses.
- Oversized single items still fail with `NIPC_ERR_OVERFLOW` or a documented equivalent.
- Rust and Go behavior is either implemented to match C or explicitly specified as a staged parity gap with tests proving current behavior.
- Tests cover mixed known/unknown results, empty apps retry-later cgroup paths, boundary-sized batches, and over-capacity single items.
- Specs/docs record the final oversized semantic batch contract.

## Analysis

Sources checked:

- `.agents/sow/current/SOW-0006-20260528-shm-lookup-regression-port.md`
- `src/libnetdata/netipc/src/service/netipc_service.c`
- `src/crates/netipc/src/service/`
- `src/go/pkg/netipc/service/`
- `docs/level2-typed-api.md`

Current state:

- Request byte capacity is intentionally internal to typed APIs.
- SOW-0006 leaves requests above `NIPC_MAX_PAYLOAD_CAP` as overflow, while fixing the downstream SHM regression.

Risks:

- Stitching responses incorrectly can create lifetime bugs because current decoded views borrow one response buffer.
- Splitting only C would create cross-language behavior drift unless explicitly documented and tested.
- Silent partial results are unacceptable; failures must be all-or-error.

## Pre-Implementation Gate

Status: not-ready

Problem / root-cause model:

- The typed API boundary says callers should not reason about encoded byte capacity. The current implementation owns normal request sizing but does not yet split semantic batches that exceed the hard per-message payload cap.

Evidence reviewed:

- SOW-0006 decision keeps `max_request_payload_bytes` out of public typed configs.
- C lookup request sizing helpers compute encoded request size before sending.

Affected contracts and surfaces:

- C cgroups/apps typed lookup APIs.
- Rust cgroups/apps typed lookup APIs.
- Go cgroups/apps typed lookup APIs.
- `docs/level2-typed-api.md`
- `docs/netipc-integrator-skill.md`

Existing patterns to reuse:

- Existing lookup request-size helpers.
- Existing call-with-retry capacity growth path.
- Existing response builders and decoded view tests.

Risk and blast radius:

- Cross-language behavior parity.
- Response view lifetime and memory ownership.
- Performance for large batches.
- Error semantics for partial failures.

Sensitive data handling plan:

- Use synthetic PIDs, paths, cgroup names, labels, and service names only.
- Do not record live process/container/customer data, secrets, private endpoints, or personal data in SOWs, specs, docs, skills, instructions, or code comments.

Implementation plan:

1. Add fail-first C tests for over-capacity semantic lookup batches and view lifetime.
2. Design response stitching so order and all-or-error semantics are explicit.
3. Implement C split/stitch.
4. Review Rust and Go parity; implement or document staged parity with tests.
5. Update docs/specs and output/reference skill guidance.

Validation plan:

- C unit tests for boundary, split, single-item overflow, mixed status, and view lifetime.
- Rust and Go tests for parity or documented staged behavior.
- Interop tests if wire output can cross language boundaries.
- Benchmarks for large batches before and after split/stitch.

Artifact impact plan:

- AGENTS.md: likely unaffected.
- Runtime project skills: none currently exist; update only if reusable workflow knowledge appears.
- Specs: update `docs/level2-typed-api.md`.
- End-user/operator docs: update public integration guidance if callers can rely on transparent oversized batching.
- End-user/operator skills: update `docs/netipc-integrator-skill.md`.
- SOW lifecycle: pending follow-up created from SOW-0006.

Open-source reference evidence:

- None checked. This is an internal typed API contract and transport cap behavior.

Open decisions:

- Choose the stitching memory model before implementation.
- Choose whether C/Rust/Go parity is one-shot or staged.

## Implications And Decisions

Pending user scheduling and design decision.

## Plan

Pending.

## Execution Log

### 2026-05-28

- Created as the tracked follow-up for SOW-0006's explicitly scoped-out transparent split/stitch work.

### 2026-06-13

- Closed before implementation because `SOW-0021 - NetIPC At Scale` supersedes this narrower request-splitting SOW.
- The old all-or-error oversized single-item assumption is explicitly replaced by SOW-0021's item-level outcome contract.
- The old staged C/Rust/Go parity option is explicitly replaced by SOW-0021's all-language completion requirement.

## Validation

- Not executed in this SOW because the implementation and validation scope moved to SOW-0021 before this SOW started.
- SOW-0021 owns the full C/Rust/Go, POSIX/Windows, interop, coverage, benchmark, and downstream vendoring validation.

## Outcome

Closed as superseded by `SOW-0021 - NetIPC At Scale`.

## Lessons Extracted

Pending.

## Followup

The active implementation and validation follow-up is SOW-0021.

## Regression Log

None yet.
