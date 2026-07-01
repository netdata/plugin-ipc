# SOW-0027 - Netdata Vendor Memory-Safety Update

## Status

Status: in-progress

Sub-state: preflight found one Netdata-only vendored-source Sonar cleanup that must be backported to `plugin-ipc` before copying source into Netdata.

## Requirements

### Purpose

Keep NetIPC memory-safety fixes source-owned in `plugin-ipc` while ensuring the Netdata vendored copy receives those fixes through the normal vendor/update path.

### User Request

The user asked to fix NetIPC library issues in `plugin-ipc`, not directly in the Netdata PR, because this repository is the source of truth.

On 2026-07-01, the user resumed the Netdata vendoring task after source CI and scanner readiness work was pushed. The target Netdata checkout is `~/src/netdata-ktsaou.git`, matching the checkout the user previously asked to compare against and the default Netdata Agent checkout in the repository instructions.

### Assistant Understanding

Facts:

- SOW-0026 implements source fixes for NetIPC memory-safety scout findings in `plugin-ipc`.
- SOW-0029 implements later scanner-readiness fixes, including the POSIX service thread handoff change.
- Netdata consumes NetIPC through a vendored copy.
- Directly patching Netdata's vendored NetIPC copy would create source-of-truth drift.
- The current candidate source commit before this SOW's backport is `fe4e31633b5372b3c93e21f9e37b38f2407aaed1`.
- The last confirmed Netdata vendor baseline is Netdata commit `b7146a36260d9ee80a976d47f57acc22c5569c93`, which matches plugin-ipc commit `96f5f2962188c2198e621fec5da8a4c90710b46a`.
- Netdata commit range `b7146a3626..HEAD` includes one downstream vendored-source edit in `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c`: uppercase `ULL` literal suffixes for Sonar rule `c:S818`.

Inferences:

- The Netdata-only `ULL` suffix cleanup is a valid vendored-source drift item. Because `plugin-ipc` is the source of truth, it must be backported here before the vendor copy is refreshed.

Unknowns:

- Whether the next pushed source commit will keep GitHub and Codacy checks green; this must be checked before copying into Netdata.

### Acceptance Criteria

- The selected Netdata checkout is confirmed before implementation.
- Source CI, GitHub code/security scanners, Dependabot, secret scanning, and Codacy Cloud are checked before any Netdata vendoring copy.
- The two-way gap analysis is recorded before vendoring.
- Any valid Netdata-only vendored-source drift is backported to `plugin-ipc` before vendoring.
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

Status: in-progress

Problem / root-cause model:

- NetIPC fixes must be propagated to the downstream Netdata vendored copy through `plugin-ipc`, the source-of-truth repository.
- A direct Netdata vendored-copy refresh from `fe4e31633b5372b3c93e21f9e37b38f2407aaed1` would overwrite one Netdata-only scanner cleanup in a vendored source file. That would recreate downstream drift and can re-trigger Netdata Sonar noise.

Evidence reviewed:

- SOW-0026 records that NetIPC source ownership belongs to `plugin-ipc`.
- Historical vendor-sync SOWs use the project-local `diff-netdata-vendor.sh` checker.
- `.agents/skills/project-netdata-vendoring/SKILL.md` requires source CI/scanner preflight, last-baseline reconstruction, two-way gap analysis, and a migration plan before Netdata is modified.
- `git -C ~/src/netdata-ktsaou.git log --oneline -- src/libnetdata/netipc src/crates/netipc src/go/pkg/netipc` identified `b7146a3626` as the latest vendor update.
- Historical Netdata SOW evidence and the vendor commit contents identify plugin-ipc `96f5f2962188c2198e621fec5da8a4c90710b46a` as the matching source baseline.
- Source CI/checks for `fe4e31633b5372b3c93e21f9e37b38f2407aaed1` were all successful: CodeQL, Static Analysis, Runtime Safety, Supply Chain Security, Codacy Local Analysis, Codacy Coverage, and the Codacy push quality check.
- Current source code-scanning analyses for `fe4e31633b5372b3c93e21f9e37b38f2407aaed1` reported zero results for CodeQL, Semgrep, gosec, OSV, and Codacy local.
- GitHub code scanning still shows one stale open Semgrep alert on old commit `b4dfe405e1f99be417c21a9c0478aba2f64facaa`; current commit analyses report zero Semgrep results.
- GitHub Dependabot open alerts: none.
- GitHub secret-scanning open alerts: none.
- Codacy Cloud analyzed `fe4e31633b5372b3c93e21f9e37b38f2407aaed1`; repository problems were empty and coverage was 90%. The remaining 23 Go standard-library dependency findings are the previously accepted `go 1.26.0` compatibility exception.
- Source tree has one unrelated modified file, `bench/drivers/go/go`; it is not part of this vendoring scope and must not be staged.
- Netdata checkout `~/src/netdata-ktsaou.git` is on `master...origin/master` with unrelated untracked files; they are not part of this vendoring scope and must not be staged.

Affected contracts and surfaces:

- Netdata vendored C, Rust, and Go NetIPC sources.
- Netdata build/test paths that consume NetIPC.
- Vendor synchronization evidence in this SOW.
- Source-side scanner hygiene for C integer literal suffixes in vendored NetIPC C files.

Existing patterns to reuse:

- `diff-netdata-vendor.sh`
- `vendor-to-netdata.sh`
- Prior SOW-0003 and SOW-0008 vendor synchronization flow.
- Netdata's existing Go import normalization from `github.com/netdata/plugin-ipc/go` to `github.com/netdata/netdata/go/plugins`.

Risk and blast radius:

- Medium: changes land in a consumer repository and may affect Netdata build/test behavior.
- Keep scope limited to NetIPC vendor propagation and required validation.
- Low source risk for the literal suffix backport: it is scanner/style normalization only, with no value or ABI change.
- Medium process risk if the stale old GitHub code-scanning alert is confused with a current-commit issue; mitigation is to record both the open alert and the current-commit zero-result analyses.

Sensitive data handling plan:

- No secrets, customer data, credentials, production logs, or private endpoints are required.
- Evidence will use source paths, commands, commit hashes, and sanitized summaries only.

Implementation plan:

1. Backport the downstream Netdata Sonar `ULL` suffix cleanup into `plugin-ipc`, applying the same numeric literal suffix style to equivalent NetIPC C numeric `ull` literals.
2. Run focused source validation for the touched C files and commit/push the source backport.
3. Re-check source GitHub CI, GitHub code/security scanners, Dependabot, secret scanning, and Codacy Cloud for the new source commit.
4. Vendor the pushed source commit into `~/src/netdata-ktsaou.git` using `vendor-to-netdata.sh`.
5. Normalize Netdata Go import paths if the vendor script leaves upstream module paths.
6. Run `diff-netdata-vendor.sh` and inspect the remaining diff.
7. Run targeted Netdata C, Rust, and Go validation.
8. Commit only the intended Netdata vendor update files, excluding unrelated untracked or dirty files.

Validation plan:

- Source-side validation: focused C build or full low-priority CTest if the scope requires it; `git diff --check`; SOW audit.
- Source-side post-push validation: GitHub Actions/check-runs, code scanning, Dependabot, secret scanning, and Codacy Cloud.
- Run the vendor diff/checker against the selected Netdata checkout.
- Run targeted Netdata build/tests for touched C/Rust/Go NetIPC integration paths.
- Run same-failure searches for the SOW-0026 finding classes in the Netdata vendored copy.
- Search for lowercase numeric `ull` suffixes in NetIPC C files after vendoring.
- Search for stale `github.com/netdata/plugin-ipc/go` imports after vendoring.

Artifact impact plan:

- AGENTS.md: no expected update; vendoring preflight and low-priority validation policy are already recorded.
- Runtime project skills: no expected update unless this SOW exposes a reusable vendoring gap not already covered by `project-netdata-vendoring`.
- Specs: no expected update unless propagation exposes source/doc drift.
- End-user/operator docs: no expected update.
- End-user/operator skills: no expected update.
- SOW lifecycle: move from pending to current before source backport and vendoring; complete only after source and Netdata validations are recorded.

Open-source reference evidence:

- None checked yet; this SOW is a local vendor propagation task.

Open decisions:

None currently blocking. The target checkout is `~/src/netdata-ktsaou.git`; branch/PR handling will be determined after local validation so unrelated local files are not staged.

## Implications And Decisions

- Decision: backport valid Netdata-only vendored-source drift into `plugin-ipc` before vendoring. This is long-term-best because it keeps the source repository authoritative and prevents the next vendor run from reintroducing the same Netdata scanner finding.
- Decision: treat the stale open GitHub Semgrep alert on old commit `b4dfe405e1f99be417c21a9c0478aba2f64facaa` as not blocking the current candidate only because current-commit code-scanning analyses report zero Semgrep results. If GitHub reports the same alert on the new source commit, vendoring blocks until fixed or explicitly risk-accepted.

## Plan

1. Backport the downstream Netdata Sonar suffix cleanup to `plugin-ipc`.
2. Validate, commit, push, and re-check source CI/scanners.
3. Run vendor propagation into `~/src/netdata-ktsaou.git`.
4. Validate vendor parity and targeted Netdata behavior.
5. Commit and push when validated.

## Execution Log

### 2026-06-29

- Created as the tracked follow-up for SOW-0026 vendor propagation.
- No implementation started.

### 2026-07-01

- Loaded `.agents/skills/project-netdata-vendoring/SKILL.md`.
- Confirmed current source candidate before backport: `fe4e31633b5372b3c93e21f9e37b38f2407aaed1`.
- Confirmed source CI and scanner status for that candidate, with only the previously accepted Go `1.26.0` Codacy dependency findings and one stale old GitHub Semgrep alert not present in current-commit analyses.
- Confirmed Netdata target checkout: `~/src/netdata-ktsaou.git`.
- Confirmed baseline: Netdata commit `b7146a36260d9ee80a976d47f57acc22c5569c93` and plugin-ipc commit `96f5f2962188c2198e621fec5da8a4c90710b46a`.
- Performed two-way gap analysis:
  - Upstream gap: 49 vendored C/Rust/Go NetIPC source files changed from `96f5f2962188c2198e621fec5da8a4c90710b46a` to `fe4e31633b5372b3c93e21f9e37b38f2407aaed1`, covering cgroups snapshot cache concurrency, memory-safety hardening, scanner cleanup, and POSIX service thread handoff.
  - Downstream gap: one Netdata-only vendored-source edit in `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c` changed `ull` to `ULL` for Sonar rule `c:S818`.
- Migration plan before vendoring: backport the downstream `ULL` suffix cleanup to source first, then re-run source validation and CI/scanner checks before copying to Netdata.
- Backported the downstream Sonar literal-suffix cleanup into `plugin-ipc`:
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c`
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
  - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
- Same-pattern source search after the edit: `rg -n '\b(0x[0-9A-Fa-f]+|[0-9]+)ull\b' src/libnetdata/netipc` returned no matches.
- Source validation after the edit:
  - `tests/run-low-priority.sh cmake --build build --target netipc_uds netipc_shm netipc_service`: passed.
  - `tests/run-low-priority.sh /usr/bin/ctest --test-dir build --output-on-failure -R 'uds|shm|service'`: 19/19 tests passed.
  - `git diff --check`: passed.
  - `bash .agents/sow/audit.sh`: passed.
- Note: the unqualified `ctest` in the user-local PATH failed because its Python `cmake` module was unavailable, so validation used `/usr/bin/ctest`.

## Validation

Acceptance criteria evidence:

- Selected Netdata checkout confirmed: `~/src/netdata-ktsaou.git`.
- Source preflight completed before Netdata vendoring: CI/checks, GitHub code scanning, Dependabot, secret scanning, and Codacy Cloud were checked for `fe4e31633b5372b3c93e21f9e37b38f2407aaed1`.
- Two-way gap analysis completed before vendoring and recorded in the 2026-07-01 execution log.
- Valid Netdata-only vendored-source drift was backported to `plugin-ipc` before any Netdata copy.
- Netdata vendoring, Netdata validation, Netdata commit, and post-vendor evidence are not started yet.

Tests or equivalent validation:

- Source-side focused validation passed:
  - focused C build for `netipc_uds`, `netipc_shm`, and `netipc_service`;
  - focused C/Rust/Go transport/service CTest subset, 19/19 passed;
  - source same-pattern numeric lowercase `ull` search, no matches;
  - `git diff --check`;
  - SOW audit.
- GitHub/Codacy validation for the new source backport commit is pending until the commit is pushed.

Real-use evidence:

- Not started for Netdata; vendor copy has not been modified yet.

Reviewer findings:

- Not started.

Same-failure scan:

- Source same-pattern scan for lowercase numeric `ull` suffixes in `src/libnetdata/netipc` returned no matches.

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
