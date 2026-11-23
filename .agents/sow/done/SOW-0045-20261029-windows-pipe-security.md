# SOW-0045 - Windows Pipe Security

## Status

Status: completed

Sub-state: completed 2026-10-31.

## Requirements

### Purpose

Decide and enforce the Windows named-pipe access policy so that unauthorized local (or remote) principals cannot open NetIPC pipes.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- Pipes are created with default security (`NULL` attributes) in C (`netipc_named_pipe.c:217`), mirrored in Rust and Go.
- Observed DACL on `costa-win11` (user-session server, all three languages): `D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;<owner-SID>)(A;;FR;;;WD)(A;;FR;;;AN)` - Everyone and Anonymous have read access.
- A read-only open is enough to occupy a pipe instance and block the accept loop (observed 7.4 s client delay against the C server), so any local principal can likely cause a denial of service. Not verified with a second user.
- `PIPE_REJECT_REMOTE_CLIENTS` is not set; remote reachability over SMB is untested speculation.
- The default DACL derives from the creating token, so a LocalSystem service may differ from the user-session result.
- Analysis 2026-10-29: pipe squatting. Clients open the pipe without `SECURITY_SQOS_PRESENT` (C `netipc_named_pipe.c:903`, Rust `windows.rs:737-745`, Go `pipe_session.go:159-167`), so a server may impersonate them at full SecurityImpersonation level, and clients never verify the server's identity. A local user who creates the pipe name first (for example while the real server restarts) receives HELLO and the auth token.
- Analysis 2026-10-29: Windows SHM mappings and events are also created with NULL security attributes (C `netipc_win_shm.c:349-356`, `:409`, `:423`; Rust `win_shm.rs:405-407`, `:453`, `:474`; Go `shm.go:198-205`, `:260`, `:283`).

- Analysis 2026-10-31, Netdata usage (`netdata/netdata @ ae9113f2cc`):
  - Windows consumers: `apps.plugin` serves apps lookup (`src/collectors/apps.plugin/apps-lookup-netipc.c:318-352`), and `network-viewer.plugin` is a client (`src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.c:727-730`).
  - The auth token comes from the agent invocation ID (`src/libnetdata/netipc/netipc_netdata.c:9-12`), so only children of one agent share it.
  - The Windows service has no `Account` attribute (`packaging/windows/netdata.wxs.in:347-369`), so it runs as LocalSystem and its plugins inherit that account. Server and clients therefore always run as the same principal in production.
- Analysis 2026-10-31, current code:
  - Servers already use `FILE_FLAG_FIRST_PIPE_INSTANCE` (C `netipc_named_pipe.c:211`, Rust `windows.rs:1407`, Go `pipe_listener.go:258`), so an attacker cannot add instances to a pipe the server already owns. Squatting needs the attacker to create the name first.
  - The Windows SHM creators already reject an existing name (`ERROR_ALREADY_EXISTS`: C `netipc_win_shm.c:358,415,430`; Rust `win_shm.rs:418,462`; Go `shm.go:210,268,292`), so a pre-created mapping or event fails that session instead of being used.
  - With `FR` for Everyone, an unprivileged user can only open the pipe read-only. That cannot send HELLO, so it gets no data, but it occupies an instance (the DoS observed in SOW-0040).

Inferences:

- In production the risk is local: any user on a Netdata Windows host can occupy pipe instances, and can squat the pipe name while the agent restarts to collect the auth token. The token only grants apps lookup data, and it changes with every agent invocation.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- Explicit pipe security descriptor (and remote-client rejection) decided by the user and implemented in C, Rust and Go.
- Verified with a second, non-privileged Windows user and with a service-context server.
- Spec `docs/level1-windows-np.md` documents the access policy.

## Analysis

Sources checked:

- See SOW-0040 (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`) for evidence, test files and commands.

Current state:

- No automated reproduction test exists yet; the DACL exposure was verified manually on `costa-win11` during SOW-0040. This SOW must add one (for example a `GetSecurityInfo` DACL assertion) before the fix.

Risks:

- Cross-language behavior change; interop and Netdata vendoring must be validated.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Every Windows NetIPC object is created with `NULL` security attributes, so it takes the creator token's default DACL (Everyone and Anonymous read on the observed host), and clients trust whatever process owns the pipe name. See Facts.

Evidence reviewed:

- Facts above; creation and open sites:
  - C: `netipc_named_pipe.c` `create_pipe_instance()` (`CreateNamedPipeW(..., NULL)`), `create_client_pipe()` (`CreateFileW(..., 0 flags)`); `netipc_win_shm.c` mapping and event creation (`NULL` attrs).
  - Rust: `transport/windows.rs` `CreateNamedPipeW` (null attrs), `create_client_pipe()` (`CreateFileW`, flags 0); `transport/win_shm.rs` `CreateFileMappingW` / `CreateEventW` (null attrs).
  - Go: `transport/windows/pipe.go` `CreateNamedPipeW` call, `pipe_session.go:162` `syscall.CreateFile(..., 0, 0)`, `shm.go` mapping/event calls with 0 attrs.
- Windows test host token: the ssh session is an elevated administrator (High integrity, `BUILTIN\Administrators` enabled and "Group owner"), so objects it creates are owned by Administrators; `LanmanServer` is running, so `\\127.0.0.1\pipe\...` reaches the pipe through SMB loopback.
- L2 mapping of L1 connect errors: C `netipc_service_win_client_connect.c:90-101`, Rust `service/raw/client_windows.rs:88-92`, Go equivalent.

Affected contracts and surfaces:

- Windows transport in C, Rust, Go: pipe creation flags and DACL, client open flags, client owner check, SHM mapping/event DACL.
- New L1 error: C `NIPC_NP_ERR_UNTRUSTED_SERVER` (appended to `nipc_np_error_t`), Rust `NpError::UntrustedServer`, Go `ErrUntrustedServer`. L2 maps it to the existing `AUTH_FAILED` client state (the client could not authenticate the server), so no L2 enum changes.
- C build: `netipc_named_pipe` links `advapi32`. Netdata's Windows build must link it too (check in SOW-0050).
- Spec: `docs/level1-windows-np.md`, `docs/level1-windows-shm.md`; integrator guidance.

Existing patterns to reuse:

- Test hooks per language (`*_for_tests` in C under `NIPC_INTERNAL_TESTING`, Rust `#[cfg(test)]` statics, Go unexported vars).
- Existing Windows test binaries/suites (`test_named_pipe.c`, `test_win_shm.c`, Rust `windows.rs` / `win_shm.rs` tests, Go `transport/windows` tests).

Risk and blast radius:

- A server and client running as different non-admin accounts can no longer talk. Netdata runs both as LocalSystem; developers run both as one user.
- The owner check rejects a pipe owned by any account other than the client's own user, SYSTEM or Administrators: a legitimate server running as a different non-admin account is refused (intended).
- `PIPE_REJECT_REMOTE_CLIENTS` blocks SMB access; no supported use needs it.
- Building the security descriptor per pipe instance and per SHM object adds a few system calls to connection setup only.

Performance assessment:

- Connection setup and session SHM creation only (pipe instance creation per accept, client open, SHM object creation per SHM session). No established-session request path changes; no benchmark run.

Sensitive data handling plan:

- SIDs in evidence are written as well-known names (`SY`, `BA`) or `<owner-SID>`; the temporary test account is named generically and its password is generated at run time and never written to a durable artifact. Covers this SOW, specs, docs, skills and code comments.

Implementation plan:

1. Shared descriptor per language: SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<process user SID>)` built from the process token (`OpenProcessToken`, `GetTokenInformation(TokenUser)`, `ConvertSidToStringSidW`, `ConvertStringSecurityDescriptorToSecurityDescriptorW`), used for every pipe instance, SHM mapping and event.
2. Pipes: add `PIPE_REJECT_REMOTE_CLIENTS` to the pipe mode.
3. Clients: open with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`; after the open and before HELLO, read the pipe owner (`GetSecurityInfo`, `OWNER_SECURITY_INFORMATION`) and require the process user SID, `S-1-5-18` or `S-1-5-32-544`; otherwise close and return the untrusted-server error.
4. L2 maps the untrusted-server error to `AUTH_FAILED`.
5. Tests per language (deterministic): the listener pipe DACL is exactly the three ACEs (protected, no Everyone/Anonymous); SHM mapping and events carry the same DACL; an open through `\\127.0.0.1\pipe\` is refused; the server sees only an identification-level token when it impersonates the client; a test hook that removes the client's trusted owners makes connect fail with the untrusted-server error.
6. Manual cross-account validation on the Windows test host (user decision 5): a temporary non-admin user is refused by the pipe; a squatter pipe created by that user is rejected by a client; a SYSTEM-context server (scheduled task) serves an elevated client; both accounts/tasks removed afterwards.
7. Docs.

Validation plan:

- Windows ctest, Rust and Go suites on the Windows test host, with mutants for each new rule; the manual cross-account run; Linux/FreeBSD builds unaffected (Windows-only files) checked by building; interop scripts for Windows (`tests/test_*_win*.sh`) as covered by the ctest run.

Artifact impact plan:

- AGENTS.md: no change expected.
- Runtime project skills: `project-netdata-vendoring` unchanged; SOW-0050 records the `advapi32` link check.
- Specs: `docs/level1-windows-np.md` (access policy, remote rejection, client server check, new error), `docs/level1-windows-shm.md` (object DACL).
- End-user/operator docs: same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (same-account rule on Windows, AUTH_FAILED meaning).
- SOW lifecycle: this SOW only.

Open-source reference evidence:

- None needed; Windows API behavior is verified on the test host.

Open decisions:

- None. User decisions 1-5 are recorded below; the error mapping (new L1 error, L2 `AUTH_FAILED`) is assistant-applied as long-term-best: distinct at L1 for diagnosis, no new L2 state.

## Implications And Decisions

User decisions (2026-10-31, user):

1. Pipe DACL: explicit descriptor granting full access to the server's own account, SYSTEM and Administrators only.
2. Always set `PIPE_REJECT_REMOTE_CLIENTS`.
3. Clients open with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION` and, before sending HELLO, verify the pipe owner is the client's own account, SYSTEM or Administrators; otherwise the connect fails.
4. The same DACL applies to Windows SHM mappings and events.
5. Validation may create, on the Windows test host, a temporary non-admin local user and a scheduled task running a test server as SYSTEM; both are removed afterwards.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

See the gate implementation plan.

## Execution Log

### 2026-10-31

- Activated; gate filled.
- C: new `src/libnetdata/netipc/src/transport/windows/netipc_win_security.{c,h}` (owner-only security attributes, pipe owner check, test hook `nipc_np_set_trusted_owner_for_tests()`), linked with `advapi32`. `netipc_named_pipe.c` uses it for every pipe instance, adds `PIPE_REJECT_REMOTE_CLIENTS`, opens clients and the listener wake with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`, and checks the owner before HELLO (`NIPC_NP_ERR_UNTRUSTED_SERVER`). `netipc_win_shm.c` creates the mapping and both events with the same descriptor. The L2 server wake (`netipc_service_win_server.c`) uses the SQOS flags too; L2 connect maps the new error to `AUTH_FAILED`.
- Rust: new `transport/win_security.rs` (own `advapi32` FFI; thread-local test hook `TRUSTED_OWNER_FOR_TESTS`, since Rust tests run in parallel threads); same changes in `transport/windows.rs`, `transport/win_shm.rs`, `service/raw/client_windows.rs` (`NpError::UntrustedServer`).
- Go: new `transport/windows/security.go` (advapi32 LazyDLL, pure Go); same changes in `pipe.go`, `pipe_listener.go`, `pipe_session.go`, `shm.go`, `service/raw/client_windows.go` (`ErrUntrustedServer`).
- Tests (per language): pipe DACL readback for the first and a later instance; SHM mapping and event DACL readback; SMB loopback open `\\127.0.0.1\pipe\...` refused; connect refused when the trusted owner is `BUILTIN\Users` (bounded by a 2000 ms connect deadline so a regression fails instead of hanging); connect accepted when the trusted owner is the real pipe owner, with the server seeing only `SecurityIdentification` when it impersonates the client; L2 client ends in `AUTH_FAILED` against an untrusted owner (C `test_win_service`, Rust `raw_windows_tests`, Go classifier test).
- MSYS gcc lacks `_snwprintf`, so the C helper builds the SDDL with narrow `snprintf` and the `A` SDDL APIs.
- The temporary test account had no batch-logon right (`SeBatchLogonRight` is Administrators, Backup Operators and Performance Log Users on the test host), and `Start-Process -Credential` from the ssh session fails with `STATUS_DLL_INIT_FAILED`. The account was therefore added to Performance Log Users (no admin rights; checked by the script) and its processes ran as scheduled tasks.

## Validation

Acceptance criteria evidence:

- Explicit descriptor and remote-client rejection, decided by the user (decisions 1-4), implemented in C, Rust and Go: see Execution Log; DACL readback tests pass in all three languages.
- Verified with a second, non-privileged user and a service-context (SYSTEM) server, on the Windows test host with a scratch script (not committed: it creates accounts and hard-codes host paths). Results, 13 of 13 PASS:
  - the temporary account is not an administrator;
  - a Go client running as that account is refused by the C, Rust and Go servers running as the admin account (`connect failed: Access is denied.`);
  - a Go server running as that account squats a pipe name; the C (`connect failed: 22` = `NIPC_NP_ERR_UNTRUSTED_SERVER`), Rust and Go (`pipe owned by an untrusted account`) clients refuse it, and the squatter logs `hello recv: peer disconnected`, so no HELLO or auth token reached it;
  - C, Rust and Go servers started by a SYSTEM scheduled task serve C, Rust and Go clients of the admin account (`PASS`).
- Spec `docs/level1-windows-np.md` documents the access policy (new "Access policy" section).

Tests or equivalent validation (Windows test host, MSYS2):

- `win-rust-go.sh`: Rust `cargo test --lib` 271 passed; Go `go test ./...` all packages ok.
- `win-ctest-full.sh`: 20/20 ctest (C unit, robustness, interop C/Rust/Go for pipe, SHM, service and cache).
- Rust 1.91.0 (MSRV, MSVC toolchain) `cargo test --lib`: 271 passed.
- Mutation checks, each rule reverted on its own, every mutant killed: C 8/8 (pipe NULL descriptor, no `PIPE_REJECT_REMOTE_CLIENTS`, no SQOS, owner check always true, owner check removed, SHM mapping NULL descriptor, SHM events NULL descriptor, unprotected DACL); Rust 8/8 and Go 8/8 with the same mutants. A first C run hung on the two owner-check mutants (the client then sent HELLO to a listener that never accepted); the test now uses a 2000 ms connect deadline and the rerun killed them.
- Linux: `cmake --build build` clean; `cargo clippy` gate (Linux and `x86_64-pc-windows-gnu`) no correctness/suspicious findings; `cargo fmt --check` with stable and 1.91.0; `go vet ./...` (Linux and `GOOS=windows`) clean except the pre-existing `possible misuse of unsafe.Pointer` notes in `shm.go`/`shm_test.go`; `gofmt -l` clean.
- Timing sleeps added: none in correctness tests. The 2000 ms connect deadline is a bound on the failure path only; the passing path returns immediately.

Real-use evidence:

- The cross-account run above uses the real interop fixtures as separate processes under real accounts.

Reviewer findings:

- No external review was requested for this SOW.

Same-failure search:

- `CreateNamedPipe*`, `CreateFile*`, `CreateFileMapping*`, `CreateEvent*`, `CreateMutex*`, `CreateSemaphore*` across `src/libnetdata`, `src/crates/netipc/src` and `src/go/pkg`: every named object now takes the owner-only descriptor. The remaining `CreateEventW(NULL, ..., NULL)` calls (`netipc_service_win_client.c:75`, `netipc_service_win.c:147`) are unnamed, process-private events. The C L2 server wake (`netipc_service_win_server.c:32`) was the one remaining pipe open without SQOS flags and was fixed.

Performance assessment:

- Connection setup and SHM session creation only: `create_pipe_instance()` per accept, the owner check per connect, descriptor creation per SHM session (C `netipc_named_pipe.c:207-230`, `netipc_win_shm.c` `create_secure_mapping`/`create_secure_event`; the Rust and Go equivalents). No established-session request path changed; no benchmark run.

Sensitive data gate:

- The SOW, docs and comments name SIDs only as well-known aliases or `<server-SID>`; the test account password was generated in memory and never written; host and user names are by role. The local account SID printed in test logs was not copied into any artifact.

Artifact maintenance gate:

- AGENTS.md: no change; no workflow or command changed.
- Runtime project skills: `project-netdata-vendoring` unchanged; the vendoring consequence (new source file for Netdata's `SourceLists.cmake`) is recorded in SOW-0050.
- Specs: `docs/level1-windows-np.md` (Access policy), `docs/level1-windows-shm.md` (mapping and event DACL). No `.agents/sow/specs/` change: the public spec holds the rule.
- End-user/operator docs: the same two docs.
- End-user/operator skills: `docs/netipc-integrator-skill.md` (Windows same-account rule; `AUTH_FAILED` meaning).
- SOW lifecycle: this SOW completed and moved to `done/`; SOW-0050 updated.
- Build/test scripts: `tests/run-coverage-c-windows.sh` lists the new C source.

Spec update: done (see above).

Project skill update: none needed; no change in how work is done here.

End-user/operator docs and skills: updated (see above).

Lessons: see below.

Follow-up mapping:

- Netdata `SourceLists.cmake` entry for `netipc_win_security.c`: tracked in SOW-0050.
- Orphan profile folders of the temporary account on the Windows test host: the account was deleted, but Windows kept its two profile hives loaded after the scheduled tasks ended, so `Remove-CimInstance` was refused, and no process ran under those accounts. This is test-host housekeeping, not product work: the two `C:\Users\` profile folders of the deleted account are removed once Windows unloads them (or after the host's next reboot, which needs the user's approval); the user was told.
- No deferred work otherwise.

## Outcome

Windows pipes, SHM mappings and events now grant access only to SYSTEM, Administrators and the creating account, refuse remote clients, and clients refuse to send HELLO to a pipe owned by any other account; servers can only identify clients. All three languages, verified by unit tests, mutants and a cross-account run.

## Lessons Extracted

- Restoring a mutated source with `cp`/`mv` can leave it older than the object built from the mutant, so ninja keeps the mutant: a full ctest after the C mutant run failed on that stale object. Mutation scripts must `touch` the restored file (the Rust/Go script did).
- On the Windows test host, processes for another account cannot be started from the ssh session with `Start-Process -Credential`; scheduled tasks need the account to have batch-logon rights.
- A negative connect test against a listener that never accepts must carry a deadline, or the regression it guards against hangs the suite.

## Followup

- Netdata source list: SOW-0050.

## Regression Log

None yet.
