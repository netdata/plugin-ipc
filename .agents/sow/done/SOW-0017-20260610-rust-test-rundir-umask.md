# SOW-0017 - Rust POSIX tests: umask-independent run directories

## Status

Status: completed

Sub-state: fix implemented, validated under umask 0002 and 0022, committed with this SOW.

## Requirements

### Purpose

Rust netipc tests must pass regardless of the developer/CI process umask, without weakening the stale-unlink safety guard.

### User Request

User shared an external discovery note (`netipc.md`) reporting that `transport::shm::tests::test_stale_recovery` and related tests fail with `umask 0002`, then asked: "fix it and commit please". The fix direction chosen in discussion was the surgical test-setup change (chmod 0700 on the test run dir), not weakening the guard.

### Assistant Understanding

Facts:

- `run_dir_allows_stale_unlink` (`src/crates/netipc/src/transport/shm.rs:976`) refuses stale unlinks when the run dir is group/other-writable (`mode & 0o022 != 0`). The same guard exists for UDS (`src/crates/netipc/src/transport/posix.rs`), in C (`src/libnetdata/netipc/src/transport/posix/`), and in Go (`src/go/pkg/netipc/transport/posix/`).
- Four Rust test files create their run dirs with bare `std::fs::create_dir_all`, which inherits the process umask: `transport/shm_tests.rs`, `transport/posix_tests.rs`, `service/raw_unix_tests.rs`, `service/cgroups_unix_tests.rs`.
- Under `umask 0002` the dirs land at mode `0775`, the guard refuses to unlink stale files, and 6 tests fail (full list in Analysis).
- C tests use `mkdir(TEST_RUN_DIR, 0700)` and Go tests use `os.MkdirAll(dir, 0700)`; `0700` is unaffected by any umask, so C and Go are not exposed.
- The external note also claimed the guard is missing from this upstream repo; that claim was verified false — it compared against a stale clone at `48c74fe` (2026-04-06), 105 commits behind. The guard landed in `8a23810` (2026-06-03).

Inferences:

- Any CI runner or developer machine with a permissive umask (0002 is the default on several distros) silently fails these tests; the guard behavior is correct, the test setup is environmentally fragile.

Unknowns:

- None blocking.

### Acceptance Criteria

- `cargo test` in `src/crates/netipc` passes with `umask 0002` (previously 6 failures).
- `cargo test` in `src/crates/netipc` passes with `umask 0022` (no regression).
- No production code changed; the safety guard semantics are untouched.

## Analysis

Sources checked:

- `src/crates/netipc/src/transport/shm.rs:972-1062` (guard and call sites)
- `src/crates/netipc/src/transport/shm_tests.rs:13-15`, `posix_tests.rs:11-13`, `service/raw_unix_tests.rs:22-24`, `service/cgroups_unix_tests.rs:16-18` (run-dir setup)
- C tests `tests/fixtures/c/*.c` and Go tests `src/go/pkg/netipc/**/*_test.go` (same-failure scan)
- External discovery note `netipc.md` (user-provided, claims cross-checked against git history)

Current state (pre-fix evidence, `umask 0002`, full `cargo test` in `src/crates/netipc`):

- FAILED: `transport::posix::tests::test_stale_recovery`
- FAILED: `transport::shm::tests::test_cleanup_stale_invalid_entries`
- FAILED: `transport::shm::tests::test_cleanup_stale_unlinks_dangling_symlink`
- FAILED: `transport::shm::tests::test_cleanup_stale_unlinks_directory_symlink_when_mmap_fails`
- FAILED: `transport::shm::tests::test_server_create_recovers_invalid_stale_file`
- FAILED: `transport::shm::tests::test_stale_recovery`
- Result: 327 passed; 6 failed.

Risks:

- None significant: change is test-only, 4 files, identical 1-line addition each.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Test run dirs under `/tmp` inherit the process umask; with `umask 0002` they are group-writable (`0775`), and the stale-unlink safety guard correctly refuses to unlink inside group/other-writable directories. Stale files survive, subsequent `O_EXCL` creates fail with `EEXIST`/`AddrInUse`, and stale-recovery assertions fail.

Evidence reviewed:

- See Analysis. Reproduced locally: 6 failures under `umask 0002`, all pass under `umask 0022`.

Affected contracts and surfaces:

- Rust test setup only. No public API, wire format, transport behavior, or production code.

Existing patterns to reuse:

- C tests: `mkdir(TEST_RUN_DIR, 0700)` (`tests/fixtures/c/test_hardening.c:47`).
- Go tests: `os.MkdirAll(dir, 0700)` (`src/go/pkg/netipc/transport/posix/uds_test.go:36` and others).
- Rust mirrors this by `std::fs::set_permissions(.., from_mode(0o700))` after `create_dir_all`, which also repairs a pre-existing dir left with a permissive mode by an earlier run.

Risk and blast radius:

- Test-only. Worst case: a test environment where `set_permissions` fails — error is ignored like the existing `create_dir_all` error, and the affected test fails exactly as before.

Sensitive data handling plan:

- No secrets, customer data, or personal data involved. SOW cites only repo paths, public commit hashes, and test names.

Implementation plan:

1. Add `set_permissions(0o700)` to `ensure_run_dir()` in the four Rust POSIX test files.

Validation plan:

- Full `cargo test` in `src/crates/netipc` under `umask 0002` and `umask 0022`, with `/tmp/nipc_*` test dirs removed first to exercise fresh creation.

Artifact impact plan:

- AGENTS.md: unaffected — no workflow/process change.
- Runtime project skills: none exist; nothing to update.
- Specs: unaffected — no protocol/API/transport behavior change; guard semantics untouched.
- End-user/operator docs: unaffected — test-internal change; `docs/` documents the product, not test scaffolding.
- End-user/operator skills: unaffected — `docs/netipc-integrator-skill.md` covers integration workflow, not test internals.
- SOW lifecycle: single SOW, completed and committed with the change.

Open-source reference evidence:

- None checked; the failure is local and fully reproducible, no external reference needed.

Open decisions:

- Fix approach (chmod 0700 vs per-test tempdir) was presented to the user before this SOW; user accepted the recommended surgical chmod-0700 variant by requesting the fix.

## Implications And Decisions

1. Fix approach: (A) `chmod 0700` the existing shared run dirs — zero new dependencies, mirrors C/Go; (B) per-test `tempfile::tempdir()` — stronger isolation, new dev-dependency, larger diff. Selected: A (surgical), per user request following the assistant's recommendation.

## Plan

1. Patch `ensure_run_dir()` in the four Rust POSIX test files (low risk, no dependencies).
2. Validate under both umasks; complete and commit.

## Execution Log

### 2026-06-10

- Reproduced 6 failures under `umask 0002` on clean `/tmp` state.
- Patched `ensure_run_dir()` in `src/crates/netipc/src/transport/shm_tests.rs`, `src/crates/netipc/src/transport/posix_tests.rs`, `src/crates/netipc/src/service/raw_unix_tests.rs`, `src/crates/netipc/src/service/cgroups_unix_tests.rs`.
- Verified full Rust suite green under `umask 0002` and `umask 0022`.

## Validation

Acceptance criteria evidence:

- `umask 0002`: `cargo test` in `src/crates/netipc` — 333 passed; 0 failed (previously 327 passed; 6 failed).
- `umask 0022`: `cargo test` in `src/crates/netipc` — 333 passed; 0 failed.
- `git diff --stat` shows only the four test files changed; no production code touched.

Tests or equivalent validation:

- Full `cargo test` run twice (umask 0002 and 0022), `/tmp/nipc_*` dirs removed before each run.

Real-use evidence:

- The tests are the runnable path; both runs above are real executions on this workstation.

Reviewer findings:

- External reviewers not run: 4-file test-only setup change falls under the documented "wasteful reviewer calls" category.

Same-failure scan:

- Grepped all C (`tests/fixtures/c/*.c`) and Go (`**/*_test.go`) test dir creation: all use mode `0700` explicitly; not exposed to umask. Grepped all Rust `create_dir_all` test call sites: the four patched files were the complete set of POSIX run-dir creators (Windows test files are not subject to POSIX modes).

Sensitive data gate:

- No secrets, credentials, customer or personal data in this SOW, the diff, or comments.

Artifact maintenance gate:

- AGENTS.md: no update — no workflow, guardrail, or process change.
- Runtime project skills: none exist (per AGENTS.md index); no update.
- Specs: no update — no behavior, contract, wire-format, or operational-guarantee change; guard semantics untouched.
- End-user/operator docs: no update — change is internal to test scaffolding, not visible to integrators or operators.
- End-user/operator skills: no update — `docs/netipc-integrator-skill.md` triggers (public APIs, examples, workflow, transports, validation commands) are unaffected.
- SOW lifecycle: `Status: completed`, file placed in `.agents/sow/done/`, committed together with the fix in one commit.

Specs update:

- Not needed — see artifact maintenance gate.

Project skills update:

- Not needed — no runtime project skills exist.

End-user/operator docs update:

- Not needed — see artifact maintenance gate.

End-user/operator skills update:

- Not needed — no docs/spec changes occurred.

Lessons:

- POSIX test scaffolding that creates directories must set modes explicitly; inheriting the process umask makes test outcomes environment-dependent, especially when production code legitimately inspects directory permissions. C and Go already followed this; Rust did not.
- External discovery notes must be re-verified against current upstream HEAD before acting: the triggering note compared against a 105-commit-stale clone and drew a false "divergence" conclusion.

Follow-up mapping:

- The note's netdata-side recommendations (CI umask check in `netdata/netdata`, vendor-sync commit trailers) concern the downstream repo, not this one; this fix flows to netdata via the normal vendor sync. No follow-up SOW needed here.

## Outcome

All Rust netipc tests pass independent of the process umask. Test run directories are now created (or repaired) with mode `0700`, matching the existing C and Go test conventions. No production code changed.

## Lessons Extracted

See Lessons under Validation.

## Followup

None.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
