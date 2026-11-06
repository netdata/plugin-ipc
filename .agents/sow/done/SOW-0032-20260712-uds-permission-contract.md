# SOW-0032 - Align POSIX UDS permission handling across languages

## Status

Status: completed

Sub-state: delivered 2026-10-31.

## Requirements

### Purpose

Define and enforce one secure, portable POSIX Unix-domain-socket permission contract for
C, Rust, and Go without process-global permission side effects in embedding programs.

### User Request

The user delegated technical choices to the assistant with an explicit requirement to
choose the long-term-best design without shortcuts. SOW-0030 scanner triage discovered
this separate cross-language security and embedding-safety concern.

### Assistant Understanding

Facts:

- C currently changes the process-global `umask()` around `bind()` under a NetIPC-local
  mutex to create the socket as owner-only.
- A NetIPC-local mutex cannot serialize unrelated threads in the embedding process that
  create files while the temporary mask is active.
- Rust and Go bind their pathname sockets without the same explicit owner-only creation
  mechanism, so final modes depend on the embedding process mask.
- The documented transport contract expects a caller-controlled private runtime
  directory.

Inferences:

- Directory ownership and permissions are the portable primary security boundary for
  pathname sockets. Exact socket mode may remain useful as defense in depth, but it must
  not silently mutate process-global state in an embeddable library.

Unknowns:

- The portable mechanism and compatibility policy that should replace or constrain the
  current language-specific behavior require focused research and cross-platform tests.

### Acceptance Criteria

- One explicit UDS runtime-directory and socket-mode contract is documented and applied
  consistently to C, Rust, and Go.
- The implementation has no uncontrolled process-global permission window and includes
  concurrency tests that exercise unrelated file creation during listener setup.
- Existing cross-language UDS interoperability and stale-endpoint recovery remain green.

## Analysis

Sources checked:

- `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:16-44,212`
- `src/crates/netipc/src/transport/posix.rs:637-679,995-1019`
- `src/go/pkg/netipc/transport/posix/uds_listener.go:23-60`
- `docs/level1-transport.md:559-561`
- GitHub code-scanning alert 7750 and its historical/fixed commits, recorded in SOW-0030.

Current state:

- The historical post-bind `chmod(path)` race is fixed. The remaining issue is the C
  process-global `umask()` window and inconsistent C/Rust/Go permission behavior.

Risks:

- An unrelated thread can create a file with unexpectedly restrictive permissions while
  C temporarily changes the process mask, causing availability or operational failures.
- Changing modes or rejecting unsafe directories can break existing integrations unless
  compatibility and migration behavior are designed explicitly.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- C makes the listener socket owner-only by setting the process-global `umask()` around `bind()` under a library mutex (`netipc_uds_lifecycle.c`, `bind_owner_only_socket`). Any other thread of the embedding process that creates a file during that `bind()` gets mode `& ~0177`. Rust (`transport/posix.rs`, `UdsListener::bind`) and Go (`transport/posix/uds_listener.go`, `Listen`) set no mode, so their socket is `0777 & ~umask` (0755 with the usual 022 umask). The three languages therefore do not share one contract, and the C mechanism has process-wide side effects.

Evidence reviewed:

- Code above, plus the lock/probe/bind/listen order shared by all three (`acquire_endpoint_lock`, stale probe, bind, test hook, listen).
- Kernel behavior, measured on this workstation (Linux 7.0) and the FreeBSD 14.3 test VM: `fchmod(fd, mode)` on an unbound AF_UNIX socket succeeds, and `bind()` then creates the file with `mode & ~umask` (probe: umask 0, `fchmod 0600` -> file 0600, `0640` -> 0640; no fchmod -> 0777). A connect before `listen()` is refused on both.
- Kernel sources:
  - `torvalds/linux @ 36aad21d50d1af3f6954bb0c988257a908202477`, `net/unix/af_unix.c`, `unix_bind_bsd()`: `mode = S_IFSOCK | (SOCK_INODE(sk->sk_socket)->i_mode & ~current_umask())`.
  - `freebsd/freebsd-src @ 1fb5d8265bc4bbbf1f5bc79cc1d2a7f280b446b2`, `sys/kern/uipc_usrreq.c`: `uipc_chmod()` stores the mode while the socket is unbound (`unp->unp_mode = mode`); `uipc_bindat()` uses `mode = unp->unp_mode & ~...pd_cmask`.
- macOS is not covered: no SEQPACKET there and no reachable host (SOW-0049).

Affected contracts and surfaces:

- POSIX UDS listener creation in C, Rust and Go (listener setup only).
- The public UDS contract (`docs/level1-posix-uds.md`) and integrator guidance (`docs/netipc-integrator-skill.md`).
- Rust and Go listener sockets become 0600 (from `0777 & ~umask`); C keeps 0600.

Existing patterns to reuse:

- The C test linker wraps (`tests/fixtures/c/uds_eintr_wrap.c`, `--wrap=send/recv`) for a deterministic hook inside `bind()`.
- Existing C test `test_listen_socket_owner_only`.

Risk and blast radius:

- A Rust or Go server whose clients run as a different user, relying on a permissive umask, stops accepting them.
- Netdata check (`netdata/netdata @ ae9113f2cc`): the cgroups-lookup server (cgroups.plugin, in the daemon) runs as `netdata`; its clients apps.plugin and ebpf.plugin run as `netdata` (file capabilities) or root (setuid fallback). The apps-lookup server is apps.plugin (`src/collectors/apps.plugin/apps-lookup-netipc.c`), running as `netdata` (`packaging/cmake/Modules/Packaging.cmake:503`, capabilities) or root (setuid fallback, `netdata-installer.sh:956`); its client network-viewer.plugin is setuid root (`netdata-installer.sh:1079`). Every pair is same-user or root-client, so owner-only sockets work; the C server already creates them. The run dir is `RuntimeDirectoryMode=0775` (`system/systemd/netdata.service.in:15`).
- A platform whose kernel rejects `fchmod` on sockets would fail `listen` with a socket error instead of creating a wider socket; this is the intended fail-closed behavior.

Performance assessment:

- Listener setup only (one `fchmod` before `bind`). Nothing on an established session's request path changes; no benchmark run.

Sensitive data handling plan:

- Synthetic paths and service names only; kernel sources cited by upstream repo and commit. Covers this SOW, specs, docs, skills and code comments.

Implementation plan:

1. C: replace the `umask` block and its mutex with `fchmod(fd, 0600)` before `bind()`.
2. Rust and Go: the same `fchmod` before `bind()`.
3. Tests: C wraps `bind()` and, inside it, creates an unrelated file with mode 0666 and checks it gets the ambient umask (fails on the old code); Rust and Go assert the listener socket is 0600 (fails on the old code under umask 022).
4. Docs: "Socket permissions" in `docs/level1-posix-uds.md`; integrator note.

Validation plan:

- Linux: ctest, `cargo test`, `go test`, UDS/service interop; mutants (old C umask code, Rust/Go without `fchmod`) fail the new tests.
- FreeBSD: the AGENTS.md procedure.
- Linux targets (C change in vendored code); Windows unaffected (POSIX-only files), checked by build.

Artifact impact plan:

- AGENTS.md: unaffected (no workflow change).
- Runtime project skills: unaffected.
- Specs: `docs/level1-posix-uds.md`.
- End-user/operator docs: same file; `docs/level1-transport.md` run-dir note checked.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: this SOW only.

Open-source reference evidence:

- Linux and FreeBSD kernel sources above.

Open decisions:

- None. The user delegated this SOW's technical choices with a long-term-best requirement; decisions below are recorded under that delegation.

## Implications And Decisions

- Decision: track this independently from SOW-0030 because it affects UDS security and
  embedding semantics, not the Linux SHM allocation crash.
- Decision (2026-10-31, long-term-best): one contract in all languages. The listener socket is created with mode `0600 & ~umask` by `fchmod()` on the unbound socket before `bind()`. No process-global state is touched and no path-based `chmod` runs. Access control is owner-only plus the caller's private `run_dir`; clients must run as the server's user (or a privileged user). Rejected alternatives: path `chmod` between `bind` and `listen` (follows a swapped path in a writable directory and reintroduces the scanner finding); bind under a temporary name and `rename` (same path operation plus stale-name cleanup); leaving the mode to the directory alone (Rust/Go stay umask-dependent).

## Plan

1. Implement the gate's plan in C, Rust and Go with tests.
2. Docs.
3. Validate per the gate.

## Execution Log

### 2026-07-12

- Created from SOW-0030 scanner triage. No implementation started.

### 2026-10-31

- C `netipc_uds_lifecycle.c`: `bind_owner_only_socket()` now calls `fchmod(fd, 0600)` and `bind()`; the `umask()` calls, the library mutex and the `<pthread.h>` include are gone.
- Rust `transport/posix.rs` `UdsListener::bind` and Go `transport/posix/uds_listener.go` `Listen`: `fchmod(fd, 0600)` before `bind()`, failing with a socket error.
- Tests: C `test_listen_keeps_process_umask` (`test_uds.c`) with a `--wrap=bind` probe in `tests/fixtures/c/uds_eintr_wrap.c` that creates an unrelated 0666 file inside `bind()`; Rust `test_listen_socket_owner_only`; Go `TestListenSocketOwnerOnly`.
- Docs: "Socket permissions" in `docs/level1-posix-uds.md`; integrator note in `docs/netipc-integrator-skill.md`.

## Validation

Acceptance criteria evidence:

- One contract in all three languages: listener socket `0600 & ~umask`, set by `fchmod` before `bind` (code above; documented in `docs/level1-posix-uds.md`, "Socket permissions"). The runtime-directory requirement is documented there too.
- No process-global permission window: no `umask()` call remains in the library (`grep -rn umask src/libnetdata src/crates/netipc/src src/go/pkg` finds only comments). The C test creates an unrelated file from inside `bind()`; it keeps the process umask (0644 under umask 022).
- Interop and stale recovery: Linux ctest (UDS/service/cache interop, stale-recovery tests) and FreeBSD ctest pass.

Tests or equivalent validation:

- Mutants: C built against the previous `netipc_uds_lifecycle.c` fails "file created during bind keeps the process umask (0644)"; Rust without `fchmod` fails "listener socket must be owner-only" (got 0o775); Go without `Fchmod` fails `TestListenSocketOwnerOnly`. Originals restored and rebuilt.
- Linux: build warning-free; ctest pass except `test_shm_full_backing_store`, which skips without root (unprivileged namespaces blocked on this host) and exercises SHM, which this SOW does not touch. `cargo test` 387 passed; `go test ./...` ok; gofmt clean; `GOOS=freebsd go vet` clean.
- FreeBSD 14.3 test VM: ctest 24/24, `cargo test` 317 passed, `go test ./...` ok, `tests/interop_codec.sh` passed; the new C test passes there.
- Windows: the changed sources are POSIX-only; `cargo clippy --target x86_64-pc-windows-gnu` gate passes.
- Clippy CI gate passes on Linux and the Windows target.
- Netdata Linux targets: all 9 pass (`x86_64-musl`, `i386-gnu`, `i386-musl`, `armv7-gnu`, `armv7-gnu-time64`, `armv7-musl`, `armv6-musl`, `aarch64-gnu`, `aarch64-musl`).

Real-use evidence:

- UDS service and cache interop between C, Rust and Go servers and clients on Linux and FreeBSD, through the changed listener code.

Reviewer findings:

- No external review run; mutants show each new test fails on the old behavior.

Same-failure search:

- Other process-global state changes in the library: `grep -rn "umask\|setlocale\|signal(\|sigaction\|chdir" src/libnetdata/netipc src/crates/netipc/src src/go/pkg` — no other process-global mutation in transport code.
- Other created files: the endpoint lock (`0600`, `O_NOFOLLOW`) and SHM regions (`0600`, `O_EXCL`) already pass explicit modes and need no umask change.

Performance assessment:

- Listener setup only; nothing on an established session's request path changed. No benchmark run.

Sensitive data gate:

- No secrets, customer data, private endpoints or host names in this SOW, docs or code comments; upstream sources cited by repo and commit.

Artifact maintenance gate:

- AGENTS.md: no workflow change.
- Runtime project skills: `project-netdata-vendoring` unaffected (vendored file set unchanged).
- Specs: `docs/level1-posix-uds.md` "Socket permissions". No `.agents/sow/specs/` entry: the public spec carries the rule.
- End-user/operator docs: same file; `docs/level1-transport.md` run-dir note is consistent and unchanged.
- End-user/operator skills: `docs/netipc-integrator-skill.md` same-user note.
- SOW lifecycle: this SOW moves to `done/`.

## Outcome

C, Rust and Go create the POSIX listener socket owner-only through `fchmod` on the unbound socket. The C library no longer changes the process umask, so other threads of an embedding program keep their file modes, and Rust and Go sockets no longer depend on the ambient umask.

## Lessons Extracted

- A probe socket path longer than `sun_path` is silently truncated by `snprintf`, so a `stat` of the intended path reports a different file. Keep probe paths short or check the bound name.
- When a kernel source claim decides a design, measure it on each target kernel too: FreeBSD added `fchmod` on unbound sockets (`uipc_chmod`), which older references do not show.

## Followup

- macOS: whether its kernel honors `fchmod` on an unbound unix socket is unknown; it is part of the macOS transport design in SOW-0049 (no SEQPACKET there, so the listener code changes anyway).
- Search of this SOW for defer/later/follow-up/future/TODO/pending: the original 2026-07 text described this SOW itself as pending; no other open items.

## Regression Log

None yet.
