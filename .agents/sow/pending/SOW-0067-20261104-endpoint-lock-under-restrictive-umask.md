# SOW-0067 - Endpoint Lock Under A Restrictive Umask

## Status

Status: open

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: confirmed by reproduction during SOW-0066 validation; needs a user design decision before implementation.

## Requirements

### Purpose

A POSIX listener must start, and must report a running peer as `AddrInUse`, whatever the process umask, as the socket bind already does.

### User Request

Pending: found by the assistant during SOW-0066 validation (2026-11-04); to be presented to the user.

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

## Analysis

Sources checked:

- SOW-0066 Validation (macOS umask runs); the three lock implementations listed above.

Current state:

- Bug present in C, Rust and Go on Linux, FreeBSD and macOS.

Risks:

- Any fix that restores the lock file's mode by path reintroduces a path-based `chmod`, which flawfinder and Semgrep flag (SOW-0066); `fchmodat(..., AT_SYMLINK_NOFOLLOW)` is not supported for files on older Linux kernels.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- The lock file inherits the umask; later opens need both owner read and write.

Evidence reviewed:

- Reproduction above.

Affected contracts and surfaces:

- POSIX listener start-up (C, Rust, Go); `docs/level1-posix-uds.md` endpoint lock section.

Existing patterns to reuse:

- The socket bind's umask handling (`fchmod` before bind on Linux and FreeBSD; fd-relative bind on macOS).

Risk and blast radius:

- Listener start-up only; no per-request path, so no benchmark run (Test Cadence).

Sensitive data handling plan:

- No sensitive data involved; evidence is test names and error codes.

Implementation plan:

1. After the user picks an option: implement in C, Rust and Go with tests under a restrictive umask.

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

1. How the lock file survives a restrictive umask. Candidate options, to be presented with evidence: (A) `fchmod` the lock descriptor to `0600` right after every successful open, and open with `O_WRONLY` as a fallback when `O_RDWR` fails with `EACCES` (`flock` needs any open mode); (B) restore the mode with `fchmodat(AT_FDCWD, lock, 0600, AT_SYMLINK_NOFOLLOW)` and retry, where supported; (C) document that the umask must keep owner read and write.

## Implications And Decisions

Pending user decision 1.

## Plan

Pending.

## Execution Log

### 2026-11-04

- Created from the SOW-0066 macOS validation finding.

## Validation

Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
