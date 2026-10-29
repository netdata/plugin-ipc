# SOW-0046 - Go Cross-Platform Build

## Status

Status: completed

Sub-state: Go builds, vets and passes its suite on Linux amd64/386/arm/arm64 (vet on nine architectures); 32-bit SHM bounds bugs found during validation fixed in C, Rust and Go; CI guards it. FreeBSD and macOS tracked in SOW-0048 and SOW-0049.

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

- C and Rust fixes are not executed on a 32-bit target locally: no 32-bit C toolchain, the ARM musl Rust target is not installed, and a root-owned unreadable cargo registry file (`fnv-1.0.7/lib.rs`, a proptest dependency) blocks compiling Rust test targets for new targets on this workstation. Tracked in SOW-0037.
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
- SOW-0037: remaining 32-bit scope (Rust ARM musl test compile, executing C/Rust suites and interop on 32-bit).
- The 9 open reproduction tests remain owned by SOW-0043 and SOW-0044; until those land, `go test ./...` in CI (existing `go-static` job and the new job) stays red by design of the zero-known-bug policy.

## Regression Log

None yet.
