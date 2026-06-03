# SOW-0002 - Root TODO Classification

## Status

Status: completed

Sub-state: root TODO history was archived under `.agents/sow/todo-history/`.

## Requirements

### Purpose

Preserve existing root TODO project memory while moving future work into the SOW system intentionally, without losing active investigation context.

### User Request

The user requested moving all root `TODO-*` files into `.agents/sow/todo-history/`.

### Assistant Understanding

Facts:

- Multiple `TODO-*` files exist at the project root.
- `TODO-perf-parity.md` contains Windows performance-parity investigation notes and experiment results.
- The bootstrap pass did not move, delete, or rewrite existing project memory without explicit approval.
- The user has now explicitly approved moving all root `TODO-*` files into `.agents/sow/todo-history/`.
- One matching file, `TODO-perf-parity.md`, is untracked but is still covered by the user's "all TODO-*" request.
- Sensitive-data scanning found old TODO content with the user's personal name and workstation-specific paths, which durable artifacts must not carry.

Inferences:

- The approved archive move preserves the TODO history without forcing classification into separate implementation SOWs in this pass.
- Personal and workstation-specific strings can be mechanically redacted without changing technical intent.

Unknowns:

- No unresolved product decision remains for this pass.

### Acceptance Criteria

- All root `TODO-*` files are moved into `.agents/sow/todo-history/` with content preserved except required personal/workstation redactions.
- No root `TODO-*` files remain.
- The archive location is recorded in project instructions.
- No content is lost.

## Analysis

Sources checked:

- `TODO-perf-parity.md`
- `find . -maxdepth 1 -type f -name 'TODO-*'`
- `git ls-files -- 'TODO-*'`
- Sensitive-data scan over root `TODO-*` files, `AGENTS.md`, and this SOW.

Current state:

- Root TODO files present before execution:
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

- Leaving root TODO files in place keeps old project memory outside the SOW area the user requested.
- Moving without updating `AGENTS.md` would leave stale instructions that say the root TODO files are preserved in place.
- Committing unredacted TODO history would preserve personal/workstation strings in durable artifacts.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Root `TODO-*` files are legacy project memory from before the SOW archive became authoritative for this work.
- The repository already has this SOW to track their classification or migration.
- The user has now selected the archive outcome: move all root `TODO-*` files to `.agents/sow/todo-history/`.
- Evidence: the root has nine matching files, eight are tracked and `TODO-perf-parity.md` is untracked.

Evidence reviewed:

- `.agents/sow/current/SOW-0002-20260501-root-todo-classification.md`
- `AGENTS.md`
- `find . -maxdepth 1 -type f -name 'TODO-*'`
- `git ls-files -- 'TODO-*'`
- Sensitive-data scan over root `TODO-*` files and this SOW.

Affected contracts and surfaces:

- Root repository file layout.
- `.agents/sow/todo-history/` archive content.
- `AGENTS.md` project-specific TODO preservation instructions.
- This SOW's lifecycle, status, validation, and closure.

Existing patterns to reuse:

- SOW status and directory consistency rules from `AGENTS.md`.
- Existing SOW lifecycle: move from `pending/` to `current/`, then to `done/` with `Status: completed`.
- Existing `.agents/sow/audit.sh` validation for root TODO files and SOW consistency.

Risk and blast radius:

- Low code/runtime blast radius because no build, source, protocol, test, or API files are changed.
- Moderate process blast radius because archived notes become committed SOW history and must avoid sensitive/personal data.
- The untracked `TODO-perf-parity.md` will become tracked if moved and committed; this is intentional because the user requested all root `TODO-*` files be moved.

Sensitive data handling plan:

- Do not read or commit `.env`.
- Do not write the user's personal name to repository artifacts.
- Mechanically redact old TODO references to the user's name and workstation paths before commit.
- Use generic placeholders such as `user`, `/home/user`, `/c/Users/user`, and `C:\msys64\home\user` where preserving path shape is useful.
- Re-run sensitive-data scans over the moved archive, `AGENTS.md`, and this SOW before commit.

Implementation plan:

1. Create `.agents/sow/todo-history/`.
2. Move every root `TODO-*` file into `.agents/sow/todo-history/`, including the untracked performance-parity note.
3. Redact personal/workstation strings in the archived TODO files.
4. Update `AGENTS.md` so project instructions point to the archived TODO location.
5. Complete and move this SOW to `.agents/sow/done/`.

Validation plan:

- Confirm `find . -maxdepth 1 -type f -name 'TODO-*'` returns no root TODO files.
- Confirm `.agents/sow/todo-history/` contains the nine expected `TODO-*` files.
- Run sensitive-data scan over the archived TODO files, `AGENTS.md`, and this SOW.
- Run `bash .agents/sow/audit.sh`.
- Run `git diff --check`.

Artifact impact plan:

- AGENTS.md: update root TODO preservation notes to reference `.agents/sow/todo-history/`.
- Runtime project skills: no runtime project skills exist, and this move does not create reusable workflow behavior.
- Specs: no protocol/API/transport/data-format behavior changes.
- End-user/operator docs: no end-user/operator surface changes.
- End-user/operator skills: no output/reference skill behavior changes.
- SOW lifecycle: move this SOW from pending to current, mark in-progress, then complete and move to done with the archive move.

Open-source reference evidence:

- Not relevant; this is repository housekeeping of existing local project notes.

Open decisions:

- Resolved: the user explicitly requested moving all root `TODO-*` files to `.agents/sow/todo-history/`.

## Implications And Decisions

1. Root TODO archive decision
   - Selected: move all root `TODO-*` files to `.agents/sow/todo-history/`.
   - Evidence: the user requested this exact move on 2026-06-03.
   - Implication: root TODO notes leave the repository root and become SOW-managed history.
   - Risk handled: old personal/workstation strings are redacted before commit.

## Plan

1. Move every root `TODO-*` file to `.agents/sow/todo-history/`.
2. Redact personal/workstation strings in the archived notes.
3. Update project instructions to reference the archive location.
4. Validate root cleanup, archive contents, sensitive-data hygiene, diff hygiene, and SOW audit.
5. Complete this SOW and commit the archive move, instruction update, and SOW close together.

## Execution Log

### 2026-06-03

- Moved all nine root `TODO-*` files into `.agents/sow/todo-history/`.
- Included `TODO-perf-parity.md`, which matched the user's request even though it was previously untracked.
- Mechanically redacted personal-name and workstation-username strings in the archived TODO notes.
- Updated `AGENTS.md` so project instructions point to `.agents/sow/todo-history/` instead of stale root TODO files.
- Completed this SOW for the archive move.

## Validation

Acceptance criteria evidence:

- `find . -maxdepth 1 -type f -name 'TODO-*'` returned zero root TODO files.
- `.agents/sow/todo-history/` contains the nine expected archived files:
  - `TODO-coverity-findings.md`
  - `TODO-fit-for-purpose-validation.md`
  - `TODO-integrator-skill.md`
  - `TODO-netdata-master-backport.md`
  - `TODO-netdata-plugin-ipc-integration.md`
  - `TODO-pending-from-rewrite.md`
  - `TODO-perf-parity.md`
  - `TODO-plugin-ipc.history.md`
  - `TODO-unified-l2-l3-api.md`
- `AGENTS.md` now identifies `.agents/sow/todo-history/` as the archived TODO history location.

Tests or equivalent validation:

- `git diff --check` passed.
- Sensitive-data scan over `AGENTS.md`, this SOW, and `.agents/sow/todo-history/` returned no matches for raw token assignment patterns or private-key markers.
- Personal/workstation string scan over `AGENTS.md`, this SOW, and `.agents/sow/todo-history/` returned no matches.

Real-use evidence:

- Local filesystem layout now matches the requested archive path.

Reviewer findings:

- No external reviewer was run because this is repository housekeeping only; no source, protocol, API, runtime, benchmark, or test behavior changed.

Same-failure scan:

- Root TODO scan returned zero matching files, so the stale root TODO pattern is removed.
- Personal/workstation string scan returned no matches in the changed durable artifacts.

Sensitive data gate:

- `.env` was not read or committed.
- No raw secrets, credential assignments, bearer tokens, private keys, personal-name strings, or workstation-username paths were found in the changed durable artifacts after redaction.

Artifact maintenance gate:

- AGENTS.md: updated to describe `.agents/sow/todo-history/` as archived TODO history.
- Runtime project skills: no runtime project skills exist, and no reusable repo workflow changed.
- Specs: not affected; this move does not change protocol, API, wire format, transport, or operational guarantees.
- End-user/operator docs: not affected; this move is internal repository housekeeping.
- End-user/operator skills: not affected; no public/operator workflow changed.
- SOW lifecycle: this SOW is marked `completed` and moved to `.agents/sow/done/` together with the archive move.

Specs update:

- Not needed; no product, protocol, API, transport, data-format, or operational behavior changed.

Project skills update:

- Not needed; no runtime project skill exists and no reusable working procedure was introduced.

End-user/operator docs update:

- Not needed; no end-user/operator documentation surface changed.

End-user/operator skills update:

- Not needed; no output/reference skill behavior changed.

Lessons:

- Old project memory may contain personal/workstation strings even when it is only being moved; archive moves still need sensitive-data scanning before commit.

Follow-up mapping:

- No deferred item remains in this SOW. Future classification or migration of archived notes requires a new explicit SOW.

## Outcome

All root `TODO-*` files were moved into `.agents/sow/todo-history/`, archived TODO content was sanitized for personal/workstation strings, and project instructions now point to the archive location.

## Lessons Extracted

- Treat archived notes as durable repository artifacts and scan them before committing.

## Followup

None.
