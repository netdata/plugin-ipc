# SOW-0032 - Align POSIX UDS permission handling across languages

## Status

Status: open

Sub-state: pending investigation after SOW-0030; no implementation started.

## Requirements

### Purpose

Define and enforce one secure, portable POSIX Unix-domain-socket permission contract for
C, Rust, and Go without process-global permission side effects in embedding programs.

### User Request

The user delegated technical choices to the assistant with an explicit requirement to
choose the long-term-best design without shortcuts. SOW-0030 scanner triage discovered
this separate cross-language security and embedding-safety concern.

### Assistant Understanding

Facts:

- C currently changes the process-global `umask()` around `bind()` under a NetIPC-local
  mutex to create the socket as owner-only.
- A NetIPC-local mutex cannot serialize unrelated threads in the embedding process that
  create files while the temporary mask is active.
- Rust and Go bind their pathname sockets without the same explicit owner-only creation
  mechanism, so final modes depend on the embedding process mask.
- The documented transport contract expects a caller-controlled private runtime
  directory.

Inferences:

- Directory ownership and permissions are the portable primary security boundary for
  pathname sockets. Exact socket mode may remain useful as defense in depth, but it must
  not silently mutate process-global state in an embeddable library.

Unknowns:

- The portable mechanism and compatibility policy that should replace or constrain the
  current language-specific behavior require focused research and cross-platform tests.

### Acceptance Criteria

- One explicit UDS runtime-directory and socket-mode contract is documented and applied
  consistently to C, Rust, and Go.
- The implementation has no uncontrolled process-global permission window and includes
  concurrency tests that exercise unrelated file creation during listener setup.
- Existing cross-language UDS interoperability and stale-endpoint recovery remain green.

## Analysis

Sources checked:

- `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:16-44,212`
- `src/crates/netipc/src/transport/posix.rs:637-679,995-1019`
- `src/go/pkg/netipc/transport/posix/uds_listener.go:23-60`
- `docs/level1-transport.md:559-561`
- GitHub code-scanning alert 7750 and its historical/fixed commits, recorded in SOW-0030.

Current state:

- The historical post-bind `chmod(path)` race is fixed. The remaining issue is the C
  process-global `umask()` window and inconsistent C/Rust/Go permission behavior.

Risks:

- An unrelated thread can create a file with unexpectedly restrictive permissions while
  C temporarily changes the process mask, causing availability or operational failures.
- Changing modes or rejecting unsafe directories can break existing integrations unless
  compatibility and migration behavior are designed explicitly.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- POSIX `umask()` is process-global, but the C library uses it inside a local mutex that
  cannot protect unrelated embedding threads. Rust and Go rely on ambient process state,
  so the three language implementations do not expose one stable security contract.

Evidence reviewed:

- Initial evidence is listed under Analysis. Full POSIX/Linux/macOS/FreeBSD behavior,
  project patterns, and mature open-source approaches remain to be researched before
  implementation.

Affected contracts and surfaces:

- POSIX UDS listener creation in C, Rust, and Go; runtime-directory requirements;
  embedding-process behavior; UDS tests; public transport docs and integrator guidance.

Existing patterns to reuse:

- Safe directory-relative operations already used by POSIX SHM and stale UDS cleanup;
  the existing private-runtime-directory contract; cross-language interop fixtures.

Risk and blast radius:

- Cross-language behavioral and source compatibility, endpoint availability, filesystem
  permissions, macOS/FreeBSD portability, and embedding-process concurrency.

Sensitive data handling plan:

- Use synthetic paths and service names only. No credentials, customer data, private
  endpoints, personal data, or production logs are required in durable artifacts.

Implementation plan:

1. Research portable directory and socket permission primitives and existing project or
   mature open-source patterns across Linux, macOS, and FreeBSD.
2. Present any irreducible public-contract choices before implementation.
3. Implement one C/Rust/Go contract with concurrency, permission, stale-path, and interop
   coverage, then update public docs and integrator guidance.

Validation plan:

- Deterministic concurrent unrelated-file creation tests; mode and ownership assertions;
  C/Rust/Go interop; stale endpoint recovery; Linux, macOS, FreeBSD, and sanitizer runs.

Artifact impact plan:

- AGENTS.md: likely unaffected unless a reusable security workflow is discovered.
- Runtime project skills: likely unaffected unless a reusable validation workflow is
  discovered.
- Specs: POSIX UDS/transport contract likely updated.
- End-user/operator docs: integrator/runtime-directory guidance likely updated.
- End-user/operator skills: `docs/netipc-integrator-skill.md` likely updated.
- SOW lifecycle: remains pending until SOW-0030 completes; must be activated separately.

Open-source reference evidence:

- None yet; research is intentionally deferred to this dedicated SOW.

Open decisions:

- Exact portable permission mechanism and compatibility/migration behavior require the
  pre-implementation investigation.

## Implications And Decisions

- Decision: track this independently from SOW-0030 because it affects UDS security and
  embedding semantics, not the Linux SHM allocation crash.

## Plan

1. Complete the pre-implementation investigation and contract comparison.
2. Obtain any irreducible user decisions, then implement and validate independently.

## Execution Log

### 2026-07-12

- Created from SOW-0030 scanner triage. No implementation started.

## Validation

Pending; this SOW is open and blocked on its required pre-implementation investigation.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
