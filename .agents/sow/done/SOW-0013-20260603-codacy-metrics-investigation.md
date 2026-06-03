# SOW-0013 - Codacy Metrics Investigation

## Status

Status: completed

Sub-state: LCOV `DA:` checksum regression repaired; remote workflow and Codacy dashboard validation passed.

## Requirements

### Purpose

Determine why Codacy reports unusually high complexity, unusually high duplication, and zero coverage for this repository, while preserving strong static-analysis coverage and avoiding noise-only rule changes.

### User Request

The user reported Codacy shows about 44% complexity, 47% duplication, and 0% coverage, which looks unusual for this repository.

### Assistant Understanding

Facts:

- The repository has C, Rust, and Go code plus tests, fixtures, generated build directories in the local worktree, and existing coverage scripts.
- The repository already has GitHub static-analysis and Codacy local-analysis workflows.
- The repository root `.env` must not be read or committed.

Inferences:

- Zero coverage is likely a coverage ingestion problem unless Codacy Cloud can show uploaded coverage data for the latest analyzed commit.
- Complexity and duplication percentages may be caused by Codacy analyzing test fixtures, interop fixtures, generated/build outputs, or intentionally parallel cross-language implementations.

Unknowns:

- Which exact files and tools contribute to the Codacy complexity and duplication percentages.
- Whether Codacy Cloud currently receives any coverage reports from CI.

### Acceptance Criteria

- Identify whether Codacy's metrics are caused by real source issues, configuration scope, missing coverage upload, or a mix.
- Provide file/tool-level evidence for complexity and duplication where available.
- Identify the missing coverage generation/upload path if coverage is not reaching Codacy.
- Record user decisions before changing rules, exclusions, workflows, or code.
- Add a GitHub Actions Codacy coverage workflow that generates and uploads C,
  Rust, and Go reports using account-token authentication.
- Keep complexity and duplication metrics enabled.

## Analysis

Sources checked:

- Pending/current SOWs.
- Project SOW specs and runtime project skills.
- Codacy Cloud CLI and setup-coverage skill instructions.
- Repository workflows, coverage scripts, and Codacy configuration.
- Codacy Cloud repository and issue data for `gh/netdata/plugin-ipc`.
- Local Lizard and JSCPD approximations to understand complexity and duplication shape.

Current state:

- Pending SOWs are unrelated to Codacy metrics.
- No current SOW existed before this one.
- No runtime project skills exist.
- Codacy Cloud reports repository metrics on latest analyzed commit
  `d1b4a89f9f95c36d013e937827066ab7e5cbda07`:
  - complex files: 44%.
  - duplicated files: 47%.
  - open issues: 0.
  - LOC: 105447.
- Codacy Cloud issue overview for `main` is empty: no categories, levels,
  languages, tags, patterns, or authors with current issues.
- Existing GitHub workflows upload SARIF to GitHub Code Scanning but do not
  upload coverage to Codacy.
- Coverage upload authentication can use either a repository token or the
  account-token tuple. The earlier statement that GitHub specifically needs
  `CODACY_PROJECT_TOKEN` was too narrow.

Risks:

- Disabling duplication or complexity rules would hide useful hygiene signals instead of fixing scope or coverage.
- Uploading malformed coverage could make Codacy metrics worse or misleading.
- Durable artifacts must not include raw tokens or sensitive values.
- Excluding too much from Codacy could hide useful static-analysis signal.
- Treating duplication/complexity percentages as pure false positives could
  ignore real maintainability debt in protocol/transport/session code.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Working theory: Codacy Cloud metrics are mixing at least one real signal with configuration gaps. Coverage appears to be 0 because no workflow currently uploads coverage to Codacy. Complexity/duplication need file-level evidence before deciding whether to fix code, exclude fixtures, or tune thresholds.

Evidence reviewed:

- `.github/workflows/codacy-analysis.yml`
- `.github/workflows/static-analysis.yml`
- `tests/run-coverage-c.sh`
- `tests/run-coverage-rust.sh`
- `tests/run-coverage-go.sh`
- `.codacy/codacy.config.json`
- `~/.agents/skills/setup-coverage/SKILL.md`
- `~/.agents/skills/setup-coverage/references/coverage-upload.md`
- Codacy Cloud CLI JSON output captured in `/tmp/plugin-ipc-codacy-*.json`
- Local metric approximations captured under `/tmp/plugin-ipc-lizard-*.csv` and
  `/tmp/plugin-ipc-jscpd-*`

Affected contracts and surfaces:

- Codacy Cloud repository configuration.
- `.codacy/codacy.config.json`
- GitHub Actions workflows.
- Coverage scripts and generated coverage report formats.
- SOW lifecycle records.

Existing patterns to reuse:

- Existing coverage scripts for C, Rust, and Go.
- Existing GitHub Actions static-analysis workflow structure.
- Existing `.codacy/codacy.config.json` exclusions and tool configuration.

Risk and blast radius:

- Low runtime risk for investigation.
- Medium CI risk from the added coverage workflow/upload steps.
- Medium quality risk if Codacy rules are weakened without evidence.

Sensitive data handling plan:

- Do not read or commit `.env`.
- Do not write tokens, credentials, account IDs, private endpoints, customer data, personal data, or raw sensitive values into durable artifacts.
- Use only file paths, rule IDs, public PR/workflow data, and redacted configuration evidence.

Implementation plan:

1. Query Codacy Cloud repository metrics, issues, tools, and coverage-related state.
2. Inspect local workflows and coverage scripts for coverage generation/upload gaps.
3. Inspect Codacy configuration and issue distribution for file-scope problems.
4. Present evidence-backed options before changing code or configuration.

Validation plan:

- Codacy Cloud CLI queries with JSON output.
- Local repository searches for coverage upload steps and exclusions.
- Validate implemented changes locally and with GitHub/Codacy after push.

Artifact impact plan:

- AGENTS.md: likely unaffected.
- Runtime project skills: likely unaffected unless a reusable Codacy workflow is discovered.
- Specs: likely unaffected; Codacy metrics do not change protocol behavior.
- End-user/operator docs: likely unaffected unless coverage commands become public guidance.
- End-user/operator skills: likely unaffected.
- SOW lifecycle: complete this SOW with final evidence and follow-up mapping.

Open-source reference evidence:

- No external open-source reference is needed for this investigation; the issue is repository-specific Codacy configuration and CI behavior.

Open decisions:

- Resolved by user selection:
  - Decision 1: A, use Codacy account-token authentication in GitHub Actions.
  - Decision 2: A, generate and upload C, Rust, and Go coverage.
  - Decision 3: A, keep complexity and duplication metrics active and treat
    real source hotspots as remediation work.

## Implications And Decisions

1. Coverage authentication:

- A selected: Use `CODACY_API_TOKEN` plus `CODACY_ORGANIZATION_PROVIDER`,
  `CODACY_USERNAME`, and `CODACY_PROJECT_NAME`.
- Implication: One account-token secret can serve this repository without
  committing repository-specific tokens.
- Risk: the account token must belong to a user or service account with Codacy
  coverage upload permissions for this repository.

2. Coverage scope:

- A selected: Generate and upload C, Rust, and Go coverage.
- Implication: Codacy coverage will reflect the cross-language SDK, not only one
  language binding.
- Risk: the workflow is heavier than Go-only coverage and may expose existing
  coverage gaps in Windows-only code not exercised on Linux.

3. Complexity and duplication:

- A selected: Keep metrics enabled and fix real source hotspots.
- Implication: Codacy keeps reporting maintainability pressure instead of hiding
  it.
- Risk: fixing the real hotspots is non-trivial and should be handled as
  focused source remediation after coverage upload is working.

## Plan

1. Gather Codacy Cloud metrics and issue distribution.
2. Gather local workflow and coverage evidence.
3. Identify likely root causes and present options.
4. Implement selected coverage workflow.
5. Validate workflow syntax, local coverage report generation, and SOW audit.

## Execution Log

### 2026-06-03

- Created this investigation SOW.
- Loaded Codacy-related skills from `~/.agents/skills/`.
- Corrected coverage authentication model:
  - `setup-coverage/SKILL.md` states `CODACY_PROJECT_TOKEN` or
    `CODACY_API_TOKEN` can be used.
  - `coverage-upload.md` documents account-token auth via
    `CODACY_API_TOKEN`, `CODACY_ORGANIZATION_PROVIDER`, `CODACY_USERNAME`, and
    `CODACY_PROJECT_NAME`.
- Queried Codacy Cloud repository metrics:
  - complex files: 44%.
  - duplicated files: 47%.
  - issues: 0.
  - LOC: 105447.
  - latest analyzed commit: `d1b4a89f9f95c36d013e937827066ab7e5cbda07`.
- Queried Codacy Cloud issues for `main`; current issue count is 0.
- Checked GitHub workflows:
  - `.github/workflows/codacy-analysis.yml` runs Codacy local analysis and
    uploads SARIF to GitHub Code Scanning.
  - `.github/workflows/static-analysis.yml` runs tests/static analysis but does
    not generate Codacy coverage reports or run the Codacy coverage reporter.
- Searched committed workflows/config for coverage upload variables and
  reporter commands; no `CODACY_API_TOKEN`, `CODACY_PROJECT_TOKEN`,
  `codacy-coverage-reporter`, `lcov`, `cobertura`, or coverage upload command
  is committed in `.github/`.
- Checked repository-level GitHub secrets with `gh secret list --repo
  netdata/plugin-ipc`; no repo-level secrets were listed. This does not rule out
  organization-level or environment secrets.
- Checked tracked source/test/bench code volume:
  - `src`: about 71101 lines.
  - `tests`: about 31677 lines of C/Go/Rust source files.
  - `bench`: about 11590 lines of C/Go/Rust source files.
- Ran local Lizard approximation with Codacy's complexity threshold 20:
  - all source/test/bench files: 141 files, 34 complex, about 24%.
  - source-like files only: 52 files, 20 complex, about 38%.
  - highest source complexity is in lookup builders/decoders, service session
    loops, and transport send/receive paths.
- Ran local JSCPD approximation:
  - `src tests bench`: 3964 duplicated lines, 10.32% duplicated lines, 130
    exact clones.
  - `src` without Go/Rust test files: 1148 duplicated lines, 5.48% duplicated
    lines, 40 exact clones.
  - largest duplicate groups include POSIX/Windows wrapper parity, apps/cgroups
    lookup service wrappers, Rust typed-service wrappers, and interop/test
    harnesses.
- User selected:
  - 1A: use account-token Codacy coverage authentication.
  - 2A: generate and upload C, Rust, and Go coverage.
  - 3A: keep complexity/duplication metrics active and fix real source
    hotspots.
- Added `tests/generate-codacy-coverage.sh` to generate:
  - `coverage/codacy/c-lcov.info`
  - `coverage/codacy/rust-lcov.info`
  - `coverage/codacy/go-coverage.out`
- Added `.github/workflows/codacy-coverage.yml` to run the generator, upload
  artifacts, upload C/Rust/Go partial reports to Codacy with account-token
  auth, and send the final coverage marker.
- Added generated coverage artifacts to `.gitignore`.
- Fixed validation issues found during local coverage generation:
  - `gcovr` needs negative-hit parser tolerance for current GCC coverage data.
  - `gcovr` needs `merge-use-line-min` because the same C sources are compiled
    into multiple test targets.
  - the transparent `run()` helper must capture the real failing exit code.
  - Go coverage must be summarized before rewriting paths for Codacy.
  - C and Rust LCOV `SF:` paths must be normalized to repository-relative
    paths.
- Verified Codacy coverage reporter 14.1.3 command help and replaced an
  unsupported workflow flag with supported `--force-coverage-parser lcov`
  usage for C and Rust reports.
- Created pending follow-up `SOW-0014` for real source complexity and
  duplication remediation.

## Validation

Acceptance criteria evidence:

- Codacy metric payload was identified and matched the user's report.
- Codacy current issue count was checked and is 0, so the dashboard percentages
  are repository-level metrics, not current issue backlog.
- Coverage upload auth correction was verified against local Codacy skills.
- Local evidence shows coverage generation exists as scripts, but Codacy upload
  is not wired into GitHub Actions.
- Local evidence shows duplication is much lower as duplicated lines than the
  Codacy dashboard percentage, so Codacy's 47% is likely a file-affected metric
  or a broader metric scope, not a duplicated-line percentage.
- Local evidence shows source complexity is real enough to investigate; it is
  not only caused by tests/fixtures/bench scope.
- Added Codacy coverage workflow using account-token authentication:
  `CODACY_API_TOKEN`, `CODACY_ORGANIZATION_PROVIDER=gh`,
  `CODACY_USERNAME=netdata`, and `CODACY_PROJECT_NAME=plugin-ipc`.
- Added C, Rust, and Go coverage report generation:
  - C LCOV: `coverage/codacy/c-lcov.info`.
  - Rust LCOV: `coverage/codacy/rust-lcov.info`.
  - Go coverage: `coverage/codacy/go-coverage.out`.
- Kept complexity and duplication metrics active; remediation is tracked in
  `SOW-0014`.

Tests or equivalent validation:

- Codacy Cloud CLI repository query succeeded.
- Codacy Cloud CLI issues query succeeded and returned 0 current issues.
- Local Lizard run completed on tracked C/Go/Rust source/test/bench files.
- Local JSCPD run completed on `src tests bench` and source-only subsets.
- `bash -n tests/generate-codacy-coverage.sh` passed.
- `shellcheck --severity=error tests/generate-codacy-coverage.sh` passed.
- `actionlint .github/workflows/codacy-coverage.yml` passed.
- `git diff --check` passed.
- `tests/generate-codacy-coverage.sh /tmp/plugin-ipc-codacy-coverage` passed
  end to end.
- `bash <(curl -Ls https://coverage.codacy.com/get.sh) report --help`
  confirmed supported account-token, `-l`, `-r`, `--partial`, and
  `--force-coverage-parser` options.
- `bash <(curl -Ls https://coverage.codacy.com/get.sh) final --help`
  confirmed supported account-token options for the final marker.
- Generated report files after the final local run:
  - `c-lcov.info`: 292K.
  - `rust-lcov.info`: 152K.
  - `go-coverage.out`: 468K.
- Generated report path scan found no workstation absolute paths and no Go
  module-prefix paths in the final report files.
- Final report path samples:
  - C: `SF:src/libnetdata/netipc/src/protocol/netipc_protocol.c`.
  - Rust: `SF:src/crates/netipc/src/protocol/cgroups.rs`.
  - Go: `src/go/pkg/netipc/protocol/cgroups.go:...`.

Real-use evidence:

- GitHub workflow files show SARIF/code-scanning upload paths but no Codacy
  coverage reporter path.
- Codacy Cloud reports the latest analyzed commit as the current `main` head
  from the last pushed SOW lifecycle commit.
- The new workflow uploads C, Rust, and Go partial reports and then sends the
  Codacy final marker. Remote upload still depends on the GitHub secret
  `CODACY_API_TOKEN` being available to the workflow context.

Reviewer findings:

- No external reviewer was requested for this CI configuration change.
- Local validation found and fixed the failed-command handling bug, C `gcovr`
  parser/merge requirements, Go local-summary path issue, and C absolute-path
  LCOV issue before commit.

Same-failure scan:

- Searched committed workflow/config paths for Codacy coverage auth and upload
  commands. No committed coverage upload path exists.
- Searched final generated reports for workstation absolute paths and Go module
  prefixes; no matches remained after path normalization.

Sensitive data gate:

- `.env` was not read.
- No raw tokens, credentials, account IDs, private endpoints, customer data,
  personal data, or proprietary incident details were written to this SOW.
- The workflow references only the secret name `CODACY_API_TOKEN`, not the
  secret value.

Artifact maintenance gate:

- AGENTS.md: no update needed; project-wide workflow rules did not change.
- Runtime project skills: no update needed; coverage generation is captured in
  a committed script and workflow, not a reusable project-specific agent
  operating procedure.
- Specs: no update needed; no protocol/API behavior changed.
- End-user/operator docs: no update needed; this is CI/internal coverage
  reporting and does not change SDK usage.
- End-user/operator skills: no update needed; no public/operator skill behavior
  changed.
- SOW lifecycle: `SOW-0013` is marked `completed` and will be moved to
  `.agents/sow/done/` in the same commit as the implementation. Follow-up
  maintainability remediation is tracked in
  `.agents/sow/pending/SOW-0014-20260603-maintainability-hotspots.md`.

Specs update:

- No spec update needed; this is CI/Codacy coverage configuration and does not
  change protocol, wire format, transport behavior, or public API contracts.

Project skills update:

- No runtime project skill update needed; the workflow is represented by the
  committed script and GitHub Actions file.

End-user/operator docs update:

- No docs update needed; users of the SDK are unaffected.

End-user/operator skills update:

- No output/reference skill update needed; no docs/spec changes affect
  downstream operators.

Lessons:

- Codacy coverage upload supports both repo-token and account-token
  authentication; account-token auth requires provider, organization/user, and
  project name environment variables.
- Codacy repository dashboard percentages can be non-issue metrics even when
  the current issue backlog is zero.
- Local coverage generation is necessary for this repository because C coverage
  needs explicit `gcovr` parser and merge options, and report paths need
  normalization before upload.

Follow-up mapping:

- Real source complexity and duplication remediation is tracked in
  `.agents/sow/pending/SOW-0014-20260603-maintainability-hotspots.md`.

## Outcome

Implemented Codacy coverage reporting for C, Rust, and Go using account-token
authentication in GitHub Actions. Complexity and duplication metrics remain
enabled; real hotspot remediation is tracked separately.

## Lessons Extracted

- Do not treat Codacy issue count and repository metric percentages as the same
  thing; this repository can have zero current issues while still showing
  complexity and duplication percentages.
- Account-token Codacy coverage upload is valid when the workflow provides the
  provider, organization/user, and project environment variables.
- C LCOV generation needs repository-specific `gcovr` options because this
  project compiles the same C sources into multiple test targets.

## Followup

- `SOW-0014`: source complexity and duplication hotspot remediation.

## Regression - 2026-06-03

What broke:

- GitHub Actions run `26877754854`, job `79269490556`, failed in the new
  `Codacy Coverage` workflow before upload.
- Failure occurred during `Generate coverage reports` while building
  `tests/fixtures/c/test_stress.c`.
- Remote evidence: GCC 13 on Ubuntu 24.04 rejected the sanitizer guard at
  `tests/fixtures/c/test_stress.c:840` with `missing binary operator before
  token "("`.

Why previous validation missed it:

- Local validation used a newer GCC that accepted the existing guard.
- The broken expression mixed `defined(__has_feature)` with direct
  `__has_feature(...)` calls in the same preprocessor expression; GCC 13 still
  parsed the function-like expression even when the macro was absent.

Repair plan:

- Define a local `__has_feature(feature)` compatibility macro as `0` when the
  compiler does not provide it.
- Simplify the sanitizer guard to call `__has_feature(...)` directly after the
  compatibility macro is defined.
- Re-run local C build/tests and final CI workflow checks.

Validation:

- Targeted local validation passed:
  - configured coverage build with `cmake -S . -B build-codacy-coverage
    -DCMAKE_BUILD_TYPE=Debug -DNETIPC_COVERAGE=ON -DCMAKE_C_COMPILER=gcc`.
  - built `test_stress` with `cmake --build build-codacy-coverage --target
    test_stress`.
  - ran `build-codacy-coverage/bin/test_stress`; result was 32 passed, 0
    failed.
- Remote GitHub Actions validation will be checked after the repair commit is
  pushed.

Artifact updates:

- SOW lifecycle: reopened from `done/` to `current/` for this regression.
- Specs/docs/skills: no protocol/API/operator behavior changes expected from
  this test portability fix.

## Regression - 2026-06-03 - Codacy LCOV Parser

What broke:

- After adding the `CODACY_API_TOKEN` GitHub Actions secret, rerun attempt 2 of
  GitHub Actions run `26877997132` passed report generation, artifact upload,
  and token verification.
- The first real upload step, `Upload C coverage to Codacy`, failed.
- Codacy reporter 14.1.3 logged `Could not parse report, unrecognized report
  format (tried: LCOV)` for `coverage/codacy/c-lcov.info`.

Evidence:

- Downloaded artifact `codacy-coverage-reports` from run `26877997132`.
- C LCOV record-type scan showed only one non-basic extension type: `VER`.
- Artifact contained four `VER:` checksum records, one per `SF:` source file.

Why previous validation missed it:

- Local validation proved `gcovr` could generate LCOV and paths were
  repository-relative, but did not prove Codacy's stricter LCOV parser accepted
  `gcovr` checksum extension records.

Repair plan:

- Strip `VER:` records in the existing LCOV normalization step.
- Keep all basic LCOV coverage records and repository-relative `SF:` paths.
- Re-run script lint, SOW audit, local coverage generation, and GitHub Actions
  Codacy Coverage.

Validation:

- `bash -n tests/generate-codacy-coverage.sh` passed.
- `shellcheck --severity=error tests/generate-codacy-coverage.sh` passed.
- `actionlint .github/workflows/codacy-coverage.yml` passed.
- `git diff --check` passed.
- `bash .agents/sow/audit.sh` passed with `SOW initialization complete and
  clean`.
- `tests/generate-codacy-coverage.sh /tmp/plugin-ipc-codacy-coverage` passed
  end to end.
- LCOV extension scan found no remaining `VER:` records in C or Rust reports.
- C LCOV record-type scan found only standard LCOV records:
  `TN`, `SF`, `FN`, `FNDA`, `FNF`, `FNH`, `DA`, `LF`, `LH`, `BRDA`, `BRF`,
  and `BRH`.
- Generated report path scan found no workstation absolute paths and no Go
  module-prefix paths.
- Final C LCOV path sample remains repository-relative:
  `SF:src/libnetdata/netipc/src/protocol/netipc_protocol.c`.
- GitHub Actions `Codacy Coverage` run `26879604593` still failed C upload
  after this repair because optional `DA:` checksum fields remained. The
  follow-up parser regression is recorded below.

Artifact updates:

- SOW lifecycle: reopened from `done/` to `current/` for this parser
  regression, then completed and moved back to `done/` with the repair.
- Specs/docs/skills: no protocol/API/operator behavior changes expected.

## Regression - 2026-06-03 - Codacy LCOV DA Checksums

What broke:

- GitHub Actions run `26879604593`, job `79275935075`, passed coverage
  generation, artifact upload, and `CODACY_API_TOKEN` verification.
- The first real upload step, `Upload C coverage to Codacy`, still failed.
- Codacy reporter 14.1.3 again logged `Could not parse report, unrecognized
  report format (tried: LCOV)` for `coverage/codacy/c-lcov.info`.

Evidence:

- Downloaded artifact `codacy-coverage-reports` from run `26879604593`.
- The repaired C LCOV artifact no longer contained `VER:` records.
- C `DA:` line records still had the gcovr checksum extension shape:
  `DA:line,count,checksum`.
- Rust `DA:` line records used the simpler shape `DA:line,count`.
- A local reporter parse test using the downloaded C artifact reproduced the
  parser failure with the original file.
- The same local reporter parse test accepted a copy of the C LCOV with only
  the third `DA:` checksum field removed; it generated a Codacy coverage JSON
  report before failing later because the local test intentionally used a dummy
  token.

Why previous validation missed it:

- The validation checked for unsupported record labels and missing
  `end_of_record` separators, but did not check whether Codacy accepts optional
  checksum fields inside otherwise standard `DA:` records.

Repair plan:

- Strip the optional third field from `DA:` records during the existing LCOV
  normalization step.
- Keep line number and hit count coverage data unchanged.
- Re-run script lint, SOW audit, local coverage generation, local reporter parse
  against the generated C report, and GitHub Actions `Codacy Coverage`.

Validation:

- `bash -n tests/generate-codacy-coverage.sh` passed.
- `shellcheck --severity=error tests/generate-codacy-coverage.sh` passed.
- `actionlint .github/workflows/codacy-coverage.yml` passed.
- `git diff --check` passed.
- `tests/generate-codacy-coverage.sh /tmp/plugin-ipc-codacy-coverage` passed
  end to end.
- Generated C and Rust LCOV reports contain no `VER:` records.
- Generated C and Rust LCOV `DA:` records all use two fields:
  `DA:line,count`.
- Generated report path scan found no workstation absolute paths and no Go
  module-prefix paths.
- Generated C LCOV syntax scan found no records outside the normalized LCOV
  shape needed by Codacy.
- Local Codacy reporter 14.1.3 parse test against the generated C report used
  the LCOV parser and generated the Codacy coverage JSON payload. The test then
  failed at upload because it intentionally used a dummy token; that is outside
  the parser path being validated locally.
- GitHub Actions `Codacy Coverage` run `26880221479` passed after this repair
  was pushed:
  - generated coverage reports.
  - uploaded the coverage artifact.
  - verified `CODACY_API_TOKEN`.
  - uploaded C coverage to Codacy.
  - uploaded Rust coverage to Codacy.
  - uploaded Go coverage to Codacy.
  - finalized the Codacy coverage upload.
- Codacy Cloud repository query for commit
  `9d48c7ec7ce5efc07111d8f71d3980befd7e519a` reported:
  - coverage: 88%.
  - issues: 0.
  - complex files: 43%.
  - duplicated files: 46%.

Artifact updates:

- SOW lifecycle: reopened from `done/` to `current/` for this parser
  regression, then completed and moved back to `done/` with the repair.
- Specs/docs/skills: no protocol/API/operator behavior changes expected.
