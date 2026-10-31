# SOW-0045 - Windows Pipe Security

## Status

Status: open

Sub-state: user decisions 1-5 recorded 2026-10-31; next to execute.

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

Status: blocked

Problem / root-cause model:

- See Facts.

Evidence reviewed:

- SOW-0040 evidence and tests.

Affected contracts and surfaces:

- To be completed before implementation.

Existing patterns to reuse:

- To be completed before implementation.

Risk and blast radius:

- To be completed before implementation.

Sensitive data handling plan:

- No secrets involved; Windows SIDs in evidence are replaced with `<owner-SID>`.

Implementation plan:

1. To be completed before implementation.

Validation plan:

- Add an automated reproduction test, confirm it fails before the fix and passes after, on `costa-win11`.

Artifact impact plan:

- AGENTS.md: to be assessed.
- Runtime project skills: to be assessed.
- Specs: to be assessed.
- End-user/operator docs: to be assessed.
- End-user/operator skills: to be assessed (`docs/netipc-integrator-skill.md`).
- SOW lifecycle: created from SOW-0040.

Open-source reference evidence:

- None.

Open decisions:

- Gathered when this SOW starts.

## Implications And Decisions

User decisions (2026-10-31, user):

1. Pipe DACL: explicit descriptor granting full access to the server's own account, SYSTEM and Administrators only.
2. Always set `PIPE_REJECT_REMOTE_CLIENTS`.
3. Clients open with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION` and, before sending HELLO, verify the pipe owner is the client's own account, SYSTEM or Administrators; otherwise the connect fails.
4. The same DACL applies to Windows SHM mappings and events.
5. Validation may create, on the Windows test host, a temporary non-admin local user and a scheduled task running a test server as SYSTEM; both are removed afterwards.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

To be written when this SOW moves to `current/`.

## Execution Log

None yet.

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
