# SOW-0035 - Coordinated Rust 2024 Style Migration

## Status

Status: open

Sub-state: Tracked follow-up to SOW-0034; no implementation may begin while SOW-0034 is active.

## Requirements

### Purpose

Migrate plugin-ipc and Netdata's corresponding vendored NetIPC Rust source to
the Rust 2024 style edition without mixing formatting churn into the semantic
edition migration.

### User Request

When rustfmt 1.97.1 exposed a repository-wide style migration during SOW-0034,
the user selected a focused semantic migration first and a separately tracked,
coordinated formatting migration afterward.

### Assistant Understanding

Facts:

- Rust language and style editions can be configured independently.
- With language edition 2024 and no style override, rustfmt 1.97.1 proposes 98
  formatting hunks across 40 standalone files.
- Thirty-six matching Netdata vendored source files also fail the same
  rustfmt-2024 check.
- SOW-0034 preserves style edition 2021 explicitly to avoid obscuring semantic
  review and creating unplanned vendor drift.

Inferences:

- A coordinated formatting-only change can be reviewed and validated more
  reliably than a mixed semantic-and-formatting diff.
- Netdata vendoring preflight is mandatory because this follow-up intentionally
  changes vendored source formatting.

Unknowns:

- The source and Netdata baseline commits at the time this SOW is activated.
- Whether later rustfmt releases or intervening source changes alter the exact
  formatting diff.

### Acceptance Criteria

- Plugin-ipc selects Rust 2024 style explicitly and all Rust targets pass
  formatting checks.
- The formatting diff is behavior-neutral and separated from semantic changes.
- Netdata vendoring preflight passes and its vendored NetIPC source uses the
  same formatting without overwriting Netdata-only files.
- Plugin-ipc and Netdata validation pass on their supported POSIX and Windows
  paths.

## Analysis

Sources checked:

- `src/crates/netipc/Cargo.toml`
- Rustfmt 1.97.1 formatting diagnostics from SOW-0034
- `.agents/skills/project-netdata-vendoring/SKILL.md`
- Netdata's vendored `src/crates/netipc/` source

Current state:

- SOW-0034 is the only executing SOW and explicitly preserves style edition
  2021.
- This formatting migration is intentionally queued until the semantic Rust
  2024 migration completes.

Risks:

- A large formatting diff can hide accidental semantic edits.
- Updating plugin-ipc alone would create avoidable downstream vendor drift.
- Netdata CI/toolchain changes between SOWs may change the expected formatting.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Rustfmt infers style edition 2024 from language edition 2024. That is a
  distinct, broad formatting migration affecting standalone and vendored
  source, so it must be coordinated separately from SOW-0034.

Evidence reviewed:

- SOW-0034 recorded 98 proposed hunks across 40 standalone files.
- A read-only Netdata check recorded 36 vendored source files with proposed
  Rust 2024 formatting.

Affected contracts and surfaces:

- Rust source formatting, contributor workflow, formatting CI, vendor parity,
  and Netdata's vendored NetIPC tree.

Existing patterns to reuse:

- Project-local low-priority validation commands.
- The mandatory Netdata vendoring preflight and normalized vendor-diff scripts.

Risk and blast radius:

- Medium review blast radius across most Rust source files; intended runtime
  behavior impact is none.

Sensitive data handling plan:

- Only public source, formatting output, public repository history, and
  sanitized validation summaries are required. Durable artifacts will contain
  no secrets, credentials, customer data, personal data, or private endpoints.

Implementation plan:

1. Reconstruct the current source-to-Netdata baseline and complete the mandatory
   CI/scanner and two-way vendoring preflight.
2. Update the style-edition configuration and apply rustfmt 2024 mechanically.
3. Prove the diff contains no semantic changes, synchronize the approved
   formatting into Netdata, and validate both repositories.

Validation plan:

- Rustfmt 2024 checks in both repositories.
- Token/AST-aware or equivalent review proving formatting-only changes.
- POSIX and native Windows Rust builds/tests.
- Normalized pre/post vendor diff and project SOW audits.

Artifact impact plan:

- AGENTS.md: inspect formatting and vendoring commands.
- Runtime project skills: update vendoring guidance only if reusable workflow
  knowledge changes.
- Specs: no product behavior change expected; record evidence at close.
- End-user/operator docs: no runtime contract change expected; inspect build
  guidance.
- End-user/operator skills: inspect only if formatting commands are exposed.
- SOW lifecycle: remain pending until SOW-0034 completes; complete independently.

Open-source reference evidence:

- Netdata baseline commit and relative paths must be refreshed when this SOW is
  activated.

Open decisions:

- Blocked until SOW-0034 completes and current baseline/CI/scanner evidence is
  reconstructed.

## Implications And Decisions

1. **Ordering - selected by the user: semantic migration before formatting.**
   - Keep SOW-0034 focused and coordinate style changes in this SOW afterward.

## Plan

1. Wait for SOW-0034 to complete.
2. Refresh the pre-implementation and Netdata vendoring gates before any edit.
3. Apply, review, validate, and commit the formatting migration independently.

## Execution Log

### 2026-07-28

- Created as the real tracked follow-up required by the user's SOW-0034
  formatting-scope decision.
- No implementation started.

## Validation

Acceptance criteria evidence:

- Pending activation.

Tests or equivalent validation:

- Preliminary read-only rustfmt evidence is recorded above.

Real-use evidence:

- Pending activation.

Reviewer findings:

- Pending activation.

Same-failure scan:

- Pending activation.

Sensitive data gate:

- This pending SOW contains only public source paths and sanitized formatting
  summaries.

Artifact maintenance gate:

- AGENTS.md: pending activation.
- Runtime project skills: pending activation.
- Specs: pending activation.
- End-user/operator docs: pending activation.
- End-user/operator skills: pending activation.
- SOW lifecycle: correctly `open` under `pending/`.

Specs update:

- Pending activation.

Project skills update:

- Pending activation.

End-user/operator docs update:

- Pending activation.

End-user/operator skills update:

- Pending activation.

Lessons:

- Pending implementation.

Follow-up mapping:

- Tracked by this SOW.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
