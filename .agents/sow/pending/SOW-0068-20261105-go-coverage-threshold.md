# SOW-0068 - Restore Go Coverage To Its Threshold

## Status

Status: open

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: queued after the SOW-0050 Netdata vendoring (user decision, 2026-11-05).

## Requirements

### Purpose

The POSIX Go coverage measured by `tests/run-coverage-go.sh` meets its 90% threshold again, through tests of real behaviour, not by lowering the threshold.

### User Request

Found during SOW-0050/SOW-0067 CI triage (2026-11-05). The user chose option A ("2a"): open a pending SOW to find the uncovered code and restore 90%, after this vendoring. Lowering the threshold was rejected.

### Assistant Understanding

Facts:

- `tests/run-coverage-go.sh` (threshold 90, `tests/run-coverage-go.sh:170`) reports 88.4% total on `292c6dc` and 88.3% on `d54b22c` (measured from a `git archive` export), so the shortfall predates SOW-0066 and SOW-0067.
- Per package on `292c6dc` (Linux): `protocol` 88.3%, `transport/posix` 84.9%, `service/raw` 90.3%; `service/internal/transportconfig`, `service/apps_lookup`, `service/cgroups_lookup` and `service/cgroups_snapshot` 100%.
- The last recorded pass is 90.0% in SOW-0004 (`.agents/sow/done/SOW-0004-20260525-cgroups-apps-lookup-methods.md:2079`).
- No GitHub workflow runs this threshold; Codacy Coverage uploads the Go profile without a gate (`tests/generate-codacy-coverage.sh`).
- C has the same gap (found during SOW-0072 validation, 2026-11-11): `tests/run-coverage-c.sh` (90% per file, `tests/run-coverage-c.sh:44`) fails on `netipc_uds.c` 84.8% and `netipc_shm.c` 89.8%, identical on `dce436c` (from a `git archive` export) and with the SOW-0072 changes; the total is 91.1%. No workflow runs this gate either. Go on `dce436c`: 88.0%. Whether this SOW also covers C is confirmed with the user at activation.

Inferences:

- SOWs since SOW-0004 added Go code (transport, SHM and lookup work) faster than tests covering its error branches.

Unknowns:

- Which functions and branches carry the gap; resolved by `go tool cover -func` at activation.

### Acceptance Criteria

- `tests/run-coverage-go.sh` reports at least 90% on Linux.
- Every added test asserts behaviour (an error code, a state, a wire result); none exists only to execute lines.
- Unreachable code found by the analysis is removed rather than covered, with evidence, and the same branch is checked in C and Rust.

## Analysis

Sources checked:

- `.tmp` coverage logs of 2026-11-05 (not durable); the numbers above.

Current state:

- Below threshold by about 1.6 points.

Risks:

- Tests written only to cover lines add maintenance without value; branches reachable only through OS faults need the existing fault-injection hooks rather than new production switches.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Blocked until SOW-0050 completes (user ordering). The gate is filled at activation from `go tool cover -func` output.

Evidence reviewed:

- To be gathered at activation: the per-function coverage report for `protocol` and `transport/posix`.

Affected contracts and surfaces:

- Go tests; possibly removal of unreachable Go branches (then C and Rust parity checks).

Existing patterns to reuse:

- The test timing and fault hooks in AGENTS.md "Test Cadence" (Go unexported fields), the existing Go fault-injection wrappers.

Risk and blast radius:

- Tests only, unless unreachable code is removed; no per-request path changes expected, so no benchmark run.

Sensitive data handling plan:

- No sensitive data involved; evidence is coverage percentages, function names and file paths in this repository.

Implementation plan:

1. Produce the per-function report and list the uncovered branches by package.
2. Classify each: reachable and worth a behavioural test, reachable only by fault injection, or unreachable.
3. Add tests or remove dead branches; check C and Rust for the same branch.

Validation plan:

- `tests/run-coverage-go.sh` at or above 90%; `go test ./...` on Linux, FreeBSD amd64 and 386, macOS and Windows for touched files.

Artifact impact plan:

- AGENTS.md: likely unaffected.
- Runtime project skills: likely unaffected.
- Specs: unaffected unless dead branches reveal a spec mismatch.
- End-user/operator docs: unaffected.
- End-user/operator skills: unaffected.
- SOW lifecycle: opens after SOW-0050 closes.

Open-source reference evidence:

- None; the evidence is this repository.

Open decisions:

- None now; the scope decision (A) is recorded below.

## Implications And Decisions

1. Go coverage below threshold (user, 2026-11-05, "2a"): restore 90% in a pending SOW after the SOW-0050 vendoring. Rejected: B, fixing it inside the vendoring; C, lowering the threshold.

## Plan

See the implementation plan above.

## Execution Log

### 2026-11-05

- Created from the SOW-0050 CI triage.

## Validation

Pending activation.

## Outcome

Pending activation.

## Lessons Extracted

Pending activation.

## Followup

None yet.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
