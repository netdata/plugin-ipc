# SOW-0002 - Root TODO Classification

## Status

Status: open

Sub-state: tracks existing root TODO files for future classification or migration.

## Requirements

### Purpose

Preserve existing root TODO project memory while moving future work into the SOW system intentionally, without losing active investigation context.

### User Request

The user requested SOW basics only for this pass and did not request root TODO migration.

### Assistant Understanding

Facts:

- Multiple `TODO*.md` files exist at the project root.
- `TODO-perf-parity.md` contains Windows performance-parity investigation notes and experiment results.
- The bootstrap pass must not move, delete, or rewrite existing project memory without explicit approval.

Inferences:

- Some TODO files likely belong in focused future SOWs, while others may be historical notes or reference material.

Unknowns:

- Which TODO files should be converted into active SOWs, preserved as root notes, split into several SOWs, or removed as obsolete.

### Acceptance Criteria

- Root TODO files are reviewed with user approval.
- Each TODO file is either migrated into SOWs, explicitly preserved in place, or rejected/removed only with explicit approval.
- No content is lost.

## Analysis

Sources checked:

- `TODO-perf-parity.md`

Current state:

- Root TODO files present at bootstrap:
  - `TODO-coverity-findings.md`
  - `TODO-fit-for-purpose-validation.md`
  - `TODO-integrator-skill.md`
  - `TODO-netdata-master-backport.md`
  - `TODO-netdata-plugin-ipc-integration.md`
  - `TODO-pending-from-rewrite.md`
  - `TODO-perf-parity.md`
  - `TODO-plugin-ipc.history.md`
  - `TODO-unified-l2-l3-api.md`

Risks:

- Mechanical migration could flatten analysis context or create an oversized implementation SOW.
- Leaving the file untracked could allow active performance work to be forgotten.

## Implications And Decisions

Pending user decision when this SOW is executed.

## Plan

1. Read root TODO files fully.
2. Classify each file as active work, historical notes, reference material, or obsolete candidate.
3. Present options with evidence before moving or rewriting anything.
4. Convert approved active work into focused pending/current SOWs.
5. Preserve or remove old root files only with explicit user approval.

## Execution Log

Not started.

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

Artifact maintenance gate:

- AGENTS.md: pending.
- Runtime project skills: pending.
- Specs: pending.
- End-user/operator docs: pending.
- End-user/operator skills: pending.
- SOW lifecycle: pending.

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
