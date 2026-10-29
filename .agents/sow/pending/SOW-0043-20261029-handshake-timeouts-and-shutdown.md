# SOW-0043 - Handshake Timeouts And Shutdown

## Status

Status: open

Sub-state: created from SOW-0040 reproduction results; awaiting prioritization and design decisions.

## Requirements

### Purpose

Bound every connect/handshake on both sides, keep one slow or silent peer from blocking other clients or shutdown, and make stop/shutdown reliable.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- K3: the server reads HELLO inline on the accept thread with no timeout (C `netipc_uds_lifecycle.c:244-248`, `netipc_uds_handshake.c:216`; Windows `netipc_named_pipe.c:845-871`, `:661`; Rust and Go equivalents). One silent client blocks all accepts and shutdown. Reproduced in all languages on Linux and Windows (SOW-0040).
- K4: client connect and HELLO_ACK read have no timeout (C `netipc_uds_handshake.c:164`, `netipc_named_pipe.c:585`, `:903`; Rust `posix.rs:1108`, `:1171`, `windows.rs:1317`; Go `uds_handshake.go:27`, `:57`, `pipe_handshake.go:40`); `refresh()` takes no timeout. Reproduced in all languages and platforms.
- N8: non-timeout failures and overflow-resize retries grant a fresh full timeout per attempt (C `netipc_service_common.c:383-436`, Rust `client_call.rs:34-99`, Go `client.go:171-235`), contrary to `docs/level2-typed-api.md:261-264`. Not yet covered by a test.
- N2: shutdown joins workers blocked in mid-message reads or response writes (C `netipc_service_posix_server.c:418-426`, Rust `server_unix.rs:115-117`, Go `server_unix.go:168`); reproduced on all languages and platforms, including C Windows despite `CancelSynchronousIo`.
- N3: stop before run is lost (run sets running=true unconditionally); reproduced everywhere. In C, drain/destroy before run can free state that a later run uses.
- Code-review-only siblings to resolve here: N4 Rust Windows `stop()` closes a handle owned by `NpListener` (double close, `service/raw/server.rs:274-285`); N6 C Windows `CancelSynchronousIo` is one-shot and `sctx->thread` is written unlocked (`netipc_service_win_server.c:143-155`, `:320`); N7 C POSIX drain may `shutdown()` a reused fd (`netipc_service_posix_server.c:373-381`); N9 EINTR treated as disconnect (C `netipc_uds.c:38`, `:67`, `:76`, Rust `posix.rs:860`, `:884-889`); N10 Go `Stop()` closes the listener fd while `Run` may use it (`uds_listener.go:65-126`).
- Design questions: handshake deadline value and whether accept/handshake become concurrent with a cap on in-handshake clients; idle authenticated clients holding worker slots indefinitely; over-limit clients receive HELLO_ACK then a disconnect; Rust Windows `run(&mut self)` prevents calling `stop()` from another thread.
- Analysis 2026-10-29, worse than first described: C POSIX `drain()` before `run()` destroys `sessions_lock` that `run()` then locks (`netipc_service_posix_server.c:399-402`, `:231`); `destroy()` before `run()` makes `run()` poll fd -1 forever; C Windows `stop()` before `run()` closes the listener and `run()` spins on `Sleep(10)` forever.
- Analysis 2026-10-29: Rust Windows `stop()` can close a live session's pipe handle or a reused handle value (`service/raw/server.rs:108-115`, `transport/windows.rs:1171`); Rust `run(&mut self)` blocks cross-thread `stop()` on Unix too (`server_unix.rs:18`).
- Sibling bugs: C Windows `stop()` reads `server->listener.pipe` unlocked while accept replaces it, and `stop()` plus the end of `run()` can both close the listener (`netipc_service_win_server.c:129-135`, `:343-353`); C POSIX `destroy()` closes the listener before the accept loop exits (`netipc_service_posix_server.c:414-415`); `FlushFileBuffers` on server close blocks on a non-reading client (`netipc_named_pipe.c:942`, Go `pipe_session.go:140`, Rust client side `windows.rs:1082`).
- Sibling bugs: C reconnects up to 20 times inside one call with a drain sleep (`netipc_service_posix_client_connect.c:158-170`) while Rust and Go reconnect once as the spec says (`docs/level2-typed-api.md:242-244`); a call on a non-READY client returns `NOT_READY` in C but `BadLayout` in Rust (`client_call.rs:21-23`) and Go (`client.go:159-162`); SHM regions are created before HELLO is read for every accepted connection (`netipc_service_posix_server.c:204-209`); the 5 s SHM attach retry ignores the caller's timeout (`netipc_service_posix_client_connect.c:122`).

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions to be gathered when this SOW starts.

### Acceptance Criteria

- SOW-0040 K3, K4, N2, N3 tests (all languages, both platforms) pass.
- New tests for N8 retry accounting.
- Specs updated: handshake timeout behavior, total call deadline semantics (connect + handshake + retries), stop/shutdown guarantees.

## Analysis

Sources checked:

- See SOW-0040 (`.agents/sow/done/SOW-0040-20261029-robustness-repro-tests.md`) for evidence, test files and commands.

Current state:

- Reproduction tests exist and run ungated (the `NETIPC_REPRO_KNOWN_BUGS` gate was removed in SOW-0041 under the zero-known-bug policy); they fail until this SOW lands.

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

- The covered SOW-0040 reproduction tests must pass on Linux and `costa-win11`.

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

None yet.

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
