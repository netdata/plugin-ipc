# SOW-0027 - Netdata Vendor Memory-Safety Update

## Status

Status: open

Sub-state: not started; tracks propagation of SOW-0026 source fixes to the Netdata vendored copy.

## Requirements

### Purpose

Keep NetIPC memory-safety fixes source-owned in `plugin-ipc` while ensuring the Netdata vendored copy receives those fixes through the normal vendor/update path.

### User Request

The user asked to fix NetIPC library issues in `plugin-ipc`, not directly in the Netdata PR, because this repository is the source of truth.

### Assistant Understanding

Facts:

- SOW-0026 implements source fixes for NetIPC memory-safety scout findings in `plugin-ipc`.
- Netdata consumes NetIPC through a vendored copy.
- Directly patching Netdata's vendored NetIPC copy would create source-of-truth drift.

Inferences:

- After SOW-0026 lands, the next safe step is a focused vendor/update pass against the relevant Netdata checkout.

Unknowns:

- The exact Netdata checkout, branch, and PR target for propagation must be confirmed before implementation starts.

### Acceptance Criteria

- The selected Netdata checkout is confirmed before implementation.
- The vendored NetIPC copy is updated from `plugin-ipc` rather than manually patched.
- The project-local vendor diff/checker is run and its result is recorded.
- Netdata build or targeted tests covering the touched NetIPC integration paths are run or a blocker is recorded with evidence.
- No unrelated Netdata changes are included.

## Analysis

Sources checked:

- SOW-0026 source-ownership decision.
- `docs/netipc-integrator-skill.md` source-of-truth guidance.
- Prior vendor synchronization SOWs in `.agents/sow/done/`.

Current state:

- Source fixes are expected to land in `plugin-ipc` through SOW-0026 first.
- No Netdata checkout has been selected for this SOW yet.

Risks:

- Copying files manually can introduce import-path or layout mistakes.
- Updating the wrong Netdata checkout can create unrelated branch drift.
- Skipping the vendor diff can hide missing language-specific updates.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- NetIPC fixes must be propagated to the downstream Netdata vendored copy, but that work belongs in a separate focused pass after the source repository is committed and pushed.

Evidence reviewed:

- SOW-0026 records that NetIPC source ownership belongs to `plugin-ipc`.
- Historical vendor-sync SOWs use the project-local `diff-netdata-vendor.sh` checker.

Affected contracts and surfaces:

- Netdata vendored C, Rust, and Go NetIPC sources.
- Netdata build/test paths that consume NetIPC.
- Vendor synchronization evidence in this SOW.

Existing patterns to reuse:

- `diff-netdata-vendor.sh`
- Prior SOW-0003 and SOW-0008 vendor synchronization flow.

Risk and blast radius:

- Medium: changes land in a consumer repository and may affect Netdata build/test behavior.
- Keep scope limited to NetIPC vendor propagation and required validation.

Sensitive data handling plan:

- No secrets, customer data, credentials, production logs, or private endpoints are required.
- Evidence will use source paths, commands, commit hashes, and sanitized summaries only.

Implementation plan:

1. Confirm the target Netdata checkout and branch.
2. Propagate the committed `plugin-ipc` source changes through the normal vendor/update path.
3. Run the vendor diff/checker and targeted Netdata validation.
4. Commit only the vendor update and required tracking artifacts.

Validation plan:

- Run the vendor diff/checker against the selected Netdata checkout.
- Run targeted Netdata build/tests for touched C/Rust/Go NetIPC integration paths.
- Run same-failure searches for the SOW-0026 finding classes in the Netdata vendored copy.

Artifact impact plan:

- AGENTS.md: no expected update.
- Runtime project skills: no expected update.
- Specs: no expected update unless propagation exposes source/doc drift.
- End-user/operator docs: no expected update.
- End-user/operator skills: no expected update.
- SOW lifecycle: this SOW remains open until the user selects the Netdata checkout.

Open-source reference evidence:

- None checked yet; this SOW is a local vendor propagation task.

Open decisions:

1. Select the target Netdata checkout, branch, and PR context before implementation.

## Implications And Decisions

- No implementation decisions have been made yet.

## Plan

1. Confirm target checkout and branch.
2. Run vendor propagation.
3. Validate vendor parity and targeted Netdata behavior.
4. Commit and push when validated.

## Execution Log

### 2026-06-29

- Created as the tracked follow-up for SOW-0026 vendor propagation.
- No implementation started.

## Validation

Acceptance criteria evidence:

- Not started.

Tests or equivalent validation:

- Not started.

Real-use evidence:

- Not started.

Reviewer findings:

- Not started.

Same-failure scan:

- Not started.

Sensitive data gate:

- The SOW contains only source paths and workflow descriptions. No secrets or customer data are included.

Artifact maintenance gate:

- AGENTS.md: no update needed for this tracking SOW.
- Runtime project skills: none exist.
- Specs: no update needed until implementation changes behavior.
- End-user/operator docs: no update needed until implementation changes behavior.
- End-user/operator skills: no update needed until implementation changes behavior.
- SOW lifecycle: created as open in `.agents/sow/pending/`.

Specs update:

- Not started.

Project skills update:

- Not started.

End-user/operator docs update:

- Not started.

End-user/operator skills update:

- Not started.

Lessons:

- None yet.

Follow-up mapping:

- This SOW tracks the SOW-0026 Netdata vendor propagation item.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
