# SOW-0051 - Client Call Deadlines

## Status

Status: open

Sub-state: split from SOW-0043 on 2026-10-31 (client-side items); starts after SOW-0043 completes.

## Requirements

### Purpose

Bound every client connect, handshake and retry by the caller's call deadline and abort signal, and make the C, Rust and Go clients agree on retry and not-ready behaviour.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (handshake/timeout bounds and shutdown among them). SOW-0043 was split again on 2026-10-31 (assistant process decision, recorded in SOW-0043) so each SOW lands as one reviewable, bisectable commit; this SOW carries the client-side items.

### Assistant Understanding

Facts (line numbers verified 2026-10-31 against commit `b450b40`):

- K4: client connect and HELLO_ACK read have no deadline. C `nipc_uds_connect` `connect()` `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:325`, `nipc_uds_client_handshake` recv `netipc_uds_handshake.c:162`; Windows `nipc_np_connect` CreateFileW `netipc_named_pipe.c:903`, `client_handshake` `raw_recv` `:585`; Rust `posix.rs:1155`/`:1082` (connect), `:1214` (recv), `windows.rs:733` (CreateFileW), `:1313` (recv); Go `uds_handshake.go:27` (connect), `:54` (recv), `pipe_session.go:159`, `pipe_handshake.go:40`. `refresh()` takes no timeout (C `netipc_service.h:191`, Rust `client.rs:296`, Go `client.go:84`). Reproduced in all languages and platforms by the SOW-0040 tests (`test_client_bounded_when_server_never_accepts`, `test_client_bounded_when_server_never_sends_hello_ack` and their Rust/Go/Windows equivalents).
- N8: non-timeout failures and overflow-resize retries grant a fresh full timeout per attempt (C `netipc_service_common.c:355-435`, Rust `client_call.rs:14-108`, Go `client.go:158-242`), contrary to the deadline text in `docs/level2-typed-api.md:266-271`. Not yet covered by a test.
- Found by SOW-0053: a client that reconnects to a drained or stopped server whose listener is still open blocks forever in the HELLO_ACK read (seen as a C `test_service` drain hang; the test now holds its sessions open across the drain). Same root cause as K4; the deadline must cover it.
- SOW-0053 added a bounded `ERROR_PIPE_BUSY` wait to the Windows connects (`WaitNamedPipeW`, at most 1000 ms, then one more `CreateFileW`; C `open_client_pipe` in `netipc_named_pipe.c`, Rust `open_client_pipe` in `windows.rs`, Go `openClientPipe` in `pipe_session.go`). The call deadline must also bound that wait.
- Client abort is observed only while waiting for a response; connect and HELLO_ACK waits, including the reconnects a call performs internally, ignore it (C `netipc_uds_handshake.c:162`, `netipc_named_pipe.c:585`; Rust `posix.rs:1214`, `windows.rs:1313`; Go `framing/handshake.go:148`). Abort is checked only before an attempt (`netipc_service_common.c:325`, `client_call.rs:118`).
- The 5 s SHM attach retry ignores the caller's timeout (C `netipc_service_posix_client_connect.c:123`, `netipc_service_win_client_connect.c:126`; Rust `client_unix.rs:27-28`; Go `client.go:11`, `client_unix.go:94`).
- C reconnects up to 20 times inside one call with a drain sleep (`netipc_service_posix_client_connect.c:171-185`, constants `netipc_service_posix_internal.h:13-16`, sleep `netipc_service_common.c:386-387`; Windows `netipc_service_win_client_connect.c:179-187`) while Rust and Go reconnect once as the spec says (`docs/level2-typed-api.md:242-244`; `client_call.rs:57`, `client.go:195`).
- A call on a non-READY client returns `NOT_READY` in C (`netipc_service_common.c:358-361`) but `BadLayout` in Rust (`client_call.rs:21-23`) and Go (`client.go:159-163`).
- Resolved by SOW-0043 (2026-10-31): Rust Windows session close called `FlushFileBuffers` for both roles, so a client close blocked on a server that stopped reading. SOW-0043 removed the flush and `DisconnectNamedPipe` from session close in C, Rust and Go (close now only closes the handle), because the server-side flush blocked shutdown in a way no other thread can release.

Inferences:

- Netdata's C clients call with a 5 s timeout; a server that accepts but never answers HELLO today blocks them indefinitely.

Unknowns:

- None that investigation cannot resolve; design choices are made at activation.

### Acceptance Criteria

- The SOW-0040 K4 tests pass in all languages on Linux and Windows.
- New tests prove the whole call (connect, handshake, attach, retries) is bounded by one deadline and observes abort.
- C, Rust and Go agree on reconnect count and the not-ready error.
- `docs/level2-typed-api.md` states the total call deadline semantics.

## Analysis

Sources checked:

- SOW-0040 evidence and tests; SOW-0043 facts (verified 2026-10-31).

Current state:

- The K4 reproduction tests run ungated and fail until this SOW lands.

Risks:

- Changing retry counts and error codes is a cross-language behaviour change; interop suites and Netdata callers must be checked.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Waits outside the response-wait path were written without a deadline; see Facts.

Evidence reviewed:

- SOW-0040 tests and the verified locations above; the full gate is completed at activation, after SOW-0043 lands, because SOW-0043 changes the server side these clients talk to.

Affected contracts and surfaces:

- L1 connect/handshake APIs, L2 client call/refresh semantics, error codes, docs, integrator skill, tests in all languages.

Existing patterns to reuse:

- The receive-with-deadline-and-abort helpers (`nipc_uds_receive_timeout`, `nipc_np_receive_timeout` and the Rust/Go equivalents).

Risk and blast radius:

- All client calls; recorded fully at activation.

Sensitive data handling plan:

- No secrets involved; Windows SIDs in evidence are written as `<owner-SID>`. Applies to this SOW, docs, the integrator skill and code comments.

Implementation plan:

1. Completed at activation.

Validation plan:

- K4 tests and new deadline/abort tests on Linux, the Netdata Linux targets and Windows; interop suites.

Artifact impact plan:

- AGENTS.md: assess at activation.
- Runtime project skills: assess at activation.
- Specs: `docs/level2-typed-api.md` deadline semantics.
- End-user/operator docs: same.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: split from SOW-0043.

Open-source reference evidence:

- None.

Open decisions:

- Made at activation (assistant-applied under the user's autonomy instruction, recorded with reasoning).

## Implications And Decisions

None yet.

## Plan

Written at activation.

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
