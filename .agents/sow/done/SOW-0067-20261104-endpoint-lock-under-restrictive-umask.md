# SOW-0067 - Endpoint Lock And SHM Region Under A Restrictive Umask

## Status

Status: completed

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: implemented in C, Rust and Go (endpoint lock and SHM region file); validated on Linux, macOS and FreeBSD.

## Requirements

### Purpose

A POSIX listener must start, and must report a running peer as `AddrInUse`, whatever the process umask, as the socket bind already does.

### User Request

Found during SOW-0066 validation (2026-11-04); the user chose option A ("11a"): restore the lock file's mode through its descriptor after every open, and fall back to a narrower open when read-write is refused.

### Assistant Understanding

Facts:

- The endpoint lock `{run_dir}/{service_name}.lock` (SOW-0052) is opened `O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW` with mode `0600` (C `netipc_uds_lifecycle.c` `acquire_endpoint_lock`; Rust `posix.rs:549-560`; Go `uds_stale.go:42`). The file stays on disk by design.
- Under a umask that strips owner read or write (for example `0477`), the creator gets a file of mode `0200`; the creator's own open succeeds, but every later `O_RDWR` open by the same account fails with `EACCES`.
- Reproduced on the macOS host with the Rust test binary under `umask 0477`: `test_listen_excludes_starting_server` gets `Err(Socket(13))` instead of `AddrInUse`, and `test_listen_close_releases_lock_held_by_child` gets `Err(Socket(13))`. The code is shared by all POSIX platforms.
- Effects: a second server is told "socket error" instead of "address in use"; after the first server exits, the lock file it left blocks every restart of that endpoint until someone deletes or chmods it.

Inferences:

- Netdata runs with ordinary umasks (`022`, `007`), so production Netdata is not expected to hit this; embedders with unusual umasks would.

Unknowns:

- None that investigation can resolve; the remaining choice is a design decision.

### Acceptance Criteria

- Under umask `0477` and `0277`, a second listener gets `AddrInUse` while the first runs, and a listener starts after a crashed server left the lock file: C, Rust and Go tests that set the umask in a child process or a serialized test.
- Under umask `0477`, an SHM client attaches to a new region and the region file is `0600` (added by the same-failure scan): C, Rust and Go tests.

## Analysis

Sources checked:

- SOW-0066 Validation (macOS umask runs); the three lock implementations listed above.

Current state:

- Bug present in C, Rust and Go on Linux, FreeBSD and macOS.

Risks:

- Any fix that restores the lock file's mode by path reintroduces a path-based `chmod`, which flawfinder and Semgrep flag (SOW-0066); `fchmodat(..., AT_SYMLINK_NOFOLLOW)` is not supported for files on older Linux kernels.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The lock file inherits the umask; later opens need both owner read and write.

Evidence reviewed:

- Reproduction above.

Affected contracts and surfaces:

- POSIX listener start-up (C, Rust, Go); `docs/level1-posix-uds.md` endpoint lock section.
- POSIX SHM region creation (C, Rust, Go; Linux and FreeBSD); `docs/level1-posix-shm.md` create step (added by the same-failure scan).

Existing patterns to reuse:

- The socket bind's umask handling (`fchmod` before bind on Linux and FreeBSD; fd-relative bind on macOS).

Risk and blast radius:

- Listener start-up and SHM session set-up only; no per-request path, so no benchmark run (Test Cadence).

Sensitive data handling plan:

- No sensitive data involved; evidence is test names and error codes.

Implementation plan:

1. C `acquire_endpoint_lock`, Rust `acquire_endpoint_lock`, Go `acquireEndpointLock`: open `O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW` with mode 0600; on `EACCES` retry `O_WRONLY`, then `O_RDONLY` (both `O_CLOEXEC|O_NOFOLLOW`, no `O_CREAT`), since `flock` works on any open mode; then `fchmod` the descriptor to 0600 (best effort: the lock still works if another account owns the file) before `flock`.
2. Tests: C `test_uds` creates and contends the lock under umask 0477 and 0277, and starts after leftover lock files of mode 0200 and 0400; Rust and Go start after leftover lock files of mode 0200 and 0400 and see the mode restored (no umask change in multithreaded test runners).
3. `docs/level1-posix-uds.md` endpoint lock text states the mode handling.

Validation plan:

- Linux, FreeBSD and macOS suites; the new umask tests.

Artifact impact plan:

- AGENTS.md: unaffected.
- Runtime project skills: unaffected.
- Specs: `docs/level1-posix-uds.md` endpoint lock section states the lock file mode.
- End-user/operator docs: same file.
- End-user/operator skills: `docs/netipc-integrator-skill.md` if it mentions the lock file.
- SOW lifecycle: open; follows SOW-0050.

Open-source reference evidence:

- None; the evidence is this repository.

Open decisions:

- Resolved: decision 1 = A (user).

## Implications And Decisions

1. Lock file under a restrictive umask (user, 2026-11-04, "11a"): option A, restore the mode with `fchmod` on the lock descriptor after every open and fall back to a narrower open when `O_RDWR` is refused. Rejected: B, path-based `fchmodat` with `AT_SYMLINK_NOFOLLOW` (unsupported for files on older Linux kernels); C, documenting a umask requirement.
2. Assistant decision within A: the fallback goes `O_RDWR`, then `O_WRONLY`, then `O_RDONLY`. The read-only step also opens a leftover 0400 lock (umask 0277), which closes the gap the option description named. `flock` locks the open file description regardless of its access mode on Linux, FreeBSD and macOS.

## Plan

The implementation plan in the Pre-Implementation Gate, extended by the same-failure scan to the SHM region file (Execution Log).

## Execution Log

### 2026-11-04

- Created from the SOW-0066 macOS validation finding.
- Implemented decision 1 = A in C (`netipc_uds_lifecycle.c` `acquire_endpoint_lock`), Rust (`posix.rs` `acquire_endpoint_lock`) and Go (`uds_stale.go` `acquireEndpointLock`): `O_RDWR|O_CREAT`, then `O_WRONLY`, then `O_RDONLY` on `EACCES`, then a best-effort `fchmod(fd, 0600)` before `flock`.
- Tests: C `test_listen_lock_restrictive_umask` (`tests/fixtures/c/test_uds.c`: umask 0477 and 0277 create, contend and restart; leftover lock files 0200 and 0400), Rust `test_listen_over_leftover_lock_modes`, Go `TestListenOverLeftoverLockModes` (leftover 0200 and 0400, mode restored, second server `AddrInUse`).
- Reproduction before the fix, on the macOS host tree that still has the old lock code: C under umask 0477 gives second=2 restart=2 lock mode 0200 and under 0277 lock mode 0400; leftover 0200 and 0400 fail to listen (`NIPC_UDS_ERR_SOCKET`); Rust `listen over a 200 lock: Socket(13)`; Go `lock open: permission denied`. With the fix, Linux gives second=12 (`ADDR_IN_USE`), restart=0, lock mode 0600 in every case.
- `docs/level1-posix-uds.md` "Endpoint lock" states the mode handling.
- Performance assessment: the lock is taken once per `listen` (start-up only); nothing runs per request on an established session, so no benchmark run.
- Same-failure scan at close found the SHM region file has the same defect: the server creates it `0600 & ~umask` and the client attaches `O_RDWR` (`netipc_shm.c:637`). The existing C `test_shm` binary under umask 0477 failed every client attach. It is the same root cause and the same remedy as decision 1, so it is fixed here: the server `fchmod`s the new region's descriptor to 0600 right after creating it (C `netipc_shm.c`, Rust `shm.rs`, Go `shm_posix.go`; it runs once per SHM session set-up, not per request). New C test `test_shm_attach_under_restrictive_umask` fails on the FreeBSD 15.1 tree without the fix ("region file is 0600", "client attaches") and passes with it. Rust and Go SHM test binaries under umask 0477: Rust 21 failures with the old code (FreeBSD tree) against 3 with the fix, Go 10 passing tests against 18. The remaining failures are tests that create their own fixture files under that umask. `docs/level1-posix-shm.md` create step states the `fchmod`.
- Added Rust `test_shm_attach_under_restrictive_umask` and Go `TestShmAttachUnderRestrictiveUmask`. The umask is process-wide, so each test re-runs its own test binary as a child (helpers `shm_umask_helper_process` and `TestShmUmaskHelperProcess`) under umask 0477, which creates a region, checks it is 0600 and attaches a client. With the `fchmod` line commented out, both fail on Linux (Rust child exit status; Go `region file mode 200, want 600`); restored, both pass.
- A Linux and a FreeBSD 15.1 rerun first failed two Rust tests (`*_directory_symlink_*`): `Permission denied` removing target directories that my earlier manual umask 0477 run had left as mode 0300 under `/tmp/nipc_shm_rust_test`. Test environment debris, not a code fault: after restoring owner rwx on those leftovers, the same tests pass.

## Validation

Acceptance criteria evidence:

- C `test_listen_lock_restrictive_umask`: under umask 0477 and 0277 the first listen succeeds, a second gets `NIPC_UDS_ERR_ADDR_IN_USE`, the endpoint restarts and the lock file is 0600; leftover lock files 0200 and 0400 no longer block a listen and come back 0600. Before the fix the same test failed on the macOS host (Execution Log).
- Rust `test_listen_over_leftover_lock_modes` and Go `TestListenOverLeftoverLockModes`: pass with the fix, fail without it (macOS host: `Socket(13)`, `lock open: permission denied`).
- SHM: C `test_shm_attach_under_restrictive_umask`, Rust `test_shm_attach_under_restrictive_umask` and Go `TestShmAttachUnderRestrictiveUmask` fail without the region `fchmod` and pass with it (Execution Log).

Tests or equivalent validation:

- Linux, final tree: CTest 32/32, `cargo test` 401 passed, `go test ./...` ok, `cargo fmt --check` on stable and 1.91, `cargo +1.91 test --no-run` builds, flawfinder level 5: 0 hits (lock-fix tree; the SHM change adds an `fchmod` on a descriptor, which flawfinder does not flag).
- macOS host (lock-fix tree): CTest 24/24, `cargo test` 331, `go test` ok, interop uds 18, service 18, service_shm 18, cache 9, cache_shm 9, lookup_scale 36, codec ok. macOS has no SHM transport; the SHM change was checked there by `cargo check --tests --target aarch64-apple-darwin` and `GOOS=darwin go vet ./...` on Linux.
- FreeBSD 15.1 and 14.3, final tree: CTest 31/31, `cargo test` 401, `go test` and `GOARCH=386 go test` ok (the new Go umask test passes on both architectures), interop uds 18, shm 9, service 18, service_shm 18, cache 9, cache_shm 9, lookup_scale 36, codec ok.
- Windows: not affected; the endpoint lock and the SHM region file exist only in the POSIX transports.

Real-use evidence:

- The interop suites start real C, Rust and Go servers through the changed lock path on Linux, macOS and FreeBSD, and the SHM interop suites create and attach real regions across the three languages on Linux and FreeBSD.

Reviewer findings:

- No separate reviewer: a few lines per language on start-up and session set-up paths whose behaviour the new tests pin down, written against the reviewed SOW-0066 code. The reproductions before the fix and the passing runs after it are the evidence.

Same-failure scan:

- Other files NetIPC creates with a fixed mode and reopens later: the socket file (umask handling already specified and tested, SOW-0066), the macOS bind directory (SOW-0066), and the SHM region file, which the client reopens `O_RDWR`. The region file had the same defect and is fixed here (Execution Log). No other file NetIPC creates is opened again by name.

Sensitive data gate:

- No secrets, customer data or private endpoints; evidence is test names, modes and error codes.

Artifact maintenance gate:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: no update; no new workflow.
- Specs: `docs/level1-posix-uds.md` "Endpoint lock" and `docs/level1-posix-shm.md` create step updated (the public specs are authoritative); no `.agents/sow/specs/` change.
- End-user/operator docs: same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` describes the lock file's existence and lifetime and the SHM transport's use, none of which changed; no update.
- SOW lifecycle: `Status: completed`, moved to `done/` in this SOW's commit; SOW-0050 remains paused.

Specs update:

- `docs/level1-posix-uds.md`, `docs/level1-posix-shm.md`.

Project skills update:

- None needed; no reusable workflow changed.

End-user/operator docs update:

- `docs/level1-posix-uds.md`, `docs/level1-posix-shm.md`.

End-user/operator skills update:

- None affected (see the artifact gate).

Lessons:

- See Lessons Extracted.

Follow-up mapping:

- None remaining.

## Outcome

A POSIX server now starts and reports a running peer as address-in-use under any umask that leaves the creator able to open its own file, and a lock file left by a server that ran under a restrictive umask no longer blocks the endpoint. SHM clients attach to regions created under such a umask.

## Lessons Extracted

- A file that is reopened later must not rely on the umask to keep the opener's own access; restore its mode through the descriptor.
- Running existing test binaries under an unusual umask found this; keep restrictive-umask cases in the suites for every file NetIPC creates and reopens.
- Process-wide state such as the umask is tested by re-running the test binary as a child, never by changing it inside a multithreaded test runner. Manual experiments under such a umask leave debris (directories without owner read) that breaks later runs; restore owner access afterwards.

## Followup

None.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
