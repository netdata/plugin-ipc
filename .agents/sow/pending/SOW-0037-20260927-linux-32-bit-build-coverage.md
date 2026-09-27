# SOW-0037 - Linux 32-bit transport build coverage

## Status

Status: open

Sub-state: separately tracked build gaps discovered during SOW-0036; no implementation started.

## Requirements

### Purpose

Make Linux 32-bit language builds and test coverage match the supported platform contract.

### User Request

Follow-up from investigation of the reported SHM timeout ABI defect. The immediate
C/Rust futex repair is SOW-0036; independent platform build failures belong here.

### Assistant Understanding

Facts: Go 386 transport builds fail on UDS Iovlen width, missing SYS_SENDMSG and
oversized integer literals in tests. ARM musl Rust unit tests fail because a
pthread_t pointer captured by a thread closure is not Send. These are distinct
from the futex timeout marshalling defect.

Inferences: target-aware syscall and test handling is needed before claiming
complete 32-bit Go/Rust transport coverage.

Unknowns: full affected-target inventory and UDS syscall strategy remain to be
investigated in this SOW before implementation.

### Acceptance Criteria

- Define and document the tested Linux 32-bit target/libc matrix.
- Build and execute transport/service tests and interop on those targets.
- Preserve pure Go and the existing shared wire contract.

## Analysis

SOW-0036 command evidence: GOARCH=386 CGO_ENABLED=0 go test
./pkg/netipc/transport/posix fails at uds.go Iovlen and SYS_SENDMSG and
uds_more_edge_test.go oversized literals. cargo test --target
arm-unknown-linux-musleabihf --lib fails at raw_unix_tests.rs pthread_kill
thread closure. SOW-0036 corrects the SHM Timespec construction separately.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model: architecture-width assumptions and libc-dependent
pthread_t types prevent cross-target compilation.

Evidence reviewed: source errors from Go 386 transport tests and Rust ARM musl
unit tests, and the SOW-0036 public-API timeout fixture.

Affected contracts and surfaces: Linux UDS syscall code, Rust/Go tests, CI,
platform documentation and integrator guidance.

Existing patterns to reuse: architecture-specific Go build files, low-priority
runner, QEMU user execution, existing C/Rust/Go interop fixtures.

Risk and blast radius: UDS sendmsg changes can affect all Go Linux transport
traffic; target matrix must be investigated before editing production code.

Sensitive data handling plan: persist only synthetic test inputs and source
references in SOWs, specs, docs, skills, instructions and code comments. No
host inventories, secrets, private endpoints or personal data are needed.

Implementation plan:

1. Inventory failing Linux target builds and define the supported matrix.
2. Fill this gate with concrete syscall and test changes before activating.
3. Implement architecture-correct UDS and test behavior and validate interop.

Validation plan: cross-target compile and runtime tests, syscall error paths,
C/Rust/Go interop, documentation review and SOW audit.

Artifact impact plan:

- AGENTS.md: update only if validated target commands become project-wide requirements.
- Runtime project skills: downstream preflight remains unchanged for source-only work.
- Specs: preserve wire formats; document any changed platform guarantees.
- End-user/operator docs: publish actual tested target matrix and commands.
- End-user/operator skills: reflect new target validation guidance.
- SOW lifecycle: pending follow-up to SOW-0036; complete independently.

Open-source reference evidence: no external repository used in this initial
failure record; syscall ABI references must be checked during investigation.

Open decisions: target matrix and syscall implementation are unresolved
investigation items, not permission to start an unbounded architecture port.

## Implications And Decisions

Keep independent build-portability failures out of the immediate timeout repair.

## Plan

Investigate and complete the pre-implementation gate before starting edits.

## Execution Log

### 2026-09-27

Recorded reproducible compile failures from SOW-0036.

## Validation

This pending SOW records compiler evidence only; implementation acceptance,
real-use testing, review, same-failure scan and artifact gates remain required
when activated. Status open agrees with pending directory.

## Outcome

Tracked; not implemented.

## Lessons Extracted

Native tests cannot prove architecture-dependent syscall types or pthread_t handling.

## Followup

SOW-0036 owns futex timeout conversion; this SOW owns the separate build gaps.

## Regression Log

No completed 32-bit build fix has been identified to reopen.
