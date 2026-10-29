# SOW-0046 - Go Cross-Platform Build

## Status

Status: open

Sub-state: created 2026-10-29 from the SOW-0041 analysis round; awaiting its turn in the one-issue-at-a-time decision sequence.

## Requirements

### Purpose

The pure-Go POSIX transport and service packages must build on every platform the docs claim and Netdata ships: Linux on all its architectures (including arm64 and 32-bit), FreeBSD and macOS.

### User Request

2026-10-29: "there are no known bugs. All bugs must be fixed now. Zero known bug policy." and "I want one issue at a time, with proper explanation, risks, proposed solutions, risk of the solutions recommended and explicit recommendations with reasoning."

### Assistant Understanding

Facts:

- `syscall.SYS_POLL` does not exist on linux/arm64 (and other newer architectures such as riscv64 and loong64, which only have `ppoll`), but is used by `src/go/pkg/netipc/transport/posix/uds_receive.go:91` and `src/go/pkg/netipc/service/raw/poll_unix.go:38`.
- Verified 2026-10-29: `GOOS=linux GOARCH=arm64 go vet ./...` in `src/go` fails with `undefined: syscall.SYS_POLL`.
- Found 2026-10-29 during SOW-0042 validation (`GOOS=<os> GOARCH=<arch> go build ./...` in `src/go`): the Go POSIX transport builds only on linux amd64, ppc64le and s390x. Failures:
  - linux/arm64, linux/riscv64: `undefined: syscall.SYS_POLL` (`uds_receive.go:91`, `service/raw/poll_unix.go:38`).
  - linux/386, linux/arm: `cannot use iovlen (variable of type uint64) as uint32 value` (`uds.go:170`, `syscall.Msghdr.Iovlen` width differs); linux/386 also `undefined: syscall.SYS_SENDMSG` (socketcall architecture).
  - freebsd/amd64: `Iovlen` is `int32` (`uds.go:170`).
  - darwin/arm64: `Iovlen` is `int32` and `undefined: syscall.MSG_NOSIGNAL` (`uds.go:177`; macOS uses `SO_NOSIGPIPE`).
- The code dates from 2026-03 (`git blame uds.go:165-172` -> 5372e97f1); `docs/level1-transport.md:463`, `:499` claim macOS/FreeBSD support.
- No CI job cross-builds Go for other architectures or operating systems, so nothing catches it.
- Unknown: whether C and Rust build on the same targets (Rust targets are not installed locally); to be checked when this SOW starts.

Inferences:

- Downstream Netdata consumers may be affected; not yet verified against Netdata usage.

Unknowns:

- Fix design choices are user decisions, presented one issue at a time.

### Acceptance Criteria

- `go build ./...` and `go vet ./...` succeed for linux amd64/386/arm/arm64/riscv64/ppc64le/s390x, freebsd/amd64 and darwin/arm64 (final list decided with the user); Go stays pure (no cgo).
- CI guards the cross-architecture build.

## Analysis

Sources checked:

- Read-only analysis round recorded in SOW-0041 (`.agents/sow/done/SOW-0041-20261029-rust-receive-timeout-slice.md`).

Current state:

- Bug confirmed; no fix.

Risks:

- Replacing `poll` with `ppoll` changes timeout encoding (timespec instead of milliseconds); errors there would affect every Go receive and SHM server wait.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- See Facts.

Evidence reviewed:

- See Facts.

Affected contracts and surfaces:

- To be completed when this SOW starts, after the user decision.

Existing patterns to reuse:

- To be completed when this SOW starts.

Risk and blast radius:

- To be completed when this SOW starts.

Sensitive data handling plan:

- No secrets or customer data involved; evidence is code paths and command output only.

Implementation plan:

1. To be completed after the user decision.

Validation plan:

- Cross-architecture `go vet`/build for linux amd64, arm64, riscv64 (and any other architecture Netdata ships) succeeds locally and in CI.
- Existing Go tests pass on linux/amd64 after the change.

Artifact impact plan:

- AGENTS.md: to be assessed.
- Runtime project skills: to be assessed.
- Specs: to be assessed.
- End-user/operator docs: to be assessed.
- End-user/operator skills: to be assessed (`docs/netipc-integrator-skill.md`).
- SOW lifecycle: created from the SOW-0041 analysis round.

Open-source reference evidence:

- None.

Open decisions:

- Presented to the user one issue at a time.

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
