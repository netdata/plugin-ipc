# SOW-0030 - SHM region creation crashes with SIGBUS when the backing store is full

## Status

Status: in-progress

Sub-state: active; long-term-best design approved by the user and pre-implementation
gate complete. Linux implementation and local validation are complete. Native Windows /
MSYS runtime validation, source CI/scanner evidence, and Netdata re-vendoring remain.

## Requirements

### Purpose

Stop netipc SHM region creation from crashing the host process (SIGBUS) when the
shared-memory backing store (tmpfs `/dev/shm`, or a disk-backed directory) has no free
space. Creation must fail gracefully with an error the caller can handle, in all three
language implementations (C, Rust, Go), preserving cross-language interoperability.

### User Request

The user analyzed the crashes of the latest Netdata nightlies reported to the
agent-events pipeline and asked for a SOW in this repository so the fix is implemented
here (upstream) and then re-vendored into netdata/netdata.

### Assistant Understanding

Facts:

- Netdata fleet evidence (agent-events crash pipeline, 48h window, nightlies
  v2.10.0-733 / v2.10.0-738, 2026-07-10..12): 765 crash events total; the largest
  real-code-bug cluster is ~22 events from ~15 distinct agents, all
  `SIGBUS/BUS_ADRERR`, in netipc server threads named `P[cglkupipc]` and
  `P[cgroupsipc]` (the Netdata cgroups lookup/cache IPC services).
- Representative decoded stack (from a glibc aarch64 build):
  `memset` -> `nipc_shm_server_create` (netipc_shm.c:372) ->
  `server_prepare_accept_config` (netipc_service_posix_server.c:74) ->
  `nipc_server_run` (netipc_service_posix_server.c:206).
  Several more events show only musl frames (`src/string/x86_64/memset.s`) consistent
  with the same zero-fill site on static builds.
- Root cause (C): `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
  - `:355` `ftruncate(fd, region_size)` — on tmpfs this creates a **sparse** file and
    succeeds regardless of available space;
  - `:362` `mmap(PROT_READ|PROT_WRITE, MAP_SHARED)`;
  - `:372` `memset(map, 0, region_size)` — the first write to a page that tmpfs cannot
    back raises `SIGBUS` (`BUS_ADRERR`). This is documented POSIX/Linux mmap behavior
    when the backing object cannot provide the page.
- The same pattern exists in the other two implementations:
  - Rust: `src/crates/netipc/src/transport/shm.rs:272` (`libc::ftruncate`) and `:303`
    (`ptr::write_bytes(base, 0, region_size)`);
  - Go: `src/go/pkg/netipc/transport/posix/shm_linux.go:174` (`syscall.Ftruncate`),
    followed by mapped-region writes.
- The crashing binary is the vendored copy in `netdata/netdata @ 5cd588067e`
  (`src/libnetdata/netipc/src/transport/posix/netipc_shm.c`, same line numbers as this
  repository), so the fix must land here first and then be re-vendored
  (`diff-netdata-vendor.sh` exists at the repo root for the sync).
- The POSIX SHM transport is Linux-only in the authoritative contract and in the
  Rust and Go build gates. FreeBSD and macOS use baseline UDS.
- Native Windows and Windows under `MSYSTEM=MSYS` use the separate Windows
  Named Pipe / file-mapping implementation. They do not use the Linux file-backed
  SHM path, but remain mandatory regression-validation targets.

Inferences:

- The affected hosts had a full (or nearly full) `/dev/shm` / backing filesystem at
  region-creation time. SIGBUS at first-write is the classic sparse-file-on-tmpfs
  failure mode; ~15 distinct agents across two nightlies in 48h makes a code-side
  memory-corruption explanation unlikely.
- After a successful create (full zero-fill completed), all pages are committed, so
  the SIGBUS window is region creation. A client attaching to a region created by a
  fixed server cannot SIGBUS on unbacked pages; mixed-version scenarios (old server,
  new client) retain the old behavior until the vendor copy is updated.

Resolved investigation:

- C, Rust, and Go managed servers already remove SHM profiles and continue over
  baseline when SHM preparation fails and baseline is configured. Existing obstruction
  tests prove the generic fallback; this SOW must add allocation-specific coverage.
- All known server implementations zero the complete region before publishing a valid
  header. An old server that fails during zero-fill cannot complete a successful SHM
  negotiation, while a successfully published old region has already backed every page.
- Deterministic validation requires both allocation fault injection and a real
  size-limited Linux tmpfs test in child processes.

### Acceptance Criteria

- With a full SHM backing store, region creation in C, Rust, and Go returns a normal
  error (no SIGBUS, no crash); verified by a test that constrains the backing store
  (e.g. a size-limited tmpfs mount in a user/mount namespace on Linux) or an
  equivalent deterministic simulation.
- When baseline is configured, the consuming service keeps operating without SHM
  acceleration when region creation fails (verified against the C service server used
  by Netdata's cgroups IPC). An explicitly SHM-only configuration may fail because it
  has no remaining transport.
- All three implementations keep identical region layout and interop behavior
  (existing interop tests still pass).
- Error taxonomy stays consistent across languages (new or existing error code used
  uniformly; docs under `docs/` updated if the error surface changes).
- Vendor follow-up tracked: re-vendor into netdata/netdata after merge (tracked as a
  follow-up item here, executed in the netdata repository).
- Native Windows and `MSYSTEM=MSYS` build/tests remain green; no Linux allocation API
  or error is introduced into the Windows SHM implementation.

## Analysis

Sources checked:

- This repo: `src/libnetdata/netipc/src/transport/posix/netipc_shm.c` (:336-:399),
  `src/libnetdata/netipc/src/service/netipc_service_posix_server.c` (:74, :206),
  `src/crates/netipc/src/transport/shm.rs` (:211-:310),
  `src/go/pkg/netipc/transport/posix/shm_linux.go` (:174).
- netdata/netdata @ 5cd588067e — vendored copy and fleet crash correlation (Netdata
  agent-events pipeline; evidence summarized above, raw records remain in the Netdata
  workstation checkout under its local-only audit directory, not in this repository).

Current state:

- All three implementations size the region with `ftruncate` only, then write to the
  mapping. On tmpfs, allocation is deferred to first write, converting ENOSPC into an
  uncatchable-by-default SIGBUS inside library code, crashing the embedding process
  (the Netdata daemon in the fleet evidence).

Risks:

- Linux filesystems or sandbox policies that return an ordinary error from native
  `fallocate()` disable SHM for that session and use baseline when available. A strict
  seccomp policy using `KILL_PROCESS`/`KILL_THREAD` terminates, while `TRAP` delivers
  `SIGSYS` before NetIPC can receive an error. NetIPC installs no process-global SIGSYS
  emulation handler, and `USER_NOTIF`/`TRACE` supervisor behavior is outside its
  contract; strict policies must explicitly allow `fallocate`. This is an intentional
  safety boundary: do not substitute a weaker allocation mechanism where the no-SIGBUS
  guarantee cannot be proven.
- Behavior parity: the Windows transport (`netipc_win_shm.h` family) uses a different
  section-object allocation model. It is not changed, but native Windows and MSYS
  regression validation remains mandatory.
- The current CMake `NOT APPLE` SHM gates are broader than the Linux-only public
  contract and include FreeBSD. That pre-existing build-contract discrepancy will be
  tracked separately rather than expanding this fleet-crash repair.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Sparse allocation (`ftruncate`) + lazy tmpfs page allocation + first-write zero-fill
  = SIGBUS on full backing store, crashing the host process. Evidence: fleet cluster
  (~22 events / ~15 agents / 48h) with stacks pinned to the zero-fill instruction;
  identical pattern present in C, Rust, and Go transports.

Evidence reviewed:

- See Analysis. External: POSIX `mmap` specification and Linux `mmap(2)` SIGBUS
  semantics for writes beyond the backing object's allocated space.
- netdata/netdata @ 5cd588067e
  src/libnetdata/netipc/src/transport/posix/netipc_shm.c:355 (vendored copy).
- Microsoft `CreateFileMappingW` documentation: a page-file-backed mapping created with
  `PAGE_READWRITE` and no explicit allocation attribute assumes `SEC_COMMIT`; the system
  must have enough committable pages for the whole mapping or creation fails normally.
- C, Rust, and Go Windows creators all use `INVALID_HANDLE_VALUE` page-file backing and
  return existing create-mapping/map-view errors before touching the view when those APIs
  fail.
- Linux kernel seccomp documentation: `ERRNO` directly returns an error,
  `KILL_PROCESS`/`KILL_THREAD` terminate, `TRAP` delivers `SIGSYS`, and
  `USER_NOTIF`/`TRACE` depend on a supervisor or tracer:
  <https://docs.kernel.org/userspace-api/seccomp_filter.html>.
- moby/profiles @ f9bc03ec19b2dc4c091449b08e88f85c0caa9f0b
  `seccomp/default.json:2,112` uses `SCMP_ACT_ERRNO` by default and explicitly allows
  `fallocate`, so the common Docker default profile satisfies the requirement.

Affected contracts and surfaces:

- C API error codes of `nipc_shm_server_create()` (and possibly client attach); Rust
  `ShmError`; Go transport errors; docs under `docs/` describing transport errors;
  interop and unit tests; the netdata vendored copy (follow-up).

Existing patterns to reuse:

- Existing stage-based error taxonomy and cleanup paths. Add an allocation-stage error
  rather than misreporting a native allocation failure as truncation or encoding only
  one OS cause such as ENOSPC.
- Cleanup-on-failure paths already exist at each create step (close/unlink pattern in
  `netipc_shm.c:355-:368`); the fix slots in as one more failable step.

Risk and blast radius:

- Small, localized change per language; no wire-format or layout change. Main risk is
  platform portability of preallocation and CI coverage for the failure path.

Sensitive data handling plan:

- Fleet evidence is summarized in aggregate only; no agent identifiers, hostnames,
  IPs, or customer-identifying details are recorded in this SOW or in any durable
  artifact of this repository. Raw crash records stay in the Netdata workstation's
  local-only audit directory.

Implementation plan:

1. C, Rust, and Go Linux creators: replace sparse-only sizing with native Linux
   `fallocate(fd, 0, 0, region_size)` before `mmap`, retrying EINTR. Do not use
   `ftruncate`, libc-emulated `posix_fallocate`, or a write-loop fallback. Unsupported
   allocation disables SHM and preserves the stronger safety guarantee.
2. Keep the existing mapped zero-fill after successful allocation to initialize the
   complete region and atomics consistently.
3. Add a general allocation-stage error while preserving existing public values:
   C `NIPC_SHM_ERR_ALLOCATE` appended to the enum, Rust `ShmError::Allocate(errno)`,
   and Go `ErrShmAllocate` with the underlying OS error preserved for `errors.Is`.
4. Do not add client-side preallocation or a process-global SIGBUS handler. The known
   mixed-version lifecycle cannot publish an under-backed valid region.
5. Add allocation-specific service tests proving normal fallback to baseline when
   baseline is configured, without changing the existing SHM-only semantics.
6. Add deterministic per-language fault injection and a real size-limited Linux tmpfs
   subprocess test that proves normal error return and no SIGBUS.
7. Re-run POSIX interop and native Windows/MSYS regression validation. Windows transport
   code remains unchanged unless validation reveals an evidence-backed equivalent gap.
8. Update the Linux SHM lifecycle and allocation-error contract under `docs/`.
9. Follow-up: re-vendor into netdata/netdata through the mandatory preflight workflow.

Validation plan:

- Per-language allocation-helper fault injection for ENOSPC, EINTR retry, cleanup,
  error classification, and proof that mmap/mapped writes are not reached on failure.
- Size-limited tmpfs via a private user/mount namespace or a dedicated privileged Linux
  CI job. Run C, Rust, and Go creators as child processes and assert normal failure,
  no SIGBUS, and no leaked region file. The real-kernel test may skip only on jobs not
  designated to provide namespace/mount coverage; at least one Linux CI path must run it.
- Full interop matrix (C/Rust/Go) unchanged behavior on the success path.
- Native Windows and `MSYSTEM=MSYS` build/tests for the Named Pipe and Windows SHM
  transports, proving Linux-only changes did not leak into Windows builds.
- Same-failure search: audit every other mapped-region write path (client attach,
  Windows section mapping, any `mremap`/grow paths) for the same lazy-allocation gap.

Artifact impact plan:

- AGENTS.md: likely unaffected.
- Runtime project skills: likely unaffected.
- Specs: transport spec under `.agents/sow/specs/` (or `docs/`) — update error
  semantics for region creation.
- End-user/operator docs: likely unaffected (library-internal).
- End-user/operator skills: none present.
- SOW lifecycle: follow-up item for the netdata re-vendor; close via `done/` when
  merged here.

Open-source reference evidence:

- netdata/netdata @ 5cd588067e
  src/libnetdata/netipc/src/transport/posix/netipc_shm.c:355,372 (vendored copy that
  produced the fleet crashes).

Open decisions:

- None. The user delegated all technical choices to the assistant with an explicit
  requirement to choose the long-term-best design without shortcuts.

## Implications And Decisions

- User decision (2026-07-12): choose the long-term-best option for every technical
  fork without shortcuts; keep both native Windows and `MSYSTEM=MSYS` support in scope.
- Decision: Linux uses direct native `fallocate()` with no weaker fallback. On
  unsupported filesystems or policies that return errno, SHM preparation fails normally
  and managed services use baseline when configured. NetIPC cannot automatically recover
  from kill/trap seccomp actions and strict policies must allow `fallocate`.
- Decision: add a general allocation-stage public error across C, Rust, and Go;
  preserve the OS cause as C `errno` and in Rust/Go error values, and append the C enum
  value to avoid renumbering existing public values.
- Compatibility implication: adding Rust `ShmError::Allocate(i32)` breaks downstream
  exhaustive matches over this public enum. The crate is still pre-1.0 (`0.1.0`), the
  new stage must be distinguishable, and preserving `ShmError::Truncate` avoids breaking
  callers that construct or selectively match the old variant. Making only this enum
  `#[non_exhaustive]` would impose the same one-time source break while leaving the
  project's other public error enums inconsistent; broader enum-evolution policy is
  outside this production crash repair.
- Decision: do not modify client attach or install SIGBUS recovery. Known servers
  cannot publish a valid under-backed region, and a process-global signal handler is
  incompatible with an embeddable library's blast-radius requirements.
- Decision: validation has two mandatory layers: deterministic allocation fault
  injection and real size-limited tmpfs subprocess coverage.
- Decision: the Linux fix must not alter native Windows or MSYS transport behavior;
  both remain mandatory regression targets.
- Decision: track the broader CMake Linux-gating inconsistency separately so this
  production crash repair stays focused.

## Plan

1. Activate this SOW after pausing the previous active SOW.
2. Implement the Linux allocation helper and allocation error in C, Rust, and Go.
3. Add allocation-failure, cleanup, service-fallback, and real tmpfs tests.
4. Update the authoritative Linux SHM contract and related operator/integrator guidance.
5. Run POSIX, cross-language interop, native Windows, and MSYS validation.
6. Run same-failure and reviewer passes, then resolve all findings.
7. Re-vendor through the project vendoring preflight and validate Netdata.

## Execution Log

### 2026-07-12

- SOW created from Netdata fleet crash analysis (agent-events pipeline, nightlies
  v2.10.0-733/738, 48h window): SIGBUS cluster attributed to sparse SHM allocation.
  No implementation started.
- Researched official Linux/POSIX allocation and mmap behavior, the complete C/Rust/Go
  lifecycle and fallback paths, and mature open-source implementations.
- Reproduced the allocation distinction in a private 64 KiB tmpfs: sparse 128 KiB
  `truncate` succeeded with zero allocated blocks while 128 KiB `fallocate` returned
  ENOSPC normally.
- Recorded the user's long-term-best authorization and the resolved design above.
- Verified the Windows allocation contract against Microsoft documentation and all three
  implementations. Native Windows and MSYS page-file-backed mappings default to
  `SEC_COMMIT`; insufficient system commit fails through `CreateFileMappingW` or
  `MapViewOfFile`, and the existing language-specific error paths return normally.
  No Windows allocation change is justified by this failure class.
- Replaced sparse `ftruncate` creation with direct Linux `fallocate` in C, Rust, and Go.
  All three retry only `EINTR`, reject unsupported allocation without a sparse fallback,
  clean up before `mmap`, and retain complete mapped-region zero-fill.
- Added the allocation-stage error surface while preserving legacy values/symbols:
  appended C `NIPC_SHM_ERR_ALLOCATE`, Rust `ShmError::Allocate(errno)`, and Go
  `ErrShmAllocate` wrapping the OS error.
- Added deterministic C/Rust/Go tests for `ENOSPC`, `EINTR`, unsupported allocation,
  cleanup, and success after retry. C allocation faults use a test-only linker wrapper;
  the production `netipc_shm` library exports no fault-control symbols and does no
  test-state work.
- Added the managed C service regression proving allocation failure removes SHM from
  negotiation, selects baseline, and completes a typed cgroups snapshot call.
- Added a private size-limited tmpfs regression for all three real creator binaries.
  The first reviewer pass found that the initial script accepted unrelated ordinary
  failures; strengthening it to require a shared `NIPC_SHM_ALLOCATE_ENOSPC` marker
  exposed a real pre-allocation directory-permission false positive. The mount is now
  private mode `0700`, and the test requires exit 1, the exact marker, no `READY`, no
  signal/timeout/invocation failure, and no leaked file.
- Added a dedicated required Linux CI job for the real tmpfs test. Ordinary local CTest
  recognizes exit 77 as skipped when namespaces are unavailable; the designated CI job
  runs with `NIPC_REQUIRE_TMPFS_TEST=1` and a privileged mount-only namespace. Both the
  unprivileged developer path and exact privileged CI command pass locally.
- Updated the Linux and Windows authoritative SHM documents, integrator guidance, and
  coverage exclusions. Added pending SOW-0031 for the pre-existing CMake Linux-gating
  discrepancy without mixing that separate scope into this repair.

## Validation

Acceptance criteria evidence:

- Full Linux success-path and failure-path evidence is recorded below. The only pending
  acceptance evidence is native Windows/MSYS execution and post-push CI/scanner status.

Tests or equivalent validation:

- Full configured Linux matrix: 49/49 CTest tests passed in 472.07 seconds, including all
  C/Rust/Go units, fuzz targets, stress, nine SHM interop directions, typed service/cache
  SHM interop, lookup scale, and `test_shm_full_backing_store`.
- C coverage: all tracked files meet the 90% gate; `netipc_shm.c` 91.1%, total 91.8%.
- Rust: 380/380 library tests passed; `transport/shm.rs` 96.35%, total 92.63% coverage.
- Go: POSIX transport 91.6%, `shm_linux.go` 91.9%, total 90.2% coverage.
- C ASAN/UBSAN: 7/7 selected suites passed, zero findings.
- C TSAN: 6/6 multithreaded suites passed, zero data races.
- Independent musl validation in an ephemeral Alpine 3.24.1 container (musl 1.2.6,
  GCC 15.2.0) built the read-only-mounted source, passed C `test_shm` 1/1, and returned
  the exact ENOSPC marker/status 1 with no leaked file on a real 64 KiB tmpfs.
- Static/security: `git diff --check`, Rust fmt, Go fmt, ShellCheck, actionlint, Clippy
  correctness/suspicious gates, cppcheck, flawfinder, Go vet/staticcheck/gosec,
  govulncheck, Cargo audit, and Cargo deny passed. Clang-tidy completed with its existing
  non-fatal project warning set and no new allocation finding.
- Windows local compile isolation: Rust library and all binaries pass
  `cargo check --target x86_64-pc-windows-gnu`; all Go Windows packages and Windows
  interop fixtures cross-compile with `GOOS=windows GOARCH=amd64 CGO_ENABLED=0`.

Real-use evidence:

- The real C, Rust, and Go creators each ran in a private 64 KiB tmpfs while requesting
  an approximately 128 KiB region. Each returned the explicit ENOSPC allocation marker
  and status 1, never emitted `READY`, received no signal, and leaked no region file.
- The managed C service path used by Netdata injected ENOSPC at `fallocate`, negotiated
  baseline, remained ready, and completed a typed cgroups snapshot call with a valid
  response.
- The independent Alpine/musl real-tmpfs run covers the libc family present in the
  original static-build fleet crash evidence, not only the workstation's glibc path.

Reviewer findings:

- First independent pass: three reviewers agreed the runtime fix was correct but found
  two acceptance blockers: the real tmpfs script could false-pass unrelated failures,
  and no checked-in Linux CI route required the test. Both were fixed as described in
  the execution log.
- One reviewer also found that the initial C fault seam shipped in the production
  library. It was replaced with a test-only `--wrap=fallocate` object; `nm` confirms the
  production library has no `nipc_shm_test_fault_set/clear` symbols while the test binary
  has them.
- Reviewers identified the Rust exhaustive-match source-compatibility implication; it
  is explicitly recorded under Implications And Decisions.
- Second full pass found that C did not explicitly preserve the allocation `errno`, the
  required CI regex could succeed with no matching test, and one test comment still
  named `ftruncate`. C now saves/restores and tests the errno, CI uses
  `--no-tests=error`, and the comment describes publication state.
- Third full pass found no code defect. It identified stale README validation counts and
  coverage values; README now records 49/49, current totals with measurement scope, and
  the real tmpfs regression.
- Fourth full pass found no additional correctness, interoperability, security, CI,
  documentation, compatibility, or unexpected-side-effect issue. Review consensus:
  the implemented Linux repair is production-grade; only the explicit external
  Windows/MSYS, post-push CI/scanner, vendoring, and lifecycle gates remain.
- A later independent portability pass found an overbroad seccomp fallback claim plus
  two harness/artifact details. The contract now states that errno-returning denials
  fall back while NetIPC cannot automatically fall back from kill/trap actions and strict
  policies must allow `fallocate`; the privileged probe trace and mode-specific skip
  diagnostic are fixed; the SOW records C errno preservation.
- Three fresh full reviews after the seccomp/harness fixes found no further source,
  test, CI, documentation, security, portability, or unexpected-side-effect issue.
  Review consensus remained production-grade code with only external process gates.
- A final documentation-precision pass noted that seccomp `TRAP` can be emulated by a
  host SIGSYS handler and `USER_NOTIF`/`TRACE` are supervisor-dependent. The contract now
  states the exact NetIPC boundary: it installs no such handler and cannot automatically
  fall back, while external supervision remains outside scope. The final full review
  found no remaining issue and retained the production-grade code verdict.

Same-failure scan:

- No production Linux SHM creator retains `ftruncate`/sparse creation. Remaining
  `ftruncate` calls are malformed/truncated-region test fixtures.
- C, Rust, and Go client attach only map a region that a server fully allocated, zeroed,
  and published; no grow/remap path exists.
- Native Windows/MSYS C, Rust, and Go use separate page-file-backed
  `CreateFileMappingW(INVALID_HANDLE_VALUE, PAGE_READWRITE)` paths. Default `SEC_COMMIT`
  provides the equivalent reservation, and create/map-view errors return normally.
- No wire-layout, offsets, capacity calculation, or negotiation profile value changed.

Sensitive data gate:

- The SOW contains only aggregate fleet counts, sanitized stack/function evidence,
  source paths, commands, and public API names. Tests will use synthetic service names,
  private mount namespaces, and generated temporary paths. No raw fleet records,
  customer identifiers, credentials, private endpoints, or production data are needed.

Artifact maintenance gate:

- `AGENTS.md`: unchanged; no project-wide responsibility or workflow rule changed.
- Runtime project skill: unchanged; Netdata vendoring still follows the existing
  mandatory `project-netdata-vendoring` workflow and has not started.
- Specs: updated authoritative `docs/level1-posix-shm.md` and
  `docs/level1-windows-shm.md`; no `.agents/sow/specs/` duplicate is needed because
  `docs/` is authoritative for this transport contract.
- End-user/operator docs: public transport lifecycle/error behavior and README current
  validation evidence updated; no CLI, configuration, schema, or operator runbook
  changed.
- End-user/operator skill: updated `docs/netipc-integrator-skill.md` with the Linux
  allocation and Windows committed-mapping behavior.
- SOW lifecycle: SOW-0027 remains paused; SOW-0030 is the sole active implementation;
  pending SOW-0031 tracks the separate platform-gating issue. Completion/move remains
  pending Windows/MSYS runtime evidence and the required vendor workflow.

SOW lifecycle and follow-up mapping:

- Netdata re-vendoring remains mandatory in this SOW after source commit, push, CI, and
  scanner preflight. The separate CMake platform-gating discrepancy is represented by
  pending SOW-0031.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

- Re-vendor the fix into netdata/netdata after merge (executed in the netdata
  repository; the fleet crash volume there is the success metric — SIGBUS cluster in
  `P[cglkupipc]` / `P[cgroupsipc]` threads should disappear in nightlies after the
  vendor update).

## Regression Log

None yet.
