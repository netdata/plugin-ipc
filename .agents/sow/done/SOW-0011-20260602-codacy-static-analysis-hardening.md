# SOW-0011 - Codacy Static Analysis Hardening

## Status

Status: completed

Sub-state: Codacy Cloud configuration, local Codacy analysis configuration,
GitHub Code Scanning SARIF workflow, local tool installation, and secret
ignore handling are complete.

## Requirements

### Purpose

Provide the strongest practical Codacy-backed static analysis setup for this
AI-generated and AI-maintained SDK, complementing the existing GitHub scanner
stack with maximum useful coverage across C, Rust, Go, Shell, Markdown,
workflow, dependency, and security surfaces.

### User Request

The user asked to continue the scanner hardening work after enabling Codacy,
installing the Codacy Cloud CLI, pointing to `codacy/codacy-skills`, and asking
to install and arrange those skills locally.

### Assistant Understanding

Facts:

- The repository is `netdata/plugin-ipc`, default branch `main`.
- `SOW-0009` already added GitHub-native scanner workflows for CodeQL,
  Dependency Review, supply-chain scanning, language static analysis, and
  runtime safety.
- Codacy Cloud is already analyzing this repository.
- Codacy Cloud reports the latest analyzed commit as
  `02d2c071a80b3fca872f3aaef7588c2a5d2c620c`.
- Before this SOW imported broader configuration, Codacy Cloud reported 1,725
  repository issues: 4 Error, 198 High, 1,448 Warning, and 75 Info.
- After this SOW imported broader configuration and requested reanalysis,
  Codacy Cloud reported 2,448 repository issues on the latest analyzed commit
  available to Codacy at that time.
- Codacy Cloud now has these relevant repository tools enabled: Agentlinter,
  Checkov, Cppcheck, Flawfinder, Lizard, markdownlint, Opengrep, Revive,
  ShellCheck, Spectral, and Trivy.
- Codacy Cloud still lists these legacy or unsupported client-side tools as
  disabled: Aligncheck, Clang-Tidy, Deadcode, GolangCI-Lint, Gosec,
  Staticcheck, and remark-lint.
- `codacy` and `codacy-analysis` are installed locally.
- The Codacy skills from `codacy/codacy-skills` are installed under
  `~/.agents/skills`, while Codex-only `.system` skills are under
  `~/.codex/skills`.
- The user reported adding `CODACY_PROJECT_TOKEN` to the repo-root `.env`.
  `.env` is now ignored by the repository and was not read, copied, staged, or
  committed.

Inferences:

- Server-side Codacy is now stronger, but this repository should also run local
  Codacy Analysis CLI in GitHub Actions and upload SARIF to GitHub Code
  Scanning so client-side results are visible without exposing Codacy tokens.
- Because the user asked for strongest scanning, the implementation should not
  reduce issue volume by disabling valid noisy patterns unless those patterns
  target generated/build artifacts outside the intended source surface.
- Existing findings are cleanup work, not scanner-enablement work, and should
  remain tracked separately unless a configuration issue makes a scanner
  unusable.

Unknowns:

- No scanner-enablement unknowns remain. Finding cleanup remains tracked by
  `SOW-0010`.

### Acceptance Criteria

- Codacy Cloud has every relevant repository tool enabled for this stack, or
  this SOW records a concrete reason a tool is not relevant or cannot be
  enabled.
- Codacy client-side analysis is represented in GitHub Actions without exposing
  tokens and without failing forks or local runs when a secret is absent.
- Local Codacy configuration is created or updated using the Codacy Analysis CLI
  and reflects the repository's actual stack.
- Existing GitHub scanner workflows remain valid.
- Validation records Codacy tool state, local analysis or inspect results,
  GitHub workflow YAML parsing, SOW audit, and git status.
- Any remaining scanner findings are mapped to an existing or new follow-up SOW
  rather than hidden.

## Analysis

Sources checked:

- `AGENTS.md`
- `.agents/sow/SOW.template.md`
- `.agents/sow/done/SOW-0009-20260602-github-security-scanning.md`
- Existing GitHub scanner workflows under `.github/`
- Codacy Cloud CLI help for `repository`, `tools`, `tool`, `issues`,
  `findings`, `patterns`, and `pattern`
- Codacy Analysis CLI help for `discover`, `init`, `analyze`, and `info`
- Codacy docs via Context7:
  - client-side tools and Codacy Analysis CLI
  - GitHub Actions Codacy CLI usage
  - tool and pattern configuration workflow
- CodeQL docs via Context7 for existing GitHub scanner context
- `codacy/codacy-skills @ bb6e7fc75159f360efa27d89568078272048be93`
  - `skills/codacy-cloud-cli/SKILL.md`
  - `skills/codacy-analysis-cli/SKILL.md`
  - `skills/configure-codacy/SKILL.md`
  - `README.md`

Current state:

- `git status --short --ignored` shows `.env` ignored and not staged, plus
  unrelated untracked root files `TODO-perf-parity.md` and `tea_debug.log`.
- A separate vendor-sync repair was committed as `40a27fe` before this SOW
  started, so scanner changes can be kept separate.
- Existing GitHub scanner files are committed at `02d2c07` and later local
  `40a27fe`.
- `codacy-analysis info` reports local support for 33 tools and 13 installed
  tools, including Cppcheck, Flawfinder, ShellCheck, Trivy, markdownlint,
  Agentlinter, ESLint 8/9, Spectral, and bundled support tools.
- Codacy repository overview shows issues across Shell, Markdown, C, Rust, and
  Go, which matches the repository stack.
- `.codacy/codacy.config.json` now contains 11 tools and 3,139 enabled
  patterns with no global excludes.
- `codacy-analysis analyze . --inspect --output-format json` reports all 11
  configured tools ready and zero unavailable configured tools.
- Local Codacy SARIF generation produced 7,736 findings across existing source,
  test, instruction, and documentation surfaces.
- GitHub repository secret name enumeration returned no repository-level
  Actions secrets; the local `CODACY_PROJECT_TOKEN` in `.env` is therefore a
  workstation-only secret unless the user later adds a GitHub secret.

Risks:

- Enabling all relevant Codacy patterns can expose a large existing backlog.
  That is expected and should be tracked, not hidden.
- Codacy upload or coverage upload would require a token in GitHub Actions. The
  implemented workflow does not require a Codacy token; it uploads Codacy SARIF
  to GitHub Code Scanning and stays safe when repository secrets are absent.
- Codacy configuration import changes remote repository settings and may
  increase issue volume immediately after reanalysis.
- `codacy-analysis` is new and its installed version output is inconsistent
  with the npm package version; local command help and actual runs must be the
  source of truth.
- Some Codacy tools may be irrelevant to this repo's languages and should not be
  enabled only to inflate tool count.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- SOW-0009 created a strong GitHub-native scanner stack, but Codacy was enabled
  afterward and is only partially configured for the repository stack.
- Codacy server-side analysis is active, but several relevant client-side tools
  are disabled. Evidence: Codacy tools show disabled Staticcheck, Aligncheck,
  GolangCI-Lint, Clang-Tidy, Gosec, and Deadcode.
- Therefore the remaining gap is Codacy-specific configuration and CI upload
  integration, not another rewrite of the existing GitHub scanner stack.

Evidence reviewed:

- `codacy repository gh netdata plugin-ipc --output json` shows Codacy has
  analyzed the repository.
- `codacy issues gh netdata plugin-ipc --overview --output json` shows 1,725
  current issues across the repository's languages.
- `codacy tools gh netdata plugin-ipc --output json` shows enabled and disabled
  tool state.
- `codacy-analysis --help`, `codacy-analysis init --help`, and
  `codacy-analysis analyze --help` show local support for discovery, automatic
  configuration, inspect mode, dependency installation, JSON/SARIF output, and
  tool scoping.
- `codacy/codacy-skills @ bb6e7fc75159f360efa27d89568078272048be93`
  documents the Cloud CLI / Analysis CLI split and the local-to-cloud import
  workflow.

Affected contracts and surfaces:

- Codacy Cloud repository tool and pattern settings.
- Optional `.codacy/` local analysis configuration.
- Optional GitHub Actions workflow for Codacy client-side analysis upload.
- SOW lifecycle artifacts.
- No SDK protocol, API, wire format, transport behavior, or public integration
  contract is expected to change.

Existing patterns to reuse:

- Existing scanner workflow style from `.github/workflows/static-analysis.yml`,
  including pinned actions, explicit permissions, and safe SARIF upload guards.
- Existing scanner cleanup tracking model from `SOW-0010` for pre-existing
  static-analysis findings.
- Codacy skill guidance: use Cloud CLI for remote tool/pattern state and
  Analysis CLI for local analysis/configuration.

Risk and blast radius:

- Remote Codacy settings may change issue counts and PR status checks.
- New workflow files can affect GitHub Actions on pushes and pull requests.
- Token handling is the main security risk; workflows must reference
  `secrets.CODACY_API_TOKEN` or `secrets.CODACY_PROJECT_TOKEN` without recording
  or exposing raw values.
- Local Codacy generated files can be large/noisy; only stable configuration
  should be committed.

Sensitive data handling plan:

- Durable artifacts will not include raw tokens, credentials, session cookies,
  API keys, personal account details, customer data, private endpoints, or
  proprietary incident details.
- SOW evidence uses repository names, commit hashes, aggregate issue counts,
  tool names, and command names only.
- Codacy account output containing personal data is treated as transient CLI
  output and will not be copied into durable artifacts.
- GitHub secrets will be checked by name only; secret values will not be read or
  written.

Implementation plan:

1. Discover the stack with `codacy-analysis discover`.
2. Fetch or initialize local Codacy configuration and inspect available tools.
3. Enable relevant Codacy Cloud tools and broad security/high-signal patterns
   for C, Go, Rust, Shell, Markdown, YAML/workflows, dependencies, and agent
   instruction files.
4. Add a GitHub Actions workflow for Codacy client-side analysis if needed,
   guarded so forks and missing secrets do not expose credentials or fail
   unrelated checks.
5. Trigger or prepare Codacy reanalysis after configuration changes.

Validation plan:

- Run `codacy tools gh netdata plugin-ipc --output json` before and after.
- Run `codacy-analysis discover --output-format json`.
- Run `codacy-analysis analyze --inspect --output-format json`.
- If a stable `.codacy/codacy.config.json` is created, validate it with `jq`.
- Parse all GitHub workflow YAML files.
- Run `git diff --check`.
- Run `bash .agents/sow/audit.sh`.
- Check GitHub secret names only, not values.
- Check final `git status --short`.

Artifact impact plan:

- AGENTS.md: likely unaffected; scanner operations remain SOW-specific.
- Runtime project skills: likely unaffected; Codacy skills are global/shared,
  not project-local runtime skills.
- Specs: unaffected because no protocol/API behavior changes.
- End-user/operator docs: likely unaffected because SDK integration guidance
  does not change.
- End-user/operator skills: likely unaffected because public integration
  behavior does not change.
- SOW lifecycle: this SOW will move to `.agents/sow/done/` with
  `Status: completed` in the same commit as any committed scanner artifacts.

Open-source reference evidence:

- `codacy/codacy-skills @ bb6e7fc75159f360efa27d89568078272048be93`
  - `skills/codacy-cloud-cli/SKILL.md`
  - `skills/codacy-analysis-cli/SKILL.md`
  - `skills/configure-codacy/SKILL.md`
  - `README.md`

Open decisions:

- None blocking. The user requested continuation after enabling Codacy and
  installing the Codacy CLI and skills.

## Implications And Decisions

1. Codacy strictness:
   - Selection: maximize relevant tool and pattern coverage; do not disable
     valid findings merely because they are numerous.
   - Reasoning: this matches the user's stated purpose of strongest possible
     static analysis for an AI-generated SDK.
   - Risk: Codacy issue counts and PR noise may increase. Existing findings
     must be handled through cleanup SOWs rather than hidden.

2. Token handling:
   - Selection: only reference GitHub secrets by name and never record raw
     Codacy tokens in files.
   - Reasoning: scanner upload requires credentials, but durable artifacts must
     remain public-safe.
   - Risk: if required GitHub secrets are absent, client-side upload cannot run
     until the secret is added.

3. Client-side tools:
   - Selection: enable relevant client-side tools and add CI support where
     Codacy Cloud cannot run them server-side.
   - Reasoning: Codacy explicitly treats client-side tools as a separate local
     or CI execution path.
   - Risk: some tools may require build artifacts or config files; validation
     must record any unsupported tool clearly.

## Plan

1. Discover the stack and inspect local Codacy tool capability.
2. Enable/configure relevant Codacy Cloud tools and patterns.
3. Add or update stable repo artifacts for Codacy client-side CI/configuration.
4. Validate locally and remotely, then close and commit the SOW.

## Execution Log

### 2026-06-02

- Created this SOW after the user asked to continue scanner/Codacy hardening.
- Installed and verified global Codacy skills and Codacy Analysis CLI before
  this SOW.
- Confirmed Codacy Cloud is already analyzing `netdata/plugin-ipc`.
- Discovered the repository stack with `codacy-analysis discover`; languages
  reported were C, Go, Markdown, Rust, Shell, and YAML.
- Fetched the existing Codacy Cloud configuration with
  `codacy-analysis init --remote gh netdata plugin-ipc` and backed it up under
  `/tmp`.
- Generated a broad local Codacy configuration with `codacy-analysis init
  --auto` using the security, high, warning, minor, error-prone, performance,
  best-practice, unused-code, compatibility, complexity, comprehensibility,
  code-style, and documentation filters.
- Merged the remote and broad generated configurations into
  `.codacy/codacy.config.json`, preserving the remote Opengrep/Semgrep rule
  coverage while adding the broader tool set.
- Imported the merged configuration into Codacy Cloud with
  `codacy tools gh netdata plugin-ipc --import -y`.
- Requested Codacy Cloud reanalysis for the repository.
- Installed Opengrep 1.22.0 locally because the Codacy Analysis CLI could not
  auto-install it; the release was checked against `opengrep/opengrep` tag
  `v1.22.0`, published 2026-05-19.
- Added `.github/workflows/codacy-analysis.yml` to run Codacy Analysis CLI in
  GitHub Actions, generate SARIF, upload the SARIF artifact, and upload SARIF
  to GitHub Code Scanning.
- Pinned the workflow's Opengrep download to
  `opengrep/opengrep` `v1.22.0` and the SHA-256 checksum
  `45bcd58440e397ed52c50e953ccf5948909ea77087c9186fc7d277216f62e319`.
- Added `.codacy/.gitignore` so generated local Codacy files remain out of the
  repository.
- Added `.env` to the root `.gitignore` after the user reported storing
  `CODACY_PROJECT_TOKEN` there. The `.env` file was not read, copied, staged,
  or committed.
- Updated `SOW-0010` with the Codacy finding backlog so cleanup remains
  explicit and separate from scanner enablement.

## Validation

Acceptance criteria evidence:

- Codacy Cloud has the relevant repository tools enabled: Agentlinter, Checkov,
  Cppcheck, Flawfinder, Lizard, markdownlint, Opengrep, Revive, ShellCheck,
  Spectral, and Trivy.
- Codacy Cloud still lists Aligncheck, Clang-Tidy, Deadcode, GolangCI-Lint,
  Gosec, Staticcheck, and remark-lint as disabled. This is intentional for this
  SOW because the current Codacy Analysis CLI path cannot run those legacy
  client-side tools, and the existing GitHub scanner stack already covers
  Gosec and Staticcheck separately.
- `.codacy/codacy.config.json` contains 11 tools and 3,139 enabled patterns
  with no global excludes.
- `.github/workflows/codacy-analysis.yml` runs local Codacy Analysis CLI and
  uploads SARIF to GitHub Code Scanning without needing a Codacy token.
- GitHub repository secret enumeration returned no repository-level Actions
  secret names, so no token-dependent GitHub Actions path was added.
- `.env` is ignored by git and not staged.

Tests or equivalent validation:

- `jq empty .codacy/codacy.config.json` passed.
- `codacy-analysis analyze . --inspect --output-format json` produced 11 ready
  configured tools and zero unavailable configured tools.
- `codacy-analysis analyze . --install-dependencies --output-format json`
  produced 6,666 existing findings and zero tool errors before Opengrep was
  installed.
- `codacy-analysis analyze . --output-format sarif` produced valid SARIF
  version 2.1.0 with 8 result-bearing runs and 7,736 existing findings:
  Agentlinter 142, Lizard 859, Revive 226, Semgrep/Opengrep 1,070,
  cppcheck 515, flawfinder 1,525, markdownlint 3,344, and ShellCheck 55.
- Ruby YAML parsing passed for GitHub workflows and `.clang-tidy`.
- `actionlint` passed for the GitHub workflows.
- `git diff --check` passed.
- `bash .agents/sow/audit.sh` passed.

Real-use evidence:

- Codacy Cloud accepted the imported configuration and reported the expanded
  enabled tool set.
- Codacy Cloud reanalysis completed against the latest commit available to
  Codacy at that time and reported 2,448 issues, up from the earlier 1,725
  baseline because more tools and patterns are now active.
- Local Codacy Analysis CLI inspected all configured tools successfully and
  produced SARIF suitable for GitHub Code Scanning.

Reviewer findings:

- No external reviewer was requested for this SOW. Local validation found one
  supply-chain hardening issue in the new workflow: the Opengrep binary
  download was pinned by version but not by hash. The workflow now verifies the
  SHA-256 before executing the binary.

Same-failure scan:

- Checked Codacy Cloud tool state after import to verify the expanded tool set
  stayed enabled.
- Checked local Codacy inspect output to verify no configured tool is
  unavailable.
- Checked GitHub repository secret names and root git status so the local
  `CODACY_PROJECT_TOKEN` in `.env` cannot be committed accidentally.
- Mapped all existing Codacy findings into `SOW-0010`; no finding class was
  hidden by disabling broad valid scanner coverage.

Sensitive data gate:

- No raw Codacy token, personal account output, customer data, private
  endpoint, API key, or session credential was written to durable artifacts.
- The root `.env` file was not read and is ignored by git.
- The workflow does not echo token-bearing environment variables and does not
  require repository secrets.

Artifact maintenance gate:

- AGENTS.md: unchanged. The existing SOW, git, and secret-handling rules already
  cover this scanner configuration work.
- Runtime project skills: unchanged. Codacy skills are installed globally/shared
  for the workstation; this repository still has no runtime `project-*` skill
  that needs scanner workflow guidance.
- Specs: unchanged. This SOW changes scanner configuration and CI reporting
  only; protocol, API, wire format, transport behavior, and operational SDK
  guarantees are unchanged.
- End-user/operator docs: unchanged. SDK integration guidance and public
  operator behavior are unchanged.
- End-user/operator skills: unchanged. `docs/netipc-integrator-skill.md`
  describes SDK integration behavior, which did not change.
- SOW lifecycle: `SOW-0010` was updated with the Codacy finding backlog, this
  SOW is marked `completed`, and it is moved to `.agents/sow/done/` with the
  scanner artifacts in the same commit.

Specs update:

- No spec update was needed because the work changes scanner coverage and CI
  reporting only.

Project skills update:

- No project skill update was needed because no new repeatable
  repository-specific operating procedure was introduced beyond the committed
  workflow and Codacy config.

End-user/operator docs update:

- No end-user/operator docs update was needed because public SDK usage and
  operator workflows are unchanged.

End-user/operator skills update:

- No end-user/operator skill update was needed because docs/spec behavior did
  not change.

Lessons:

- Codacy Analysis CLI local configuration and Codacy Cloud configuration are
  separate; committing `.codacy/codacy.config.json` is useful for local/CI
  analysis, while Cloud changes require `codacy tools ... --import`.
- Opengrep was not auto-installable through the Codacy Analysis CLI in this
  environment, so CI needs a pinned binary plus checksum verification.
- Broad scanner enablement exposes a real existing backlog. Keep enablement and
  cleanup separate so scanner strength is not reduced just to make the initial
  rollout quiet.

Follow-up mapping:

- Existing findings from local static analysis and Codacy analysis are tracked
  by `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md`.
- No other follow-up item remains from this SOW.

## Outcome

Completed.

The repository now has a committed Codacy local configuration, a GitHub Actions
workflow that runs Codacy Analysis CLI and uploads SARIF to GitHub Code
Scanning, expanded Codacy Cloud tool coverage, pinned Opengrep installation with
checksum verification, and `.env` ignored so the local Codacy project token is
not committed.

## Lessons Extracted

No reusable project runtime skill was created. The durable lesson is recorded
above and in `SOW-0010`: scanner enablement should stay strict, while existing
findings move through a separate cleanup SOW.

## Followup

Tracked by `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md`.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
