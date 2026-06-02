# SOW-0009 - GitHub Security Scanning

## Status

Status: completed

Sub-state: GitHub scanner automation is complete; Supply Chain Security, CodeQL, and Static Analysis first-run regressions were repaired and validated locally.

## Requirements

### Purpose

Provide the strongest practical GitHub-based code scanning and static analysis setup for this SDK, aligned with security by design, maintainability, and cross-language quality across C, Rust, and Go.

### User Request

The user asked whether the SDK can have the strongest possible GitHub code scanners and static code analysis, then approved implementation.

### Assistant Understanding

Facts:

- The repository is `netdata/plugin-ipc`, public, default branch `main`.
- No `.github/` automation exists yet in this repository.
- The SDK includes C, Rust, and Go surfaces:
  - CMake declares C as the project language and C11 as the standard in `CMakeLists.txt:1` and `CMakeLists.txt:12`.
  - The Rust crate is `src/crates/netipc/Cargo.toml:1`.
  - Go modules exist at `src/go/go.mod:1`, `tests/fixtures/go/go.mod:1`, and `bench/drivers/go/go.mod:1`.
- Existing runtime safety scripts already cover ASAN/UBSAN, TSAN, Valgrind, and Go race detection in `tests/run-sanitizer-asan.sh:1`, `tests/run-sanitizer-tsan.sh:1`, `tests/run-valgrind.sh:1`, and `tests/run-go-race.sh:1`.
- GitHub documentation supports CodeQL advanced setup, compiled-language build steps, Dependency Review, Dependabot updates for `cargo`, `gomod`, and `github-actions`, secret scanning/push protection, and SARIF upload for third-party scanners.

Inferences:

- The highest-value setup is a layered scanner stack: GitHub-native code scanning and dependency gates, third-party SARIF uploads, language-specific static analysis, workflow linting, and runtime safety checks.
- Repository security settings such as secret scanning push protection may need to be enabled in GitHub settings or through an authenticated API path; workflow files alone cannot fully guarantee those settings are on.

Unknowns:

- Whether future GitHub-hosted runner execution will expose runner-specific package or service differences after the first pushed workflow run.

### Acceptance Criteria

- Add CodeQL advanced setup for C/C++, Go, and Rust with extended security and quality queries.
- Add Dependabot version update coverage for GitHub Actions, Cargo, and every Go module directory.
- Add Dependency Review gating for pull requests with the lowest vulnerability threshold available.
- Add third-party SARIF-producing security scanners and upload results to GitHub Code Scanning where permissions allow.
- Add language-specific static analysis for C, Rust, Go, GitHub Actions workflow files, and shell scripts without depending on external paid services.
- Add scheduled/runtime safety checks using the existing sanitizer, Valgrind, and race detector scripts.
- Validate YAML syntax and run available local validation commands.
- Complete the SOW lifecycle with status/directory consistency and commit/push only files touched by this work.

## Analysis

Sources checked:

- Project instructions in `AGENTS.md`.
- Existing SOW directories and project-local SOW template.
- `CMakeLists.txt`, Rust `Cargo.toml`, Go module files, and existing safety scripts.
- GitHub Docs:
  - CodeQL compiled-language build options: https://docs.github.com/en/code-security/reference/code-scanning/codeql/codeql-build-options-and-steps-for-compiled-languages
  - CodeQL advanced setup customization: https://docs.github.com/code-security/secure-coding/configuring-code-scanning
  - SARIF upload: https://docs.github.com/en/code-security/code-scanning/integrating-with-code-scanning/uploading-a-sarif-file-to-github
  - Dependabot supported ecosystems: https://docs.github.com/code-security/dependabot/ecosystems-supported-by-dependabot/supported-ecosystems-and-repositories
  - Dependency Review action configuration: https://github.com/actions/dependency-review-action
  - Secret scanning and push protection: https://docs.github.com/en/code-security/secret-scanning/enabling-secret-scanning-features
- Tool documentation:
  - OSV-Scanner GitHub Action and SARIF behavior: https://google.github.io/osv-scanner/github-action/
  - OSV-Scanner output formats: https://google.github.io/osv-scanner/output/
  - Semgrep CE CI/SARIF behavior: https://semgrep.dev/docs/deployment/oss-deployment
  - OpenSSF Scorecard action: https://github.com/ossf/scorecard-action

Current state:

- `.github/` is absent, so no GitHub Actions, CodeQL, Dependency Review, or Dependabot configuration is committed.
- Local ShellCheck reports existing warning/info findings in scripts; this SOW will not silently broaden into a script cleanup task.

Risks:

- Strong scanners can surface pre-existing findings and make the first CI runs fail.
- Third-party scanner SARIF uploads require `security-events: write`; uploads can be unavailable on forked pull requests with read-only tokens.
- Go version `1.25` depends on current GitHub runner/setup-go availability.
- External scanner tools installed at CI runtime can change behavior over time when installed with `@latest`.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The repository has no GitHub scanner automation despite being a cross-language SDK. Evidence: `.github/` is absent and `git status` shows no tracked workflow/config files.
- The existing implementation has multiple analyzable surfaces: C libraries/tests via CMake, a Rust crate, three Go modules, shell scripts, and GitHub workflow files once created.

Evidence reviewed:

- `CMakeLists.txt:1` and `CMakeLists.txt:12` establish C/C11 build context.
- `src/crates/netipc/Cargo.toml:1` establishes the Rust crate.
- `src/go/go.mod:1`, `tests/fixtures/go/go.mod:1`, and `bench/drivers/go/go.mod:1` establish Go module roots.
- `tests/run-sanitizer-asan.sh:1`, `tests/run-sanitizer-tsan.sh:1`, `tests/run-valgrind.sh:1`, and `tests/run-go-race.sh:1` provide existing runtime safety validation paths.
- GitHub official docs listed in `## Analysis` establish supported GitHub scanner mechanisms.
- Open-source reference evidence listed below shows established projects using CodeQL extended queries, Dependabot grouping, and Scorecard SARIF upload.

Affected contracts and surfaces:

- New `.github/` repository automation files.
- GitHub Security tab/code scanning results.
- Pull request CI checks.
- Scheduled scanner/runtime-safety jobs.
- SOW lifecycle artifacts.
- No SDK API, protocol wire format, transport behavior, or generated SDK artifacts are changed.

Existing patterns to reuse:

- Existing project build entry points: `cmake -S . -B build`, `cmake --build`, `ctest`.
- Existing runtime safety scripts instead of duplicating sanitizer/Valgrind/race logic inside workflow YAML.
- Project preference for transparent shell command output in existing scripts.
- OSS references for CodeQL extended queries and SARIF upload patterns.

Risk and blast radius:

- CI noise risk: strict scanners can report pre-existing issues. The initial design separates core gates from reporting-heavy scanners and avoids paid service dependencies.
- Fork PR risk: SARIF uploads can fail on read-only tokens. Upload steps will be guarded where practical.
- Runtime cost risk: sanitizer and Valgrind checks can be expensive. They will be scheduled and manually runnable, with focused PR triggers.
- Security risk: scanner workflows must use minimal GitHub token permissions and avoid `pull_request_target`.
- Product/API risk: none expected because no SDK source, protocol, API, or docs behavior changes.

Sensitive data handling plan:

- No raw secrets, credentials, bearer tokens, SNMP communities, customer data, personal data, private endpoints, or proprietary incident details are needed.
- SOW evidence cites file paths, line numbers, public docs, and public OSS repositories only.
- GitHub authentication status was checked without recording token values.
- New workflow comments and SOW content will not include user personal names or sensitive local details.

Implementation plan:

1. Add GitHub-native security configuration: CodeQL advanced setup, Dependabot, and Dependency Review.
2. Add third-party supply-chain/SARIF scanning: OpenSSF Scorecard, Semgrep CE, and OSV-Scanner.
3. Add language static analysis: C clang-tidy/cppcheck/flawfinder, Rust fmt/clippy/audit/deny, Go vet/staticcheck/gosec/govulncheck, workflow linting, and ShellCheck error-level scanning.
4. Add runtime safety workflow that reuses existing ASAN/UBSAN, TSAN, Valgrind, and Go race scripts.
5. Validate YAML syntax, workflow linting where possible, SOW audit, and selected local checks; then commit and push exact touched files.

Validation plan:

- Parse all new YAML files with Ruby `YAML.load_file`.
- Run local ShellCheck at error severity on scripts to verify the chosen shell gate does not fail on existing warning/info backlog.
- Run local SOW audit.
- Run `git diff --check`.
- Run available local language validation that is practical without adding unrelated tool installations.
- After push, inspect GitHub Actions/Code Scanning status where available.

Artifact impact plan:

- AGENTS.md: likely unaffected; this work adds repository automation, not workflow policy.
- Runtime project skills: likely unaffected; no reusable project-specific operator procedure changes are expected.
- Specs: unaffected because protocol/API/wire behavior does not change.
- End-user/operator docs: likely unaffected because this is repository CI/security automation, not SDK integration guidance.
- End-user/operator skills: likely unaffected because public integration guidance does not change.
- SOW lifecycle: this SOW will move from current to done with `Status: completed` in the same commit as the workflow/config additions.

Open-source reference evidence:

- `libbpf/libbpf @ dcaac95035044ff7e59bcaa2da4b9ae7f0a78a97`, `.github/workflows/codeql.yml:37` uses CodeQL initialization and `.github/workflows/codeql.yml:41` adds `security-extended,security-and-quality`.
- `aya-rs/aya @ 2453204a9b07a3a92d538f7cbe22582eaef432e1`, `.github/dependabot.yml:4` configures Dependabot and `.github/dependabot.yml:6` plus `.github/dependabot.yml:14` cover Cargo and GitHub Actions.
- `open-telemetry/opentelemetry-cpp @ 83c135d80d000f4f132351f133e84364320bbfa6`, `.github/workflows/ossf-scorecard.yml:31` uses OpenSSF Scorecard, `.github/workflows/ossf-scorecard.yml:33` emits SARIF, and `.github/workflows/ossf-scorecard.yml:49` uploads to code scanning.

Open decisions:

- None blocking. The user approved implementation after the scanner stack recommendation.

## Implications And Decisions

1. Scanner strictness:
   - Selection: use strong gates for GitHub-native dependency/code scanning and language checks; use SARIF/reporting workflows for broader third-party scanners where findings may need triage.
   - Reasoning: this catches security issues early while avoiding a first-run CI deadlock from unknown pre-existing third-party scanner findings.
   - Risk: existing findings can still fail deterministic jobs such as CodeQL, Dependency Review, clippy, gosec, or clang-tidy.

2. Secret scanning and push protection:
   - Selection: record that GitHub settings must be enabled outside workflow files; do not fake this with a local workflow.
   - Reasoning: GitHub secret scanning/push protection is a repository/org security feature, not something a committed workflow can fully enforce.
   - Risk: until repository settings are confirmed, committed workflows improve scanning but do not guarantee push-time secret blocking.

3. ShellCheck scope:
   - Selection: gate error-level ShellCheck now; do not remediate existing warning/info findings in this SOW.
   - Evidence: local full ShellCheck found existing warning/info findings across many existing scripts, including the SOW audit script and current transparent `printf` pattern.
   - Reasoning: changing many scripts is unrelated to adding scanner automation and increases regression risk.
   - Risk: ShellCheck warnings remain visible only when run manually with warning severity.

## Plan

1. Create `.github` scanner configs and workflows.
2. Validate new YAML and local scanner commands that are available.
3. Update SOW validation/outcome, move it to done, commit exact files, push `main`.

## Execution Log

### 2026-06-02

- Created this SOW after the user approved implementation.
- Confirmed no existing `.github/` automation.
- Confirmed public repository metadata and default branch `main`.
- Researched current GitHub docs and public OSS workflow examples before implementation.
- Local ShellCheck full run found existing warnings/info findings; selected error-level shell gate for initial scanner rollout.
- Added `.github/codeql.yml`, `.github/dependabot.yml`, and four GitHub Actions workflows for CodeQL, Dependency Review, static analysis, supply-chain security, and runtime safety.
- Added `.clang-tidy` for C static-analysis configuration.
- Installed local scanner tools: `actionlint`, `staticcheck`, `govulncheck`, `gosec`, `osv-scanner`, `semgrep`, `cargo-audit`, `cargo-deny`, `cppcheck`, and `flawfinder`.
- Enabled GitHub repository secret scanning, AI secret detection, non-provider secret patterns, push protection, and secret validity checks through the GitHub repository API.
- Confirmed Dependabot security updates and Dependabot vulnerability alerts are enabled through the GitHub API.
- Updated `rand` from `0.9.2` to `0.9.3` in `src/crates/netipc/Cargo.lock` after OSV/RustSec reported `RUSTSEC-2026-0097`.
- Created `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md` to track pre-existing scanner findings that are not scanner configuration work.

## Validation

Acceptance criteria evidence:

- CodeQL advanced setup added in `.github/workflows/codeql.yml` and `.github/codeql.yml` for C/C++, Go, and Rust, with `security-extended` and `security-and-quality` queries.
- Dependabot coverage added in `.github/dependabot.yml` for GitHub Actions, Cargo, and all three Go modules.
- Dependency Review gating added in `.github/workflows/dependency-review.yml` with `fail-on-severity: low` and all dependency scopes.
- Third-party SARIF scanning added in `.github/workflows/supply-chain-security.yml` for Semgrep CE, OSV-Scanner, and OpenSSF Scorecard.
- Language-specific static analysis added in `.github/workflows/static-analysis.yml` for C, Rust, Go, GitHub Actions workflow files, and shell scripts.
- Runtime safety workflow added in `.github/workflows/runtime-safety.yml`, reusing the existing sanitizer, Valgrind, and Go race scripts.
- Repository security settings were enabled through the GitHub API: secret scanning, AI secret detection, non-provider patterns, push protection, and validity checks.
- Dependabot vulnerability alerts were confirmed enabled through `gh api -i repos/netdata/plugin-ipc/vulnerability-alerts` returning `204 No Content`.

Tests or equivalent validation:

- `ruby -e 'require "yaml"; Dir[".github/**/*.yml"].sort.each { |f| YAML.load_file(f) }; YAML.load_file(".clang-tidy")'` passed.
- `/home/costa/.local/bin/actionlint` passed.
- `shellcheck --severity=error diff-netdata-vendor.sh vendor-to-netdata.sh tests/*.sh .agents/sow/audit.sh` passed.
- `git diff --check` passed.
- `bash .agents/sow/audit.sh` passed.
- C gate passed: `cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, `cmake --build build-static --parallel`, scoped `clang-tidy`, `cppcheck --enable=warning,performance,portability --error-exitcode=1`, and `flawfinder --minlevel=5 --error-level=5`.
- Rust gate passed: `cargo fmt --manifest-path src/crates/netipc/Cargo.toml --all --check`, `cargo test --manifest-path src/crates/netipc/Cargo.toml --all-targets --all-features --no-run`, `cargo clippy --manifest-path src/crates/netipc/Cargo.toml --all-targets --all-features -- -D clippy::correctness -D clippy::suspicious`, `cargo audit`, and `cargo deny check advisories bans sources`.
- Go hard gate passed for `src/go`, `tests/fixtures/go`, and `bench/drivers/go`: `go test ./...`, `go vet ./...`, and `govulncheck ./...`.
- OSV dependency scan passed after the Rust lockfile update: `/home/costa/.local/bin/osv-scanner scan --recursive .`.
- OSV SARIF generation passed with `/home/costa/.local/bin/osv-scanner scan --recursive --format sarif --output-file /tmp/plugin-ipc-osv.sarif .`; the SARIF file reports version `2.1.0`.
- Semgrep SARIF generation passed with `/home/costa/.local/bin/semgrep scan --metrics=off --config p/default --config p/security-audit --config p/secrets --sarif --output /tmp/plugin-ipc-semgrep.sarif .`; the SARIF file reports version `2.1.0`.

Real-use evidence:

- Local scanner tool versions were verified:
  - `actionlint v1.7.12`
  - `staticcheck 2026.1 (v0.7.0)`
  - `govulncheck v1.3.0`, vulnerability DB updated `2026-06-01`
  - `gosec` built from latest source, version output `dev`
  - `osv-scanner 2.3.8`
  - `semgrep 1.164.0`
  - `cargo-audit 0.22.1`
  - `cargo-deny 0.19.8`
  - `cppcheck 2.20.0`
  - `flawfinder 2.0.20`
- GitHub repository security settings were verified after update: `secret_scanning`, `secret_scanning_ai_detection`, `secret_scanning_non_provider_patterns`, `secret_scanning_push_protection`, `secret_scanning_validity_checks`, and `dependabot_security_updates` are `enabled`.

Reviewer findings:

- No external reviewer was requested for this SOW.
- Local scanner review found pre-existing findings that should not be hidden:
  - `staticcheck` reports four findings in `src/go`: `lookup.go:758`, `lookup.go:1274`, `lookup.go:1278`, and `uds.go:667`.
  - `gosec` reports 115 findings in `src/go`, 18 in `tests/fixtures/go`, and 28 in `bench/drivers/go`.
  - `semgrep` reports 84 findings across tracked files.
  - Broader C scanner runs outside the initial hard gate report test-source findings.
- Handling: the initial workflows hard-gate clean deterministic checks, upload/report noisy scanner findings, and track cleanup in `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md`.

Same-failure scan:

- Searched all Go modules with local `staticcheck`; only `src/go` reports findings, while `tests/fixtures/go` and `bench/drivers/go` are clean for `staticcheck`.
- Searched all Go modules with local `gosec`; all three modules report findings, so they are tracked together in `SOW-0010`.
- Semgrep scanned 148 git-tracked files and reported 84 findings; these are tracked in `SOW-0010`.

Sensitive data gate:

- Durable artifacts contain no raw secrets, credentials, bearer tokens, SNMP communities, community member names, customer names, customer identifiers, personal data, non-private customer-identifying IPs, private endpoints, or proprietary incident details.
- GitHub API output recorded only repository feature status values, not tokens or secret values.

Artifact maintenance gate:

- AGENTS.md: no update needed; this work adds scanner automation and does not change project workflow policy or responsibilities.
- Runtime project skills: no update needed; no reusable project-specific workflow procedure was discovered beyond commands already captured in this SOW and `AGENTS.md`.
- Specs: no update needed; no protocol, API, wire format, transport behavior, data format, or operational SDK guarantee changed.
- End-user/operator docs: no update needed; public SDK integration behavior and examples did not change.
- End-user/operator skills: no update needed; `docs/netipc-integrator-skill.md` is unaffected because public integration guidance did not change.
- SOW lifecycle: this SOW is marked `Status: completed` and will be moved to `.agents/sow/done/` in the same commit as the scanner files and `SOW-0010`.

Specs update:

- No spec update needed; scanner automation and dependency lockfile maintenance did not change protocol/API behavior.

Project skills update:

- No runtime project skill update needed; no recurring repository operation changed beyond the scanner configuration committed here.

End-user/operator docs update:

- No end-user/operator docs update needed; no SDK usage, integration, command, schema, or workflow visible to SDK consumers changed.

End-user/operator skills update:

- No end-user/operator skill update needed; public integration guidance did not change.

Lessons:

- Strong initial scanner rollouts need separate reporting and hard-gating lanes when existing debt is unknown; otherwise the first CI run can deadlock before the repository has a cleanup plan.
- Local SARIF command validation catches CLI drift such as OSV-Scanner deprecating `--output` in favor of `--output-file`.

Follow-up mapping:

- Pre-existing static-analysis findings are tracked by `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md`.
- GitHub delegated bypass and delegated alert dismissal remain disabled intentionally because they are governance/approval controls, not scanner coverage, and should not be enabled without a maintainer process decision.

## Outcome

Completed. The repository now has GitHub-native code/dependency/security scanning, third-party SARIF scanners, language static-analysis workflows, runtime safety workflows, local scanner tools installed, and scanner-related GitHub repository settings enabled.

## Lessons Extracted

See `## Validation` lessons.

## Followup

Tracked by `.agents/sow/pending/SOW-0010-20260602-static-analysis-finding-cleanup.md`.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.

## Regression - 2026-06-02

What broke:

- First pushed GitHub run `26812274331` failed in `.github/workflows/supply-chain-security.yml`.
- OSV-Scanner job failed during tool installation because `github.com/google/osv-scanner/v2@v2.3.8` requires Go `>=1.26.2`, while the workflow installed Go `1.25.10` from `src/go/go.mod`.
- OpenSSF Scorecard job failed while publishing results because Scorecard rejects workflows with global `security-events: write`; the workflow had that permission at top level.
- First pushed GitHub run `26812274378` failed in `.github/workflows/codeql.yml`.
- Rust CodeQL failed because Rust does not support manual build mode.
- C/C++ CodeQL failed because the workflow built every CMake target and hit an existing GCC preprocessor issue in `tests/fixtures/c/test_stress.c:840`.
- First pushed final Static Analysis run `26812569114` failed in the C Static Analysis job because it also built every CMake target before running library-scoped analyzers.

Evidence:

- `gh run view 26812274331 --repo netdata/plugin-ipc --json jobs` showed `OSV-Scanner` and `OpenSSF Scorecard` failed while `Semgrep CE` succeeded.
- `gh run view 26812274331 --repo netdata/plugin-ipc --log-failed` showed `requires go >= 1.26.2 (running go 1.25.10; GOTOOLCHAIN=local)`.
- The same log showed Scorecard publish failed with `global perm is set to write: permission for security-events is set to write`.
- `gh run view 26812274378 --repo netdata/plugin-ipc --json jobs` showed `Analyze Rust` and `Analyze C/C++` failed while `Analyze Go` succeeded.
- The CodeQL Rust log showed `Rust does not support the manual build mode. Please try using one of the following build modes instead: none`.
- The CodeQL C/C++ log showed `tests/fixtures/c/test_stress.c:840:46: error: missing binary operator before token "("`.
- `gh run view 26812569114 --repo netdata/plugin-ipc --json jobs` showed `C Static Analysis` failed at `Build C targets`, while Go and workflow/shell jobs completed successfully.

Why previous validation missed it:

- Local OSV ran under the workstation Go toolchain, which is newer than the SDK module `go.mod` version used by `actions/setup-go`.
- Local `actionlint` verifies workflow syntax but cannot validate Scorecard's runtime publishing restrictions.
- Local CodeQL was not run; `actionlint` cannot validate per-language CodeQL build-mode restrictions.
- Local full CMake used the workstation compiler environment, while the GitHub C/C++ CodeQL job used the hosted runner compiler path and built all tests.
- The Static Analysis C job had the same over-broad build step as the original CodeQL C/C++ job.

Repair plan:

1. Use Go `1.26.x` only for the OSV-Scanner tool job.
2. Keep top-level workflow permissions read-only and move `security-events: write` to SARIF-uploading jobs.
3. Use CodeQL `build-mode: none` for Rust.
4. Limit C/C++ CodeQL manual build to the C library targets.
5. Limit Static Analysis C build to the C library targets before running library-scoped analyzers.
6. Re-run YAML parse, `actionlint`, C library target build, SOW audit, and `git diff --check`.
7. Commit and push the repair, then inspect the new GitHub run.

Validation:

- `ruby -e 'require "yaml"; Dir[".github/**/*.yml"].sort.each { |f| YAML.load_file(f) }; YAML.load_file(".clang-tidy")'` passed after the repair.
- `/home/costa/.local/bin/actionlint` passed after the repair.
- `/home/costa/.local/bin/osv-scanner scan --recursive --format sarif --output-file /tmp/plugin-ipc-osv.sarif .` passed and produced SARIF `2.1.0`.
- `bash .agents/sow/audit.sh` passed with this SOW reopened in `current/`.
- `git diff --check` passed.
- CodeQL repair validation passed: YAML parsing, `actionlint`, local CMake build of `netipc_protocol`, `netipc_uds`, `netipc_shm`, and `netipc_service`, SOW audit, and `git diff --check`.
- Static Analysis C repair validation passed: YAML parsing, `actionlint`, local CMake build of `netipc_protocol`, `netipc_uds`, `netipc_shm`, and `netipc_service`, scoped `clang-tidy`, `cppcheck`, `flawfinder`, SOW audit, and `git diff --check`.

Artifact updates:

- Updated `.github/workflows/supply-chain-security.yml`, `.github/workflows/codeql.yml`, and `.github/workflows/static-analysis.yml`.
- No specs, public docs, or project skills changed because the repair is CI configuration only.
