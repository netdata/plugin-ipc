# SOW-0049 - macOS Support

## Status

Status: open

Sub-state: transport decided (stream + framing) 2026-10-31; scheduled after SOW-0050.

## Requirements

### Purpose

NetIPC must build and run on macOS in C, Rust and Go, with cross-language interoperability.

### User Request

2026-10-29: "netipc should build and run on all these platforms" (Linux on all architectures, Windows, FreeBSD and macOS). Fix designs are presented to the user one issue at a time.

### Assistant Understanding

Facts:

- macOS has no AF_UNIX `SOCK_SEQPACKET` (apple-oss-distributions/xnu `bsd/kern/uipc_proto.c` registers only stream and datagram unix protocols), so the documented UDS SEQPACKET baseline (`docs/level1-transport.md:466`) cannot work as specified.
- Go: darwin lacks `syscall.MSG_NOSIGNAL` (`transport/posix/uds.go`; macOS uses `SO_NOSIGPIPE`); `service/raw/client_unix.go` and `server_unix.go` reference Linux-only SHM types.
- Rust: same Linux-only symbols as FreeBSD (`server_session_unix.rs:299` `__errno_location`, `server_unix.rs:43`).
- A macOS host is reachable for testing (macOS 26.5, arm64; has clang, cargo/rustc and bash 3.2; no cmake, ninja or go; checked 2026-10-31).
- Measured on that host (2026-10-31): `socket(AF_UNIX, SOCK_SEQPACKET, 0)` fails with "Protocol not supported"; `fchmod()` on an unbound AF_UNIX stream socket fails with EINVAL, and the socket file is created `0777 & ~umask`. The SOW-0032 owner-only mechanism (`fchmod` before `bind`) therefore fails closed on macOS, so the macOS listener needs its own owner-only mechanism.

Inferences:

- A macOS baseline transport needs a design: stream socket with message framing, or datagram socket; either changes the transport spec for macOS.

Unknowns:

- The transport design choice (user decision).

### Acceptance Criteria

- C, Rust and Go build and pass their test suites on macOS; interop across all language pairs passes there.
- The transport spec documents the macOS baseline.

## Analysis

Sources checked:

- SOW-0046 investigation (`.agents/sow/done/SOW-0046-20261029-go-cross-platform-build.md`).

Current state:

- Does not build; the specified transport does not exist on the platform.

Risks:

- A framing layer for stream sockets is new protocol surface that must stay identical across three languages.

## Pre-Implementation Gate

Status: needs-user-decision

Problem / root-cause model:

- The baseline transport relies on a socket type macOS lacks; see Facts.

Evidence reviewed:

- See Facts.

Affected contracts and surfaces:

- To be completed after the transport decision: transport spec, all three POSIX transports, build gating.

Existing patterns to reuse:

- To be completed after the transport decision.

Risk and blast radius:

- To be completed after the transport decision.

Sensitive data handling plan:

- No secrets, customer data or host details; evidence is code paths and command output only, across SOWs, specs, docs, skills and code comments.

Implementation plan:

1. To be completed after the user decisions.

Validation plan:

- Full C/Rust/Go suites and interop on a macOS host.

Artifact impact plan:

- AGENTS.md: add macOS validation commands if they become project-wide.
- Runtime project skills: assess at close.
- Specs: `docs/level1-transport.md`, `docs/level1-posix-uds.md`.
- End-user/operator docs: same.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: split from SOW-0046.

Open-source reference evidence:

- apple-oss-distributions/xnu `bsd/kern/uipc_proto.c` (commit to be recorded when activated).

Open decisions:

- macOS baseline transport design, presented to the user when this SOW starts.

## Implications And Decisions

User decisions (2026-10-31, user):

1. macOS baseline transport: AF_UNIX `SOCK_STREAM` with explicit message framing for the NetIPC envelope (new spec section; C, Rust and Go).
2. cmake, ninja and Go (latest stable) may be installed on the macOS test host.
3. Scheduling: after the SOW-0050 Netdata vendoring (Netdata does not build NetIPC on macOS).

## Plan

To be written when activated.

## Execution Log

### 2026-10-29

Created from the SOW-0046 platform-scope split.

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
