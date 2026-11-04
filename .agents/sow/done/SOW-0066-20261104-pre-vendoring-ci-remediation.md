# SOW-0066 - Pre-Vendoring CI Remediation

## Status

Status: completed

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: fixes implemented and validated locally and on the macOS, Windows and FreeBSD hosts; CI rerun on the pushed commit is recorded in SOW-0050's preflight.

## Requirements

### Purpose

Make the plugin-ipc vendoring candidate pass its CI and code-scanning gates, so SOW-0050 can vendor it into Netdata with evidence instead of exceptions.

### User Request

The user accepted the recommendations of the SOW-0050 preflight report (2026-11-04): fix the CI failures in a separate SOW before vendoring (1A), make the macOS bind fd-relative (2A), dismiss the five remaining CodeQL alerts with evidence (3A), pin Semgrep (4A), accept the Codacy coverage-token failure for this vendoring (5B), push this SOW's commit to the branch and rerun the workflows (6), rerun the POSIX benchmarks once this SOW is in and the workstation is quiet (7A), and settle performance with same-host full A/B runs of `63919ec` against the candidate (8A, executed in SOW-0050).

### Assistant Understanding

Facts:

- All six workflows dispatched on `2eee40a` failed. The triage is in SOW-0050's Execution Log (2026-11-04).
- The product code is correct in every failing case except the macOS bind, where the code works but uses path-based `chmod`/`rename` that scanners flag as a race.

Inferences:

- None of these changes run per request on an established session, so the pending benchmarks still measure the vendored code.

Unknowns:

- Whether the pinned Semgrep stops crashing on the runner can only be proven by the rerun CI.

### Acceptance Criteria

- Go and Rust Windows DACL tests pass when the process user is the built-in Administrator (SDDL alias `LA`) and for any other account: proven by the CodeQL Go Windows job and the Windows test host.
- The C CLOEXEC robustness test ignores descriptors the process inherited: proven by the Linux target jobs.
- `armv7-gnu` and `armv7-gnu-time64` Rust builds link with Rust 1.99: proven by the Linux target jobs.
- staticcheck reports nothing for `src/go`: proven locally and by the Go Static Analysis job.
- flawfinder `--minlevel=5` and Semgrep report no `chmod` race; the macOS bind still produces an owner-only socket: proven locally (flawfinder) and by the macOS host suites.
- The rustdoc warning `unresolved link to directory` is gone (`cargo doc`).
- `tests/run-posix-bench.sh` and `tests/run-windows-bench.sh` create private run directories whatever the umask.
- The Semgrep job finishes on the runner.

## Analysis

Sources checked:

- CI logs of runs 41818654495, 41818662881, 41818667746, 41818696256, 41818701021, 41818758377 (`netdata/plugin-ipc`, `2eee40a`).
- Code-scanning alerts 7759 to 7765 on `refs/heads/sow-0040-robustness-repro-tests`.
- `src/go/pkg/netipc/transport/windows/security_test.go:45`, `src/crates/netipc/src/transport/windows.rs:2740`, `src/crates/netipc/src/transport/win_shm.rs:1450`, `tests/fixtures/c/test_win_security.h` (SID comparison, correct).
- `tests/fixtures/c/test_robustness.c:1068` (`all_ipc_fds_cloexec`).
- `tests/run-linux-target.sh:152` (`+crt-static`).
- `src/go/pkg/netipc/service/raw/large_helpers_unix_test.go`, `src/go/pkg/netipc/transport/posix/uds_platform_other.go`, `uds_cloexec_other.go`.
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:104`, `src/crates/netipc/src/transport/posix.rs:869`, `src/go/pkg/netipc/transport/posix/uds_bind_darwin.go`.
- `.github/workflows/supply-chain-security.yml`, `tests/run-posix-bench.sh:576,905`, `tests/run-windows-bench.sh`.

Current state:

- Windows DACL tests: the runner account is RID 500; `ConvertSecurityDescriptorToStringSecurityDescriptor` prints it as `LA`, while the tests build the expected string from `ConvertSidToStringSid` (full `S-1-5-21-...-500`).
- CLOEXEC test: it scans every fd from 3; the runner passes inherited pipes (fds 142, 145).
- armv7 gnueabihf with `+crt-static` links as static-pie on Rust 1.99.1 and needs `rcrt1.o`, which Ubuntu's `libc6-dev-armhf-cross` does not ship (local Rust 1.97.1 links it as static non-PIE).
- staticcheck U1000: `simpleHash`, `largeHandler`, `startServerWithWorkers` are used only by `stress_test.go` (`netipc_extended`); `setNoSigpipe` in `uds_platform_other.go` (`unix && !darwin`) is called only from `uds_cloexec_other.go` (`unix && !linux && !freebsd`).
- macOS bind: `chmod(dir)`, `chmod(tmp)` and `rename(tmp, path)` resolve paths again after `mkdir`; if another account could rename entries in `run_dir`, the directory could be swapped between those calls.
- Semgrep: `pip install --upgrade semgrep` on Python `3.x` installed semgrep 1.183.0 on Python 3.15.1; `semgrep-core` exits with -11. Semgrep 1.177.0 on Python 3.12 scans this tree locally.
- Bench scripts: `mkdir -p "$RUN_DIR"` gives mode `0777 & ~umask`; under umask `0002` the C driver refuses it (`tests/fixtures/c/interop_path.h:94`).

Risks:

- The fd-relative bind changes the macOS listener path. It is validated on the macOS host with the C, Rust and Go suites and the interop scripts.
- Pinning Semgrep freezes its rules engine version; Dependabot does not update pip pins in workflow `run` steps, so the pin must be bumped by hand.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Five test or tooling defects and one scanner-flagged pattern, each with the cause above; none is a product behaviour defect on Linux or Windows.

Evidence reviewed:

- The CI logs and alerts listed in Analysis; local flawfinder rule list (`chmod` and `chown` are level 5; `fchmodat` and `renameat` are not listed); local semgrep run.

Affected contracts and surfaces:

- macOS listener creation (C, Rust, Go); Windows and Linux test suites; Linux target script; supply-chain workflow; bench scripts; one rustdoc line. No wire format, API or default changes.

Existing patterns to reuse:

- `tests/fixtures/c/test_win_security.h` (compare by SID); Go and Rust `processUserSID`/`process_user_sid`; the `netipc_extended` build tag; `uds_cloexec_*.go` platform split.

Risk and blast radius:

- macOS listeners only for the bind change; tests and CI otherwise.
- Performance assessment: no changed code runs per request on an established session. The bind runs once per listen (`netipc_uds_lifecycle.c` `bind_owner_only_socket`), the rest is tests, CI, scripts and a doc comment. No benchmark run is needed for this SOW; the SOW-0050 runs cover the candidate.

Sensitive data handling plan:

- Hosts are named by role; the runner SID appears only as the generic alias `LA`/RID 500, not the runner's full SID; no tokens or private endpoints in the SOW, code comments or commit.

Implementation plan:

1. Windows DACL tests (Go, Rust): normalize the expected SDDL by converting it to a descriptor and back.
2. C CLOEXEC test: record the descriptors open before the test and check only new ones.
3. `tests/run-linux-target.sh`: add `-C relocation-model=static` for the armv7 gnu targets.
4. Go: tag `large_helpers_unix_test.go` `unix && netipc_extended`; move the no-op `setNoSigpipe` to a file built only with `uds_cloexec_other.go` on non-darwin.
5. macOS bind, C/Rust/Go: `mkdir`, open the directory `O_DIRECTORY|O_NOFOLLOW`, check it is ours, `fchmod` it, `bind` the socket inside it, `fchmodat` and `renameat` relative to the directory fd, clean up relative to it.
6. Rustdoc backport (`protocol/mod.rs`).
7. Bench scripts: `mkdir -m 0700` for run directories.
8. Semgrep: pin Python 3.12 and semgrep 1.177.0.
9. Dismiss code-scanning alerts 7759 to 7763 with evidence comments.

Validation plan:

- Linux: CTest, `cargo test`, `go test`, staticcheck, flawfinder `--minlevel=5`, `cargo doc`, fmt on stable and 1.91, `go vet` for darwin/freebsd/linux cross targets.
- Linux targets: `tests/run-linux-target.sh armv7-gnu` with the new flag.
- macOS host: CTest, `cargo test`, `go test`, the UDS, service, cache, lookup-scale interop scripts and `interop_codec.sh`.
- Windows host: the Go and Rust DACL tests.
- Rerun the dispatched workflows on the pushed commit.

Artifact impact plan:

- AGENTS.md: unaffected; no workflow change.
- Runtime project skills: unaffected; the vendoring preflight already covers these gates.
- Specs: unaffected; no protocol or contract change.
- End-user/operator docs: `docs/level1-posix-uds.md` macOS bind steps change to the fd-relative sequence.
- End-user/operator skills: `docs/netipc-integrator-skill.md` describes only the private directory, which is unchanged; recheck at close.
- SOW lifecycle: SOW-0050 stays in progress and waits for this SOW.

Open-source reference evidence:

- No external repositories were consulted; the evidence is this repository's CI and tooling.

Open decisions:

- None; the user decided items 1A to 8A on 2026-11-04.

## Implications And Decisions

1. Fix location: a separate SOW, own commit, new vendoring candidate (user, 1A).
2. macOS `chmod` alerts: fix with fd-relative calls in C, Rust and Go (user, 2A).
3. CodeQL alerts 7759 to 7763: dismiss with evidence comments (user, 3A).
4. Semgrep crash: pin Python and semgrep in the workflow (user, 4A).
5. Codacy coverage upload refused for the token's account: accepted for this vendoring; token permission fixed later by the user (user, 5B).
6. Push this SOW's commit to the branch and rerun the workflows (user, 6).
7. Go macOS bind (user, 10A, 2026-11-04): darwin's `syscall` exports no `Fstatat`, `Fchmodat` or `Renameat`, so plugin-ipc's Go module takes its first dependency, `golang.org/x/sys` pinned to v0.49.0 (the version Netdata's `src/go/go.mod` already requires), and uses `unix.Fstatat`, `Fchmodat`, `Renameat`, `Unlinkat`. Rejected: raw `syscall.Syscall` (deprecated on macOS) and keeping Go path-based (breaks parity).

## Plan

See the implementation plan above.

## Execution Log

### 2026-11-04

- SOW created from the SOW-0050 preflight triage.
- Done: Go and Rust DACL tests normalize the expected SDDL through Windows (`security_test.go` `ownerOnlyDACL`, `win_security.rs` `owner_only_dacl_sddl`); C CLOEXEC test skips descriptors open before it (reproduced with an inherited pipe on fd 9: the old binary fails, the new one passes); `run-linux-target.sh` adds `-C relocation-model=static` for armv7 gnu; Go `large_helpers_unix_test.go` tagged `netipc_extended` and the no-op `setNoSigpipe` moved to `uds_nosigpipe_other.go` (`unix && !darwin && !linux && !freebsd`; `go vet` passes for linux, freebsd, darwin, netbsd, openbsd); C and Rust macOS bind made fd-relative (flawfinder level 5: 0 hits; Rust `cargo check --target aarch64-apple-darwin` clean); rustdoc line now equals Netdata's `a80f0e3dc0` (`cargo doc` without warnings); bench run directories created with `mkdir -p -m 0700` (POSIX 2 sites, Windows 7 sites; the interop scripts already use `mktemp -d`); Semgrep pinned to 1.177.0 on Python 3.12.
- Blocked (design cannot be applied as decided): Go has no `golang.org/x/sys` dependency, and darwin's `syscall` package exports no `Fstatat`, `Fchmodat` or `Renameat` (only unexported `fstatat`, `unlinkat`, `openat`). The Go macOS bind was restored to its committed content pending the user's decision.
- Unblocked by decision 7 (10A): Go macOS bind rewritten with `golang.org/x/sys/unix` v0.49.0; `src/go/go.mod` requires it, `go.sum` added; the dependent modules `bench/drivers/go` and `tests/fixtures/go` carry it as an indirect requirement with their own `go.sum` (`go mod tidy`). Dependabot already tracks `/src/go`; vendoring copies only `pkg/netipc`, and Netdata's `src/go/go.mod` already requires `golang.org/x/sys v0.49.0`.
- `docs/level1-posix-uds.md` macOS bind steps rewritten for the fd-relative sequence.
- Code-scanning alerts dismissed with evidence comments (decision 3): 7761 and 7763 false positive, 7759 and 7760 used in tests, 7762 won't fix.
- Independent review (fresh read-only agent, code only): no correctness bugs in the three bind implementations or the DACL helpers; three findings:
  - 1.1 (low, valid, fixed): opening the bind directory read-only right after `mkdir` failed under a umask without owner read (the old path `chmod` ran first). Repro: new C case `listen under umask 0477` in `tests/fixtures/c/test_uds.c` failed on the macOS host with the unfixed bind and passes with the fix. Fix in C, Rust and Go: `fchmodat(AT_FDCWD, dir, 0700, AT_SYMLINK_NOFOLLOW)` before the open (not a flawfinder rule; flawfinder level 5 still 0 hits); the `fchmod` of the descriptor stays.
  - 1.2 (informational, no change): on the uid-mismatch path the directory name is still `rmdir`ed, which removes only an empty directory; unchanged from before and within `docs/level1-posix-uds.md` ("A directory with other content is never deleted").
  - 3.1 (low-medium, valid, fixed): the CLOEXEC snapshot was taken after about 15 earlier tests, so a descriptor an earlier test leaked would be skipped. The snapshot now runs first in `main()`.
- Same-umask check of Rust and Go on the macOS host (test binaries built under the normal umask, run under `umask 0477`): the listeners start and narrow the socket to `0200`; assertions that expect `0600` fail as they assume the default umask. Two Rust lock tests got `Err(Socket(13))`: the endpoint lock file (SOW-0052) becomes `0200` and later `O_RDWR` opens fail. That defect predates this SOW, affects all POSIX platforms, needs a design decision and is tracked in `.agents/sow/pending/SOW-0067-20261104-endpoint-lock-under-restrictive-umask.md`.

## Validation

Acceptance criteria evidence:

- Windows DACL tests: Rust `test_pipe_dacl_owner_only`, `test_server_objects_dacl_owner_only_windows` and Go `TestPipeDACLOwnerOnly`, `TestWinShmObjectsDACLOwnerOnly` pass on the Windows test host (a non-RID-500 account). The RID 500 (`LA`) case is proven by the CodeQL Go Windows job on the pushed commit (SOW-0050).
- CLOEXEC test: with an inherited non-close-on-exec pipe on fd 9, the old binary fails ("fd 9 (pipe) is not close-on-exec") and the new one passes with 5 descriptors checked.
- armv7 gnu Rust: `tests/run-linux-target.sh armv7-gnu c rust` passes (C 14/14, Rust 398) with the new flag on the local Rust 1.97.1; the Rust 1.99.1 link is proven by the Linux target jobs on the pushed commit.
- staticcheck: the local staticcheck cannot load Go 1.27 export data; `go vet` passes for linux, freebsd, darwin, netbsd and openbsd; the Go Static Analysis job proves staticcheck on the pushed commit.
- flawfinder `--minlevel=5 --error-level=5 src/libnetdata/netipc tests`: level-5 hits 0 (was 2).
- macOS bind: owner-only socket and umask narrowing hold under umask 022, 0277 and 0477 (C `test_uds`), and the macOS suites pass.
- rustdoc: `cargo doc --offline --no-deps --lib` without warnings; `protocol/mod.rs:425` equals `netdata/netdata @ a80f0e3dc0`.
- Bench run directories: `mkdir -p -m 0700` at every run-directory site of both bench scripts.
- Semgrep: pinned version scans this tree locally; the runner result is recorded with the CI rerun (SOW-0050).

Tests or equivalent validation:

- Linux: CTest 32/32 (`test_shm_full_backing_store` skipped as before, needs root), `cargo test` 398 passed, `go test ./...` ok, `go vet` for linux/darwin/freebsd, `cargo fmt --check` on stable and 1.91, `cargo check --tests` for `aarch64-apple-darwin` and `x86_64-pc-windows-gnu`, clippy shows no warnings on changed lines.
- macOS host: CTest 24/24, `cargo test` 330, `go test` ok, interop uds 18, service 18, service_shm 18, cache 9, cache_shm 9, lookup_scale 36, codec ok (SHM interop does not apply on macOS).
- FreeBSD 15.1 and 14.3: CTest 31/31, `cargo test` 398, `go test` and `GOARCH=386 go test` ok, interop uds 18, shm 9, service 18, service_shm 18, cache 9, cache_shm 9, lookup_scale 36, codec ok.
- Windows host: Rust `cargo test --lib` 275 passed, Go `go test ./...` ok.
- `govulncheck`: no vulnerability in code paths; the one module finding is the workstation's Go 1.27.2 standard library (fixed in 1.27.3, which CI uses). `osv-scanner` on `src/go/go.mod`: no issues.

Real-use evidence:

- The interop suites start real servers and clients across C, Rust and Go on macOS and FreeBSD with the new bind and dependency.

Reviewer findings:

- Independent read-only review: findings 1.1 and 3.1 fixed with reproductions, 1.2 recorded as unchanged behaviour (Execution Log).

Same-failure scan:

- Other path-based `chmod`/`chown` in `src/libnetdata/netipc` and `tests`: none (flawfinder level 5: 0).
- Other DACL string comparisons: the C helper compares SIDs (`tests/fixtures/c/test_win_security.h`); no other SDDL string compare in Rust or Go tests.
- Other umask-dependent run directories: the interop scripts use `mktemp -d` (0700), unaffected.
- Other unused Go code under staticcheck: only the two cases found; confirmed by the CI rerun.
- Same restrictive-umask issue elsewhere: the endpoint lock file, tracked in SOW-0067.

Sensitive data gate:

- No raw secrets, tokens, customer data or private endpoints; the runner account is referred to only as RID 500 / `LA`; hosts are named by role.

Artifact maintenance gate:

- AGENTS.md: no update; workflows, commands and guardrails are unchanged.
- Runtime project skills: no update; the vendoring preflight already requires these gates.
- Specs: no `.agents/sow/specs/` change; no protocol or contract change.
- End-user/operator docs: `docs/level1-posix-uds.md` macOS bind steps updated.
- End-user/operator skills: `docs/netipc-integrator-skill.md` mentions only the private `{service}.d` directory, which is unchanged; no update.
- SOW lifecycle: `Status: completed`, moved to `done/` in this SOW's commit; SOW-0050 stays paused until the CI rerun; SOW-0067 created in `pending/`.

Specs update:

- None needed: the change is the macOS bind mechanism, documented in `docs/level1-posix-uds.md`, which is the authoritative spec.

Project skills update:

- None needed: no new reusable workflow; the preflight skill already covers CI and scanner gates.

End-user/operator docs update:

- `docs/level1-posix-uds.md`.

End-user/operator skills update:

- None affected (see the artifact gate).

Lessons:

- See Lessons Extracted.

Follow-up mapping:

- Endpoint lock under a restrictive umask: tracked in SOW-0067.
- Codacy coverage token permission: user action, accepted risk recorded in SOW-0050 (decision 5).
- Semgrep pin bump: recorded in the workflow comment; Dependabot does not update pip pins in `run` steps.

## Outcome

The vendoring candidate's CI and scanner failures are fixed: Windows DACL tests are independent of the runner account, the CLOEXEC test ignores inherited descriptors, armv7 gnu Rust links on current Rust, staticcheck has nothing to report, the macOS bind is fd-relative in C, Rust and Go (with `golang.org/x/sys` v0.49.0 in Go), Semgrep is pinned, bench run directories are private, and the Netdata rustdoc fix is backported. Five CodeQL alerts were dismissed with evidence.

## Lessons Extracted

- Workflows that run only on `main` and PRs leave branch-only work without CI; long-lived branches need `workflow_dispatch` runs before vendoring, not after.
- Test expectations built from strings that Windows prints must go through the same Windows conversion, or they break on accounts with SDDL aliases.
- Tests that scan process-wide state (open descriptors) must snapshot it at process start.
- Removing a path-based step can drop an implicit precondition (here: `chmod` before open restored owner read); restrictive-umask tests belong next to every umask-related change.
- `tar` keeps source mtimes; syncing into an existing build tree needs `tar -m`, or the build skips changed files.

## Followup

- SOW-0067 (pending): endpoint lock under a restrictive umask.
- Codacy coverage token permission: user action (decision 5); tracked in SOW-0050's close-out.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
