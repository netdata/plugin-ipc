# SOW-0028 - Netdata Vendor Preflight Policy

## Status

Status: completed

Sub-state: durable workflow instructions implemented and validated.

## Requirements

### Purpose

Prevent unsafe or unreviewed `plugin-ipc` source changes from being copied into Netdata's vendored NetIPC tree. The rule must protect Netdata by requiring source-repository CI and GitHub code/security scanner evidence before any Netdata vendoring work starts, even when the user does not ask for that check explicitly.

### User Request

The user asked to update `AGENTS.md` and related skills so assistants always check CI and GitHub code/security scanners before vendoring into Netdata, even if the user does not ask for it.

The user then added that, before vendoring to Netdata, assistants must also perform a two-way gap analysis:

1. what changed in NetIPC since the last time it was vendored;
2. what changed in Netdata's version of NetIPC since the last time it was vendored.

Only after understanding both directions and producing a migration plan can vendoring be done.

### Assistant Understanding

Facts:

- `plugin-ipc` is the source-of-truth repository for NetIPC.
- Netdata consumes NetIPC through a vendored copy.
- `SOW-0027` tracks a pending Netdata vendor propagation pass.
- The current `plugin-ipc` commit has failing CI/static/security checks and open GitHub code-scanning alerts, so blindly vendoring would be unsafe.
- `AGENTS.md` currently has no explicit Netdata vendoring preflight gate.
- No runtime `project-*` skill exists yet, and the project instructions allow adding one incrementally when concrete reusable workflow knowledge exists.
- `diff-netdata-vendor.sh` compares current source and current Netdata vendor trees, but it does not by itself explain both histories since the last vendoring event.

Inferences:

- A repo-level rule alone is helpful but easy to miss during task routing.
- A runtime project skill with a strong trigger is the right place for the operational HOW of vendoring checks.
- The output/reference integrator skill is related because it already tells downstream integrators not to edit vendored copies first.
- A two-way gap analysis must happen before copying because Netdata may have valid local vendored-source fixes that should be backported to `plugin-ipc` before source is recopied downstream.

Unknowns:

- None blocking. The requested policy is explicit.

### Acceptance Criteria

- `AGENTS.md` requires CI and GitHub code/security scanner checks before any Netdata vendoring starts.
- A runtime project skill exists for Netdata vendoring work and includes the mandatory preflight workflow.
- The runtime project skill requires identifying the last vendored baseline, analyzing both upstream and downstream drift since that baseline, and writing a migration plan before vendoring.
- `docs/netipc-integrator-skill.md` tells integrators to perform the same preflight before copying source changes into Netdata.
- Validation confirms the new instructions are discoverable and do not mention raw sensitive data.

## Analysis

Sources checked:

- `AGENTS.md`
- `docs/netipc-integrator-skill.md`
- `.agents/sow/pending/SOW-0027-20260629-netdata-vendor-memory-safety-update.md`
- `.agents/sow/current/SOW-0015-20260605-codacy-scope-and-maintainability.md`
- `.agents/sow/current/SOW-0021-20260613-netipc-at-scale.md`
- `vendor-to-netdata.sh`
- `diff-netdata-vendor.sh`
- global `sync-docs-specs-skills` skill instructions
- global `sync-docs-specs-skills` compliance model
- global `sync-docs-specs-skills` inventory script

Current state:

- `AGENTS.md` says runtime input project skills are none yet.
- `docs/netipc-integrator-skill.md` already states that new service work starts in `github.com/netdata/plugin-ipc` and should not begin by editing only a vendored copy.
- `SOW-0027` requires running the project-local vendor diff/checker but does not currently require source CI and scanner checks before vendoring starts.
- `vendor-to-netdata.sh` copies current source into Netdata while preserving Netdata-specific wrappers, Rust workspace files, and Netdata Go module ownership.
- `diff-netdata-vendor.sh` normalizes expected Netdata-local differences and reports current source/vendor drift; it is useful but insufficient as a historical two-way gap analysis.

Risks:

- Without a hard preflight, future assistants may vendor a commit with failing CI or open scanner findings.
- Without a two-way gap analysis, future assistants may overwrite valid Netdata-local vendored fixes or fail to backport them to `plugin-ipc`.
- If the new rule is too vague, assistants may treat a broad `git status` or local build as enough evidence.
- If the rule is too rigid, known false positives may block urgent work unnecessarily; the policy must allow documented triage and explicit user risk acceptance.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Netdata vendoring copies source-owned code into a downstream repository. If the source commit has failing CI, open code-scanning alerts, dependency advisories, or secret-scanning findings, copying it downstream spreads risk and makes later triage harder.
- The existing workflow checks current vendor drift but does not force source CI/scanner status checks before the copy begins.
- Current drift is not enough evidence. A safe migration needs the last vendored baseline plus both histories since that point, because either side may contain legitimate changes that need preservation or backporting.

Evidence reviewed:

- `AGENTS.md` project-specific commands list build/test/vendor checks but no CI/security preflight for Netdata vendoring.
- `docs/netipc-integrator-skill.md` source ownership section tells integrators not to edit only the vendored copy, but it does not yet require checking source CI and scanners before vendoring.
- Current GitHub evidence for commit `b4dfe405e1f99be417c21a9c0478aba2f64facaa` includes failing Static Analysis, Supply Chain Security, and Codacy Local Analysis workflows, plus open code-scanning alerts.

Affected contracts and surfaces:

- `AGENTS.md` repository workflow and project-skill index.
- Runtime project skill discovery under `.agents/skills/project-*`.
- `docs/netipc-integrator-skill.md` output/reference skill guidance.
- Future Netdata vendoring SOWs, including `SOW-0027`.

Existing patterns to reuse:

- Runtime project skills live under `.agents/skills/project-*/SKILL.md`.
- `AGENTS.md` already distinguishes runtime input skills from output/reference skills.
- Vendor synchronization already uses `diff-netdata-vendor.sh`.
- `vendor-to-netdata.sh` is the current copy tool and preserves Netdata-only wrapper and workspace surfaces by design.
- GitHub evidence can be gathered through `gh run list`, `gh api` check-runs/status/code-scanning/dependabot/secret-scanning endpoints, and workflow logs.

Risk and blast radius:

- Low runtime risk: this is a process/documentation change only.
- Medium workflow impact: future Netdata vendoring is blocked until source CI/scanner status is known and handled, the last vendored baseline is identified, both directions are analyzed, and the migration plan is written.
- Positive security impact: avoids copying untriaged source alerts into Netdata.

Sensitive data handling plan:

- No secrets, credentials, customer data, personal data, private endpoints, or proprietary incident details are required.
- Durable artifacts will mention only repository names, command names, file paths, workflow categories, and sanitized policy language.
- GitHub token values and scanner raw logs will not be written to durable artifacts.

Implementation plan:

1. Add a runtime project skill for Netdata vendoring preflight.
2. Update `AGENTS.md` to require the preflight and index the runtime skill.
3. Update `docs/netipc-integrator-skill.md` with the same source-to-Netdata preflight gate.
4. Include the last-vendored-baseline and two-way gap-analysis requirement in all three durable instruction surfaces.
5. Validate discoverability, SOW audit, and sensitive-data hygiene.

Validation plan:

- Run `rg` to verify the policy appears in `AGENTS.md`, the runtime project skill, and the integrator skill.
- Run `rg` to verify the two-way gap-analysis rule appears in the same durable instruction surfaces.
- Run the docs/specs/skills inventory/audit scope after repair.
- Run `bash .agents/sow/audit.sh`.
- Run `git diff --check`.
- Check durable artifacts for raw secrets or personal data.

Artifact impact plan:

- AGENTS.md: update required.
- Runtime project skills: add `.agents/skills/project-netdata-vendoring/SKILL.md`.
- Specs: no update expected because this changes workflow, not protocol/API behavior.
- End-user/operator docs: update `docs/netipc-integrator-skill.md` because it is the relevant output/reference skill for NetIPC integration workflows.
- End-user/operator skills: update `docs/netipc-integrator-skill.md`.
- SOW lifecycle: create and complete this SOW with the process change.

Open-source reference evidence:

- None. This is repository-local workflow policy, not an implementation pattern that benefits from external source comparison.

Open decisions:

- Resolved by user request: always perform CI and GitHub code/security scanner checks before Netdata vendoring, even when not explicitly requested.

## Implications And Decisions

1. Mandatory Netdata vendoring preflight

- Decision: before any assistant modifies a Netdata checkout or copies vendored NetIPC files, it must check source `plugin-ipc` CI and GitHub code/security scanners for the candidate commit.
- Implication: vendoring can pause even when the user only asked to "vendor", "copy", "sync", "push", or "merge" the NetIPC files.
- Risk: known false positives may block until triaged.
- Mitigation: the active SOW can record evidence-backed false-positive triage or explicit user risk acceptance.

2. Mandatory two-way gap analysis

- Decision: before vendoring, assistants must identify the last vendored baseline, analyze upstream `plugin-ipc` changes since that baseline, analyze Netdata vendored-copy changes since that baseline, and write a migration plan.
- Implication: current `diff-netdata-vendor.sh` output is not enough by itself; assistants must explain how both trees reached the current drift state.
- Risk: baseline discovery may be ambiguous if prior vendoring commits did not record the source commit.
- Mitigation: stop before vendoring and record a baseline reconstruction plan when the baseline cannot be identified with evidence.

3. Runtime project skill

- Decision: add the first runtime project skill because this is concrete reusable HOW-to-work-here knowledge.
- Implication: future assistants must load the skill whenever Netdata vendoring is in scope.
- Risk: another instruction location may drift.
- Mitigation: `AGENTS.md` indexes the skill and `docs/netipc-integrator-skill.md` mirrors the gate.

## Plan

1. Add the runtime project skill.
2. Update `AGENTS.md`.
3. Update the integrator skill.
4. Validate and close the SOW.

## Execution Log

### 2026-07-01

- Created this SOW for the requested process change.

## Validation

Acceptance criteria evidence:

- `AGENTS.md` now indexes `.agents/skills/project-netdata-vendoring/SKILL.md` and requires its preflight before Netdata vendoring.
- `.agents/skills/project-netdata-vendoring/SKILL.md` defines the mandatory source CI, GitHub code-scanning, Dependabot, secret-scanning, last-baseline, two-way gap-analysis, and migration-plan checks.
- `docs/netipc-integrator-skill.md` now includes a Netdata vendoring preflight step, two-way gap-analysis requirement, migration-plan rules, and checklist item.

Tests or equivalent validation:

- `rg -n "project-netdata-vendoring|Netdata vendoring preflight|code/security scanner|secret-scanning|Dependabot|diff-netdata-vendor|vendor-to-netdata" AGENTS.md docs/netipc-integrator-skill.md .agents/skills/project-netdata-vendoring/SKILL.md .agents/sow/current/SOW-0028-20260701-netdata-vendor-preflight-policy.md` found the expected policy references.
- `rg -n "two-way|baseline|migration plan|changed in Netdata|changed in.*plugin-ipc|changed in NetIPC" AGENTS.md docs/netipc-integrator-skill.md .agents/skills/project-netdata-vendoring/SKILL.md .agents/sow/current/SOW-0028-20260701-netdata-vendor-preflight-policy.md` found the expected gap-analysis references.
- `bash .agents/sow/audit.sh` passed.
- `git diff --check -- AGENTS.md docs/netipc-integrator-skill.md .agents/skills/project-netdata-vendoring/SKILL.md .agents/sow/current/SOW-0028-20260701-netdata-vendor-preflight-policy.md` passed.
- After SOW close, `bash .agents/sow/audit.sh` passed with this SOW in `.agents/sow/done/`.
- After SOW close, `git diff --check -- AGENTS.md docs/netipc-integrator-skill.md .agents/skills/project-netdata-vendoring/SKILL.md .agents/sow/done/SOW-0028-20260701-netdata-vendor-preflight-policy.md` passed.
- The docs/specs/skills inventory shows the new runtime project skill under `.agents/skills/project-netdata-vendoring/SKILL.md`.

Real-use evidence:

- This is a durable workflow-policy update. Real-use evidence is the new runtime skill trigger plus `AGENTS.md` rule that future Netdata vendoring work must load it before modifying the Netdata checkout.

Reviewer findings:

- External reviewers were not run because the user did not request external reviewers and this is a docs/process-only change.

Same-failure scan:

- `rg` confirmed the preflight and two-way gap-analysis rules appear in `AGENTS.md`, the new runtime project skill, the integrator skill, and this SOW.

Sensitive data gate:

- Durable artifacts contain no raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary incident details.
- The SOW avoids workstation absolute paths and uses generic references to global skill instructions.
- Final scan for personal-name, absolute-home-path, and token patterns across changed durable artifacts returned no matches.

Artifact maintenance gate:

- AGENTS.md: updated with the mandatory Netdata vendoring preflight and runtime skill index entry.
- Runtime project skills: added `.agents/skills/project-netdata-vendoring/SKILL.md`.
- Specs: no update needed because no protocol behavior, public API contract, wire format, transport behavior, data format, or operational guarantee changed.
- End-user/operator docs: no separate public docs update needed because this is an assistant/operator workflow rule, not a user-facing NetIPC API change.
- End-user/operator skills: updated `docs/netipc-integrator-skill.md`, the relevant output/reference skill for Netdata integration workflows.
- SOW lifecycle: completed and moved to `.agents/sow/done/` with the policy changes.

Specs update:

- No spec update needed. This SOW changes HOW assistants work before Netdata vendoring, not WHAT NetIPC does.

Project skills update:

- Added `.agents/skills/project-netdata-vendoring/SKILL.md`.

End-user/operator docs update:

- No separate end-user/operator docs were affected; public NetIPC behavior and integration API did not change.

End-user/operator skills update:

- Updated `docs/netipc-integrator-skill.md`.

Lessons:

- Vendor-drift checks are not enough. Source CI/scanner status and two-way historical drift analysis must be separate preflight gates before copying code downstream.

Follow-up mapping:

- No follow-up remains. `SOW-0027` will consume this policy when the actual Netdata vendor propagation work starts.

## Outcome

Completed.

- Added a runtime Netdata vendoring skill.
- Updated `AGENTS.md` so future Netdata vendoring work must load the skill and complete the CI/scanner, two-way gap-analysis, and migration-plan gates before touching Netdata's vendored copy.
- Updated the integrator skill so downstream NetIPC integration guidance carries the same gate.
- No follow-up remains for this process change.

## Lessons Extracted

- Netdata vendoring needs three distinct gates: source safety preflight, two-way source/downstream gap analysis with a migration plan, then vendor diff and downstream validation after copying.

## Followup

None.

## Regression Log

None yet.
