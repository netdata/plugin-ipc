# SOW-0048 - FreeBSD Support

## Status

Status: completed

Sub-state: delivered 2026-10-31. SOW-0031 (Linux SHM build gating) was merged in before implementation.

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
- From SOW-0031: Linux C/Rust/Go SHM and service targets keep their behavior and tests; native Windows and `MSYSTEM=MSYS` builds and tests stay green; FreeBSD (and, for the build gates, macOS) never compile Linux futex sources; CMake conditions, public docs and target availability agree.
- A POSIX peer on a platform without SHM never offers or accepts an SHM profile, in any language.

## Analysis

Sources checked:

- SOW-0046 investigation (`.agents/sow/done/SOW-0046-20261029-go-cross-platform-build.md`).
- SOW-0031 (merged in).
- FreeBSD 14.3 build of `b66b224` on the test VM, 2026-10-31 (rust 1.98.1 and go 1.26.10 installed there from FreeBSD packages under the user's standing permission for that VM).

Current state (2026-10-31, `b66b224`):

- C: `netipc_shm.c:37` and `tests/fixtures/c/test_service.c:29` include `<linux/futex.h>`; CMake builds SHM, the service library and SHM tests `if(NOT APPLE)` (`CMakeLists.txt:73,291,309,342,478,548,609,638,665,684,734,747,771,1144,1157,1176`), which includes FreeBSD. `netipc_service` links `netipc_shm` unconditionally (`CMakeLists.txt:100-102`).
- Rust: `server_session_unix.rs:368` uses the glibc-only `libc::__errno_location`; `tests/fixtures/rust/src/bin/interop_shm.rs` gates on `not(windows)` instead of Linux. The SHM module and service call sites are already `cfg(target_os = "linux")`.
- Go: `service/raw/client_unix.go` and `server_unix.go` reference `posix.Shm*`, defined only in `transport/posix/shm_linux.go`.
- Rust and Go on non-Linux unix still pass the configured SHM profile bits to the handshake, so two such peers can agree on an SHM profile in HELLO_ACK and then use the socket. This breaks the HELLO_ACK contract (`docs/level1-wire-envelope.md`, "selected_profile is final").
- Netdata builds NetIPC only `if(OS_LINUX)` / `elseif(OS_WINDOWS)` (`netdata/netdata @ ae9113f2cc`, `src/libnetdata/CMakeLists.txt:88-118`), and its consumers use `#if OS_LINUX || OS_WINDOWS`. FreeBSD support is a library deliverable; enabling it in Netdata is a separate Netdata change.

Risks:

- Packet-size negotiation differences across FreeBSD versions.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The build and the handshake treat "unix" as "Linux". C gates by `NOT APPLE`, Go's service layer assumes Linux SHM symbols exist, Rust uses one glibc errno accessor, and non-Linux peers can negotiate a profile their platform lacks.

Evidence reviewed:

- See Analysis, current state.

Affected contracts and surfaces:

- CMake target availability on Linux, FreeBSD, macOS, native Windows and MSYS.
- C `netipc_shm.h` error enum (one new value at the end), `netipc_shm.c` platform split.
- L1 POSIX handshake in C, Rust and Go: platform profile mask for client and server.
- Go `transport/posix` exported SHM API on non-Linux unix.
- Test fixtures and scripts that assume Linux.
- Docs: `docs/level1-transport.md`, `docs/level1-posix-uds.md`, `docs/level1-posix-shm.md`, `docs/netipc-integrator-skill.md`.

Existing patterns to reuse:

- Rust `cfg(target_os = "linux")` gates; Go per-OS files (`internal/syspoll` from SOW-0046); CMake `NETIPC_WINDOWS_RUNTIME`.
- The profile-drop path of SOW-0055 (unchanged; the mask runs before negotiation).

Risk and blast radius:

- Build gates can drop Linux targets silently; validation compares the Linux ctest list before and after.
- The handshake mask runs on every connect on every POSIX platform; on Linux it must be a no-op, which the existing Linux SHM tests and interop prove.
- Windows is untouched except by the shared CMake file; the Windows suites run at close.

Performance assessment:

- The mask is one AND on handshake config at connection setup. Nothing on an established session's request path changes on Linux, so no benchmark run.

Sensitive data handling plan:

- No secrets, customer data or private host details. The VM is named only as "the FreeBSD test VM"; evidence is code paths and command output. This covers this SOW, specs, docs, skills and code comments.

Implementation plan:

1. L1 POSIX handshake platform mask (decision 2) in C, Rust and Go, with tests.
2. C: `netipc_shm.c` Linux implementation under `__linux__`, unsupported stubs otherwise (decision 3); CMake gates by explicit capability; SHM-only C tests and SHM sections of mixed tests gated to Linux.
3. Rust: portable errno, `interop_shm` gated to Linux.
4. Go: `shm_other.go` stubs (decision 3).
5. Make the C, Rust and Go suites, ctest and interop pass on the FreeBSD VM; fix every FreeBSD-specific failure (packet size, socket semantics, test-harness portability).
6. Docs and a FreeBSD validation procedure (decision 4).

Validation plan:

- FreeBSD VM: configure, build, ctest, `cargo test`, `go test ./...`, interop scripts.
- Linux: ctest (same test list as before), `cargo test`, `go test`, the 9 Netdata targets.
- Windows: ctest, Rust, Go.
- Cross-checks: `cargo check --target x86_64-unknown-freebsd`, `GOOS=freebsd go vet ./...`.
- Mask tests fail before the fix.

Artifact impact plan:

- AGENTS.md: add the FreeBSD validation command (decision 4).
- Runtime project skills: none expected; checked at close.
- Specs: `docs/level1-transport.md`, `docs/level1-posix-uds.md`, `docs/level1-posix-shm.md`.
- End-user/operator docs: same files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` platform notes.
- SOW lifecycle: SOW-0031 merged in and closed.

Open-source reference evidence:

- FreeBSD unix socket rework: review D44151 (cited with commit if used).

Open decisions:

- None; decisions 1-4 below are assistant-applied under the user's autonomy instruction (long-term-best).

## Implications And Decisions

1. SOW-0031 is merged into this SOW and closed (process decision; AGENTS.md requires merging overlapping SOWs before implementation).
2. The L1 POSIX handshake removes profiles the platform cannot provide from both supported and preferred sets, on the client and on the server, before HELLO / negotiation. Linux keeps baseline + SHM_HYBRID + SHM_FUTEX; other POSIX systems keep baseline only. Reason: one place per language that every caller passes through; L2 and integrators cannot get it wrong.
3. C and Go keep the SHM API present on every POSIX system. Off Linux it returns a new "unsupported" error (C `NIPC_SHM_ERR_UNSUPPORTED`, appended to the enum; Go `ErrShmUnsupported`). Rust keeps its existing `cfg(target_os = "linux")` gates. Reason: the C file list stays the same on every POSIX platform (Netdata's `NETIPC_POSIX_TRANSPORT_FILES` needs no split) and the service code needs no guards. Rust already has idiomatic gates, and decision 2 keeps the stubs unreachable.
4. FreeBSD is guarded by a documented manual procedure on the FreeBSD test VM, recorded in AGENTS.md project commands. Reason: GitHub-hosted runners do not offer FreeBSD natively; a CI VM action can follow as its own SOW if the user wants it.

## Plan

The gate's implementation plan, executed in order.

## Execution Log

### 2026-10-29

Created from the SOW-0046 platform-scope split.

### 2026-10-31

- Platform profile mask in the L1 POSIX handshake: C `NIPC_UDS_PLATFORM_PROFILES` (`netipc_uds_internal.h`, applied in `netipc_uds_handshake.c` to the client HELLO and the server supported/preferred sets), Rust `PLATFORM_PROFILES` (`transport/posix.rs`), Go `HelloConfig.PlatformProfiles` / `ServerHelloConfig.PlatformProfiles` (`framing/handshake.go`, set by `transport/posix/shm_linux.go` and `shm_other.go`).
- Bug found while adding the mask: clients accepted a successful HELLO_ACK whose `selected_profile` they never offered, or that had zero or several bits set. All three languages, on POSIX and Windows, now reject it as a protocol error (C `netipc_uds_handshake.c`, `netipc_named_pipe.c`; Rust `transport/mod.rs` `selected_profile_offered()` used by `posix.rs` and `windows.rs`; Go `framing.ClientHandshake`, shared by both platforms). Same-failure search: every client handshake path (UDS and Named Pipe, C/Rust/Go) was checked and fixed.
- C SHM: `NIPC_SHM_ERR_UNSUPPORTED` appended to the enum; `netipc_shm.c` keeps the implementation under `__linux__` and has stubs elsewhere.
- CMake: `NETIPC_POSIX_RUNTIME` (POSIX, not Apple) and `NETIPC_POSIX_SHM` (Linux) replace the `NOT APPLE` gates; SHM tests, SHM interop and all POSIX benchmarks build only with `NETIPC_POSIX_SHM`.
- Rust: portable errno in `server_session_unix.rs`; `interop_shm` and SHM-only code gated to Linux; per-platform `SNDBUF_OVERHEAD`; dead-code gates so the library, tests and `bench_posix` build warning-free on Linux, FreeBSD and Windows.
- Go: SHM errors and `ShmRole` moved to `transport/posix/shm.go`; `shm_other.go` stubs; `uds_sndbuf_freebsd.go` / `uds_sndbuf_other.go`; SHM service tests moved to `service/raw/shm_linux_test.go`.
- Robustness suites now run on every POSIX system, like the C one: Rust `raw_robustness_unix_tests.rs` (module gate dropped; SHM cases gated to Linux) and Go (`robustness_unix_test.go` now `unix`; SHM cases moved to `robustness_shm_linux_test.go`).
- C tests: SHM parts of `test_service.c`, `test_service_extra.c`, `test_service_payload_limits.c`, `test_service_method_limits.c` and `test_robustness.c` gated to Linux; `test_robustness.c` uses `realpath(argv[0])` instead of `/proc/self/exe` and an fstat scan instead of `/proc/self/fd`; new `test_uds.c` case "Platform profile mask".
- `tests/interop-lib.sh`: `interop_pid_runs` uses `ps` where `/proc/PID` does not exist.
- FreeBSD measurement on the test VM: the largest SEQPACKET datagram equals `SO_SNDBUF` exactly (default 8192, `net.local.seqpacket.maxseqpacket`); Linux accepts `SO_SNDBUF - 32`.
- The Rust and Go mask tests first derived their expectation from the constant under test, so a mutant mask still passed. Both now key off the OS (`cfg!(target_os = "linux")`, `runtime.GOOS`).

## Validation

Acceptance criteria evidence:

- C, Rust and Go build and pass on FreeBSD 14.3 (test VM): ctest 24/24 (includes `test_uds_interop`, `test_service_interop`, `test_cache_interop`, `interop_codec`), `cargo test` 316 passed, `go test ./...` ok, `tests/interop_codec.sh` "ALL INTEROP TESTS PASSED". The Rust and Go robustness suites run and pass there (14 tests each, counting the helper-process entry).
- FreeBSD guard: AGENTS.md project commands carry the FreeBSD procedure (decision 4).
- SOW-0031 criteria: the Linux ctest list is unchanged (32 tests, compared before/after); Linux, native Windows and MSYS suites pass (below); FreeBSD builds no futex source (`netipc_shm.c` stubs, `NETIPC_POSIX_SHM` off).
- No SHM profile off Linux: mask tests in all three languages (C `test_uds` "Platform profile mask", Rust `test_profile_preference_selects_shm_futex`, Go `TestProfileSelection`) pass on FreeBSD and fail with the mask removed (mutants on the VM: C "FAIL: selected profile matches the platform", Rust "left: 4 right: 1", Go "selected = 0x4/0x4, want 0x1"); the tree was restored afterwards.

Tests or equivalent validation:

- Linux: build warning-free; ctest 32/32. `test_shm_full_backing_store` skips without root (`kernel.apparmor_restrict_unprivileged_userns = 1`, pre-existing), so it was run as root with `NIPC_REQUIRE_TMPFS_TEST=1`: PASS. `cargo test` 386 passed; `go test ./...` ok.
- Selected-profile fix: C `test_uds.c` modes `MALFORMED_ACK_SELECTED_NOT_OFFERED/NONE/TWO`, C `test_named_pipe.c` `FAKE_ACK_SELECTED_*`, Rust `test_connect_rejects_selected_profile_not_offered` (POSIX and Windows), Go three cases in `uds_more_edge_test.go`; mutation-checked on Linux.
- Windows (costa-win11, MSYS2): Rust 266 passed (one new), Go ok, ctest 20/20.
- Netdata Linux targets: all 9 pass (`x86_64-musl`, `i386-gnu`, `i386-musl`, `armv7-gnu`, `armv7-gnu-time64`, `armv7-musl`, `armv6-musl`, `aarch64-gnu`, `aarch64-musl`). The first run of `armv7-gnu` failed to build its Go tests because it read `uds_test.go` while that file was mid-edit; the four targets that ran before the last edits (`x86_64-musl`, `i386-gnu`, `i386-musl`, `armv7-gnu`) were rerun on the final tree and pass.
- Static checks: `cargo clippy --all-targets --all-features --locked -- -D clippy::correctness -D clippy::suspicious` passes on Linux, `x86_64-pc-windows-gnu` and FreeBSD; `gofmt` clean; `go vet` clean for linux and freebsd. `GOOS=windows go vet` reports pre-existing "possible misuse of unsafe.Pointer" in `transport/windows/shm.go` and `shm_test.go`, files this SOW does not touch.

Real-use evidence:

- FreeBSD cross-language interop (C, Rust, Go servers and clients over UDS, service and cache layers) is the runnable path; it passes.

Reviewer findings:

- No external review run; the mutants above serve as the check that the new tests can fail.

Same-failure search:

- Linux-only assumptions: `grep` for `/proc/self`, `__errno_location`, `linux/futex.h`, `NOT APPLE`, `shm_linux` symbols across C, Rust, Go, CMake and test scripts; every hit is gated or ported.
- Selected-profile validation: all client handshakes (UDS, Named Pipe) in three languages.

Performance assessment:

- The mask and the selected-profile check run once per handshake (`netipc_uds_handshake.c` client and server config setup; Rust `posix.rs` handshake functions; Go `framing/handshake.go`). Nothing on an established session's request path changed on Linux or Windows; no benchmark run.

Sensitive data gate:

- No secrets, customer data, private endpoints or host names in this SOW, docs, AGENTS.md, specs or code comments; the VMs are named by role only.

Artifact maintenance gate:

- AGENTS.md: FreeBSD validation command added.
- Runtime project skills: `project-netdata-vendoring` is unaffected; vendoring preflight does not change (Netdata builds NetIPC on Linux and Windows only).
- Specs: `docs/level1-transport.md` (platform mask), `docs/level1-posix-uds.md` (FreeBSD overhead), `docs/level1-wire-envelope.md` (client rejects an unoffered `selected_profile`), `docs/level1-posix-shm.md` (stubs off Linux). No `.agents/sow/specs/` change: the public docs carry these rules.
- End-user/operator docs: the same `docs/` files.
- End-user/operator skills: `docs/netipc-integrator-skill.md` platform notes.
- SOW lifecycle: SOW-0031 merged and closed in `done/`; this SOW moves to `done/`; SOW-0056 created for FreeBSD 15.

## Outcome

NetIPC builds and passes its C, Rust and Go suites and cross-language interop on FreeBSD, using the UDS baseline only. On non-Linux POSIX systems the handshake can no longer negotiate SHM, whatever the configuration. Clients on every platform now reject a HELLO_ACK that selects a profile they did not offer.

## Lessons Extracted

- A test that derives its expectation from the constant it checks cannot fail. Platform-dependent expectations must come from the platform (`cfg!`, `runtime.GOOS`, `#if`), not from the code under test. The mutants found two such tests.
- Editing tree files while a Linux-target run is reading them can produce false failures (armv7-gnu Go saw a half-applied edit). Do not edit files during a target run, or rerun the targets that read them.

## Followup

- FreeBSD 15 and the supported 14.x point releases: SOW-0056 (needs a test host from the user).
- FreeBSD CI runner: rejected for now (decision 4: no native GitHub-hosted FreeBSD runner); the AGENTS.md procedure guards FreeBSD.
- macOS (the darwin Go `MSG_NOSIGNAL` build failure and Apple gates): SOW-0049.
- Enabling NetIPC in Netdata's FreeBSD build is a Netdata change outside this repository; no Netdata consumer uses it today.
- Search of this SOW for defer/later/follow-up/future/TODO/pending: only the items above and the word "pending" in historical text.

## Regression Log

None yet.
