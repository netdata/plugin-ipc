# SOW-0069 - Benchmark Host Drift And Floor Re-Baseline

## Status

Status: open

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: queued after the SOW-0050 Netdata vendoring (user decision 2A, 2026-11-06).

## Requirements

### Purpose

The committed benchmark reports and their floors describe a known host state again, so a floor failure means a code regression and not a slower host.

### User Request

SOW-0050 decision 2 (user, 2026-11-06, option A): keep the June CSVs during the vendoring; track why both benchmark hosts now run at about half their committed numbers, then re-baseline the CSVs and floors.

### Assistant Understanding

Facts:

- `benchmarks-posix.csv` and `benchmarks-windows.csv` date from 2026-06-15 (`95c53ef`). POSIX uds-ping-pong c->c was 156k/s with a 4 us p50.
- On 2026-11-05 the Linux workstation gives 65k to 78k/s for uds-ping-pong on every pair with both `63919ec` and `4261919`. The generator reports 26 floor violations on the candidate CSV and the `63919ec` run misses 31 floors after retries (SOW-0050 Execution Log).
- The Windows test host runs `95c53ef` at 0.35x to 0.45x of its own June numbers (SOW-0050 A/B), so the drop there is the host.
- Workstation state on 2026-11-05: `schedutil` governor, STIBP always on, kernel 7.0.0-42, unrelated workloads (load average 10 to 25 on 112 cores). The June state was not recorded.
- Windows SHM ping-pong and snapshot-shm rows fail the stability gate on every recent run, so `tests/generate-benchmarks-windows.sh` rejects full runs as incomplete.

Inferences:

- Host configuration (frequency policy, mitigations, kernel, background load) changed since June; which factor dominates is unknown.

Unknowns:

- The June host state; whether a dedicated quiet host or a recorded host configuration is available.

### Acceptance Criteria

- The cause of the drop is identified with evidence on each host, or recorded as unrecoverable with the measurements that rule out code.
- Each benchmark report records the host state it was measured on.
- New CSVs pass their floors, and any floor change is justified by the recorded host state, not by fitting the numbers.
- The Windows SHM stability-gate failures are explained and either fixed or handled by the gate with evidence.

## Analysis

Sources checked:

- SOW-0050 Execution Log (A/B runs of 2026-11-05).

Current state:

- Floors fail on both hosts for both the old and the new code.

Risks:

- Changing host configuration on the production workstation needs user approval; lowering floors without a cause would hide regressions.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Blocked until SOW-0050 completes; filled at activation.

Evidence reviewed:

- SOW-0050 A/B evidence; the committed CSVs.

Affected contracts and surfaces:

- `benchmarks-posix.*`, `benchmarks-windows.*`, `tests/generate-benchmarks-*.sh` floors, `tests/run-*-bench.sh` host-state capture.

Existing patterns to reuse:

- The same-host A/B and row-interleaved methods recorded in SOW-0050.

Risk and blast radius:

- Benchmark artifacts and scripts only; no library code.

Sensitive data handling plan:

- Host state is recorded by role and configuration (governor, kernel, mitigations); no host names, addresses or accounts.

Implementation plan:

1. Record host state (CPU governor, frequency, kernel, mitigations, power plan; by role, never host names) as durable metadata in each benchmark CSV or a committed sidecar, and make `tests/generate-benchmarks-posix.sh` and `tests/generate-benchmarks-windows.sh` require it and print it in the committed reports.
2. Measure the Linux workstation under controlled settings, with user approval for any change, and run an A/B against the June baseline.
3. Windows host: capture the same host state, A/B the current tree against the June numbers under controlled settings, and analyse the SHM ping-pong and snapshot-shm stability-gate failures (per-run spread, scheduling and timer effects) until they are fixed or the gate handles them with evidence.
4. Re-baseline both CSVs and review floors with the user.

Validation plan:

- Generators pass on fresh full runs; an A/B shows the floors catch a known slowdown.

Artifact impact plan:

- AGENTS.md: possibly the benchmark command notes.
- Runtime project skills: `project-netdata-vendoring` performance step if the method changes.
- Specs: none expected.
- End-user/operator docs: the benchmark reports.
- End-user/operator skills: none expected.
- SOW lifecycle: opens after SOW-0050 closes.

Open-source reference evidence:

- None; the evidence is this repository.

Open decisions:

- Host configuration changes (later, with evidence).

## Implications And Decisions

1. Keep the June CSVs during the SOW-0050 vendoring and track the drop here (user, 2026-11-06, 2A). Rejected: lowering floors to today's numbers; committing a report that fails its own floors.

## Plan

See the implementation plan above.

## Execution Log

### 2026-11-06

- Created from SOW-0050 decision 2A.

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
