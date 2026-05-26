# SOW-0005 - Lookup Method Batch-One Benchmarks

## Status

Status: completed

Sub-state: targeted Linux benchmark complete; upstream benchmark-driver
support is ready for commit before vendoring.

## Requirements

### Purpose

Measure the most common Linux/POSIX shape for the new lookup messages:
one cgroup path or one PID per request. The result must make clear what
was measured, what was not measured, and how it compares to the existing
16-item and 256-item lookup benchmark matrix.

### User Request

The user asked whether batch-1 ping-pong was measured for the new
`CGROUPS_LOOKUP` and `APPS_LOOKUP` messages, then asked to measure it.

### Assistant Understanding

Facts:

- Existing POSIX lookup-method benchmark scenarios only cover 16-item
  and 256-item lookup requests.
- Existing generic batch ping-pong benchmarks intentionally randomize
  2-1000 items and therefore do not measure `item_count == 1`.
- The new lookup-method benchmark driver is codec+dispatch only; it
  does not include `/proc` scanning, cgroup discovery, or Netdata plugin
  integration work.

Inferences:

- `known-1` and `unknown-1` are the meaningful single-item lookup
  benchmark shapes for both new methods.
- `mixed-1` would duplicate `known-1` under the current alternating
  mixed definition because item index 0 is known.

Unknowns:

- None that block implementation.

### Acceptance Criteria

- The C, Rust, and Go POSIX lookup benchmark drivers accept
  `cgroups-lookup-known-1`, `cgroups-lookup-unknown-1`,
  `apps-lookup-known-1`, and `apps-lookup-unknown-1`.
- A targeted Linux benchmark run records throughput, p50, p95, p99, and
  CPU for all four scenarios in C, Rust, and Go.
- The result is reported with evidence and without changing unrelated
  worktree files.

## Analysis

Sources checked:

- `tests/run-posix-bench.sh`
- `tests/generate-benchmarks-posix.sh`
- `bench/drivers/c/bench_posix.c`
- `bench/drivers/rust/src/main.rs`
- `bench/drivers/go/main.go`
- `benchmarks-posix.md`

Current state:

- `tests/run-posix-bench.sh:457` lists only 16-item and 256-item lookup
  method scenarios.
- `tests/generate-benchmarks-posix.sh:29` validates and reports only
  those same scenarios.
- `bench/drivers/c/bench_posix.c:1187`,
  `bench/drivers/rust/src/main.rs:1384`, and
  `bench/drivers/go/main.go:1233` only parse `-256` and `-16`.
- `bench/drivers/c/bench_posix.c:542` documents that batch ping-pong
  uses random 2-1000 item batches because `item_count == 1` is
  normalized to the single-item path.

Risks:

- Full benchmark regeneration would expand the published matrix. For
  this request, a targeted run is lower risk and faster.
- Single-item lookup measurements are codec+dispatch measurements, not
  full plugin IPC transport or Netdata integration measurements.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The desired `item_count == 1` lookup-message benchmark is absent
  because the lookup-method scenario lists and parsers only accept `-16`
  and `-256`. Generic batch ping-pong does not fill the gap because its
  drivers intentionally avoid single-item batches.

Evidence reviewed:

- `tests/run-posix-bench.sh:457` lists the current lookup-method
  scenarios.
- `tests/generate-benchmarks-posix.sh:29` lists the current report and
  validation scenarios.
- `bench/drivers/c/bench_posix.c:1187`,
  `bench/drivers/rust/src/main.rs:1384`, and
  `bench/drivers/go/main.go:1233` parse only `-256` and `-16`.
- `bench/drivers/c/bench_posix.c:542` shows generic batch ping-pong
  uses random 2-1000 item batches.

Affected contracts and surfaces:

- Benchmark CLI scenarios for C, Rust, and Go benchmark drivers.
- POSIX benchmark runner and report generator.
- SOW evidence for this performance investigation.

Existing patterns to reuse:

- Existing lookup-method scenario naming:
  `<method>-lookup-<known|unknown|mixed>-<item-count>`.
- Existing `lookup-method-bench` command and CSV format.
- Existing POSIX benchmark report section and performance-floor pattern.

Risk and blast radius:

- Runtime protocol and service APIs are unaffected.
- The blast radius is limited to benchmark harness behavior.
- Published benchmark row count changes if the full POSIX report is
  regenerated after adding scenarios.

Sensitive data handling plan:

- The benchmark uses synthetic cgroup paths and synthetic PIDs only.
- Durable artifacts must not include local usernames, secrets,
  customer data, private endpoints, or raw machine-specific sensitive
  details. Report only sanitized benchmark outputs and repository paths.

Implementation plan:

1. Add `-1` item-count parsing to C, Rust, and Go lookup-method
   benchmark drivers.
2. Build benchmark drivers and run targeted Linux measurements for the
   four single-item scenarios across C, Rust, and Go.
3. Keep the full POSIX matrix runner and generator unchanged until a
   full 345-row benchmark regeneration is explicitly requested.

Validation plan:

- Build C, Rust, and Go benchmark drivers.
- Run targeted `lookup-method-bench` commands for each new scenario.
- Run formatting/checks for touched benchmark code where applicable.
- Scan touched durable artifacts for sensitive local path/name leaks.

Artifact impact plan:

- AGENTS.md: unaffected; no workflow or guardrail changes.
- Runtime project skills: unaffected; no reusable workflow change.
- Specs: unaffected; no protocol behavior changes.
- End-user/operator docs: unaffected unless the published benchmark
  report is regenerated.
- End-user/operator skills: unaffected; no operator workflow changes.
- SOW lifecycle: close this SOW after measurements and validation are
  recorded.

Open-source reference evidence:

- None checked; this is a repository-local benchmark harness gap, not a
  protocol or algorithm design question needing external references.

Open decisions:

- No user decision is blocked. The request is to measure single-item
  lookup requests; known and unknown are the non-duplicative
  single-item shapes.

## Implications And Decisions

1. Single-item scenarios to measure: `known-1` and `unknown-1` for both
   lookup methods.
   - Reason: these are the distinct semantic outcomes for one item.
   - Implication: `mixed-1` is intentionally omitted because it would
     duplicate `known-1` under the existing alternating mixed logic.
2. Full POSIX matrix scripts remain unchanged in this SOW.
   - Reason: adding `-1` rows to `tests/run-posix-bench.sh` and
     `tests/generate-benchmarks-posix.sh` would require regenerating the
     entire published 345-row matrix.
   - Implication: the single-item numbers are targeted benchmark
     evidence, not part of the checked-in `benchmarks-posix.md` matrix.

## Plan

1. Add single-item scenario support to benchmark drivers.
2. Run targeted Linux benchmark measurements.
3. Record validation and report results.

## Execution Log

### 2026-05-26

- Started SOW after confirming no current SOW existed.
- Confirmed existing benchmark coverage misses lookup `item_count == 1`.
- Added suffix-based `-1` scenario parsing to the C, Rust, and Go POSIX
  benchmark drivers:
  - `bench/drivers/c/bench_posix.c`
  - `bench/drivers/rust/src/main.rs`
  - `bench/drivers/go/main.go`
- Built all three POSIX benchmark drivers and ran targeted 5-second
  Linux benchmark rows for `known-1` and `unknown-1` scenarios at max,
  100k/s, 10k/s, and 1k/s target rates.

## Validation

Acceptance criteria evidence:

- The C, Rust, and Go POSIX benchmark drivers accept:
  `cgroups-lookup-known-1`, `cgroups-lookup-unknown-1`,
  `apps-lookup-known-1`, and `apps-lookup-unknown-1`.
- Targeted benchmark CSV: `/tmp/netipc-lookup-batch1-posix.csv`.
- Max-throughput results:

  | Scenario | C | Rust | Go |
  |----------|--:|-----:|---:|
  | `cgroups-lookup-known-1` | 3,646,973/s | 5,704,809/s | 2,893,590/s |
  | `cgroups-lookup-unknown-1` | 6,520,225/s | 9,263,145/s | 4,151,112/s |
  | `apps-lookup-known-1` | 4,179,779/s | 7,448,463/s | 3,302,942/s |
  | `apps-lookup-unknown-1` | 6,420,424/s | 10,745,988/s | 3,932,891/s |

- At `100000` target RPS, all measured rows reached the target within
  normal scheduler tolerance and used low single-digit CPU:

  | Scenario | C CPU | Rust CPU | Go CPU |
  |----------|------:|---------:|-------:|
  | `cgroups-lookup-known-1` | 5.6% | 4.4% | 5.2% |
  | `cgroups-lookup-unknown-1` | 4.4% | 3.5% | 3.9% |
  | `apps-lookup-known-1` | 5.2% | 3.8% | 4.5% |
  | `apps-lookup-unknown-1` | 3.8% | 3.1% | 4.3% |

Tests or equivalent validation:

- `gofmt -w bench/drivers/go/main.go` completed.
- `cargo fmt --manifest-path src/crates/netipc/Cargo.toml` completed.
- `cmake --build build --target bench_posix_c bench_posix_go bench_posix_rs -j12` passed.
- Targeted benchmark loop passed for 48 rows: 4 scenarios x 4 target
  rates x 3 languages.
- Compatibility smoke for the existing `cgroups-lookup-mixed-16`
  scenario passed in C, Rust, and Go after changing parser matching to
  suffix-based checks.
- `git diff --check` passed.

Real-use evidence:

- The benchmark binaries were executed directly on Linux using the same
  `lookup-method-bench` command shape as the existing POSIX benchmark
  runner, with 5-second duration per row.

Reviewer findings:

- No external reviewer was run; this is a narrow benchmark harness and
  measurement update.

Same-failure scan:

- The old `-16` parser path was tested after adding `-1`, preventing the
  likely suffix collision where `-1` could accidentally match `-16`.

Sensitive data gate:

- Touched durable artifacts were scanned for local personal paths and
  names; no matches were found.

Artifact maintenance gate:

- AGENTS.md: no update needed; repository workflow did not change.
- Runtime project skills: no update needed; no reusable workflow skill
  changed.
- Specs: no update needed; protocol and public API behavior did not
  change.
- End-user/operator docs: no update needed; the published benchmark
  matrix was not regenerated in this SOW.
- End-user/operator skills: no update needed; no operator workflow
  changed.
- SOW lifecycle: status set to `completed`; file moved to
  `.agents/sow/done/` before commit.

Specs update:

- No spec update needed; this work only changes benchmark-driver
  scenario parsing.

Project skills update:

- No runtime project skill update needed.

End-user/operator docs update:

- No end-user/operator docs update needed.

End-user/operator skills update:

- No end-user/operator skill update needed.

Lessons:

- Single-item lookup benchmark rows are distinct from generic
  single-message ping-pong; both need to be named explicitly to avoid
  false coverage assumptions.

Follow-up mapping:

- Full POSIX matrix publication for `*-1` lookup rows is not done here;
  it should be handled only if the project wants `benchmarks-posix.md`
  regenerated with a 345-row matrix.

## Outcome

Completed. Linux single-item lookup-method performance has been
measured, and the benchmark drivers now support direct `-1` lookup
scenario execution.

## Lessons Extracted

- Do not infer `item_count == 1` coverage from generic batch
  ping-pong rows. The batch driver explicitly avoids single-item
  batches, and the lookup-method driver needs explicit `-1` scenarios.

## Followup

None yet.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or
closed and later testing or use found broken behavior. Use a dated
`## Regression - YYYY-MM-DD` heading at the end of the file. Never
prepend regression content above the original SOW narrative.
