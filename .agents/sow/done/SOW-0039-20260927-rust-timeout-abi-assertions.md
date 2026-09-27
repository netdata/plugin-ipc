# SOW-0039 - Assert Rust timeout ABI test layouts

## Status

Status: completed

Sub-state: layout enforcement validated; three AI review threads explained and resolved; publishing with implementation.

## Requirements

### Purpose
Address the three reviewed AI findings and resolve each with a fix or evidence.

### User Request
The user explicitly requests fixing code/docs or explaining incorrect findings and resolving all comments.

### Assistant Understanding
Facts: both ARM Rust ABI modes currently pass and print different sizes, but the runner does not assert them. The GNU riscv32 claim assumes a time64 alias absent from pinned libc. The Netdata private-field finding applies a Go collector skill to a Rust unit test; the public integration test retains normal spinning.
Inferences: enforcing the existing diagnostic in the mode-owning runner closes the validation gap without changing shared fixtures or transports.
Unknowns: no unresolved implementation decisions.

### Acceptance Criteria
- The runner checks pointer, tv_sec and timespec sizes for both intended modes.
- Actual ARM execution passes on Rust MSRV and stable; wrong/missing layouts and test failures are rejected.
- Every existing AI review thread receives an evidence-based reply and is resolved.

## Analysis

Sources checked: current/paused SOW-0015/0021/0027 and pending SOW-0031/0032/0035/0037; completed SOW-0036 and SOW-0038; empty local specs directory; project vendoring skill; docs/code-organization.md; docs/level1-posix-shm.md; Rust public fixture and ABI runner; C ABI runner and Go timeout construction.
Current state: no active overlapping execution. This is extra validation, not a failed previously proven timeout repair, so no regression reopening is required.
Risks: parsing a diagnostic introduces a small coupling; exact whole-line matching and negative cases make it explicit.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model: mode selection is not independently checked against compiled field sizes. Both runs could silently test one ABI if a dependency changes its configuration behavior.
Evidence reviewed: public fixture prints pointer width, actual tv_sec size (labelled time_t), and timespec size; libc build.rs tracks the mode environment; real ARM runs show 4/4/8 and 4/8/16.
Affected contracts and surfaces: standalone test runner and validation guidance only; no C/Rust/Go API, protocol or production transport changes.
Existing patterns to reuse: low-priority helper, mktemp/EXIT cleanup, pipefail and the existing public-fixture diagnostic.
Risk and blast radius: runner failure behavior only; preserve cargo exit status with pipefail and stream output with tee.
Sensitive data handling plan: public code paths, commit IDs and sanitized test summaries only in SOWs, specs, docs, project skills, instructions and code comments; no credentials, identities, endpoints or raw private logs.
Implementation plan: capture each invocation into one temporary log; require the exact expected ABI line per mode; explain asserted layouts in public docs and integrator guidance.
Validation plan: execute ARM time32/time64 with MSRV 1.91.0 and stable; inject wrong, missing and failing command outputs using a temporary command harness; ShellCheck, bash syntax, diff check and SOW audit.
Artifact impact plan:
- AGENTS.md: no workflow change.
- Runtime project skills: no vendoring occurs; source-owned runner only.
- Specs: validation section of authoritative SHM doc changes, no duplicate local spec.
- End-user/operator docs: explain layout enforcement.
- End-user/operator skills: mention asserted compiled layout in integrator validation guidance.
- SOW lifecycle: new validation-hardening SOW, completed with its implementation in one commit; existing work remains paused/pending.
Open-source reference evidence: rust-lang/libc @ 42620ffc4109dc32e02f1cae9e63a3f4311b4b71, src/unix/linux_like/linux/gnu/b32/riscv32/mod.rs:667 and src/unix/linux_like/linux/musl/b32/riscv32/mod.rs:644 confirm distinct syscall constants.
Open decisions: none; user authorized fixes, explanations and thread resolution.

## Implications And Decisions

Keep the shared fixture unchanged and assert its actual compiled diagnostic in the runner that owns mode selection. No downstream copy is required. The two false-positive findings receive source-linked explanations.

## Plan

1. Explain and resolve the two inaccurate findings.
2. Implement, validate, document and commit the layout gate.
3. Push, reply with the fix and resolve the remaining thread; refresh all selected comments.

## Execution Log

### 2026-09-27

- Verified both false positives and replied/resolved them individually using the Netdata project PR review workflow.
- Implementation gate recorded before edits. Added runner layout enforcement and synchronized validation documentation.
- Source pre-push CI: 33 success, 3 configured skips, 1 neutral, no failures/running checks. Netdata retains one pre-compilation ARM container exec-format infrastructure failure and three running checks; no new downstream changes.

## Validation

Acceptance criteria evidence:
- Runner asserts the expected pointer/seconds/timespec representation independently of libc configuration.
- All three original threads received substantive replies and resolveReviewThread returned true individually: plugin-ipc PR17 discussions 4115703293 and 4115703296; Netdata PR24049 discussion 4115730229.

Tests or equivalent validation:
- Actual ARM/QEMU runner passes time32 (4/4/8 bytes) and time64 (4/8/16) under Rust 1.91.0 and stable 1.98.1. Installed the missing MSRV ARM standard library before rerunning.
- Temporary negative command harness: old runner accepts duplicated legacy ABI; new runner rejects it and missing ABI output with exit 1; preserves Cargo exit 7; correct modes pass and temporary logs are removed in every case.
- ShellCheck, bash syntax and git diff --check pass. SOW audit run before commit.

Real-use evidence:
- Production Rust SHM receives in the public fixture wait at least 100 ms and 1100 ms on both actual emulated ABIs and wake on delayed messages for finite/infinite/max budgets.

Reviewer findings:
- Qodo ABI assertion request implemented in the mode-owning runner, avoiding changes to the shared fixture.
- Qodo GNU riscv32 claim rejected against pinned upstream constants: claimed time64 alias exists only for musl, which the branch handles. No full GNU riscv32 runtime support is asserted.
- Qodo spin-test claim rejected: normal public construction is already covered, while the unit test deliberately isolates blocking; cited Go collector skill is outside Rust transport scope.
- Direct self-review of the runner/docs working diff is sufficient: no production behavior changes or material unresolved interactions; negative tests cover failure propagation, stale output and cleanup. No repeated external review is needed for this scoped safeguard.

Same-failure scan:
- Searched ABI diagnostics/mode switches in tests, Rust integration tests and Runtime Safety CI. This is the single Rust mode-owning runner. C time64 CI already uses NIPC_TEST_REQUIRE_TIME64_32 static assertions; no matching missing check in the selected scope.

Sensitive data gate:
- Durable changes contain public paths, ABI sizes and sanitized validation evidence only; no credentials, identities, private endpoints or incident logs.

Artifact maintenance gate:
- AGENTS.md: unchanged because responsibilities/workflow are unchanged.
- Runtime project skills: unchanged; no Netdata vendoring or new integration procedure.
- Specs: authoritative docs/level1-posix-shm.md validation section updated; empty local specs directory needs no duplicate of public guidance.
- End-user/operator docs: updated the expected ABI and failure behavior.
- End-user/operator skills: docs/netipc-integrator-skill.md now requires both layout and behavior checks.
- SOW lifecycle: completed file moved to done and committed with its runner/docs changes; paused/pending SOWs untouched.

Specs update: validation contract clarified in public SHM documentation; runtime protocol unchanged.
Project skills update: vendoring gate unaffected because runner/docs changes do not alter Netdata's vendored sources.
End-user/operator docs update: expected representations and rejection behavior documented.
End-user/operator skills update: compiled-layout verification added to integration checks.
Lessons: do not infer ABI coverage solely from a configuration toggle; verify the actual compiled representation.
Follow-up mapping: layout check implemented; two inaccurate findings rejected with evidence; independent 32-bit build coverage remains tracked in SOW-0037. No new deferred implementation.

## Outcome

Rust cross-ABI validation now fails when the selected layout was not actually compiled. All three original AI findings have been addressed and resolved; remote CI on the new commit is not claimed here.

## Lessons Extracted

Cross-ABI test configuration must be checked against the representation actually compiled.

## Followup

No additional source transport work is included. Existing independent 32-bit build gaps remain in SOW-0037.

## Regression Log

No regression of a previously proven timeout result; this adds an explicit validation safeguard.

