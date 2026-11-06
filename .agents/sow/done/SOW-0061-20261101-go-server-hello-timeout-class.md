# SOW-0061 - Go Server HELLO Timeout Error Class

## Status

Status: completed

Sub-state: delivered 2026-11-01; Go servers on POSIX and Windows report HELLO timeout and abort as C and Rust do.

## Requirements

### Purpose

The Go server handshake must report a HELLO timeout with the same error class as C and Rust, so callers of the public transport APIs can tell a silent client from a failed receive.

### User Request

Found during the SOW-0049 review (2026-11-01). Zero-known-bug policy: confirmed bugs are fixed.

### Assistant Understanding

Facts:

- `src/go/pkg/netipc/transport/internal/framing/handshake.go` `ServerHandshake` wraps every `Recv` error as `config.ErrRecv("hello recv: ...")`. The message keeps only the text, so `errors.Is(err, posix.ErrTimeout)` is false.
- Probe (2026-11-01, Linux, Go 1.27): a client connects and sends nothing; `PendingConn.Handshake(1, cfg, 50, nil)` returns `recv failed or peer disconnected: hello recv: receive deadline expired`, with `errors.Is(err, ErrTimeout) == false` and `errors.Is(err, ErrRecv) == true`.
- The Go client handshake already keeps the wait error (`transport/posix/uds_handshake.go`, `waitErr`), so only the server side is affected.
- `service/raw` servers drop the handshake error (`service/raw/server_unix.go` `serveConnection`), so the visible effect is on direct users of `posix.Listener`/`PendingConn` (and the Windows equivalents).

- The Windows Go server goes through the same `framing.ServerHandshake` (`transport/windows/pipe_handshake.go:126`); its `Recv` is `rawRecvWithTimeout` (`pipe_receive.go:109`), whose wait returns `ErrTimeout`/`ErrAborted` (`pipe_receive.go:78`, `:90`), so it has the same gap.
- C and Rust return the HELLO wait's error unchanged: C `nipc_uds_wait_for_readable` before `nipc_uds_server_handshake` (`netipc_uds_lifecycle.c`, "One deadline covers the wait for HELLO"); Rust `handshake_inner` (`transport/posix.rs`, `wait_fd_readable(...)?`; `transport/windows.rs:1115`, `wait.wait_slice_ms()?` and `Err(NpError::Aborted)`).

Inferences:

- None.

Unknowns:

- None.

### Acceptance Criteria

- A Go server HELLO timeout (POSIX and Windows) satisfies `errors.Is(err, ErrTimeout)`; an abort satisfies `errors.Is(err, ErrAborted)`; matching C and Rust; covered by tests on both platforms.

## Analysis

Sources checked: the files cited in Facts, and the Go client handshakes, which already keep the wait error in a `waitErr` captured by their `Recv` closure (`transport/posix/uds_handshake.go`, `transport/windows/pipe_handshake.go:46-61`).

Current state: Go servers on POSIX and Windows report a HELLO timeout or abort as `ErrRecv`; C and Rust report TIMEOUT or ABORTED.

Risks: low. Only the error value returned by a failed server handshake changes; `service/raw` servers drop it (`service/raw/server_unix.go` `serveConnection`, same on Windows).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- `framing.ServerHandshake` turns every `Recv` error into a new `ErrRecv` with only the text, so the wait's `ErrTimeout`/`ErrAborted` is lost. See Facts.

Evidence reviewed:

- Code paths in Facts and Analysis; the 2026-11-01 probe.

Affected contracts and surfaces:

- Go `posix.PendingConn.Handshake`/`HandshakePrepared` and the Windows equivalents: error class only. Wire format, C and Rust: unchanged.

Existing patterns to reuse:

- The Go client handshake's `waitErr` capture; the posix server's existing `streamErr` capture (macOS), which this generalizes.

Risk and blast radius:

- Go only; handshake error class only; no hot path.

Sensitive data handling plan:

- Code paths and command output only, in the SOW, specs, docs, skills and code comments; hosts named by role.

Implementation plan:

1. POSIX `serverHandshakeWait`: keep the `Recv` error when it is `ErrTimeout` or `ErrAborted` (and, on a stream, `ErrProtocol`, as now) and return it.
2. Windows `serverHandshakeWait`: keep `ErrTimeout` or `ErrAborted` the same way.
3. Tests: call `serverHandshakeWait` on a connected pair whose client never sends HELLO, with a short deadline (expect `ErrTimeout`) and with a closed abort channel (expect `ErrAborted`); POSIX in `transport/posix`, Windows in `transport/windows`.

Validation plan:

- Each new test fails before the fix and passes after it; `go test ./...` and `go vet` on Linux, macOS, FreeBSD and Windows; Go part of the Netdata Linux targets is not needed (no architecture-specific code).

Artifact impact plan:

- Specs: check `docs/level1-posix-uds.md` and the Windows transport doc for a server HELLO timeout statement; update if they describe the error class.
- AGENTS.md, project skills, README, integrator skill: expected unaffected; confirm at close.

Performance assessment:

- Handshake only; nothing on the established-session path; no benchmark run.

Open decisions:

- None: the fix restores the documented parity with C and Rust and has no design fork.

## Implications And Decisions

- Scheduling (2026-11-01, user: "5A"): right after the SOW-0052 regression fix, before SOW-0059.

## Plan

See Pre-Implementation Gate, Implementation plan.

## Execution Log

### 2026-11-01

Created from the SOW-0049 review follow-up.

- Gate filled; moved to `current/`.
- `transport/posix/uds_handshake.go` `serverHandshakeWait`: the existing macOS-only `streamErr` capture became `waitErr`, which also keeps `ErrTimeout` and `ErrAborted` on every POSIX platform.
- `transport/windows/pipe_handshake.go` `serverHandshakeWait`: the same `waitErr` capture for `ErrTimeout` and `ErrAborted`.
- Tests: `TestServerHandshakeWaitKeepsTimeoutAndAbort` in `transport/posix/uds_more_edge_test.go` (socket pair) and `transport/windows/pipe_edge_test.go` (raw pipe pair). The client end never sends HELLO; the cases are a 20 ms deadline and an already-closed abort channel, so the outcome does not depend on timing.
- Specs: `docs/level1-posix-uds.md` and `docs/level1-windows-np.md` now state the server's expiry and abort error classes, as they already did for the client.

## Validation

Acceptance criteria evidence:

- Both new tests fail before the fix with "recv failed or peer disconnected: hello recv: receive deadline expired" (and "receive aborted"), on Linux and on Windows (pre-fix `pipe_handshake.go` copied onto the Windows host's tree), and pass after it.
- Linux: `go vet ./...` (linux, freebsd, darwin, windows), `go test ./...` all ok; `test_uds_interop.sh`, `test_service_interop.sh`, `test_service_shm_interop.sh` 18/18 each.
- macOS test host: `go vet`, `go test ./...` ok; `test_service_interop.sh` 18/18.
- FreeBSD 14.3: `go vet`, `go test ./...` ok; `test_service_interop.sh` 18/18.
- Windows: `go test ./...` all packages ok; CTest 20/20 (C/Rust/Go interop over pipes and SHM); `cargo test` 273 passed.
- Netdata Linux targets: not run; the change is Go source with no architecture- or libc-specific code, and the Go handshake paths are already covered by the native runs above.

Reviewer findings: none requested; the defect itself came from the SOW-0049 review.

Same-failure search: the only other place where a `Recv` error is wrapped as `ErrRecv` in the Go handshakes is the client side, which already keeps the wait error (`waitErr`) on both platforms. `framing.ServerHandshake` has no other callers.

Performance assessment: handshake only; nothing on the established-session path; no benchmark run.

Sensitive data gate: code paths, test names and command output only; hosts named by role.

Artifact maintenance gate:

- AGENTS.md: no change; no workflow or command changed.
- Runtime project skills: no change; the vendoring preflight is unaffected.
- Specs: `docs/level1-posix-uds.md` and `docs/level1-windows-np.md` (server expiry/abort error classes); `.agents/sow/specs/` holds no handshake error rules.
- End-user/operator docs: README does not describe handshake errors; no change.
- End-user/operator skills: `docs/netipc-integrator-skill.md` does not list handshake error classes; no change.
- SOW lifecycle: completed, moved to `done/`.

Spec update: see the artifact gate.

Project skill update: none needed (see above).

Lessons: see Lessons Extracted.

Follow-up mapping: none left.

## Outcome

Go servers on POSIX and Windows now return `ErrTimeout` for a HELLO that misses the deadline and `ErrAborted` for an aborted wait, matching C and Rust.

## Lessons Extracted

- A shared framing layer that rewraps errors as text loses their class; the transport layer must capture the errors it needs to keep, as the client already did.

## Followup

None.

## Regression Log

None yet.
