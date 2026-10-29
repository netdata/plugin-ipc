# SOW-0048 - FreeBSD Support

## Status

Status: open

Sub-state: split from SOW-0046 by the 2026-10-29 platform-scope decision; investigation facts recorded, no design decisions made yet.

## Requirements

### Purpose

NetIPC must build and run on FreeBSD in C, Rust and Go, with cross-language interoperability, as the docs already claim.

### User Request

2026-10-29: "netipc should build and run on all these platforms" (Linux on all architectures, Windows, FreeBSD and macOS). Fix designs are presented to the user one issue at a time.

### Assistant Understanding

Facts:

- C: the build fails on `#include <linux/futex.h>` because CMake enables the SHM transport `if(NOT APPLE)` (`CMakeLists.txt:69`), which includes FreeBSD.
- Rust: `transport/posix` server code uses Linux-only symbols: `__errno_location` (`server_session_unix.rs:299`) and the Linux-only `prepare_unix_accept` (`server_unix.rs:43`).
- Go: `GOOS=freebsd go vet` failed before SOW-0046 on the `Msghdr.Iovlen` width; SOW-0046 fixed that. `service/raw/client_unix.go` and `server_unix.go` reference Linux-only SHM types, so the service layer still does not build.
- FreeBSD 13/14 cap an AF_UNIX SEQPACKET packet at `SO_SNDBUF` (default 8 KiB); FreeBSD 15 reworked unix sockets (review D44151). The SOW-0042 packet-size proposal `min(SO_SNDBUF-32, 262144)` must be checked against this.
- `docs/level1-transport.md:466` and `:502` claim FreeBSD support with UDS only.

Inferences:

- SHM stays Linux-only on FreeBSD per the docs; the build gate must say so in all three languages.

Unknowns:

- FreeBSD test host availability (the user is preparing a VM).

### Acceptance Criteria

- C, Rust and Go build and pass their test suites on FreeBSD; interop across all language pairs passes there.
- CI or a documented manual procedure guards FreeBSD.

## Analysis

Sources checked:

- SOW-0046 investigation (`.agents/sow/done/SOW-0046-20261029-go-cross-platform-build.md`).

Current state:

- Does not build in any language.

Risks:

- Packet-size negotiation differences across FreeBSD versions.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Linux-only code is compiled unconditionally on non-Linux unix; see Facts.

Evidence reviewed:

- See Facts.

Affected contracts and surfaces:

- To be completed when activated: build gating in CMake, Rust cfg, Go build tags; UDS packet sizing.

Existing patterns to reuse:

- SOW-0046 `internal/syspoll` split (`poll_other.go` already serves non-Linux unix).

Risk and blast radius:

- To be completed when activated.

Sensitive data handling plan:

- No secrets, customer data or host details; evidence is code paths and command output only, across SOWs, specs, docs, skills and code comments.

Implementation plan:

1. To be completed after the user decisions.

Validation plan:

- Full C/Rust/Go suites and interop on a FreeBSD host.

Artifact impact plan:

- AGENTS.md: add FreeBSD validation commands if they become project-wide.
- Runtime project skills: assess at close.
- Specs: `docs/level1-transport.md`, `docs/level1-posix-uds.md` platform notes.
- End-user/operator docs: same.
- End-user/operator skills: `docs/netipc-integrator-skill.md`.
- SOW lifecycle: split from SOW-0046. Overlaps pending SOW-0031 (C SHM CMake gating with `NOT APPLE`); merge the two before implementation (one SOW at a time).

Open-source reference evidence:

- FreeBSD unix socket rework: review D44151 (to be cited with commit when used).

Open decisions:

- Presented to the user one issue at a time.

## Implications And Decisions

None yet.

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
