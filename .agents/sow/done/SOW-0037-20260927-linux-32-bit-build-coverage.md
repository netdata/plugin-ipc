# SOW-0037 - Linux 32-bit transport build coverage

## Status

Status: closed

Sub-state: merged into SOW-0046 by user decision (2026-10-29). SOW-0046 delivered the Go part and owns all remaining scope; see `.agents/sow/done/SOW-0046-20261029-go-cross-platform-build.md`. Correction (2026-11-07, SOW-0070): the link pointed at `current/`; SOW-0046 has since completed and delivered that remaining scope.

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

Status: superseded; merged into SOW-0046, whose gate covers this scope.

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

### 2026-10-29

SOW-0046 delivered the Go part of this SOW: Go 386 `Iovlen` width, the
missing `SYS_SENDMSG` (socketcall on linux/386), the oversized test literals,
Go test execution on 386/arm/arm64 in Runtime Safety CI, and a 32-bit SHM
attach overflow fix in C, Rust and Go. Remaining scope here: the Rust ARM musl
`pthread_t` test compile failure, and executing the C and Rust test suites and
interop on 32-bit targets (the C/Rust attach fix from SOW-0046 is verified on
64-bit only, because no local 32-bit C/Rust toolchain exists).

## Validation

Closed without separate implementation; acceptance evidence, reviews, same-failure
scan and artifact gates are recorded in SOW-0046. Status closed agrees with the
done directory.

## Outcome

Closed as merged into SOW-0046 (user decision 2026-10-29). No work remains here; acceptance criteria are validated in SOW-0046.

## Lessons Extracted

Native tests cannot prove architecture-dependent syscall types or pthread_t handling.

## Followup

SOW-0036 owns futex timeout conversion; this SOW owns the separate build gaps.

## Regression Log

No completed 32-bit build fix has been identified to reopen.
