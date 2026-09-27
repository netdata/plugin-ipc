# SOW-0038 - Publish and vendor the SHM time64 repair

## Status

Status: completed

Sub-state: both authorized PRs published; source preflight and targeted downstream validation passed. Merging and deployment are outside scope.

## Requirements

### Purpose
Publish the time64 SHM repair in plugin-ipc and Netdata through pull requests.

### User Request
Create a Netdata git worktree under ~/src/PRs from the existing checkout, re-vendor
plugin-ipc, and open a Netdata PR. Also open a plugin-ipc PR because main is protected.
The request explicitly authorizes the worktree, branches, pushes and PR publication.

### Acceptance Criteria
- Source PR contains the validated repair from SOW-0036.
- Vendoring starts only after source CI/scanner and two-way drift gates pass.
- Netdata PR preserves wrappers, workspace packaging and normalized imports.
- Targeted downstream builds/tests and source equality checks pass.
- Both PR URLs and actual CI status are reported; merging is outside this request.

## Analysis

The current source commit is 5c1e145; origin/main is its parent 0d17a9d.
GitHub reports main protected. Prior source and Netdata baseline is plugin-ipc
37cce82d4b0e1e9d1fbee2ff2ab561cc9920ffa6 and Netdata
d5796cb0a841adaabdd68d427664155d8e30d46e (SOW-0030/0033 evidence).
Netdata post-baseline source changes are the five Rust compatibility edits already
backported by SOW-0033; Cargo.toml workspace changes are downstream-owned.
Paused SOW-0027 covers the earlier memory-safety delivery, not this new time64 repair.
Other current/pending SOWs do not own this delivery; no other SOW is activated.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:
- The source repair is committed locally but neither published for protected-main review
  nor included in Netdata. Direct downstream patching would lose source ownership.

Evidence reviewed:
- SOW-0036 validation, SOW-0030/0033 vendor baseline, source and Netdata histories,
  vendor/diff scripts, project-netdata-vendoring skill, public SHM spec and integrator guide.

Affected contracts and surfaces:
- Git branches, PRs, vendored C/Rust/Go SHM code, existing Netdata build integration.

Existing patterns to reuse:
- Source-first PR and CI, project vendor/diff scripts, Go module normalization,
  low-priority validation, downstream-owned wrappers and workspace manifests.

Risk and blast radius:
- Overwriting downstream changes or publishing unvalidated code. Reconstruct and
  classify drift before copying; require source GitHub checks/scanners to pass.

Sensitive data handling plan:
- Store only source identifiers, sanitized checks and public PR links in SOWs,
  specs, docs, skills, instructions and comments. Do not copy local host notes,
  credentials, personal data, private endpoints or raw secret-scanning content.

Implementation plan:
1. Publish 5c1e145 on a source topic branch and open its PR.
2. Create the user-authorized Netdata worktree; inspect applicable instructions,
   local SOWs/specs, build rules and drift while source CI runs.
3. Record exact source checks, scanner findings, baseline and file migration plan;
   fix source blockers before any vendor write.
4. Vendor, normalize imports, validate downstream, commit and open the Netdata PR.
5. Record actual PR/check outcomes and complete this delivery SOW with one commit.

Validation plan:
- Source Actions/check runs/status and code/Dependabot/secret scanning.
- Exact baseline reconstruction and bidirectional file comparison.
- Downstream C production compile and public timeout fixture, Rust and Go tests,
  normalized post-vendor diff and explicit wrapper/package preservation.
- Self-review of deterministic vendor diff; obtain independent review if material
  integration uncertainty remains after these checks.
- SOW audit, sensitive-data review and git diff --check.

Artifact impact plan:
- AGENTS.md: no workflow change; comply with branch protection and worktree authorization.
- Runtime skills: existing preflight governs; update only for demonstrated workflow gaps.
- Specs: source SHM ABI contract is already updated in SOW-0036.
- End-user/operator docs: preserve source instructions and reference them from PR evidence.
- End-user/operator skills: source integrator guide already captures time64 validation.
- SOW lifecycle: new delivery SOW; source repair SOW remains completed; downstream
  local SOW follows Netdata's ignored-queue convention.

Open-source reference evidence:
- netdata/plugin-ipc @ 5c1e145, docs/level1-posix-shm.md and vendor scripts.
- netdata/netdata @ 4464a9ed71c8a7351b540ce4ca207e1418faffea,
  src/libnetdata/netipc, src/crates/netipc, src/go/pkg/netipc.

Open decisions:
- None. The user fixed the delivery target and authorized PR publication. Source
  failures must be repaired or reported; no risk waiver is presumed.

## Implications And Decisions

Use a source topic branch and a fresh Netdata branch off its current master.
The mandatory source preflight is a condition of vendoring, not a new permission request.

## Plan

Follow the five implementation steps in the gate. Do not merge either PR.

## Execution Log

### 2026-09-27
- Confirmed protected plugin-ipc main and the exact historical vendor baseline.

## Validation

Acceptance criteria evidence:
- plugin-ipc PR: https://github.com/netdata/plugin-ipc/pull/17
- Netdata PR: https://github.com/netdata/netdata/pull/24049
- Netdata commit e73dd3903357f0a2f9825247f0fd284111bcaea2 exports source SDK
  commit 5c1e14545fd3c367db3851084bce949088ee1c0f. The delivery-log commit changes
  only this SOW; it does not alter the source files checked and vendored.
- User-authorized Netdata worktree uses branch fix/netipc-shm-time64 under the
  requested PRs directory. Source PR uses fix/shm-futex-time64-abi.
- Source preflight details and exact baseline/migration plan are recorded below.

Tests or equivalent validation:
- All eight source workflows succeeded before vendoring; scans have zero open alerts.
- Netdata CMake netipc target built; public SHM fixture passed against its archive.
- Netdata 32-bit glibc time64 reproducer failed before the copy (100 ms returned in
  about 0.06 ms) and passed after (100.06 ms), including finite/infinite/max wakeup.
- Netdata Rust workspace: 50 SHM unit tests and standalone public test passed.
- Netdata Go: all pkg/netipc tests passed, including 110-second raw-service suite;
  go vet and changed-file gofmt validation passed.
- Post-vendor normalized C/Rust/Go comparison reports no differences. All seven
  downstream wrapper/package files retain their hashes. Exactly five source/test
  files are committed downstream; no SOW/spec memory or build configuration is staged.
- git diff --check and source SOW audit pass.
- Netdata local SOW passes all its structural checks and the durable sensitive-data
  scan passes. Whole-queue audit has one pre-existing invalid status in an unrelated
  local netflow SOW plus advisory legacy gaps; no unrelated local memory is repaired.

Real-use evidence:
- Real public SHM contexts, idle receives and delayed peer messages executed against
  the downstream implementation. No production service was changed or remeasured.

Reviewer findings:
- Main-agent self-review is sufficient for exact deterministic vendor import with
  upstream ABI proof, downstream compile/runtime tests and preserved wrappers.
  Checked clean target vs final five-file diff: no unrelated or unresolved drift.
- Source review identified the standalone Rust fixture omitted by src-only vendor
  sync; copied it byte-for-byte. No downstream patch or local protocol fork exists.

Same-failure scan:
- Source-owned C/Rust timed futex wrappers are updated together. Go remains pure Go.
  No other Netdata NetIPC syscall wrapper or consumer API needed modification.

Sensitive data gate:
- Records contain public source/check/PR identifiers and synthetic test evidence.
  No private host names, endpoints, investigation notes, credentials or personal
  data were written to committed artifacts.

Artifact maintenance gate:
- AGENTS.md: unchanged; authorized worktree and protected-branch PR workflows followed.
- Runtime project skills: existing vendoring preflight fully applied; no policy change.
- Specs: source authoritative SHM ABI requirement already shipped in SOW-0036;
  this import introduces no different API/wire guarantee requiring another spec.
- End-user/operator docs: upstream documentation already covers the ABI and tests;
  downstream PR carries exact source and validation links, with no configuration change.
- End-user/operator skills: upstream integrator guide already updated; no new
  downstream operator action or deployment procedure introduced.
- SOW lifecycle: this source delivery SOW completed and moved with its evidence commit;
  Netdata local SOW remains ignored and records the PR's pending remote review/checks.
  Earlier paused source SOWs and pending SOW-0037 remain unchanged.

Specs update: no new contract; authoritative upstream SHM spec already current.
Project skills update: no new runtime workflow; explicit fixture copy recorded in
migration plan and PR, preserving complete normalized source equality.
End-user/operator docs update: PR explains symptoms, fix, reproduction and validation.
End-user/operator skills update: existing source integration guidance remains correct.
Lessons: verify copied test fixtures as well as src trees when the vendor checker
compares a complete crate. Source checks can pass before opening a downstream PR.
Follow-up mapping: all requested publication/vendoring implemented. Merge, deployment
and production remeasurement were not requested. No delivery item is deferred.

## Outcome

Published source PR #17 and Netdata PR #24049 without pushing to protected main.
Netdata's SDK copy exactly matches the validated source after import normalization.
Remote checks on the newly published downstream PR are pending/running, and the
source PR may rerun checks when this documentation-only delivery record is pushed.
No PR is merged and no production host is changed.

## Lessons Extracted

Source PR checks must precede downstream vendor writes.

## Followup

No implementation item is deferred; independent platform gaps remain in SOW-0037.

## Regression Log

This is publication and integration of SOW-0036, not a new regression.

## Vendor Baseline And Migration Plan

- Source candidate: netdata/plugin-ipc @ 5c1e14545fd3c367db3851084bce949088ee1c0f, PR #17.
- Historical baseline: plugin-ipc 37cce82d4b0e1e9d1fbee2ff2ab561cc9920ffa6;
  Netdata d5796cb0a841adaabdd68d427664155d8e30d46e. Reconstructed 227 source
  files across C include/src, Rust src and Go package: zero normalized mismatches.
- Netdata target: 4464a9ed71c8a7351b540ce4ca207e1418faffea. All 227 normalized
  files exactly match plugin-ipc 0d17a9d (candidate parent).
- Source gap since historical baseline: five Rust compatibility files already
  backported in SOW-0033, followed by four time64 source/test edits from SOW-0036.
- Downstream gap: those same five Rust compatibility edits, all exactly retained;
  Cargo.toml edition/dependencies use workspace settings and must remain unchanged.
- Migration: source wins for C netipc_shm.c, Rust shm.rs/shm_tests.rs, Go shm_linux.go.
  Preserve all C wrappers, Cargo metadata/lockfiles and Netdata Go module paths.
- The new standalone Rust tests/shm_timeout.rs is self-contained and useful in the
  downstream workspace. The existing vendor script copies src/ only, so copy this
  exact upstream fixture separately after preflight. This yields five changed
  Netdata files and zero normalized diff, including the new fixture.
- No downstream source fix remains to backport; no conflict or design choice exists.
- Preflight so far: code scanning, Dependabot and secret scanning queries return
  zero open alerts. The initial source checks were still running at that checkpoint;
  the subsequent passed-preflight section records approval to copy.

## Source Preflight Passed - 2026-09-27

- Candidate: netdata/plugin-ipc @ 5c1e14545fd3c367db3851084bce949088ee1c0f.
- All eight Actions workflows completed successfully: Runtime Safety 36325853646,
  Static Analysis 36325853609, CodeQL 36325853650, Codacy Local Analysis 36325853629,
  Codacy Coverage 36325853630, Supply Chain Security 36325853656, Dependency Review
  36325853615 and Code Quality 36325851891. All applicable check-runs succeeded.
- ARM ABI, native sanitizers, Windows MSYS runtime, MSRV/latest Rust, all Go static
  targets, Go race, CodeQL and coverage gates passed. Valgrind, OSV and Scorecard
  are skipped by their configured event predicates, not failed or missing PR jobs.
- Current code scanning, Dependabot and secret-scanning open alert counts: 0/0/0.
  PR merge analysis 867d1d21f34956ee07427c3ea3d537912e54639c has zero findings from
  CodeQL, Semgrep, gosec and Codacy analyses.
- CodeRabbit's optional review status remains pending. It is not an Actions
  validation or security-scanner result; main has no required status-check
  contexts and requires code-owner review for merge. No merge is requested here.
- Decision: proceed with vendoring. Source SDK tree is clean; the only uncommitted
  source artifact is the active delivery SOW, which is not a vendored input.
