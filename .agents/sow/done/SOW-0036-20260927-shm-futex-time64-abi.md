# SOW-0036 - Linux SHM futex timeout ABI portability

## Status

Status: completed

Sub-state: source repair, local ABI validation, documentation and CI configuration completed; no downstream deployment.

## Requirements

### Purpose

Preserve real blocking timeouts for idle SHM sessions on 32-bit time64 libc builds.

### User Request

The user supplied a corrected investigation identifying a libc/kernel timespec ABI
mismatch in the C futex wrapper, with immediate timeouts and idle session CPU use.
Treat this as an upstream repair; deployment and host configuration are outside scope.

### Assistant Understanding

Facts: C and Rust pass libc timespec pointers to SYS_futex. The receive timeout is
an unsigned 32-bit millisecond duration. Go uses syscall.Timespec but hardcodes
64-bit field assignments. Existing Rust timeout tests assert the error only.

Inferences: explicit kernel ABI marshalling repairs the C and Rust failure without
changing the shared-memory layout or service polling policy.

Unknowns: affected deployed binaries have not been independently traced in this
session; no production CPU reduction will be claimed from local tests.

### Acceptance Criteria

- Empty SHM receives actually wait for subsecond and multisecond budgets.
- Finite and infinite receives still wake when a peer sends.
- ARM time64 C regression fails before the fix and passes after it.
- Native C/Rust/Go SHM tests and cross-language interop remain passing.
- ABI policy and reproducible portability checks are documented.

## Analysis

Sources checked: docs/level1-posix-shm.md, docs/code-organization.md, C/Rust/Go SHM
sources, C/Rust tests, CMakeLists.txt, runtime-safety workflow, SOW-0006 and SOW-0016.
Pending/current SOWs concern scale, maintainability, downstream integration, platform
build gating, permissions, and Rust style; all current SOWs are paused. This is a
latent initial-implementation defect (C wrapper originates in f71db33), not a reversal
of a completed SOW's specific time64 fix. No SOW claims prior 32-bit time64 validation.
The local SOW specs directory contains only .gitkeep. The only runtime project skill
is downstream vendoring preflight; it does not apply to source-only repairs.

Go 386 build inspection also found pre-existing UDS and test compilation errors;
track those separately rather than expanding this repair into a platform port.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- On 32-bit time64 libc, libc timespec seconds occupy eight bytes, while the
  legacy futex ABI consumes kernel-sized seconds and nanoseconds. A subsecond
  timeout can therefore become zero. Rust shares this pointer-layout assumption.

Evidence reviewed:

- Source wrappers and callers, existing timeout tests, public SHM synchronization
  contract, local Linux UAPI types, and musl's __timedwait.c web source.
- User live observations are reported evidence, not independently reproduced here.

Affected contracts and surfaces:

- Linux C and Rust SHM waiting, Go relative-time conversion, regression fixtures,
  portability test script, runtime CI, public SHM docs and integrator guide.
- Public APIs, wire layout, service polling intervals and Windows remain unchanged.

Existing patterns to reuse:

- Preserve monotonic deadlines, EINTR/EAGAIN retries, shared FUTEX_WAIT/WAKE,
  low-priority test runner and existing SHM interop fixtures.

Risk and blast radius:

- Wrong ABI selection can busy-loop or hang all Linux SHM users. Keep legacy
  syscall support for older kernels; validate timeout and wake paths. The maximum
  relative timeout is UINT32_MAX milliseconds, whose seconds fit signed 32 bits,
  so legacy futex marshalling suffices wherever that syscall exists.

Sensitive data handling plan:

- Do not persist hostnames, host paths, private endpoints, personal data or raw
  investigation notes in SOWs, specs, docs, skills, agent instructions or comments.
  Record only source references, synthetic test paths and sanitized measurements.

Implementation plan:

1. Add public receive timing/wake regression tests and reproduce on ARM time64.
2. Marshal kernel timeout fields in C and Rust; correct Go duration construction.
3. Add repeatable 32-bit regression command and CI coverage; update public guidance.
4. Run focused native, cross-ABI and interoperability validation, review changes,
   and commit implementation and completed SOW together.

Validation plan:

- Real ARM Linux user ABI execution under QEMU with musl time64, plus native tests.
- Short and >1-second idle waits, delayed message wake, zero timeout wake,
  CPU-versus-wall waiting evidence, maximum API timeout interrupted by a message.
- Rust stable and MSRV 1.91.0; C/Rust/Go SHM and service SHM interop.
- Search raw timed syscalls, review ABI/endian/null handling, diff check, SOW audit.

Artifact impact plan:

- AGENTS.md: no new responsibility or global workflow.
- Runtime project skills: source-only repair does not change downstream preflight.
- Specs: clarify ABI invariant in authoritative docs; avoid duplicate SOW spec.
- End-user/operator docs: describe portability check and its validation limits.
- End-user/operator skills: add 32-bit validation guidance to integrator guide.
- SOW lifecycle: new SOW for latent initial defect; track separate Go port issues.

Open-source reference evidence:

- Web reference: https://git.musl-libc.org/cgit/musl/tree/src/thread/__timedwait.c
  (blob 666093be98516a1c84b2997f075a8dbfcb797b2c), explicitly marshals futex timeouts.
  No external mirrored/cloned repository was used.

Open decisions:

- No product decision is needed: restore existing blocking semantics and retain
  older-kernel compatibility. Do not disable cgroups or deploy changes.

## Implications And Decisions

Use the legacy futex syscall when available with its kernel layout, since every
supported relative timeout fits; time64-only targets require a two-int64 layout.
This avoids depending on new kernel support merely because libc uses time64.

## Plan

Follow the four gate implementation steps above, keeping other SOWs paused.

## Execution Log

### 2026-09-27

- Confirmed the unsafe libc pointer handoff in C and Rust.
- Confirmed Go 386 build failures in SHM duration construction, UDS Iovlen/sendmsg,
  and oversized test literals. Broader UDS portability is tracked in SOW-0037.
- Reproduced C immediate expiry on ARM musl and i386 glibc time64. Native x86_64
  and i386 time32 pass the same fixture before the repair.
- Added explicit kernel-word marshalling in C and Rust, preserving null infinite
  waits and legacy syscall use for representable relative durations.
- Rust time64 builds exposed private timespec padding: use Default initialization
  in transport and the benchmark clock helper. The benchmark helper is built by
  Cargo integration tests; no benchmark methodology or performance claim changed.
- Reproduced the old Rust wrapper under ARM musl time64 in a temporary standalone
  crate with only the timespec initialization compatibility adjustments retained:
  100 ms returned in 0.894 ms. Repaired public-API test passes time32 and time64.
- Added C native CTest fixture, C cross-ABI script, Rust public-API integration
  fixture and cross-ABI script, and Runtime Safety ARM CI job.
- User asked about libc wrappers and language scope. Confirmed that syscall does
  not marshal timespec, musl helpers are internal, C/Rust share the issue, and
  pure Go's syscall.Timespec does not have this libc/kernel mismatch.
- Full Rust ARM unit tests exposed a separate pthread_t Send test compilation
  failure; tracked in SOW-0037. The standalone SHM public-API fixture runs without
  that unrelated unit-test dependency.

## Validation

Acceptance criteria evidence:

- C ARM musl time64 before: 100 ms returned in 0.278 ms; 1100 ms returned in
  1000.168 ms; delayed finite receive returned timeout. After: 100.308 ms and
  1100.151 ms, all finite/infinite/UINT32_MAX message wake checks passed.
- C i386 glibc time64 independently failed before and passed after. i386 time32
  and native x86_64 pass. Big-endian ARM musl time64 (nsec offset 12) also passes.
- Rust ARM musl time32 and time64 public API waits and delayed-message checks pass;
  old wrapper with time64 returns in under 1 ms and fails the elapsed-time assertion.
- C idle test records less than 1 ms process CPU per 100 ms wait under ARM QEMU;
  this is local blocking evidence, not a production performance estimate.

Tests or equivalent validation:

- Low-priority CMake configure/build and focused CTest: 7/7 passed (C SHM,
  timeout ABI, C service, Rust SHM, Go SHM, SHM interop, service SHM interop).
- After Rust constructor updates: rebuilt and reran affected Rust SHM, C timeout
  and both interop suites: 4/4 passed. Interop covers all nine C/Rust/Go pairings.
- Rust 1.91.0 and latest stable 1.98.1: 50 SHM unit tests and public-API
  integration fixture passed on each. Latest stable also passed both ARM musl
  time layouts and the Clippy correctness/suspicious gate.
- C cross-ABI commands: run-shm-timeout-abi.sh with native cc, cc -m32, cc -m32
  -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64, Zig ARM musl and Zig big-endian ARM musl.
- run-rust-shm-timeout-abi.sh: both ARM libc crate time configurations pass.
- Actionlint, ShellCheck, Rust format check, YAML parse and diff check pass.
- Clippy correctness/suspicious gate passes; existing advisory warnings remain.
  Added a narrow documented allowance for field reassignment because the suggested
  struct literal fails to compile with libc's private time64 padding.
- New GitHub job is configured but has not run remotely; local C glibc coverage
  uses i386 and local ARM C coverage uses musl. No CI result is claimed.

Real-use evidence:

- Public SHM server/client mappings with no traffic actually block; forked C
  peers and Rust thread peers wake finite/infinite/maximum-timeout receives.
- Production hosts and downstream source were not modified or remeasured.

Reviewer findings:

- Assistant self-review checked timeout width bounds, null pointers, shared futex
  flags, x32 kernel word size, endian handling, Rust private padding, cleanup and
  CI prerequisites. Corrected missing explicit cross-libc headers in the CI install
  and ShellCheck's masked-command-status warning. No independent reviewer used.

Same-failure scan:

- Searched C/Rust/Go raw syscall and timespec use. Only C and Rust SHM wait wrappers
  passed libc timespec to raw timed syscalls; both repaired. Rust benchmark clock
  helper also needed Default construction for time64 compilation. Go uses kernel
  Timespec; fixed its architecture-dependent duration construction. Remaining
  independent 32-bit builds are represented by pending SOW-0037.

Sensitive data gate:

- Durable changes contain synthetic test paths and sanitized timing evidence,
  not host identities, private endpoints, raw investigation notes, personal data
  or credentials. Public musl source URL and source file paths are safe references.

Artifact maintenance gate:

- AGENTS.md: unchanged; responsibilities, protocol layers and low-priority workflow
  are preserved. No new project-wide guardrail is required.
- Runtime project skills: unchanged; no downstream copy or preflight workflow changed.
- Specs: updated docs/level1-posix-shm.md with local ABI invariant; no duplicate
  .agents/sow/specs document needed for an existing public contract.
- End-user/operator docs: docs/level1-posix-shm.md includes reproducible C/Rust
  portability commands and explicitly limits emulator evidence.
- End-user/operator skills: docs/netipc-integrator-skill.md adds target-libc timeout
  validation guidance; it does not claim complete 32-bit language support.
- SOW lifecycle: SOW-0036 closes with the implementation in the same commit;
  SOW-0037 remains open/pending for independent platform build gaps. Existing
  current SOWs remain paused. No archived TODO history changed.

Specs update: authoritative SHM spec updated as above; wire layout unchanged.

Project skills update: runtime vendoring skill unchanged because no vendoring occurs.

End-user/operator docs update: public ABI requirement and test commands updated.

End-user/operator skills update: integrator guide points consumers to target testing.

Lessons:

- A timeout error alone cannot prove real waiting. Check elapsed time and wakeup.
- libc time types are not raw syscall layouts; test both widths and endian variants.

Follow-up mapping:

- Timeout marshalling, tests, docs and CI: implemented in SOW-0036.
- Go UDS 32-bit and Rust pthread_t unit-test build issues: tracked in SOW-0037.
- Deployment, host configuration changes and CPU claims: outside the source-repair
  scope; no promised production outcome is marked complete here.

## Outcome

C and Rust marshal the selected futex timeout ABI explicitly. Go constructs its
kernel Timespec portably. Subsecond/multisecond waiting and delayed-peer wakeup
are covered by real cross-ABI execution, native tests and CI configuration.
Public APIs, wire layout and service polling intervals are unchanged. Production
CPU reduction remains unmeasured because deployment was outside this task.

## Lessons Extracted

Timeout error assertions alone cannot distinguish real waiting from immediate expiry.

## Followup

Independent Go 32-bit UDS and Rust pthread_t unit-test build failures are tracked
in pending SOW-0037. No unrepresented deferred implementation remains.

## Regression Log

No prior completed time64 repair exists; this is a latent initial defect.
