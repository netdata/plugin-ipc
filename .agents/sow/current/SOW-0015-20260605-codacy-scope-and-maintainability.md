# SOW-0015 - Codacy Scope And Maintainability

## Status

Status: in-progress

Sub-state: Codacy test/bench exclusion and refreshed production-source maintainability baseline in progress.

## Requirements

### Purpose

Keep Codacy complexity and duplication signals useful for production SDK hygiene by excluding test/benchmark code from Codacy maintainability metrics, then continue improving real production files one file at a time without hiding useful rules.

### User Request

The user approved proceeding after the assistant reported that test/bench exclusion was not properly completed and recommended committing/importing Codacy global excludes for `tests/**` and `bench/**`, reanalyzing Codacy, then continuing file-by-file on remaining production files.

### Assistant Understanding

Facts:

- Codacy Cloud currently reports 0 issues, 88% coverage, 42% complex files, and 41% duplicated files for commit `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- `.codacy/codacy.config.json` currently has top-level `"exclude": []`, so test and benchmark paths are not globally excluded in the committed local Codacy configuration.
- The installed Codacy Cloud CLI skill states that committing `.codacy/codacy.config.json` does not by itself change Codacy Cloud; `codacy tools ... --import` is required.
- Codacy's official Cloud configuration file documentation says Cloud path ignores come from a repository-root `.codacy.yml` or `.codacy.yaml` file with `exclude_paths`, not from `.codacy/codacy.config.json`.
- `SOW-0013` recorded the decision to keep complexity and duplication metrics active and fix real source hotspots.
- `SOW-0014` already implemented substantial production-source organization work, including C protocol splits, Rust/Go lookup codec splits, Rust raw service splits, and C service splits.

Inferences:

- Excluding test and benchmark paths should make Codacy maintainability percentages closer to the production SDK surface the user wants to improve.
- Remaining production-source complexity and duplication should be selected from fresh Codacy data after the exclusion is applied and reanalysis completes.

Unknowns:

- Which production files remain above Codacy's complexity and duplication goals after Codacy reanalyzes with `tests/**` and `bench/**` excluded.
- Whether Codacy Cloud reports file-level metric contributors through the CLI; if not, local Lizard/JSCPD approximations will be used for the next file decision.

### Acceptance Criteria

- `.codacy/codacy.config.json` excludes `tests/**` and `bench/**` at global scope.
- The Codacy configuration is imported to Codacy Cloud.
- Codacy Cloud reanalysis is triggered and checked after import.
- Refreshed metrics are recorded.
- The next production-file maintainability target is selected with evidence.
- Validation passes for changed configuration and SOW state.

## Analysis

Sources checked:

- `.codacy/codacy.config.json`
- `.github/codeql.yml`
- `.agents/sow/done/SOW-0013-20260603-codacy-metrics-investigation.md`
- `.agents/sow/done/SOW-0014-20260603-maintainability-hotspots.md`
- `~/.agents/skills/configure-codacy/SKILL.md`
- `~/.agents/skills/codacy-cloud-cli/SKILL.md`
- Codacy Cloud CLI repository query for `gh/netdata/plugin-ipc`

Current state:

- Codacy Cloud latest analyzed commit is `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- Codacy Cloud metrics are: 0 issues, 107960 LOC, 88% coverage, 42% complex files, and 41% duplicated files.
- Codacy Cloud goals are: max duplicated files 10%, max complex files 10%, file duplication block threshold 1, and file complexity value threshold 20.
- `.codacy/codacy.config.json` has partial Opengrep tool-specific excludes for a few Windows fixture files, but top-level global excludes are empty.
- Codacy Cloud API `listIgnoredFiles` reports `hasCodacyConfigurationFile: false` for commit `5a55246b9b4698317bdf8440c898369034a1f408`, proving Cloud did not treat `.codacy/codacy.config.json` as the repository-level Cloud configuration file.
- CodeQL is intentionally broader and still scans `src`, `tests`, and `bench`; this SOW is about Codacy maintainability scope, not weakening GitHub code scanning.

Risks:

- Global Codacy excludes may remove Codacy issue scanning for test/bench paths, not only complexity/duplication metrics.
- If Codacy Cloud import changes more than the global excludes, the Cloud configuration could drift unexpectedly.
- If Codacy does not expose file-level complexity/duplication contributors through the CLI, the next-file decision needs local approximation plus dashboard confirmation.

## Pre-Implementation Gate

Status: ready.

Problem / root-cause model:

- Codacy maintainability percentages are still calculated with committed global exclusions set to an empty list.
- Test and benchmark code is large and intentionally repetitive; including it in Codacy maintainability metrics makes production-source hygiene harder to read.
- Previous production-source cleanup started and reduced real complexity/duplication, but follow-on target selection should use fresh metrics after Codacy scope is corrected.

Evidence reviewed:

- `.codacy/codacy.config.json:8185` has top-level `"exclude": []`.
- `.codacy/codacy.config.json:7154` has only tool-specific Opengrep excludes for selected fixtures.
- `SOW-0013` records the decision to keep complexity and duplication metrics active and treat real source hotspots as remediation work.
- `SOW-0014` records completed C/Rust/Go production-source organization work and says further complexity or duplication work should start from fresh Codacy/GitHub evidence.
- Codacy Cloud CLI repository query reports current metrics for commit `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- Codacy Cloud CLI skill states `.codacy/codacy.config.json` is local-only until imported with `codacy tools ... --import`.
- Codacy official docs state Cloud repository configuration uses root `.codacy.yml` or `.codacy.yaml`, starting with `---`, and path ignores are defined under `exclude_paths`.
- Codacy Cloud API file queries after commit `5a55246b9b4698317bdf8440c898369034a1f408` still list `tests/**`, `bench/**`, Go `*_test.go`, and Rust `*_tests.rs` files in complexity and duplication data.

Affected contracts and surfaces:

- Local Codacy configuration file `.codacy/codacy.config.json`.
- Cloud Codacy configuration file `.codacy.yml`.
- Codacy Cloud repository configuration after import.
- Codacy Cloud maintainability metrics and issue scope.
- GitHub Codacy local-analysis workflow trigger paths.
- SOW lifecycle records.
- No protocol, API, wire format, runtime behavior, or public SDK behavior should change during the exclusion step.

Existing patterns to reuse:

- Preserve strong CodeQL and GitHub static-analysis coverage; do not alter `.github/codeql.yml` in this SOW unless evidence proves it is necessary.
- Preserve Codacy rules and tools; this scope change should exclude non-production paths rather than disable useful rules.
- Follow SOW-0014's one-file-at-a-time maintainability workflow for production-source remediation.

Risk and blast radius:

- Low runtime risk for the exclusion step because no product code changes are expected.
- Medium quality-reporting risk because global Codacy excludes may also suppress Codacy issues in tests/bench paths.
- Low to medium future implementation risk for the next production-file cleanup, depending on the selected file.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details into durable artifacts.
- Codacy CLI output will be summarized without recording account tokens or credentials.

Implementation plan:

1. Update `.codacy/codacy.config.json` global excludes to include `tests/**` and `bench/**`.
2. Add root `.codacy.yml` with Cloud-native `exclude_paths` for test and benchmark files.
3. Add `.codacy.yml` and `.codacy.yaml` to the Codacy local-analysis workflow path filters.
4. Validate JSON/YAML syntax and run local Codacy analysis enough to prove the config is accepted.
5. Commit and push the configuration and SOW record.
6. Import the committed Codacy tool configuration into Codacy Cloud and trigger reanalysis.
7. Record refreshed Codacy metrics.
8. Select the next production-file maintainability target from refreshed Codacy data or local production-source approximation if Codacy does not expose contributors.

Validation plan:

- `jq empty .codacy/codacy.config.json`
- YAML parser check for `.codacy.yml`.
- `codacy-analysis analyze --output-format json` with the updated configuration.
- `codacy tools gh netdata plugin-ipc --import -y`
- `codacy repository gh netdata plugin-ipc --reanalyze`
- Re-query Codacy Cloud metrics after reanalysis completes.
- `git diff --check`
- `bash .agents/sow/audit.sh`

Artifact impact plan:

- AGENTS.md: no workflow or guardrail change expected.
- Runtime project skills: no reusable workflow change expected.
- Specs: no protocol/API behavior change expected.
- End-user/operator docs: no public SDK docs change expected.
- End-user/operator skills: no exported/operator skill change expected.
- SOW lifecycle: new current SOW because this is a new Codacy scope correction plus next maintainability pass.

Open-source reference evidence:

- No external open-source reference is needed; this is repository-specific Codacy configuration and local source hygiene work.

Open decisions:

- Resolved: the user approved proceeding with Codacy test/bench exclusion and continued production-source maintainability work.

## Implications And Decisions

1. Codacy scope

- Decision: exclude `tests/**` and `bench/**` from Codacy global analysis scope.
- Benefit: Codacy complexity and duplication percentages should better represent production SDK files.
- Implication: Codacy will likely stop reporting issues from test and benchmark paths too, not only maintainability metrics.
- Mitigation: GitHub CodeQL and static-analysis workflows continue scanning test and benchmark paths separately.

2. Production-source maintainability

- Decision: continue one production file at a time after refreshed metrics.
- Benefit: avoids broad mechanical refactors and keeps review/validation tractable.
- Implication: metric reduction will be incremental, not a single bulk cleanup.
- Mitigation: each target will use file-level evidence and validation appropriate to the touched language/runtime surface.

## Plan

1. Correct Codacy scope and import it to Cloud.
2. Reanalyze Codacy and record refreshed metrics.
3. Build the next production-file candidate list.
4. Start the next low-risk production-file cleanup only after evidence shows the target and the intended refactor.

## Execution Log

### 2026-06-05

- Started SOW after user approval to proceed.
- Updated `.codacy/codacy.config.json` global excludes from `[]` to:
  - `bench/**`
  - `tests/**`
- Verified this is a Codacy-only scope change; `.github/codeql.yml` still scans `src`, `tests`, and `bench`.
- Ran local Codacy analysis with the updated config:
  - total issues: 0.
  - Checkov: 10 files, 0 issues.
  - Opengrep/Semgrep: 264 files, 0 issues.
  - Trivy: 265 files, 0 issues.
  - cppcheck: 45 files, 0 issues.
  - ShellCheck: 3 files, 0 issues.
  - Spectral: 10 files, 0 issues.
- Committed and pushed `5a55246b9b4698317bdf8440c898369034a1f408`.
- Imported `.codacy/codacy.config.json` to Codacy Cloud.
- The import temporarily disabled Cloud Revive because the local file did not contain Revive. Revive was immediately re-enabled in Cloud.
- Added Revive back to `.codacy/codacy.config.json` with the 21 enabled Cloud Revive patterns so future tool imports do not disable it again.
- Verified local Codacy Analysis CLI `0.8.0` is the latest npm-published version available locally, but its Revive adapter reports a non-fatal `findings is not iterable` invocation error even when scanning one Go file. The Revive binary itself runs, and Cloud Revive remains enabled.
- Checked Codacy Cloud after commit `5a55246b9b4698317bdf8440c898369034a1f408`:
  - latest analyzed commit: `5a55246b9b4698317bdf8440c898369034a1f408`.
  - metrics stayed at 0 issues, 88% coverage, 42% complex files, and 41% duplicated files.
  - `listIgnoredFiles` reported `hasCodacyConfigurationFile: false` and only one ignored file: `bench/drivers/go/go.mod`.
- Queried Codacy Cloud file metrics through the generated API client:
  - `bench/drivers/go/main_windows.go`: complexity 321, duplication 1706.
  - `bench/drivers/c/bench_windows.c`: complexity 292, duplication 1357.
  - `tests/fixtures/c/test_uds.c`: complexity 281, duplication 2445.
  - `src/go/pkg/netipc/transport/posix/uds_test.go`: complexity 257, duplication 4091.
  - `src/crates/netipc/src/service/raw_unix_tests.rs`: complexity 329.
- Added Cloud-native `.codacy.yml` with `exclude_paths` for:
  - `bench/**`
  - `benchmarks-*`
  - `tests/**`
  - `**/*_test.go`
  - `**/*_tests.rs`
  - `**/tests.rs`
- Added `.codacy.yml` and `.codacy.yaml` to the Codacy local-analysis workflow path filters.
- Validated the new local state:
  - `.codacy.yml` parses as YAML and contains the intended `exclude_paths`.
  - `.codacy/codacy.config.json` parses as JSON and now includes Revive with 21 patterns.
  - `git diff --check` passed.
  - `bash .agents/sow/audit.sh` passed.
  - `codacy-analysis analyze . --output-format json` exited with Codacy analysis status 0, produced 0 issues, and produced 1 known Revive adapter error:
    - Checkov: success, 11 files, 0 issues.
    - Opengrep/Semgrep: success, 265 files, 0 issues.
    - Trivy: success, 266 files, 0 issues.
    - cppcheck: success, 45 files, 0 issues.
    - ShellCheck: success, 3 files, 0 issues.
    - Spectral: success, 11 files, 0 issues.
    - Revive: partial, 90 files, 0 issues, 1 `findings is not iterable` invocation error.
  - The previous pushed commit `5a55246b9b4698317bdf8440c898369034a1f408` has green CI for CodeQL, Static Analysis, Supply Chain Security, Codacy Local Analysis, Codacy Coverage, and the Codacy GitHub check.

## Validation

Acceptance criteria evidence:

- Pending.

Tests or equivalent validation:

- Pending.

Real-use evidence:

- Pending.

Reviewer findings:

- No external reviewer used yet.

Same-failure scan:

- Pending.

Sensitive data gate:

- Pending final scan.

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

## Regression Log

None yet.
