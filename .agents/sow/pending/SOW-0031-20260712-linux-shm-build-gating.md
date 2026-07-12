# SOW-0031 - Align Linux SHM build gating with the platform contract

## Status

Status: open

Sub-state: tracked follow-up discovered by SOW-0030; execution must wait until the
active SOW completes.

## Requirements

### Purpose

Make the build system match NetIPC's public platform contract: Linux and native
Windows provide SHM transports, while FreeBSD and macOS use their baseline transports
without attempting to compile Linux futex code.

### User Request

The user delegated all SOW-0030 technical choices to the assistant with a requirement
to choose long-term-best solutions without shortcuts and reminded the project to keep
both native Windows and `MSYSTEM=MSYS` support. The approved plan tracks the broader
build-gating inconsistency separately from the urgent Linux SIGBUS fix.

### Assistant Understanding

Facts:

- `docs/level1-posix-shm.md` defines POSIX SHM as Linux-only and says FreeBSD/macOS
  use baseline UDS.
- The C Linux SHM source includes Linux futex headers and syscalls.
- Rust and Go use explicit Linux target/build gates for POSIX SHM.
- CMake commonly uses `NOT APPLE` for C SHM, service, tests, and interop targets; that
  condition also includes FreeBSD.
- CMake treats Windows, Cygwin, and `MSYSTEM=MSYS` as the Windows runtime and must
  preserve native Windows transport coverage.

Inferences:

- The CMake gates encode a broader platform set than the implementation and public
  contract support.
- Repair may require separating baseline POSIX service targets from Linux-only SHM
  components rather than mechanically replacing every `NOT APPLE` condition.

Unknowns:

- The exact smallest coherent CMake target split must be established by a complete
  dependency graph and cross-platform configure/build investigation when this SOW is
  activated.

### Acceptance Criteria

- Linux C/Rust/Go SHM and service targets retain their current behavior and tests.
- Native Windows and `MSYSTEM=MSYS` Named Pipe/Windows SHM builds and tests remain green.
- FreeBSD and macOS configure/build only supported baseline components and never compile
  Linux futex sources.
- CMake conditions, public docs, and actual target availability agree.

## Analysis

Sources checked:

- `docs/level1-posix-shm.md`
- `docs/level1-transport.md`
- `CMakeLists.txt`
- `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
- `src/crates/netipc/src/transport/mod.rs`
- `src/go/pkg/netipc/transport/posix/shm_linux.go`

Current state:

- The platform contract and language build gates are Linux-specific, while several
  CMake gates use the broader `NOT APPLE` condition.

Risks:

- A mechanical condition replacement could remove useful baseline service targets on
  FreeBSD/macOS or accidentally affect Windows/MSYS target selection.
- Cross-platform proof requires access to the authorized test systems or equivalent CI.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- CMake uses exclusion-based `NOT APPLE` conditions where the contract requires an
  explicit Linux capability boundary. The correct fix depends on whether each gated
  target is entirely Linux-only or mixes reusable baseline code with SHM dependencies.

Evidence reviewed:

- `docs/level1-posix-shm.md:5-11`
- `docs/level1-transport.md:470-500`
- `CMakeLists.txt:31,67-100,290-305,488-508`
- `src/libnetdata/netipc/src/transport/posix/netipc_shm.c:1-25`
- `src/crates/netipc/src/transport/mod.rs:6-10`
- `src/go/pkg/netipc/transport/posix/shm_linux.go:1-6`

Affected contracts and surfaces:

- CMake target availability, Linux/FreeBSD/macOS/native-Windows/MSYS builds, POSIX
  baseline service composition, Linux and Windows SHM transports, tests, CI, and public
  platform documentation.

Existing patterns to reuse:

- Rust `cfg(target_os = "linux")`, Go `//go:build linux`, and CMake's existing
  `NETIPC_WINDOWS_RUNTIME` capability boundary.

Risk and blast radius:

- Medium: build-system changes can silently omit targets or compile unsupported sources.
  Keep the work separate from SOW-0030 and validate every supported platform family.

Sensitive data handling plan:

- No sensitive data is needed. Durable artifacts will contain only platform names,
  source paths, build commands, and sanitized test outcomes.

Implementation plan:

1. Map every CMake platform condition to its source dependencies and public contract.
2. Define explicit Linux SHM gates while retaining baseline POSIX and Windows/MSYS
   components on their supported platforms.
3. Update tests, CI, and docs where target availability changes.

Validation plan:

- Linux configure/build/test and POSIX interop.
- Authorized FreeBSD and macOS configure/build/test for baseline UDS behavior.
- Authorized native Windows and `MSYSTEM=MSYS` configure/build/test for Named Pipe and
  Windows SHM behavior.
- Same-pattern search for broad platform exclusions that stand in for Linux capability.

Artifact impact plan:

- AGENTS.md: update only if supported validation commands or platform workflow changes.
- Runtime project skills: unaffected unless a reusable cross-platform validation workflow
  is discovered.
- Specs: update if investigation changes actual supported target availability.
- End-user/operator docs: update platform/build documentation if commands or targets change.
- End-user/operator skills: update `docs/netipc-integrator-skill.md` if platform guidance changes.
- SOW lifecycle: remain pending until SOW-0030 completes; execute independently.

Open-source reference evidence:

- None needed yet; this follow-up tracks an internal build-contract mismatch. External
  build-system references may be researched when the SOW is activated.

Open decisions:

- None at creation. Any design fork exposed by the dependency graph will be investigated
  and presented before implementation.

## Implications And Decisions

- User decision (2026-07-12): choose the long-term-best design without shortcuts and
  preserve both native Windows and `MSYSTEM=MSYS` support.
- Decision: track this build-contract repair separately so the urgent SOW-0030 crash fix
  remains focused.

## Plan

1. Wait for SOW-0030 to complete.
2. Complete the target/dependency analysis and pre-implementation gate.
3. Implement and validate the approved cross-platform target split.

## Execution Log

### 2026-07-12

- Created as the real pending follow-up for the CMake/platform-contract discrepancy
  discovered during SOW-0030 research.

## Validation

Acceptance criteria evidence:

- Pending activation.

Tests or equivalent validation:

- Pending activation.

Real-use evidence:

- Pending activation.

Reviewer findings:

- Pending activation.

Same-failure scan:

- Pending activation.

Sensitive data gate:

- This SOW contains only public platform names, source paths, and build-contract facts.

Artifact maintenance gate:

- Pending activation for all artifact classes.

Specs update:

- Pending activation.

Project skills update:

- Pending activation.

End-user/operator docs update:

- Pending activation.

End-user/operator skills update:

- Pending activation.

Lessons:

- Pending activation.

Follow-up mapping:

- This file is the tracked follow-up from SOW-0030.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
