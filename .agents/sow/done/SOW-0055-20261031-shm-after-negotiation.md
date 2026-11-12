# SOW-0055 - Create SHM Regions Only For Sessions That Negotiate SHM

## Status

Status: completed

Sub-state: activated 2026-10-31 after SOW-0052 (`e38ffa7`) completed.

## Requirements

### Purpose

Stop managed servers from creating and destroying a per-session SHM region for every accepted connection, including rejected, silent and baseline-only clients.

### User Request

Zero-known-bug policy (AGENTS.md); the SOW-0040 decisions to fix every review-found defect. Split from SOW-0052 (assistant process decision, recorded there) because the fix needs a new step inside the L1 server handshake in C, Rust and Go on POSIX and Windows, and lands best as its own reviewable commit.

### Assistant Understanding

Facts (from SOW-0052, verified 2026-10-31 against `6609157`):

- C POSIX creates the region before HELLO is read (`netipc_service_posix_server_session.c` `server_prepare_accept_config`, called from `server_session_handshake` before `nipc_uds_accept_handshake`); it already waits for HELLO to be readable first, so silent clients no longer cost a region, but rejected and baseline-only clients still do.
- C Windows, Rust (`server_unix.rs`, `server_windows.rs`) and Go (`server_unix.go`, Windows server) follow the same create-before-negotiation order; exact lines to re-verify at activation.
- The SHM profile guarantee (`docs/level1-posix-uds.md`, `docs/level1-windows-np.md`): a successful HELLO_ACK that selects SHM means the region already exists, so creation must happen after negotiation but before HELLO_ACK is sent.

Inferences:

- Creating after negotiation also lets the region use the agreed payload sizes instead of the server maximums.

Unknowns:

- None that investigation cannot resolve.

### Acceptance Criteria

- Servers in C, Rust and Go, POSIX and Windows, create a region only when negotiation selects an SHM profile, before HELLO_ACK; a failed creation falls back to baseline in the same handshake when the client supports it.
- Tests prove a rejected or baseline-only client creates no region, and the SHM interop suites pass.

## Analysis

Sources checked:

- SOW-0052 facts.

Current state:

- No test covers region creation for non-SHM sessions.

Risks:

- Changes the L1 server handshake API shape in three languages; interop must stay identical on the wire.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The managed servers create SHM regions before the L1 handshake has read HELLO, because the L1 handshake decides the profile and sends HELLO_ACK in one step. The SHM profile guarantee requires the region to exist before an ACK that selects SHM. With no step between negotiation and ACK, the service layer can only create regions up front for every connection and destroy the ones it did not need.

Evidence reviewed (2026-10-31, `e38ffa7`):

- Up-front creation:
  - C POSIX: `netipc_service_posix_server_session.c` `server_prepare_accept_config` (`nipc_shm_server_create` at :321), called from `server_session_handshake` before `server_wait_hello` and `nipc_uds_accept_handshake`.
  - C Windows: `netipc_service_win_server_session.c` `server_prepare_accept_config` creates a hybrid region and a busywait region (`nipc_win_shm_server_create` at :287) for every pipe.
  - Rust: `server_unix.rs` `prepare_shm` (:277) and the Windows `AcceptContext` path (`server_windows.rs:238`).
  - Go: `server_unix.go` `prepareAcceptConfig` (`ShmServerCreate` at :88) and `server_windows.go` (`WinShmServerCreate` at :131, both profiles).
- L1 negotiation: C `netipc_uds_handshake.c` `nipc_uds_server_handshake` (:205-311) and `netipc_named_pipe.c` `server_handshake` (:651); Rust `posix.rs` `server_handshake` (:1540) and `windows.rs` `server_handshake` (:1563); Go `uds_handshake.go` `serverHandshakeWait` (:153) and `pipe_handshake.go` `serverHandshakeWait` (:120). Each computes the intersection, checks auth, selects the profile, checks the limits, then encodes and sends HELLO_ACK.
- Test hooks: C server test events (`netipc_service.h:349-350`, `tests/fixtures/c/test_sync.h:223`), Rust `TestHooks` (`server.rs:48`), Go `handshakeStarted` channels (`server_unix.go:39`, `server_windows.go:41`).

Affected contracts and surfaces:

- L1 server handshake APIs in C, Rust and Go, POSIX and Windows: a new optional prepare step. The wire format and the existing entry points are unchanged.
- Managed servers in all three languages, on both platforms.
- `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, and the SHM docs where they describe when regions exist.

Existing patterns to reuse:

- The split accept (`accept_pending` / `accept_handshake`) from SOW-0043.
- The server test-event hooks, for counting region creations.
- The current profile-drop fallback: when creation fails, the profile is removed from the server's supported and preferred sets.

Risk and blast radius:

- Every SHM session setup in all three languages. A wrong order would break the SHM profile guarantee (ACK selects SHM, client cannot attach). The interop suites run every client against every server.
- Wire: HELLO_ACK content changes only when a region creation fails. The failed profile is then removed from the server-supported and intersection fields, just as the up-front fallback does today.
- Request regions shrink from the server's learned maximum to the agreed request size. Clients never send more than the agreed size, which the L2 senders already check.

Performance assessment:

- Connection setup only (handshake and region creation). No code on an established session's request path changes, so no benchmark run.

Sensitive data handling plan:

- No secrets involved. This covers this SOW, the docs and the code comments.

Implementation plan:

1. L1, all three languages and both platforms: an optional prepare callback, called after negotiation and the limit checks and before HELLO_ACK (decision 1).
2. Managed servers: create the selected profile's region inside the callback, sized from the agreed limits, and drop the up-front creation.
3. Test event "SHM region created", in C, Rust and Go. Tests: a baseline-only client and a rejected client create no region; an SHM client creates exactly one; a failed creation falls back to baseline in the same handshake.
4. Docs.

Validation plan:

- The new tests fail on the previous code and pass now.
- Linux: full ctest, including the SHM interop suites, plus `cargo test` and `go test`.
- Windows: ctest, Rust and Go.
- The 9 Netdata Linux targets, run one at a time.
- rustfmt, the CI clippy gate, gofmt and go vet.

Artifact impact plan:

- AGENTS.md: no workflow change expected.
- Runtime project skills: none.
- Specs: `docs/level1-posix-uds.md`, `docs/level1-windows-np.md` (prepare step, region timing).
- End-user/operator docs: the same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` only if it states when regions are created. Checked at close.
- SOW lifecycle: split from SOW-0052.

Open-source reference evidence:

- None.

Open decisions:

- None; decisions 1-3 below are assistant-applied under the user's autonomy instruction (long-term-best).

## Implications And Decisions

1. The L1 server handshake gains an optional prepare callback, rather than a two-phase negotiate/finish API.
   - Signatures: C a function pointer plus user pointer, Rust `&mut dyn FnMut(profile, request_payload, response_payload) -> bool`, Go `func(profile, requestPayload, responsePayload uint32) bool`.
   - When it runs: after the intersection, auth, selection and limit checks, with the selected profile and the agreed payload limits.
   - If it returns false, the server removes that profile from its supported set and from the intersection, and selects again. When no profile is left, it rejects with UNSUPPORTED, as today's up-front fallback does.
   - The existing entry points keep their signatures and pass no callback.
   - Reason: one hook point keeps the ACK path in a single function per language. A two-phase API would split it and duplicate the state across calls.
2. Regions are sized from the agreed limits (request: agreed request payload + header; response: agreed response payload + header). Reason: they are known at that point, and the request region no longer needs to cover any proposal the client might make.
3. Windows creates only the selected profile's region, not both. Reason: the selection is known, and one region per session is the contract.

## Plan

As in the Pre-Implementation Gate implementation plan, in that order.

## Execution Log

2026-10-31:

- L1 prepare step, with the same reselect loop in every implementation (on false: drop the profile from the server set and the intersection, select again; nothing left: UNSUPPORTED / `NO_PROFILE`):
  - C POSIX: `nipc_uds_profile_prepare_fn` and `nipc_uds_accept_handshake_prepared()` (`netipc_uds.h`); `nipc_uds_server_handshake(..., prepare, prepare_user, session)` (`netipc_uds_handshake.c`); `nipc_uds_accept_handshake()` passes NULL (`netipc_uds_lifecycle.c`).
  - C Windows: `nipc_np_profile_prepare_fn` and `nipc_np_accept_handshake_prepared()` (`netipc_named_pipe.h/.c`).
  - Rust: `PendingConnection::handshake_prepared()` (`transport/posix.rs`) and `PendingPipe::handshake_prepared()` (`transport/windows.rs`); `handshake()` passes `None`.
  - Go: `ServerHandshakeConfig.Prepare` (`transport/internal/framing/handshake.go`); `ProfilePrepare` and `PendingConn.HandshakePrepared()` in `transport/posix` and `transport/windows`.
- Managed servers create the region inside the callback, only for an SHM profile, sized from the agreed limits plus the message header, using overflow-checked addition:
  - C: `server_prepare_profile()` (POSIX and Windows session files);
  - Rust: `create_shm()` (`server_unix.rs`, `server_windows.rs`);
  - Go: `createShm()` and the `headerPayloadLen()` helper (`server.go`, `server_unix.go`, `server_windows.go`).
  - `server_prepare_accept_config()` / `prepareAcceptConfig()` now only copy the learned limits and allocate the session id.
  - Removed: the up-front creation, Windows' two-profile `prepared_win_shm_t` / `PreparedWinShm` / `preparedWinShm`, and Rust's `prepare_shm`.
- Test hooks:
  - C: `NIPC_SERVER_TEST_EVENT_SHM_CREATED` (`netipc_service.h`), the `shm_created` counter (`tests/fixtures/c/test_sync.h`);
  - Rust: `TestHooks.shm_created` and `fail_shm_create` (`server.rs`);
  - Go: `shmCreated` and `failShmCreate` fields, plus `shmCreatedCount()` (`server_helpers_test.go`).
- Tests, each with the same scenario (baseline client 0 regions, wrong-auth client 0, SHM client 1, forced creation failure means baseline in one handshake and 0 regions):
  - C `test_shm_region_only_for_shm_sessions` (`test_service_extra.c`, `test_win_service_extra.c`);
  - Rust `test_shm_region_only_for_shm_sessions_unix` / `_windows`;
  - Go `TestUnixShmRegionOnlyForShmSessions` / `TestWinShmRegionOnlyForShmSessions`.
  - `server_windows_unit_test.go`: the `preparedWinShm` tests are replaced by `TestPrepareAcceptConfigUsesLearnedCapacities` (2 values now) and `TestCreateShmFailsForInvalidWinShmService`. An invalid service name now fails at SHM creation inside the handshake, not before it.
- Docs:
  - `docs/level1-wire-envelope.md`: the handshake rules allocate `session_id` before SHM preparation (the region name needs it, which matches the code), and the prepare/reselect step is stated.
  - `docs/level1-transport.md`: the SHM sequence diagram follows the same order.
  - `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`: the prepared accept APIs.
  - `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`: when a region is created and how it is sized.
  - `docs/netipc-integrator-skill.md`: SHM preparation failure.

## Validation

Acceptance criteria evidence:

- Region only for SHM sessions, before HELLO_ACK: the prepare callback runs inside each L1 server handshake, after the auth, selection and limit checks and before the ACK is encoded (for example C `netipc_uds_handshake.c`, the loop after the limit checks). Managed servers create regions only there, and only for an SHM profile.
- Same-handshake fallback: the forced-failure step of every new test gets a ready baseline session (`SelectedProfile == BASELINE`) from one refresh/handshake.
- Tests prove that baseline and rejected clients create no region: the new tests in C, Rust and Go, on POSIX and Windows, count `SHM_CREATED` events per step (0 / 0 / 1 / 0).

Tests or equivalent validation:

- Mutation and old-code checks on Linux:
  - C: the new test fails on the previous code (scratch tree with the old server session file);
  - Rust and Go: making the prepare callback create a region for every profile fails the new test at its baseline step ("baseline session ... left: 1 right: 0"; "baseline session created 1 SHM regions, want 0"). Both files were restored afterwards.
- Linux:
  - `ctest`: 32/32. `test_shm_full_backing_store` skips here because unprivileged namespaces cannot mount tmpfs (a pre-existing environment limit), so it was run as root with `NIPC_REQUIRE_TMPFS_TEST=1`: PASS, "C, Rust, and Go returned normal errors on a full tmpfs; no SIGBUS or leaked region".
  - `cargo test`: 385 passed.
  - `go test ./...`: ok.
  - The SHM and service interop suites run inside ctest.
- Windows (costa-win11, MSYS):
  - ctest 20/20;
  - Rust 265 passed (+1, the new test);
  - Go ok, with the three new or rewritten tests confirmed by `-v`;
  - `test_win_service_extra.exe`: 808 passed, including the new test.
- Static checks:
  - `cargo fmt`, `gofmt -l` clean; `go vet` clean for GOOS=linux and GOOS=windows.
  - CI clippy gate (`-D clippy::correctness -D clippy::suspicious`) passes. Strict `-D warnings` fails only on pre-existing style lints in files this SOW does not touch or on lines outside its diff (checked line by line).
- Netdata Linux targets: all 9 pass (x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl), run one at a time with no native C fixtures running alongside.

Real-use evidence:

- The interop suites run real C, Rust and Go clients against real servers over UDS+SHM, and the Windows suites do the same over pipes+SHM.

Reviewer findings:

- No external review was run for this SOW. The mutation checks stand in for the review of the new tests.

Same-failure search:

- `nipc_shm_server_create`, `nipc_win_shm_server_create`, `ShmServerCreate`, `WinShmServerCreate` and the Rust `ShmContext::server_create` / `WinShmContext::server_create` callers: the only service-layer callers left are the prepare callbacks. Test helpers create regions directly on purpose.

Performance assessment:

- Changed code runs at connection setup only: the handshake and region creation. The established-session request path (send/receive, SHM signalling, dispatch) is unchanged, so no benchmark run, per Test Cadence.
- Request regions now follow the agreed request size instead of the server's learned maximum. Clients never send more than the agreed size; the L2 senders already check it.

Artifact maintenance gate:

- AGENTS.md: no workflow change.
- Runtime project skills: none affected (the vendoring skill does not describe handshake internals).
- Specs: `docs/` is the authoritative spec surface; updated as listed in the Execution Log. No `.agents/sow/specs/` file covers SHM timing.
- End-user/operator docs: the same `docs/` files.
- End-user/operator skills: `docs/netipc-integrator-skill.md`, the SHM preparation failure section.
- SOW lifecycle: completed and moved to `done/` in the same commit as the code.

Sensitive data gate:

- No credentials, customer data, private endpoints or personal data were written to this SOW, the docs or the code comments. Auth tokens in the tests are synthetic constants.

Spec update: yes, `docs/level1-wire-envelope.md`, `docs/level1-transport.md`, `docs/level1-posix-uds.md`, `docs/level1-windows-np.md`, `docs/level1-posix-shm.md`, `docs/level1-windows-shm.md`.

Project skill update: none needed; no project workflow changed.

Lessons: see below.

Follow-up mapping: see below.

## Outcome

Managed servers in C, Rust and Go, on POSIX and Windows, create a session's SHM region only when the handshake selects an SHM profile. The region is created after the auth and limit checks and before HELLO_ACK, and is sized from the agreed limits. Windows creates only the selected profile's objects. A failed creation falls back to the next profile in the same handshake. Rejected and baseline clients no longer cost a region.

## Lessons Extracted

- The docs listed `session_id` allocation after SHM preparation, but every implementation needs the id first because it names the region. When a doc lists steps in order, check that order against the code whenever the code moves.
- `test_shm_full_backing_store` skips on this workstation without root. Run it as root with `NIPC_REQUIRE_TMPFS_TEST=1` whenever SHM creation changes.

## Followup

- None. Search of this SOW for defer/later/follow-up/future/TODO/pending: the only matches are API names (`accept_pending`, `PendingConnection`, `PendingPipe`, `PendingConn`) and the follow-up references themselves.

## Regression Log

None yet.
