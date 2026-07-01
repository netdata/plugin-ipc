# SOW-0029 - CI Scanner Readiness

## Status

Status: completed

Sub-state: continued CodeQL regression fixed with source-level POSIX session-thread handoff change, low-priority validation policy added, and local validation completed.

## Requirements

### Purpose

Make `plugin-ipc` safe enough to resume Netdata vendoring preflight by clearing actionable CI and scanner blockers while preserving the current SDK functional contract. The work should use the long-term-best fix in every case, except the Go module directive must remain compatible with Netdata's current `go 1.26.0` requirement.

### User Request

The user asked to verify all issues and create a plan to fix them. After reviewing the verified findings, the user decided:

- use the long-term-best fix for all actionable cases;
- do not raise Go module directives to `go 1.26.4`, because Netdata currently needs `go 1.26.0` compatibility;
- leave the Go stdlib SCA alerts open;
- proceed with all other fixes because the SDK functional behavior should remain the same.

### Assistant Understanding

Facts:

- `origin/main` is `b4dfe405e1f99be417c21a9c0478aba2f64facaa`.
- Local `HEAD` is one unpushed workflow-policy commit ahead of `origin/main`, so exact local `HEAD` does not yet have GitHub CI evidence.
- GitHub checks for `b4dfe405e1f99be417c21a9c0478aba2f64facaa` show failures in `Static Analysis`, `Supply Chain Security`, and `Codacy Local Analysis`.
- GitHub code scanning has open CodeQL warnings, gosec G115 errors, one Semgrep `chmod` finding, and one OSV advisory for Rust `anyhow`.
- Codacy Cloud has Go stdlib SCA findings from `src/go/go.mod` declaring `go 1.26.0`; the user explicitly chose to leave these open for Netdata compatibility.
- GitHub Dependabot and secret-scanning open-alert queries returned no open alerts.

Inferences:

- The actionable fixes can be limited to formatting, Rust lockfile dependency refresh, C POSIX socket permission creation, Go checked integer conversions, and C CodeQL warning cleanup.
- The Go stdlib SCA alerts are a conscious compatibility exception, not a blocker for this SOW.

Unknowns:

- Whether all GitHub code-scanning alerts will auto-close only after the next pushed SARIF uploads; local validation can prove the expected scanner classes are fixed before push.

### Acceptance Criteria

- Rust formatting passes with `cargo fmt --manifest-path src/crates/netipc/Cargo.toml --all --check`.
- OSV no longer reports `RUSTSEC-2026-0190` for `anyhow`.
- `flawfinder --minlevel=5 --error-level=5 src/libnetdata/netipc tests` no longer reports the POSIX UDS `chmod` finding.
- Local gosec against `src/go` no longer reports the verified G115 findings.
- Local Codacy Analysis no longer reports the Opengrep `chmod` issue; any known Codacy tool adapter error is recorded separately.
- CodeQL warning classes are fixed locally where practical or explicitly triaged with evidence in this SOW.
- Go module directives remain at `go 1.26.0`; Codacy Cloud Go stdlib SCA alerts are left open by user decision.
- SDK public API and functional behavior remain unchanged.

## Analysis

Sources checked:

- `.agents/skills/project-netdata-vendoring/SKILL.md`
- `AGENTS.md`
- `.agents/sow/current/SOW-0015-20260605-codacy-scope-and-maintainability.md`
- `.agents/sow/pending/SOW-0027-20260629-netdata-vendor-memory-safety-update.md`
- `.github/workflows/static-analysis.yml`
- `.github/workflows/supply-chain-security.yml`
- `.github/workflows/codacy-analysis.yml`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c`
- `src/go/pkg/netipc/protocol/cgroups_lookup.go`
- `src/go/pkg/netipc/protocol/frame.go`
- `src/go/pkg/netipc/protocol/lookup_common.go`
- `src/crates/netipc/Cargo.lock`

Current state:

- `cargo fmt --check` fails on Rust formatting in `cgroups_cache.rs`, `raw.rs`, and `raw_unix_tests.rs`.
- `flawfinder` reports `chmod(path, S_IRUSR | S_IWUSR)` at `netipc_uds_lifecycle.c`.
- Local gosec reports 21 G115 findings across Go lookup/frame conversion paths.
- Local OSV reports `anyhow 1.0.102` affected by `RUSTSEC-2026-0190`; dry-run lockfile update moves only `anyhow` to `1.0.103`.
- Local Codacy Analysis reports one Opengrep `chmod` issue and the known Revive adapter invocation error `findings is not iterable`.
- Codacy Cloud reports 23 Go stdlib SCA findings on `go 1.26.0`; these are intentionally left open.

Risks:

- Replacing path `chmod()` with process `umask()` around `bind()` must restore the previous umask on every path; because `umask` is process-global, the implementation must keep the critical section minimal and serialized.
- Go conversion cleanup must preserve 32-bit and 64-bit behavior, including defensive overflow tests.
- CodeQL warnings may require source rewrites or documented suppressions; suppressions are acceptable only where the code invariant is proven and behavior should remain unchanged.
- Raising Go module directives would conflict with Netdata and is explicitly out of scope.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- CI is red because scanner and formatter gates detect several actionable hygiene/security issues.
- Some alerts are real code-quality problems (`chmod` TOCTOU pattern, unchecked-looking Go conversions, stale Rust lockfile package, Rust format drift).
- Some alerts are scanner limitations over already-defensive code or compatibility exceptions (CodeQL constant-comparison on portable overflow guards, Go stdlib SCA from required `go 1.26.0`).

Evidence reviewed:

- GitHub run list and check-runs for `b4dfe405e1f99be417c21a9c0478aba2f64facaa`.
- GitHub code-scanning open-alert list.
- Codacy Cloud repository, issues, and findings queries.
- Local reproductions with `cargo fmt --check`, `flawfinder`, `gosec`, `osv-scanner`, and `codacy-analysis`.

Affected contracts and surfaces:

- POSIX UDS listener socket creation mode.
- C protocol/service defensive overflow helper code.
- Go protocol checked conversion helpers and callers.
- Rust lockfile and formatting.
- CI/scanner status and SOW evidence.
- No public SDK API, wire format, transport semantics, or data format should change.

Existing patterns to reuse:

- Existing C overflow helpers such as `mul_would_overflow` and `nipc_lookup_add_u64_over_limit`.
- Existing Go checked conversion helpers in `lookup_common.go`.
- Existing local scanner workflows and SOW validation gates.
- Existing vendor preflight rule from `SOW-0028`.

Risk and blast radius:

- Low public API risk: intended behavior is unchanged.
- Medium low-level transport risk around POSIX socket mode creation; validation must include C build/tests and UDS tests.
- Low dependency risk: `anyhow` is only a lockfile transitive package in this crate lockfile; update is patch-level.
- Low Go behavior risk if conversions are centralized through checked helpers and existing tests pass.

Sensitive data handling plan:

- No secrets, credentials, customer data, private endpoints, or proprietary incident data are required.
- Durable artifacts will record paths, command names, commit hashes, and sanitized summaries only.
- Command output containing token placeholders, account email lists, or personal data will not be copied into durable artifacts.

Implementation plan:

1. Apply Rust formatting.
2. Update Rust lockfile package `anyhow` to `1.0.103`.
3. Replace POSIX UDS post-bind path `chmod()` with restrictive socket creation mode using a serialized temporary `umask(0177)` around `bind()`, restoring the previous umask immediately.
4. Refactor Go G115 sites through checked conversion helpers so the bounds are explicit and local to the conversion.
5. Rewrite or triage C CodeQL warning sites without changing behavior.
6. Run targeted and broad local validation.

Validation plan:

- `cargo fmt --manifest-path src/crates/netipc/Cargo.toml --all --check`
- Rust tests for the crate.
- OSV scan.
- CMake build plus CTest.
- `flawfinder --minlevel=5 --error-level=5 src/libnetdata/netipc tests`
- go test/vet/staticcheck/gosec under `src/go`.
- Codacy local analysis.
- SOW audit and whitespace checks.

Artifact impact plan:

- AGENTS.md: no expected update; workflow policy already added in `SOW-0028`.
- Runtime project skills: no expected update; workflow policy already added in `SOW-0028`.
- Specs: no expected update because SDK behavior and public contracts should remain unchanged.
- End-user/operator docs: no expected update because there is no public API change.
- End-user/operator skills: no expected update unless validation reveals a durable operator workflow change.
- SOW lifecycle: this SOW tracks the source readiness cleanup; `SOW-0027` remains the later Netdata vendor propagation SOW.

Open-source reference evidence:

- None. The work is driven by repository-local CI/scanner evidence and existing code invariants.

Open decisions:

- Resolved by user: use long-term-best fixes for actionable issues, keep `go 1.26.0`, and leave Go stdlib SCA alerts open.

## Implications And Decisions

1. Overall fix posture

- Decision: use long-term-best fixes, not quick suppressions, for actionable issues.
- Implication: fixes may touch shared helpers instead of adding local scanner comments everywhere.
- Risk: broader local validation is required because shared helpers affect multiple codec paths.

2. Go stdlib SCA findings

- Decision: do not raise Go module directives from `go 1.26.0` to `go 1.26.4`; leave those alerts open.
- Evidence: Netdata currently declares `go 1.26.0` and fails if compatibility is raised.
- Implication: Codacy Cloud will continue reporting Go stdlib SCA findings until Netdata can move.
- Risk: vendoring readiness must explicitly record this as accepted compatibility debt, not an accidentally ignored scanner result.

3. SDK behavior

- Decision: keep SDK functional behavior unchanged.
- Implication: no public API changes, wire-format changes, or semantic changes are allowed in this SOW.
- Risk: any fix that requires semantic change must stop and ask before implementation.

## Plan

1. Fix mechanical Rust format drift and Rust lockfile advisory.
2. Fix POSIX UDS socket permission creation without path `chmod()`.
3. Fix Go G115 conversion findings by centralizing checked conversions.
4. Clean up or evidence-triage C CodeQL warnings.
5. Validate locally, update this SOW, then commit.

## Execution Log

### 2026-07-01

- Created this SOW after the user selected long-term-best fixes and the Go `1.26.0` compatibility exception.
- Applied Rust formatting and updated `anyhow` in `src/crates/netipc/Cargo.lock` from `1.0.102` to `1.0.103`.
- Replaced POSIX UDS post-bind `chmod()` with a serialized temporary `umask(0177)` around `bind()` so the socket is created as owner read/write without a public permission window.
- Centralized Go checked integer conversions and removed the verified G115 findings without changing the Go public API.
- Kept portable C overflow checks, but wrapped checks that are only meaningful on 32-bit `size_t` platforms in `#if SIZE_MAX <= UINT32_MAX` so CodeQL does not flag constant-false branches on 64-bit builds.
- Moved the existing CodeQL stack-address invariant suppression onto the assignment line and kept the lifetime explanation.
- Updated `.codacy/codacy.config.json` so generated local Codacy excludes match `.codacy.yml`, and removed the broken Revive adapter from local Codacy analysis. Evidence: Revive reported 0 issues but always emitted `findings is not iterable`; Go checks remain covered by `go vet`, `staticcheck`, `govulncheck`, and `gosec`.

## Validation

Acceptance criteria evidence:

- Rust formatting: `cargo fmt --manifest-path src/crates/netipc/Cargo.toml --all --check` passed.
- Rust advisory: `osv-scanner scan --recursive --format json .` returned no vulnerabilities; `cargo audit --file src/crates/netipc/Cargo.lock --json` returned `vulnerabilities.found=false`.
- POSIX UDS scanner issue: `flawfinder --minlevel=5 --error-level=5 src/libnetdata/netipc tests` returned no level-5 hits.
- Go G115 scanner issue: `gosec -quiet -fmt json -exclude=G404 ./...` passed in `src/go`, `tests/fixtures/go`, and `bench/drivers/go`.
- Codacy local analysis: `codacy-analysis analyze . --install-dependencies --output-format json --output /tmp/plugin-ipc-codacy-final-clean.json --parallel-tools 2 --tool-timeout 900000` completed with 0 issues and 0 tool errors after the config cleanup.
- CodeQL warning classes: local CodeQL CLI was not installed, so full CodeQL closure still needs the next GitHub SARIF run; source-side changes address the verified classes by guarded 32-bit-only overflow checks and an inline lifetime invariant suppression.
- Go module directives were not raised; `go 1.26.0` compatibility debt remains an explicit user-approved exception.

Tests or equivalent validation:

- POSIX build: `cmake -S . -B build && cmake --build build` passed.
- POSIX CTest: `/usr/bin/ctest --test-dir build --output-on-failure` passed, 48/48 tests.
- Targeted UDS regression check after the socket-mode fix: `build/bin/test_uds` passed, 176/176 checks.
- Rust tests: `cargo test --manifest-path src/crates/netipc/Cargo.toml --all-targets --all-features` passed.
- Rust Clippy correctness gate: `cargo clippy --manifest-path src/crates/netipc/Cargo.toml --all-targets --all-features -- -D clippy::correctness -D clippy::suspicious` passed. Existing non-denied style warnings remain outside this SOW.
- Rust deny: `cargo deny --manifest-path src/crates/netipc/Cargo.toml check advisories bans sources` passed.
- Go static matrix: `go test`, `go vet`, `staticcheck`, `govulncheck`, and `gosec` passed in `src/go`, `tests/fixtures/go`, and `bench/drivers/go` using local Go `1.26.4`.
- C static gate: CMake debug compile database and static targets built; `clang-tidy -p build-static ...` and `cppcheck --enable=warning,performance,portability --error-exitcode=1 ...` completed with exit 0. Existing clang-tidy warnings are not configured as fatal in the workflow.
- Secrets scan: `semgrep scan --metrics=off --config p/secrets --sarif --output /tmp/plugin-ipc-semgrep-secrets.sarif .` completed with 0 findings.
- Windows validation: on `win11`, the MSYS CI-style Windows runtime build and targeted CTest slice passed, 12/12 tests.

Real-use evidence:

- The POSIX test binary verified that listener socket paths are still created as owner-only `0600`.
- The Windows runtime slice verified named-pipe, Windows SHM, typed service, cache, and cross-language interop paths after the source changes were copied to a dedicated `win11` test directory.

Reviewer findings:

- No external AI reviewers were run because the user did not explicitly request external reviewers for this SOW. Validation used local test, scanner, and CI-equivalent command evidence instead.

Same-failure scan:

- `rg` found no remaining production C `chmod()` calls under the touched POSIX UDS path. The remaining `chmod` match is in Rust SHM tests.
- `rg` found the C `SIZE_MAX` guard patterns still present, but the touched production instances are under `#if SIZE_MAX <= UINT32_MAX`, preserving 32-bit behavior while removing the 64-bit CodeQL constant-comparison shape.
- `rg` found `anyhow` in the lockfile at `1.0.103`; no `1.0.102` remains.
- `rg` found Go test-only `uint64(maxIntValue())` checks, but the production G115 findings were removed and gosec is clean.

Sensitive data gate:

- No raw secrets, credentials, customer data, private endpoints, or personal data were written to durable artifacts. The SOW records command names, paths, and sanitized tool summaries only.

Artifact maintenance gate:

- AGENTS.md: no update needed; this SOW did not change repository-wide assistant workflow beyond the already-completed vendoring preflight policy.
- Runtime project skills: no update needed; this SOW did not add reusable operator workflow beyond Codacy local config cleanup.
- Specs: no update needed; SDK API, wire formats, transport contracts, and data formats remain unchanged.
- End-user/operator docs: no update needed; no user-facing API, command, or integration behavior changed.
- End-user/operator skills: no update needed; no exported/operator skill behavior changed.
- SOW lifecycle: this SOW is marked completed and will be moved to `.agents/sow/done/` with the implementation commit. The Go stdlib SCA alerts are not a deferred fix; they are accepted compatibility debt by user decision.

Specs update:

- Not needed. The source behavior remains within the existing specs: no public API, protocol, wire-format, or transport semantics changed.

Project skills update:

- Not needed. The durable vendoring preflight policy was already added in `SOW-0028`; this SOW only applied that policy to current scanner findings.

End-user/operator docs update:

- Not needed. No end-user or operator-visible behavior changed.

End-user/operator skills update:

- Not needed. No output/reference skill contract changed.

Lessons:

- Scanner-compatible security fixes can still introduce real regressions: `umask(077)` created `0700` sockets, not `0600`; the targeted UDS test caught it.
- Codacy local config must be kept aligned with `.codacy.yml`; otherwise generated local analysis can scan excluded test files and produce avoidable tool timeouts.
- Broken duplicate scanner adapters should be removed from the local gate when stronger project-native gates already cover the same language.

Follow-up mapping:

- No new SOW is required from this work. The remaining Go stdlib SCA alerts are intentionally left open until Netdata can move beyond `go 1.26.0`.

Follow-up mapping:

- The pushed GitHub CodeQL run for this regression commit must be checked immediately after push. If the alert remains live on the new commit, reopen this SOW again as a continued regression.

## Outcome

Scanner-readiness cleanup is complete locally. The first pushed implementation left one live CodeQL suppression-placement alert; this regression fix moves the suppression into the official CodeQL-supported position without changing runtime behavior.

## Lessons Extracted

- For CodeQL C/C++ alert suppressions, `// codeql[query-id]` must be a standalone comment on the line before the alert, not an end-of-line comment.
- When local CodeQL is unavailable, pushed GitHub CodeQL is the first authoritative validation point; any suppression-only fix must be checked remotely immediately after push.

## Followup

None yet.

## Regression Log

See the appended regression section below.

## Regression - 2026-07-01 Remote CodeQL Suppression Placement

What broke:

- After commit `d00beebb0538b478a9e8515b1e75439bc1efb208` was pushed, GitHub Actions completed successfully, but GitHub Code Scanning still reported one live CodeQL alert:
  - rule: `cpp/stack-address-escape`
  - file: `src/libnetdata/netipc/src/service/netipc_service_posix_server.c`
  - line: assignment of `sctx->server = server`
  - commit: `d00beebb0538b478a9e8515b1e75439bc1efb208`

Evidence:

- `gh run list --repo netdata/plugin-ipc --commit d00beebb0538b478a9e8515b1e75439bc1efb208` showed all GitHub Actions workflows completed successfully.
- `gh api repos/netdata/plugin-ipc/code-scanning/alerts/7758` showed the `cpp/stack-address-escape` instance on the pushed commit.
- Official CodeQL documentation says `// codeql[query-id]` suppression comments must be on a blank line before the alert. The original fix placed the suppression on the assignment line, so CodeQL did not apply it.

Why previous validation missed it:

- Local CodeQL CLI is not installed, so the first authoritative CodeQL validation was the GitHub run after push.
- The local source review used the correct query id but the wrong comment placement.

Repair plan:

1. Move the `// codeql[cpp/stack-address-escape]` comment to its own line immediately before the assignment.
2. Keep the existing lifetime explanation in the source.
3. Run focused local validation and SOW audit.
4. Commit, push, and re-check GitHub Code Scanning for the pushed commit.

Validation plan:

- `cmake --build build --target netipc_service`
- `/usr/bin/ctest --test-dir build --output-on-failure -R service`
- `bash .agents/sow/audit.sh`
- `git diff --check`
- GitHub CodeQL check after push.

Validation results:

- `cmake --build build --target netipc_service` passed.
- `/usr/bin/ctest --test-dir build --output-on-failure -R service` passed, 9/9 tests.
- Local CodeQL validation was not available: `command -v codeql`, `~/.local/bin`, and `gh extension list` showed no CodeQL CLI or extension installed.

Artifact updates:

- AGENTS.md: no update needed; the workflow rule did not change.
- Runtime project skills: no update needed; no reusable project operation changed.
- Specs: no update needed; public API, wire format, transport semantics, and behavior did not change.
- End-user/operator docs: no update needed; no user-facing behavior changed.
- End-user/operator skills: no update needed.
- SOW lifecycle: this completed SOW was moved back to `current/`, marked `in-progress`, this regression section was appended after the original SOW narrative, then the SOW was marked `completed` again for the regression fix commit.

Outcome:

- The source now uses the CodeQL-documented suppression placement.
- Runtime behavior is unchanged.
- Remote GitHub CodeQL must be rechecked after this commit is pushed.

## Regression - 2026-07-01 Continued Stack Pointer Escape Alert

What broke:

- Commit `90305c116eae9b81202f2311c1636a4b1ac31a2c` moved the CodeQL suppression to the documented position and GitHub CodeQL completed successfully.
- GitHub Code Scanning marked the old alert `7758` fixed, but opened new alert `7725` for the same `cpp/stack-address-escape` rule at the same server-pointer assignment, now shifted to line 270.

Evidence:

- `gh api repos/netdata/plugin-ipc/code-scanning/alerts/7758` reported `state=fixed` at `2026-07-01T07:10:21Z`.
- `gh api repos/netdata/plugin-ipc/code-scanning/alerts/7725` reported a live `cpp/stack-address-escape` instance on commit `90305c116eae9b81202f2311c1636a4b1ac31a2c`.
- CodeQL C/C++ POSIX analysis for `90305c116eae9b81202f2311c1636a4b1ac31a2c` had `results_count=1`.

Why previous validation missed it:

- The previous fix relied on CodeQL suppression behavior. GitHub Code Scanning did not suppress this alert in the repository's current CodeQL setup.
- Local CodeQL is still unavailable, so remote GitHub CodeQL remains the first authoritative check.

Repair plan:

1. Stop storing the caller-owned POSIX server pointer in the heap session context.
2. Pass the server pointer and session context to the POSIX session thread through a stack start argument guarded by a mutex and condition variable.
3. Make the accept loop wait until the child thread copies those arguments before the stack object leaves scope.
4. Keep the existing session lifetime and shutdown behavior unchanged.
5. Validate POSIX service tests locally, close the SOW, push, and re-check GitHub Code Scanning.

Risk:

- This changes the POSIX session-thread start path. The runtime behavior should stay the same, but validation must cover service tests because a missed handoff synchronization bug could deadlock or race at session startup.

Validation plan:

- `cmake --build build --target netipc_service`
- `tests/run-low-priority.sh /usr/bin/ctest --test-dir build --output-on-failure -R service`
- SOW audit and `git diff --check`
- GitHub CodeQL after push

Additional user request during validation:

- The user asked that validation and benchmark scripts/instructions always use low scheduler priority so the desktop remains responsive while still allowing the workstation to use its full available capacity.

Process update:

- Added `tests/run-low-priority.sh` as a reusable wrapper for direct commands and as a sourceable helper for validation scripts.
- Updated validation and benchmark shell scripts under `tests/` that have a standard Bash prologue to self-reexec through the helper.
- Updated `AGENTS.md` project commands so direct CTest and other long validation commands use `tests/run-low-priority.sh`.
- Updated `docs/netipc-integrator-skill.md`, `docs/README.md`, and `docs/code-organization.md` because validation command guidance changed.

Low-priority validation evidence:

- `tests/run-low-priority.sh bash -c 'printf "ni=%s\n" "$(ps -o ni= -p $$ | tr -d " ")"'` printed `ni=19`.
- `ps` showed the active `ctest` process and its Go fuzz children running with nice value `19`.

Validation results:

- `cmake --build build --target netipc_service` passed.
- `/usr/bin/ctest --test-dir build --output-on-failure -R service` passed, 9/9 tests.
- Full low-priority CTest passed: `nice -n 19 ionice -c 3 /usr/bin/ctest --test-dir build --output-on-failure`, 48/48 tests, total real time 478.30 seconds.
- `bash -n tests/*.sh` passed after adding the low-priority helper and script prologues.
- `git diff --check` passed after the source, script, docs, and SOW changes.

Artifact updates:

- AGENTS.md: updated direct local validation command guidance to use `tests/run-low-priority.sh`.
- Runtime project skills: no runtime input skill update needed; the new rule is repository-wide command behavior, not Netdata vendoring-specific procedure.
- Specs: no update needed; no public API, wire format, data format, or transport semantics changed.
- End-user/operator docs: updated `docs/README.md` and `docs/code-organization.md` because validation command behavior changed.
- End-user/operator skills: updated `docs/netipc-integrator-skill.md` because validation guidance changed.
- SOW lifecycle: this SOW was reopened for the continued CodeQL regression and marked completed again after validation.

Outcome:

- The POSIX session context no longer stores the caller-owned server pointer in heap session state.
- The POSIX session thread receives the server pointer through a synchronized stack start argument and copies it before the accept loop continues.
- Local validation and benchmark scripts now self-run at low CPU/I/O scheduler priority by default.
- GitHub CodeQL must be checked again after push; if a same-rule alert remains on the new commit, this SOW must be reopened again.
