# SOW-0061 - Go Server HELLO Timeout Error Class

## Status

Status: open

Sub-state: bug confirmed on Linux by a probe test; not yet analyzed for Windows or designed.

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

Inferences:

- The Windows Go server goes through the same `framing.ServerHandshake`, so it likely has the same gap (to verify).

Unknowns:

- None that investigation cannot resolve: C and Rust server timeout classes are reported by the SOW-0049 review and must be checked against the code.

### Acceptance Criteria

- A Go server HELLO timeout (POSIX and Windows) satisfies `errors.Is(err, ErrTimeout)`; an abort satisfies `errors.Is(err, ErrAborted)`; matching C and Rust; covered by tests on both platforms.

## Analysis

Pending.

## Pre-Implementation Gate

Status: blocked

Not filled yet; required before implementation.

## Implications And Decisions

None yet.

## Plan

Pending.

## Execution Log

### 2026-11-01

Created from the SOW-0049 review follow-up.

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
