# SOW-0010 - Static Analysis Finding Cleanup

## Status

Status: open

Sub-state: Scanner rollout exposed pre-existing findings that need focused triage before stricter gates are enabled.

## Requirements

### Purpose

Make the SDK clean under the strongest practical local and GitHub static-analysis scanners, so future CI can hard-gate more findings without blocking on existing debt.

### User Request

This follow-on SOW was created from scanner validation during `SOW-0009`; the user approved installing strong GitHub and local scanners.

### Assistant Understanding

Facts:

- `SOW-0009` added GitHub and local scanner coverage.
- Local `staticcheck` reports Go findings in `src/go`.
- Local `gosec` reports findings in the SDK, fixture, and benchmark Go modules.
- Local C scanners report findings in test code when run beyond the initial hard gate.
- Local full ShellCheck reports existing warning/info findings; the initial scanner rollout gates only ShellCheck errors.

Inferences:

- These findings should be triaged in a dedicated cleanup pass because they affect existing SDK/test/benchmark code rather than the scanner workflow configuration itself.
- After this cleanup, the static-analysis workflow can likely promote more scanners from reporting mode to hard gates.

Unknowns:

- Which findings are true bugs versus acceptable test/benchmark patterns that need local suppressions with documented rationale.

### Acceptance Criteria

- Triage each scanner finding class with file/line evidence.
- Fix true bugs and unsafe patterns.
- Add narrow suppressions only when the finding is intentional, test-only, or a false positive.
- Re-run C, Rust, Go, shell, and workflow scanners locally.
- Tighten `.github/workflows/static-analysis.yml` gates where the cleaned results make that safe.
- Update specs, docs, skills, or SOW artifacts if any cleanup changes public behavior or durable workflow guidance.

## Analysis

Sources checked:

- Local scanner validation from `SOW-0009`.
- Go SDK source around `src/go/pkg/netipc/protocol/lookup.go:758`, `src/go/pkg/netipc/protocol/lookup.go:1274`, `src/go/pkg/netipc/protocol/lookup.go:1278`, and `src/go/pkg/netipc/transport/posix/uds.go:667`.
- C test source around `tests/fixtures/c/test_shm.c:1033`, `tests/fixtures/c/test_shm.c:1667`, and `tests/test_protocol.c:369`.

Current state:

- `staticcheck ./...` in `src/go` reports unused assignment findings at `lookup.go:758`, `lookup.go:1274`, and `lookup.go:1278`, plus unused function `maxU32` at `uds.go:667`.
- `gosec ./...` reports findings across `src/go`, `tests/fixtures/go`, and `bench/drivers/go`; common classes include integer conversion, file path, unsafe block, and unchecked return findings.
- `cppcheck` against the broader test tree reports uninitialized-buffer findings in test code, including `tests/fixtures/c/test_shm.c:1033` and `tests/test_protocol.c:369`.
- `flawfinder --minlevel=4` reports `access()` usage in Windows shared-memory cleanup logic and C tests, including `tests/fixtures/c/test_shm.c:1667`.

Risks:

- Fixing findings mechanically can change wire encoding, transport behavior, or benchmark semantics.
- Suppressing findings too broadly can hide real bugs.
- Tightening gates before cleanup will make the initial scanner rollout fail immediately.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- Stronger scanners now expose existing code patterns that were previously not enforced. The local evidence above shows both likely real cleanup items and scanner findings that may be acceptable in tests or benchmarks.

Evidence reviewed:

- `src/go/pkg/netipc/protocol/lookup.go:758`, `src/go/pkg/netipc/protocol/lookup.go:1274`, and `src/go/pkg/netipc/protocol/lookup.go:1278` from local `staticcheck`.
- `src/go/pkg/netipc/transport/posix/uds.go:667` from local `staticcheck`.
- `tests/fixtures/c/test_shm.c:1033` and `tests/test_protocol.c:369` from local `cppcheck`.
- `tests/fixtures/c/test_shm.c:1667` from local `flawfinder --minlevel=4`.

Affected contracts and surfaces:

- Go SDK source, Go fixtures, Go benchmark drivers, C tests, shell scripts, scanner workflows, and future CI behavior.
- Public protocol/API behavior may be affected if fixes touch encoding, conversion, or transport paths.

Existing patterns to reuse:

- Existing C, Rust, Go, sanitizer, Valgrind, race, and interop validation commands listed in `AGENTS.md`.
- Existing SOW validation and artifact-maintenance gates.

Risk and blast radius:

- Medium. The findings span SDK code, tests, fixtures, and benchmarks. Some fixes can be local, but conversion and encoding fixes require cross-language interoperability checks.

Sensitive data handling plan:

- No sensitive data is required. Evidence should cite source paths, line numbers, scanner classes, and sanitized output summaries only.

Implementation plan:

1. Reproduce all scanner findings and classify each as true positive, false positive, intentional test fixture, or acceptable benchmark pattern.
2. Fix true positives with focused code changes and add narrow suppressions where justified.
3. Promote static-analysis workflow gates only after local validation is clean enough to avoid immediate CI deadlock.

Validation plan:

- Run the final scanner matrix locally.
- Run C/Rust/Go test and interoperability commands affected by touched code.
- Run `bash .agents/sow/audit.sh` and `git diff --check`.

Artifact impact plan:

- AGENTS.md: likely unaffected unless validation policy changes.
- Runtime project skills: likely unaffected unless repeatable scanner workflow knowledge emerges.
- Specs: update only if protocol/API behavior changes.
- End-user/operator docs: likely unaffected unless public SDK guidance changes.
- End-user/operator skills: update only if public integration workflow changes.
- SOW lifecycle: complete this SOW only after all deferred scanner findings are implemented, rejected with evidence, or moved to separate SOWs.

Open-source reference evidence:

- None yet for this cleanup. Future implementation should check comparable scanner suppression/fix patterns if a class of finding is ambiguous.

Open decisions:

- None yet. Future implementation may need user decisions if a scanner finding requires accepting a behavior change, broad suppression, or benchmark/test rewrite.

## Implications And Decisions

- No user decision has been requested yet. This SOW records cleanup work discovered by the scanner rollout.

## Plan

1. Reproduce scanner findings and classify them.
2. Fix true positives and document narrow suppressions.
3. Tighten CI gates where validation proves they are ready.

## Execution Log

### 2026-06-02

- Created from `SOW-0009` validation after local scanner installation surfaced pre-existing findings.

## Validation

Acceptance criteria evidence:

- Pending.

Tests or equivalent validation:

- Pending.

Real-use evidence:

- Pending.

Reviewer findings:

- Pending.

Same-failure scan:

- Pending.

Sensitive data gate:

- Pending.

Artifact maintenance gate:

- AGENTS.md: Pending.
- Runtime project skills: Pending.
- Specs: Pending.
- End-user/operator docs: Pending.
- End-user/operator skills: Pending.
- SOW lifecycle: Pending.

Specs update:

- Pending.

Project skills update:

- Pending.

End-user/operator docs update:

- Pending.

End-user/operator skills update:

- Pending.

Lessons:

- Pending.

Follow-up mapping:

- Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
