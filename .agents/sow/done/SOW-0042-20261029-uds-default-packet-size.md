# SOW-0042 - UDS Default Packet Size

## Status

Status: completed

Sub-state: packet size capped at the kernel datagram limit in C, Rust and Go; client validates the agreed size; specs updated.

## Requirements

### Purpose

Messages larger than one packet must be deliverable over UDS with the default (auto-detected) packet size on Linux.

### User Request

From SOW-0040 decisions (2026-10-29): split the fix work into separate SOWs (N1 Rust timeout; N11 default packet size; handshake/timeout bounds and shutdown; dead-peer detection) and track the Windows pipe DACL exposure as its own security SOW.

### Assistant Understanding

Facts:

- C (`src/libnetdata/netipc/src/transport/posix/netipc_uds.c:24-34`), Rust (`src/crates/netipc/src/transport/posix.rs:792-811`) and Go (`src/go/pkg/netipc/transport/posix/uds.go:105-112`) set the packet size to `SO_SNDBUF`.
- On Linux a SOCK_SEQPACKET datagram of exactly `SO_SNDBUF` bytes fails with EMSGSIZE (socketpair check: 212992 fails, 211968 succeeds), so every chunked message fails with the default configuration.
- Reproduced (SOW-0040) in C, Rust and Go with a 512 KiB message; the same message with an explicit 64 KiB packet size passes.
- Spec: `docs/level1-posix-uds.md:73-86` defines `agreed_packet_size` chunking but not how the local packet size is derived.
- Analysis 2026-10-29 (measured, Linux 7.0 SEQPACKET socketpair): the largest sendable datagram is `getsockopt(SO_SNDBUF) - 32` (default 212992 -> 212960 works, 212992 fails); with a 1 MiB buffer sends fail with ENOBUFS above about 1.07 MiB. The peer's `SO_RCVBUF` does not matter.
- Sibling bug: clients accept any agreed packet size from HELLO_ACK, even above their own proposal (C `netipc_uds_handshake.c:102`, Go `uds_handshake.go:75`, Rust `posix.rs:1225` checks only the lower bound).
- Sibling bug: an explicitly configured `packet_size` is not capped by the kernel limit, so values above `SO_SNDBUF - 32` fail the same way.

Inferences:

- Downstream Netdata consumers are likely affected; not yet verified against Netdata usage.

Unknowns:

- Exact kernel source rule for the 32-byte overhead (measured, not read from kernel source); macOS/FreeBSD datagram limits (not measurable here, and the Go transport does not build there - SOW-0046).

### Acceptance Criteria

- `test_robust_default_packet_size_carries_large_message` (Rust), `TestRobustDefaultPacketSizeCarriesLargeMessage` (Go) and the C `test_robustness` N11 check pass.
- Packet-size derivation documented in `docs/level1-posix-uds.md`; interop across languages unchanged or migrated consistently.
- Fix design decided by the user: option 3A keeps single-packet behavior for messages up to the Linux default limit, so no throughput change and no benchmark gate.
- Configured sizes above the kernel limit are clamped; clients reject an agreed size above their proposal.

## Analysis

Sources checked:

- SOW-0040 evidence and tests; analysis-round measurements (SEQPACKET socketpair, several `SO_SNDBUF` values).
- C `netipc_uds.c`, `netipc_uds_internal.h`, `netipc_uds_handshake.c`, `netipc_uds.h`; Rust `transport/posix.rs`; Go `transport/posix/uds.go`, `uds_handshake.go`, `uds_session.go`, `transport/internal/framing/handshake.go`.
- Specs `docs/level1-posix-uds.md` (Packet size, Chunking), `docs/level1-transport.md` (packet size negotiation), `docs/netipc-integrator-skill.md`.
- Benchmarks: `bench/drivers/c/bench_posix.c:427` (largest POSIX benchmark message 52096 bytes) - no benchmark chunks today.

Current state (before the fix):

- Proposal = full `SO_SNDBUF`; any chunked message failed with EMSGSIZE; explicit sizes unchecked; Go clients did not even check the lower bound of the agreed size.

Risks:

- Relies on the measured Linux `SO_SNDBUF - 32` rule; the always-running N11 reproduction test and the new kernel-limit test guard it.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Each side proposed its full `SO_SNDBUF` as packet size; Linux accepts at most `SO_SNDBUF - 32` bytes per datagram, so the first full-size chunk of every chunked message failed. Explicit sizes and peer-announced sizes were never checked against the kernel limit.

Evidence reviewed:

- Code, specs and measurements listed under Analysis.

Affected contracts and surfaces:

- POSIX UDS handshake proposal (C, Rust, Go) and client-side HELLO_ACK validation; public config field comments (`netipc_uds.h`, Rust `ClientConfig`/`ServerConfig`, Go `ClientConfig`/`ServerConfig`). No wire-format change; mixed versions stay compatible because the server picks the minimum.
- Windows named pipes are not touched: they default to 64 KiB packets (`netipc_named_pipe.h:96`) and message-mode pipes have no per-datagram kernel limit comparable to SEQPACKET (reasoned, not measured).

Existing patterns to reuse:

- Existing detection helpers and their 65536 fallback; existing client lower-bound check in C and Rust; existing malformed-HELLO_ACK test harnesses in all three languages; `large_message_round_trip` in `test_robustness.c`.

Risk and blast radius:

- Default systems: packet size drops by 32 bytes (212992 -> 212960); messages up to that size still travel in one packet. Systems with raised buffers: capped at 256 KiB instead of failing near 1 MiB.
- A client now refuses a server that announces a larger size than proposed; compliant servers never do (they take the minimum).

Sensitive data handling plan:

- No secrets or customer data involved; evidence is code paths, sizes and test output.

Implementation plan:

1. Replace the SO_SNDBUF detector with a limit helper in each language: `min(SO_SNDBUF - 32, 262144)`, fallback 65536, configured value used only when non-zero and below the limit.
2. Use the helper for client and server proposals.
3. Clients reject `agreed_packet_size` above their proposal or not above 32 (Go gains both checks).
4. Update config comments and specs.
5. Tests: kernel-limit test (exact limit sends, +1 fails while kernel-limited) in Rust and Go; clamp tests; agreed-size rejection in C, Rust and Go; 512 KiB round trip with an oversized 4 MiB configured size in C.

Validation plan:

- New tests fail without the fix (C) and pass with it; N11 reproduction tests pass in C, Rust and Go; full CTest, Rust and Go suites show no failures other than reproduction tests owned by open SOWs; UDS and service interop pass; fmt, clippy and vet clean.

Artifact impact plan:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: no update.
- Specs: update `docs/level1-posix-uds.md` and `docs/level1-transport.md`.
- End-user/operator docs: config field comments in public headers/types updated; no other doc describes packet size derivation.
- End-user/operator skills: `docs/netipc-integrator-skill.md` states raw packet size is not exposed; no update.
- SOW lifecycle: completes; SOW-0046 widened with the Go cross-platform build failures found during validation.

Open-source reference evidence:

- None.

Open decisions:

- None; decision recorded below.

## Implications And Decisions

1. 2026-10-29, Issue 3: user chose option 3A - propose `min(SO_SNDBUF - 32, 256 KiB)`, clamp configured sizes to the same limit, and make clients reject an agreed size larger than their proposal. Rejected 3B (fixed 64 KiB: chunks 64-208 KiB messages and needs benchmark evidence), 3C (raise `SO_SNDBUF`: does not fix the off-by-32, limited by `wmem_max`), 3D (re-split on EMSGSIZE: breaks receivers that expect exact chunk sizes).

## Plan

Executed as the implementation plan in the Pre-Implementation Gate.

## Execution Log

### 2026-10-29

- C: `nipc_uds_packet_size()` replaces `nipc_uds_detect_packet_size()`; constants in `netipc_uds_internal.h`; client agreed-size check in `netipc_uds_handshake.c`.
- Rust: `packet_size_limit()` replaces `detect_packet_size()`; `PACKET_SIZE_MAX`, `SNDBUF_OVERHEAD`; client check in `connect_and_handshake`.
- Go: `packetSizeLimit()` replaces `detectPacketSize()`; client range check in `connectAndHandshake`.
- Tests added in `transport/posix_tests.rs`, `transport/posix/uds_more_edge_test.go`, `tests/fixtures/c/test_uds.c`, `tests/fixtures/c/test_robustness.c`; the old detector tests were replaced.
- Cross-building Go during validation exposed that the Go POSIX transport fails to build on linux/386, arm, arm64, riscv64, freebsd and darwin; recorded in SOW-0046.

## Validation

Acceptance criteria evidence:

- C `test_robustness` N11: "512 KiB message delivered with auto-detected packet size" PASS; new "oversized 4 MiB packet size (clamped)" PASS.
- Rust `test_robust_default_packet_size_carries_large_message` ok; Go `TestRobustDefaultPacketSizeCarriesLargeMessage` passes (absent from the failure list).

Tests or equivalent validation:

- Library fix reverted (reverse patch, then reapplied): C "HELLO_ACK packet size above the client proposal rejected" FAIL, auto-detected 512 KiB FAIL, oversized 4 MiB FAIL. With the fix all pass. (Rust and Go new tests call the new helpers and cannot compile against the old code; their N11 reproduction tests failed before this SOW.)
- Rust `test_packet_size_limit_matches_kernel_datagram_limit`: for default, 4096 and 65536 `SO_SNDBUF`, the limit sends and limit + 1 fails. Go `TestPacketSizeLimitMatchesKernelDatagramLimit`: same.
- Full CTest: 48/51 pass; failures are `test_robustness`, `test_service_rust`, `test_shm_rust`, all from reproduction tests owned by open SOWs (`test_shm_rust` runs `cargo test shm`, which matches the ungated K1/N5 SHM reproductions and exceeds its 30 s CTest timeout).
- Rust `cargo test --lib`: 388 passed, 9 failed (K1 x2, N5, K3 x2, K4 x2, N2, N3 - open SOWs).
- Go `go test ./...`: all packages ok except `service/raw` with 9 failures (K1 x2, N5, K3 x2, K4 x2, N2, N3 - open SOWs).
- UDS and service interop (`test_uds_interop`, `test_service_interop`, `test_service_shm_interop`, codec/cache interop) pass in the CTest run.
- `cargo fmt --check` clean; CI clippy gates pass; `gofmt -l` clean; `GOOS=windows go vet ./...` reports only pre-existing `unsafe.Pointer` warnings in untouched `transport/windows/shm.go`.

Real-use evidence:

- Interop binaries (real client/server processes, C/Rust/Go pairs) pass over UDS with the new proposals.

Reviewer findings:

- None requested.

Same-failure search:

- All three languages derived the proposal the same way and all three are fixed. Windows pipes use a fixed 64 KiB default and are not affected by the SEQPACKET limit.

Artifact maintenance gate:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: no update; vendoring workflow unaffected.
- Specs: updated `docs/level1-posix-uds.md` (Packet size) and `docs/level1-transport.md` (packet size negotiation).
- End-user/operator docs: public config comments updated in `netipc_uds.h`, Rust `transport/posix.rs`, Go `uds_session.go`; no other doc describes packet-size derivation.
- End-user/operator skills: no update; `docs/netipc-integrator-skill.md` states raw packet size is not exposed.
- SOW lifecycle: completed and moved to `done/` with the work.

Spec update:

- Done (see artifact gate).

Project skill update:

- None needed (see artifact gate).

End-user/operator docs update:

- Config comments only (see artifact gate).

End-user/operator skill update:

- None needed (see artifact gate).

Lessons:

- Cross-compiling every claimed platform is cheap and found a whole class of build failures nobody had seen; it belongs in CI (SOW-0046).

## Outcome

POSIX UDS messages of any size now work with the default configuration in C, Rust and Go. Proposals never exceed what the kernel accepts, configured sizes are clamped, and clients refuse an agreed size larger than they proposed.

## Lessons Extracted

- See Validation -> Lessons.

## Followup

- Go POSIX transport build failures on linux/386, arm, arm64, riscv64, freebsd, darwin: SOW-0046.
- macOS/FreeBSD datagram limits unverified: SOW-0046 (cannot be checked until those builds work).

## Regression Log

None yet.
