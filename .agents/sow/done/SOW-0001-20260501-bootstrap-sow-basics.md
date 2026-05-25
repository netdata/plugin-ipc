# SOW-0001 - Bootstrap SOW Basics

## Status

Status: completed

Sub-state: project-local SOW basics installed without generated project skills.

## Requirements

### Purpose

Install a self-contained SOW framework for this repository so future non-trivial work preserves project memory, uses explicit SOW tracking, validates behavior, and keeps durable artifacts current.

### User Request

The user requested SOW basics for this repository but explicitly requested not to create `project-*` skills during this pass. Project skills should grow incrementally later.

### Assistant Understanding

Facts:

- `README.md` identifies the repository as a cross-language IPC library for Netdata plugins and helper services.
- `docs/README.md` says the specs under `docs/` are authoritative and implementation must align with them.
- The repository has C, Rust, and Go implementations plus POSIX and Windows validation scripts.
- No pre-existing `AGENTS.md` was found.
- Multiple `TODO*.md` files exist at the project root; one contains current Windows performance-parity investigation notes.

Inferences:

- This is an existing stable/active project, not an empty project.
- The bootstrap should install SOW mechanics and preserve existing docs/TODO memory, but not invent runtime project skills.

Unknowns:

- None blocking for SOW basics.

### Acceptance Criteria

- `AGENTS.md` exists with the initialized marker and preserves project facts from README/docs.
- `.agents/sow/{specs,pending,current,done}/` exists with placeholders where needed.
- `.agents/sow/SOW.template.md` and `.agents/sow/audit.sh` are project-local copies.
- No generated `project-*` skill is created.
- Existing root TODO memory is preserved and tracked by a real SOW.
- Local audit runs without untracked SOW bootstrap defects.

## Analysis

Sources checked:

- `README.md`
- `docs/README.md`
- `docs/code-organization.md`
- `Makefile`
- `CMakeLists.txt`
- `TODO-perf-parity.md`
- repository file scan for existing agent instruction files

Current state:

- No existing `AGENTS.md` or tool-specific agent instruction file was found.
- Specs and public docs already exist under `docs/`.
- Root TODO files exist and should not be moved or deleted without user approval.

Risks:

- Creating generic project skills now would add low-value noise and contradict the user's instruction.
- Moving root TODO files during bootstrap could lose context or disrupt active work.

## Implications And Decisions

1. Runtime project skills
   - Selected: do not create `project-*` skills during this pass.
   - Reason: the user explicitly requested incremental skill creation, and generic bootstrap-generated skills are a known failure mode.

2. Root TODO handling
   - Selected: preserve `TODO-perf-parity.md` in place and track future classification with a pending SOW.
   - Reason: the file is existing project memory and should not be moved or rewritten without explicit approval.

## Plan

1. Install project-local SOW directories, template, audit script, and placeholders.
2. Create `AGENTS.md` with SOW runtime rules and project-specific facts.
3. Create a pending SOW for root TODO classification/migration.
4. Run the local audit and `git diff --check`.
5. Close this bootstrap SOW if validation passes.

## Execution Log

### 2026-05-01

- Created `.agents/sow/{specs,pending,current,done}/`.
- Copied the SOW template to `.agents/sow/SOW.template.md`.
- Copied the audit script to `.agents/sow/audit.sh`.
- Created `AGENTS.md`.
- Created pending `SOW-0002-20260501-root-todo-classification.md`.

## Validation

Acceptance criteria evidence:

- `AGENTS.md` exists with `Project SOW status: initialized`.
- `.agents/sow/{specs,pending,current,done}/` exists.
- `.agents/sow/SOW.template.md` and `.agents/sow/audit.sh` exist.
- `.agents/skills/project-*` was not created.
- Root TODO files remain in place and are tracked by pending `SOW-0002-20260501-root-todo-classification.md`.

Tests or equivalent validation:

- `bash .agents/sow/audit.sh` reports `SOW initialization complete and clean`.
- `git diff --check` passes.

Real-use evidence:

- The project-local audit script was executed from the repository root and validated marker, canonical sections, SOW directories, local framework files, status/directory consistency, skill state, and root TODO tracking.

Reviewer findings:

- No external reviewer used for this bootstrap-only SOW.

Same-failure scan:

- Existing agent instruction scan found no pre-existing `AGENTS.md` or tool-specific instruction file to merge.

Artifact maintenance gate:

- AGENTS.md: updated `AGENTS.md` with project-local SOW runtime rules.
- Runtime project skills: no update; user explicitly requested no bootstrap-created `project-*` skills.
- Specs: no update; this SOW changes workflow only, not product/protocol behavior.
- End-user/operator docs: no update; public docs behavior and examples were not changed.
- End-user/operator skills: no update; `docs/netipc-integrator-skill.md` was indexed but not changed.
- SOW lifecycle: bootstrap SOW and pending root TODO SOW created; status/directory consistency passed local audit.

Specs update:

- No spec update needed because product behavior and protocol contracts did not change.

Project skills update:

- No runtime project skill update; user requested incremental creation later.

End-user/operator docs update:

- No end-user/operator docs update; bootstrap changed assistant workflow only.

End-user/operator skills update:

- No output/reference skill update; integration guidance behavior did not change.

Lessons:

- Project skill creation can be intentionally deferred when the user wants incremental skills and there is no need to create a runtime checklist during bootstrap.

Follow-up mapping:

- Root TODO classification/migration is tracked by pending `SOW-0002-20260501-root-todo-classification.md`.

## Outcome

Completed. Project-local SOW basics are installed and self-contained.

## Lessons Extracted

Do not create placeholder `project-*` skills just to satisfy bootstrap structure. If project skills are intentionally incremental, record that decision and rely on future SOW retrospection to create them when there is concrete reusable knowledge.

## Followup

- Pending root TODO classification/migration SOW created.
