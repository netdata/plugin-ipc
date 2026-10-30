# SOW-0046 - Go Cross-Platform Build

## Status

Status: completed

Sub-state: reopened 2026-10-29 because the first close parked C/Rust 32-bit coverage in SOW-0037 (the user ruled an untestable Netdata target a blocker); completed 2026-10-30 with C, Rust and Go suites and interop executed on every Netdata Linux target. See `## Reopened - 2026-10-29` and `## Validation (reopened scope)`.

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
- 2026-10-29: Rust library `cargo check --lib` passes for aarch64-unknown-linux-gnu and armv7-unknown-linux-gnueabihf. No local 32-bit C toolchain exists (no multilib, no ARM cross gcc).
- Found 2026-10-29 while running the Go suite on 32-bit (zero-known-bug policy, fixed here):
  - `transport/posix/shm_linux.go` `ShmReceive` checked `int(mlen) <= maxCopy`; on 32-bit `int(0xFFFFFFFF)` is -1, so a peer's forged length passed and `buf[:mlen]` panicked (`TestShmChaosForgedLength` on 386 and arm). A client could crash a 32-bit Go server.
  - `ShmClientAttach` summed offsets and capacities as `uint32` and `int`; on 32-bit a forged header with offsets near 4 GiB passed validation (attach returned no error).
  - The same response-end wrap exists in C (`netipc_shm.c`, 32-bit `size_t`) and Rust (`shm.rs`, 32-bit `usize`); request end was already guarded there. Windows C and Go attach are guarded; Windows Go receive rejects negative `int32` lengths and is not affected.
  - `TestShmSendBadParamGuards` built `ShmContext.data` from small byte slices that may be stack-allocated and unaligned; atomics on them raise SIGBUS on arm64 (test-only defect; production data is a page-aligned mmap).

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

- Fixed by this SOW; see Validation.

Risks:

- Replacing `poll` with `ppoll` changes timeout encoding (timespec instead of milliseconds); errors there would affect every Go receive and SHM server wait.

## Pre-Implementation Gate

Status: filled 2026-10-29

Problem / root-cause model:

- The Go POSIX transport calls raw syscalls through `syscall.Syscall` with linux/amd64 assumptions: `SYS_POLL` (absent on arm64, riscv64, loong64), `Msghdr.Iovlen` assigned from a `uint64` variable (the field is `uint32` on 386/arm, `int32` on BSD/darwin), and `SYS_SENDMSG` (absent on linux/386, which multiplexes socket calls through `socketcall(2)`).
- 32-bit test files use untyped constants above `MaxInt32` (`1<<31`, `uint64(^uint32(0))+1`) that fail to compile even where the test skips at runtime.

Evidence reviewed:

- `GOARCH=<arch> go vet ./...` failures listed under Facts.
- Go standard library `syscall` tables: linux/386 sends through `SYS_SOCKETCALL` with call number 16 (`syscall_linux_386.go`); `SYS_PPOLL` exists on every Linux architecture.
- Kernel `ppoll(2)`: a NULL sigmask ignores `sigsetsize`; a NULL timespec waits indefinitely. On 32-bit architectures the legacy `ppoll` takes the 32-bit `struct timespec`, which matches `syscall.Timespec` there; relative timeouts of at most a few seconds are not affected by the 2038 limit.

Affected contracts and surfaces:

- Go only: `transport/posix` send and receive paths, `service/raw` SHM/UDS readiness polling. No wire format, API or behavior change; C and Rust are untouched (Rust library `cargo check` already passes on aarch64/armv7).

Existing patterns to reuse:

- Build-tagged per-platform files already used in the Go packages (`*_unix.go`, `*_windows.go`).
- A small internal package for shared syscall helpers, so `transport/posix` and `service/raw` share one poll implementation instead of two copies.

Risk and blast radius:

- Every Go UDS receive and SHM server wait goes through the new `syspoll.Poll`; an encoding error in the timespec would break all Go receives. Mitigated by the full Go test suite on amd64 and 386 plus interop.
- The 386 socketcall path is new code used only on linux/386; covered by running the full Go test suite natively as 386.

Sensitive data handling plan:

- No secrets, customer data or private endpoints involved; evidence is code paths, command names and test output only. Applies to this SOW, specs, docs, skills and code comments.

Implementation plan:

1. New `internal/syspoll` package: `PollFd` plus `Poll` using `SYS_PPOLL` on Linux and `SYS_POLL` on other unix systems.
2. `transport/posix/uds_receive.go` and `service/raw/poll_unix.go` use `syspoll`.
3. `uds.go` sets `Iovlen` from untyped constants and calls `rawSendmsg`, implemented in `uds_sendmsg.go` (direct `SYS_SENDMSG`) and `uds_sendmsg_linux_386.go` (socketcall), per decision 1A.
4. Make 32-bit test constants non-constant expressions.
5. CI: cross-architecture Go vet plus Go tests on 386 and, under qemu, arm64/arm.

Validation plan:

- `go vet ./...` (library and tests) for linux amd64, 386, arm, arm64, riscv64, loong64, ppc64le, s390x, mips64le.
- Full Go test suite on amd64 and natively on 386; on arm64 and arm under qemu user emulation. Only the known open reproduction tests (SOW-0043/0044) may fail.
- `tests/test_uds_interop.sh` and `tests/test_service_interop.sh` with the standard build, plus a 386 Go fixture against C/Rust.
- `gofmt`, staticcheck and gosec-relevant annotations kept consistent.

Artifact impact plan:

- AGENTS.md: assessed at close; no workflow change expected.
- Runtime project skills: none affected (vendoring skill unaffected by internal Go file layout).
- Specs: `docs/` describe protocol and transport behavior, not the syscall used; Linux architecture coverage noted where the docs list platforms.
- End-user/operator docs: same as specs.
- End-user/operator skills: `docs/netipc-integrator-skill.md` assessed at close.
- SOW lifecycle: the Go part of SOW-0037 is delivered here and noted there; FreeBSD and macOS need their own SOWs.

Open-source reference evidence:

- `golang/go` standard library `src/syscall/syscall_linux_386.go` (socketcall numbers); cited by file, no local mirror used.

Open decisions:

- None for this SOW. FreeBSD and macOS design questions belong to their own SOWs.

## Implications And Decisions

1. 2026-10-29, platform scope: the user stated NetIPC must build and run on every platform Netdata builds for - Linux on all architectures, Windows, FreeBSD and macOS ("netipc should build and run on all these platforms"). An earlier assistant recommendation to narrow support to Linux + Windows was wrong: it inferred product scope from Netdata's current CMake gates (`src/libnetdata/CMakeLists.txt:89-118`, `NetdataPluginApps.cmake:33-48`, `NetdataOptions.cmake:102`, `rust-tests.yml:86` excluding netipc), which exist only because NetIPC does not build elsewhere. The work is split: this SOW covers Linux on all architectures; FreeBSD and macOS get their own SOWs, each presented to the user separately.
2. 2026-10-29, linux/386 send path (decision 1A): on linux/386 only, send through `SYS_SOCKETCALL` (sendmsg = 16); keep the no-copy raw `sendmsg` on every other platform.
3. 2026-10-29, scope extension without a design fork: the 32-bit SHM bounds bugs found by this SOW's validation are fixed here under the zero-known-bug policy and the cross-language parity rule (same contract in C, Rust and Go). The fixes only widen arithmetic; behavior on 64-bit is unchanged. Executing the C/Rust suites on 32-bit stays in SOW-0037, which owns that acceptance criterion.

## Plan

1. `internal/syspoll` (ppoll on Linux, poll elsewhere) used by `transport/posix` and `service/raw`.
2. `Iovlen` from untyped constants; `rawSendmsg` split per decision 1A.
3. 32-bit-safe test constants; aligned test SHM buffers.
4. 32-bit-safe SHM receive and attach bounds in Go; matching attach guard in C and Rust; regression tests in all three.
5. Runtime Safety CI job; docs and integrator skill; follow-up SOWs.

## Execution Log

### 2026-10-29

- Implemented plan items 1-5. Files: `src/go/pkg/netipc/internal/syspoll/*`, `transport/posix/{uds.go,uds_receive.go,uds_sendmsg.go,uds_sendmsg_linux_386.go,shm_linux.go}`, `service/raw/poll_unix.go`, Go tests (`uds_more_edge_test.go`, `shm_more_edge_test.go`, `cache_test.go`, `lookup_common_test.go`), `netipc_shm.c`, `tests/fixtures/c/test_shm.c`, `shm.rs`, `shm_tests.rs`, `.github/workflows/runtime-safety.yml`, `docs/level1-posix-shm.md`, `docs/netipc-integrator-skill.md`.
- Created SOW-0048 (FreeBSD) and SOW-0049 (macOS); noted SOW-0046 delivery in SOW-0037 and the SOW-0031/SOW-0048 overlap.

## Validation

Acceptance criteria evidence:

- `GOARCH=<arch> go vet ./...` (library and tests) passes for amd64, 386, arm, arm64, riscv64, loong64, ppc64le, s390x, mips64le. Go stays pure: stdlib `syscall` only, no cgo.
- CI: new `linux-architectures` job in `.github/workflows/runtime-safety.yml` (actionlint clean). Not yet observed running on GitHub; the binfmt check step fails loudly if QEMU handlers are missing.

Tests:

- Full Go suite (`go test ./...`, low priority) on amd64 and native 386 (~170 s each), and on arm64 and arm under qemu user emulation (~177 s each): every package passes except the same 9 open reproduction tests on every architecture (`TestRobust*` for SOW-0043/SOW-0044). The 386/arm-only `TestShmChaosForgedLength` panic and the arm64 SIGBUS are gone.
- Proof of the attach bug: the new `request-end-wraps-uint32` and `response-end-wraps-uint32` cases fail on 386 against the previous `shm_linux.go` (`attach error = <nil>`) and pass after the fix; they pass on amd64 both before and after, as expected.
- C: `build/bin/test_shm` 131 passed, 0 failed, including `test_shm_response_end_wraps_uint32`.
- Rust: `cargo test --lib test_client_attach` 9 passed, including `test_client_attach_rejects_response_end_wrapping_u32`; `cargo fmt --check` clean; `cargo check --lib` clean for aarch64 and armv7.

Real-use evidence:

- Interop with Go fixtures built as 386 (native) and arm64 (qemu) against native C and Rust: `tests/test_uds_interop.sh` 18/18, `tests/test_shm_interop.sh` 9/9, `tests/test_service_interop.sh` 18/18 (including the 300 ms handler round) for both.

Limits:

- C and Rust fixes are not executed on a 32-bit target locally: no 32-bit C toolchain, the ARM musl Rust target is not installed, and a root-owned unreadable cargo registry file (`fnv-1.0.7/lib.rs`, a proptest dependency) blocks compiling Rust test targets for new targets on this workstation. Tracked in SOW-0037 at the time; resolved in the reopened scope (see `## Validation (reopened scope)`).
- Local staticcheck is older than the Go 1.27 toolchain (export data error); CI runs staticcheck with its own toolchain.

Reviewer findings:

- No external review was requested for this SOW; self-review corrected an initial false claim that Windows Go receive shared the `int(mlen)` defect (it rejects negative `int32` lengths), and removed a redundant `respEnd > MaxUint32` clause to keep parity with C and Rust.

Same-failure search:

- `int(<uint32>)` comparisons across Go non-test code: framing (`receive.go`) is bounded by `totalMessageLen` against `maxInt`; Windows SHM attach has explicit overflow guards; only POSIX SHM receive and attach were affected.
- C/Rust attach: POSIX response end unguarded (fixed); request end already guarded; Windows C guarded for both.
- Test SHM buffers: all nine hand-built `ShmContext.data` slices now use `alignedShmData`; other atomic-helper tests hit the bounds check before any atomic access.

Artifact maintenance gate:

- AGENTS.md: no change; commands and workflow unchanged (the new CI job uses existing scripts' conventions).
- Runtime project skills: no change; the vendoring skill is unaffected by internal Go files.
- Specs: `docs/level1-posix-shm.md` client attach step 6 now states the header validation and overflow-safe arithmetic that all implementations perform.
- End-user/operator docs: `docs/level1-transport.md` still claims FreeBSD/macOS UDS support; that discrepancy is owned by SOW-0048/SOW-0049, not changed here.
- End-user/operator skills: `docs/netipc-integrator-skill.md` lists the Linux architectures CI covers.
- SOW lifecycle: status `completed`, moved to `done/` in the same commit as the work.

Lessons extracted: see below.

Follow-up mapping: see Followup.

## Outcome

Go builds and runs on Linux amd64, 386, arm and arm64 (tested) and vets on nine Linux architectures. Four 32-bit memory-safety and robustness bugs were fixed (Go SHM receive forged-length panic; Go, C and Rust SHM attach overflow), and CI now guards Linux architectures for Go and the Rust library.

## Lessons Extracted

- Running the suite on 32-bit and on a strict-alignment architecture found real bugs that amd64 tests cannot: `int(uint32)` wraps on 32-bit, and unaligned atomics only fault off x86. Cross-architecture test execution, not just builds, belongs in CI.
- Treat every `int(x)` of a peer-controlled `uint32` as suspect on 32-bit; compare in the unsigned domain or in 64-bit.

## Followup

- SOW-0048: FreeBSD support (C/Rust/Go build gating, packet sizing).
- SOW-0049: macOS support (needs a baseline transport design).
- SOW-0037: merged into this SOW and closed (its scope is delivered in the reopened scope).
- The 9 open reproduction tests remain owned by SOW-0043 and SOW-0044; until those land, `go test ./...` in CI (existing `go-static` job and the new job) stays red by design of the zero-known-bug policy.

## Regression Log

None yet.

## Reopened - 2026-10-29

Why reopened:

- The closed SOW verified the C/Rust attach fix on 64-bit only and parked 32-bit execution in SOW-0037. User, 2026-10-29: "A NETDATA SUPPORTED CONFIGURATION/ARCHITECTURE/OS WHICH WE CANNOT TEST IS A BLOCKER. IT IS NOT A NOTE."
- Netdata-supported Linux targets (netdata/netdata `.github/data/distros.yml`, `packaging/makeself/build-static.sh`): x86_64, i386 (Debian 12 packages), armv7/armhf, armv6l (static), aarch64; glibc and musl (Alpine 3.22/3.23 Core, all static builds).

Done so far (uncommitted):

- Workstation cross toolchains documented in the host docs (versioned `-14` cross gcc names, no 32-bit x86 C, `gcc-multilib` conflict); cargo registry ownership fixed with user approval.
- C suite cross-built for armhf (glibc, time64/LFS) and aarch64, run under QEMU; Rust suite on armv7 and aarch64 under QEMU; Go armv6 under `QEMU_CPU=arm1176`.
- 32-bit proof of the attach fix: C (`test_shm` old code `FAIL: wrapping response end rejected`, fixed passes) and Rust (old code panics at `shm.rs:448`, fixed passes) on 32-bit ARM.
- Test-harness bugs found on these targets and fixed: `fallocate` link wrapper missed `fallocate64` on glibc LFS builds (9 `test_shm` + 1 `test_service` false failures); Rust fd-reuse races (`test_poll_fd_invalid_fd_returns_error`, `test_accept_on_closed_listener_returns_accept_error`, which cascaded into `test_pipeline_10` ENOTSOCK and `test_stress_rapid_connect_disconnect`); Rust listener readiness race (socket file exists at bind, before listen) in four test helpers plus the raw hello-ack server; follow-up-subcall timeout tests with 30/75 ms budgets widened in C, Rust and Go (POSIX and Windows).
- `test_shm_timeout` hung once (25 s) in the first armhf full run and never again in 242 runs; fixture now unbuffers stdout and bounds the forked child so a recurrence reports evidence.

Results (only the known SOW-0043/0044 reproduction tests fail):

| Target | C | Rust | Go |
|---|---|---|---|
| x86_64 glibc | 7 known | 9 known | 9 known |
| armhf glibc (QEMU) | 7 known | 9 known (x3) | 9 known |
| aarch64 glibc (QEMU) | 7 known | 9 known | 9 known |
| i386 | no toolchain | no toolchain | 9 known |
| armv6l (QEMU arm1176) | no toolchain | no toolchain | 9 known |
| musl (any arch) | no toolchain | no target | libc-independent |

User decisions (2026-10-29, recorded before implementation):

1. Toolchains (A, long-term-best): install on the workstation the apt packages `gcc-i686-linux-gnu`, `libc6-dev-i386-cross` and `musl-tools` (dry run: no removals; never `gcc-multilib`, which removes the ARM cross compilers), rustup targets `i686-unknown-linux-gnu`, `x86_64-unknown-linux-musl`, `i686-unknown-linux-musl`, `armv7-unknown-linux-musleabihf`, `arm-unknown-linux-musleabihf`, `aarch64-unknown-linux-musl`, and the latest stable Zig as a user-level tarball for musl C on i386/armv6/armv7/aarch64.
2. CI (A, long-term-best): a CI matrix running the full C, Rust and Go suites on every Netdata Linux target (x86_64, i386, armv6l, armv7, aarch64) for glibc and musl.
3. SOW scope (A, long-term-best): SOW-0037 is merged into this SOW and closed as merged; its remaining scope (Rust ARM musl `pthread_t` test compile, C/Rust suites and interop on 32-bit) is delivered here.
4. Product race (A, long-term-best): the bind-to-listen window treated as stale by stale-socket recovery is tracked in SOW-0043 (server lifecycle) with a reproduction test first, then the fix.

Reopened-scope gate addendum (2026-10-29):

- Problem model: C and Rust (and Go on ARMv6) were never executed on the 32-bit, ARMv6 and musl targets Netdata ships, so target-specific product and test-harness defects were invisible.
- Netdata 32-bit glibc layout: Netdata's build sets neither `_TIME_BITS` nor `_FILE_OFFSET_BITS` (netdata/netdata @ 57e634eb4f `CMakeLists.txt`, no matches), so Debian 12 i386/armhf packages use the legacy 32-bit `time_t`/`off_t` layout, while Debian 13/Ubuntu armhf compilers default to time64 (`arm-linux-gnueabihf-gcc -dM -E` defines `_TIME_BITS 64`). Both layouts are therefore tested for armv7 glibc; i386 glibc (Debian 12 only) is legacy.
- Target list: x86_64-musl, i386-gnu (legacy), i386-musl, armv7-gnu (legacy), armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl; x86_64 glibc is the existing host CI.
- Implementation: one shared runner `tests/run-linux-target.sh` used locally and by a `linux-targets` CI matrix; static linking so no target sysroot is needed at run time; QEMU pinned to each target's baseline CPU (arm1176, cortex-a7, cortex-a53) so newer instructions trap; Zig 0.17.0 (SHA-256 pinned in CI) for musl C.
- Risk: CI cost (9 parallel jobs of about 4-7 minutes); emulator timing makes tight timing tests flaky, so timing-dependent tests found flaky are fixed to wait on conditions, never gated.
- Sensitive data: only target names, file paths, command names and test results are recorded; no host inventories or private endpoints in SOWs, docs, skills or comments.

Findings and fixes (reopened scope):

- Product bug: the Rust library did not compile for 64-bit musl (x86_64/aarch64, Netdata static builds): `posix.rs` built `libc::msghdr` with a struct literal, but musl's 64-bit `msghdr` has private padding fields. Fixed by zero-initializing and assigning fields; clean on all 9 targets and on MSRV 1.91.
- Test bug (SOW-0037 scope): `raw_unix_tests.rs` moved a musl `pthread_t` (a raw pointer, not `Send`) into a thread; now carried as an integer.
- Test bug, all languages: SHM dead-owner tests forged owner PID 99999 as "dead"; hosts with a large `pid_max` (here 4194304) reuse it, and FreeBSD's PID range ends at 99999. Replaced by `INT32_MAX`, above every OS PID limit (C `test_shm.c`, Rust `shm_tests.rs`, Go `shm_linux_test.go`). (The armv7-gnu `test_shm` "stale file cleaned up" failure first attributed to this had a different cause; see the LFS product bug below.)
- Test bug, Rust and Go: the worker-capacity test assumed the first session's worker slot frees within a fixed 100 ms; under QEMU the server still dropped the verifier at capacity (`verification call: Truncated` on aarch64-gnu and armv7-gnu-time64). Now retries within a 5 s deadline. The C equivalent (`test_multi_server`) already passes.
- Test bug, C: `test_stress` long-run memory check took its RSS baseline before starting client threads, so thread and, under QEMU, emulator warm-up counted as growth (10.5 MB on arm1176, 9.2 MB on cortex-a7, 1.5 MB for the same armv6 binary on QEMU `max`). Baseline now taken after a 10 s warm-up; steady-state growth over 50 s is -52 kB (arm1176), -44 kB (cortex-a7), -12 kB (host native).
- Product bug, C only: on 32-bit glibc builds without large-file support (Netdata's Debian 12 i386/armhf packages), `readdir()` and `fstat()`/`fstatat()` fail with `EOVERFLOW` when a directory offset or inode number exceeds 32 bits, so SHM stale cleanup silently skipped the run directory (`netipc_shm.c` `nipc_shm_cleanup_stale`: a NULL from `readdir()` looks like end of directory) and stat failures classified files as live. Reproduced deterministically on armv7-gnu under qemu-user (guest trace: `getdents64(...) = 112` then `close`, no entry inspected); on real systems it affects 32-bit userlands on 64-bit kernels with filesystems that hand out 64-bit inode numbers (XFS inode64, btrfs, tmpfs inode64). Rust (std `readdir64`) and Go (`getdents64`) were unaffected. Fix: `_FILE_OFFSET_BITS 64` defined at the top of `netipc_shm.c` and `netipc_uds_lifecycle.c`; no shared header exposes `off_t`, `struct stat`, `ino_t` or `DIR`, `time_t` is unaffected, and Netdata's build has no unity/precompiled-header mode, so the per-file define is ABI-neutral and travels with the vendored source. The test `fallocate` wrapper now defines `__wrap_fallocate64` on every glibc build. Assistant judged the option set had one sound choice; user confirmed (2026-10-30).
- Test bug, Rust and Go: the worker-capacity test deadlocked forever when an assertion failed before the blocked handler was released (Rust: unwinding dropped `TestServer`, whose stop joins a handler spinning on `release`; Go: `defer releaseOnce()` was registered before `defer ts.stop()`, so stop ran first). Observed as an 8-hour hang of the armv7-gnu-time64 Rust suite, diagnosed from the futex join chain of the hung threads. Fixed with a release guard dropped before the server (Rust) and reordered defers (Go); the Rust 500 ms "handler entered" wait is now a 5 s deadline. Same-pattern search: the other blocking test handlers are bounded (`recv_timeout`, or the release is unconditional).
- Test bug, harness: the CTest wrapper `test_shm_rust` (`cargo test shm -- --test-threads=1`) also selects the SHM robustness reproduction tests added in 526db47; run serially the set takes about 34 s against a 30 s CTest timeout, so the wrapper reported `Timeout` and hid the results of the other 69 SHM tests. Timeout raised to 120 s (as for `test_service_rust`, which also carries robustness tests); the wrapper now reports 69 passed and only the 3 known SHM robustness failures.
- Environment: a host memory-reclaim storm (memory pressure "some" 60-88%, system CPU 70-98%, per the host's Netdata, 21:53-22:18 UTC on 2026-10-29) froze emulated test processes for about 85 s at a time and produced a burst of timing failures on armv7-gnu; those results were discarded and the matrix rerun.
- Observation moved to SOW-0043: peer disconnect during a call is reported as `TRUNCATED` in all three languages, which misclassifies over-capacity disconnects.
- Harness lesson: C fixtures use fixed `/tmp` paths, so two targets must never run in parallel on one host (recorded in `AGENTS.md`).

## Validation (reopened scope)

Acceptance criteria evidence (2026-10-30, `bash tests/run-linux-target.sh <target>`, all parts, final tree):

| Target | C | Rust | Go | Interop |
|---|---|---|---|---|
| i386-gnu (legacy) | 7 known (`test_robustness`) | 9 known | 9 known | 6/6 |
| i386-musl | 7 known | 9 known | not run (Go is libc-independent; covered by i386-gnu) | 6/6 |
| x86_64-musl | 7 known | 9 known | not run (as above) | 6/6 |
| armv7-gnu (legacy) | 7 known | 9 known | 9 known | 6/6 |
| armv7-gnu-time64 | 7 known | 9 known | 9 known | 6/6 |
| armv7-musl | 7 known | 9 known | not run (as above) | 6/6 |
| armv6-musl | 7 known | 9 known | 9 known | 6/6 |
| aarch64-gnu | 7 known | 9 known | 9 known | 6/6 |
| aarch64-musl | 7 known | 9 known | not run (as above) | 6/6 |

"Known" means only the SOW-0043/SOW-0044 robustness reproduction tests fail. The full matrix ran serially with host memory pressure sampled every 10 s; three timing failures (armv7-musl interop "server not ready", aarch64-musl `test_stress_5000_items` and interop, armv6-musl apps-lookup facade timeout) each fell inside a measured memory-reclaim window (peaks 62-70%) and passed when those parts were rerun on a calm host (no pressure above 10%).

Tests:

- Host x86_64 glibc: C CTest (all but Windows-only and fuzz) only `test_robustness` and the Rust wrapper `test_service_rust` (9 known) fail after the `test_shm_rust` timeout fix (69 passed, 3 known); `cargo test --lib` 389 passed, 9 known; `go test ./...` only `TestRobust*`.
- Windows (costa-win11, MSYS): `test_win_service` and `test_win_service_extra` pass, `test_win_robustness` known; Rust `cargo test --lib` 266 passed, 7 failures all Windows robustness reproduction tests; Go every package passes except `TestWinRobust*` in `service/raw`.
- Rust MSRV 1.91 and latest stable build on all 9 targets; `cargo fmt --check` clean; clippy adds no warnings on changed lines; `gofmt`/`go vet` clean; `shellcheck tests/run-linux-target.sh` and `actionlint` clean; `git diff --check` clean.
- `test_stress` memory check, armv6 arm1176 / armv7 cortex-a7 / host: -52 kB / -44 kB / -12 kB over 50 s after warm-up.

Real-use evidence:

- Cross-language interop (UDS, SHM, service, service SHM, cache, cache SHM) between C, Rust and Go binaries built for each target passes on all 9 targets.
- LFS fix proof: armv7-gnu `test_shm` "stale file cleaned up" failed 3/3 before (guest trace: `getdents64` returned entries, then `close` with none inspected) and passes 131/131 after.

Reviewer findings:

- No external review requested. Self-review corrected the earlier attribution of the armv7-gnu stale-file failure to PID reuse (the real cause was the LFS bug) and found the `test_shm_rust` timeout masking.

Same-failure search:

- Non-LFS directory/stat use: only `netipc_shm.c` and `netipc_uds_lifecycle.c` in library C code use `opendir`/`readdir`/`stat`; both now define `_FILE_OFFSET_BITS 64`. Rust std uses `readdir64`, Go uses `getdents64`. No shared header exposes `off_t`, `struct stat`, `ino_t` or `DIR`.
- Blocked-handler deadlocks in tests: the other handlers that block (`test_client_abort_unblocks_call` in Rust Unix/Windows, Go `TestUnixClientAbortUnblocksCall`/`TestWinClientAbortUnblocksCall`, `TestRobustClientBoundedWhenServerNeverSendsHelloAck`) are bounded by `recv_timeout` or release unconditionally; only the worker-capacity tests deadlocked.
- CTest wrappers whose name filter selects robustness tests: `test_shm_rust` (fixed) and `test_service_rust` (120 s, sufficient); `test_uds_rust` finishes in about 3 s.
- Fixed "dead" PIDs: all 99999 uses in C, Rust and Go SHM tests replaced.

Artifact maintenance gate:

- AGENTS.md: added the `tests/run-linux-target.sh` command and the rule never to run two targets in parallel (fixed `/tmp` fixture paths).
- Runtime project skills: unchanged; the vendoring skill's preflight is unaffected (no vendoring in this SOW).
- Specs: `docs/level1-posix-shm.md` stale-scan section now requires 64-bit inode/offset handling on 32-bit; validation sentence updated for the target matrix.
- End-user/operator docs: no README or operator guide describes target coverage or 32-bit build flags; the public spec and integrator skill carry these.
- End-user/operator skills: `docs/netipc-integrator-skill.md` lists the target matrix, the local command, and that the C sources enable LFS themselves.
- SOW lifecycle: SOW-0037 merged and closed in `done/`; bind/listen stale race and `TRUNCATED` misclassification recorded in SOW-0043; this SOW `completed` in `done/`.

Lessons extracted (reopened scope):

- qemu-user exposes host 64-bit directory offsets to 32-bit guests, which makes a latent LFS bug deterministic; native i386 on a 64-bit kernel hides it (compat syscalls return 32-bit hashes). Keep both emulated and native 32-bit targets.
- A test that blocks a server handler must release it on every exit path before the server is stopped, or a failed assertion becomes a hang (Rust drop order, Go defer order).
- On a shared workstation, emulated timing results are only meaningful alongside a memory/CPU pressure log; rerun pressure-correlated failures on a calm host before classifying them.
- A wrapper timeout shorter than its selected tests hides every result in the run; size wrapper timeouts for the tests their filter selects.

Follow-up mapping (pre-close search for defer/later/follow-up/future/TODO/pending):

- Robustness reproduction failures (C 7, Rust 9, Go 9; Windows equivalents): tracked in SOW-0043 and SOW-0044.
- Bind/listen stale-recovery race and peer-disconnect-as-`TRUNCATED`: tracked in SOW-0043.
- FreeBSD and macOS (including Rust test readiness on non-Linux, which falls back to file existence): tracked in SOW-0048 and SOW-0049.
- `linux-targets` CI job not yet observed on GitHub: it runs on the next push; failures there are handled under the SOW that owns the push.
- SOW-0037: merged here and closed.
- No other deferred items remain.

