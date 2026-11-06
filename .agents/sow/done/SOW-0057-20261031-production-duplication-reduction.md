# SOW-0057 - Production Duplication Reduction

## Status

Status: completed

Sub-state: delivered; benchmarked with the pre-vendoring run (user ordering).

## Requirements

### Purpose

Continue SOW-0015's file-by-file maintainability work on production sources: reduce real duplication without hiding scanner rules.

### User Request

SOW-0015 (approved 2026-06-05): exclude tests and benchmarks from Codacy metrics, then "continue file-by-file on remaining production files".

### Assistant Understanding

Facts:

- Codacy Cloud on `main` `63919ecb7d` (2026-09-27, public API): 32% complex files, 32% duplicated files, goals 10% / 10% (`fileDuplicationBlockThreshold 1`: a file counts as duplicated with any one duplicated block).
- Local `jscpd --min-tokens 50` over `src/` without tests (2026-10-31, branch tip `dbfa64a`): 4653 of 48542 production lines duplicated (9.6%).
- Largest contributors (duplicated lines per file): Rust `transport/windows.rs` 745 and `transport/posix.rs` 577 (handshake and chunking logic repeated per platform); apps/cgroups lookup codecs in C (`netipc_protocol_apps_lookup.c` 417, `netipc_protocol_cgroups_lookup.c` 339), Rust (`service/apps_lookup.rs` 428, `service/cgroups_lookup.rs` 314, `protocol/lookup/cgroups_lookup.rs` 299) and Go (`protocol/apps_lookup.go` 286, `protocol/cgroups_lookup.go` 214); C Windows transports (`netipc_named_pipe.c` 346, `netipc_win_shm.c` 326).

Inferences:

- The first target is the Rust POSIX/Windows transport pair: Go already shares this logic in `transport/internal/framing`, so a Rust shared module follows an existing project pattern.

Unknowns:

- Whether the user wants this before the next Netdata vendoring (it touches the per-request chunking path, so it needs the benchmark run that precedes vendoring).

### Acceptance Criteria

- Each refactored file pair loses its duplicated blocks with no behavior change (existing suites, interop).
- Codacy duplicated-files percentage drops, measured after push.

## Analysis

Rust transport pair, measured 2026-10-31 at `891faa1` (`jscpd --min-tokens 50` on `transport/posix.rs` 1794 lines and `transport/windows.rs` 2784 lines, tests included): 49 clones, 587 duplicated lines (12.8%). The production clones fall in four groups:

- pure helpers: `header_version_incompatible`, `hello_layout_incompatible`, `hello_ack_layout_incompatible` (posix.rs:119-145 vs windows.rs:223-249), `highest_bit`, `apply_default`;
- config types: `ClientConfig`/`ServerConfig` and their `Default` (posix.rs:160-225 vs windows.rs:264-330). They differ: POSIX `ServerConfig` has `backlog`, and the packet-size doc differs, so they stay per transport;
- per-request paths: `send`/`send_inner` (posix.rs:314-438 vs windows.rs:783-906) and `receive_with_control` chunk reassembly (posix.rs:448-638 vs windows.rs:913-1088);
- handshake: `connect_and_handshake`/`server_handshake` (posix.rs:1434-1790) vs `client_handshake`/`server_handshake` (windows.rs:1457-1800).

Go already shares the last three groups in `src/go/pkg/netipc/transport/internal/framing` (`handshake.go` `ClientHandshake`/`ServerHandshake`/`NegotiateHello`, `send.go` `SessionSend`, `receive.go` `Receiver`), with each transport passing its I/O as functions.

Netdata's Rust code does not use `transport::posix`/`transport::windows` config types directly (`ktsaou/netdata @ ae9113f2cc`, grep outside `src/crates/netipc/`: no hits).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- `transport/posix.rs` and `transport/windows.rs` each carry their own copy of the message framing (send with chunking, receive with reassembly and validation), the HELLO/HELLO_ACK encoding and negotiation, and small helpers. Only the I/O and the error enum differ (Analysis).

Evidence reviewed:

- jscpd measurement and the four clone groups (Analysis); line-by-line diffs of `send`/`send_inner`, `receive_with_control`, the client and server handshakes (2026-10-31).
- Go's shared `transport/internal/framing` (`handshake.go`, `send.go`, `receive.go`).
- C and Go post-HELLO_ACK packet-size checks are per transport (Go `transport/posix/uds_handshake.go:115`, C `netipc_named_pipe.c` client handshake returns `NIPC_NP_ERR_HANDSHAKE` for a too-small size).

Behavior differences found between the two Rust copies, and how the shared code handles them:

- Receive: POSIX rejects `HEADER_SIZE + payload_len > u32::MAX` before chunk validation; Windows does not, so its `total_message_len` comparison can truncate-match. The shared receive keeps the check for both (zero-known-bug policy; Go and C check it).
- Send failure: POSIX fails all in-flight ids on `Send`, Windows on `Send` or `Disconnected`. Kept per transport through the error trait.
- Client HELLO_ACK packet size: POSIX rejects `<= HEADER_SIZE` and `> proposal` as `Protocol`; Windows rejects `<= HEADER_SIZE` as `Handshake`. Kept per transport, after the shared ack parsing, as in Go.
- Client ack wait (POSIX `wait_fd_readable`; Windows tolerates a disconnected pipe that still holds the reply), packet-size derivation, and the POSIX platform profile mask stay per transport; the shared code receives the masked and defaulted values.

Affected contracts and surfaces:

- Internal Rust code organization only. Public types (`UdsSession`, `NpSession`, configs, `Role`, error enums) and their variants stay as they are; no wire change. The Windows receive gains the `u32::MAX` total-length rejection.

Existing patterns to reuse:

- Go `transport/internal/framing`; Rust `transport/mod.rs` already holds a shared helper (`selected_profile_offered`).

Risk and blast radius:

- Every Rust session's send and receive runs through the new code; a mistake breaks all Rust traffic on both platforms. Mitigated by the existing unit, robustness and interop suites on Linux, FreeBSD and Windows, plus the Linux targets.

Performance assessment:

- Changes the established-session per-request path: Rust send/receive on UDS and Named Pipe (`posix.rs:314-638`, `windows.rs:783-1088`). The engine is generic over the I/O closures, so it monomorphizes like today's code; the pre-vendoring benchmark run (user ordering) measures it with the other SOWs.

Sensitive data handling plan:

- Code and tool output only; no hosts or data in artifacts.

Implementation plan:

1. Add `src/crates/netipc/src/transport/framing.rs` (`pub(crate)`, unix and windows): header/layout helpers, `highest_bit`, `apply_default`, a `FramingError` trait implemented by `UdsError` and `NpError`, `send_message()` and `receive_message()` taking the session's role, limits, packet size, in-flight set, buffers and I/O closures, `hello_packet()`, `parse_hello_ack()` and `server_negotiate()` (HELLO parse, negotiation, prepare callback, rejections, HELLO_ACK).
2. Switch `posix.rs` and `windows.rs` to it, deleting their copies.
3. Move unit tests of moved helpers if they referenced the old private items.

Validation plan:

- `cargo test` (Linux, FreeBSD, Windows), clippy gate (Linux and Windows target), fmt on stable and 1.91; ctest with interop on Linux, FreeBSD and Windows; Linux targets (Rust part); jscpd re-measure.

Artifact impact plan:

- `docs/code-organization.md`: add the Rust shared framing module next to Go's, if it lists per-language transport modules. No public docs otherwise (no behavior change visible to users).

Open decisions:

- None (user decisions 1-2 below).

## Implications And Decisions

User decisions (2026-10-31, user):

1. Scope: the Rust transport pair (`transport/posix.rs`, `transport/windows.rs`) only. Each further pair (C/Rust/Go lookup codecs, C Windows transports) gets its own SOW, picked by re-measuring.
2. Depth: full, like Go's `transport/internal/framing`: one shared Rust module holds the helpers, the hello negotiation and both handshakes, and the send/receive chunking engine; each transport supplies its I/O. This touches the per-request path, so the pre-vendoring benchmark run covers it.

User decision (2026-10-31, user): do this after the SOW-0050 Netdata vendoring; benchmark it in the following cycle.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

See the gate.

## Execution Log

### 2026-10-31

- Created from SOW-0015.
- Added `src/crates/netipc/src/transport/framing.rs`: `FramingError` (implemented by `UdsError` and `NpError`), `Framing` (one direction's limits and packet size), `send_message`, `receive_message`, `hello_packet`, `parse_hello_ack`, `ServerOffer` and `server_negotiate`, plus the moved helpers and HELLO size constants.
- `posix.rs` and `windows.rs` now call it; each keeps its I/O, connect/accept, ack wait, post-ack packet-size checks, packet-size derivation, POSIX platform profile mask, and session construction. A `framing(sending)` session method picks the direction's limits.
- Moved `test_highest_bit`, `test_apply_default` and `test_incompatible_classifiers` from the transport tests into `framing.rs`; added `test_receive_rejects_unrepresentable_total_length`.
- The i386 target run exposed a 32-bit bug that the new test reached: `HEADER_SIZE + payload_len as usize` overflows a 32-bit `usize` (debug builds panic on a peer header, release builds wrap past the `u32::MAX` guard). The old POSIX code had the same expression. Fixed with `checked_add`; the chunk bound check became `chunk_data > needed - assembled`. Go (`uint64` sum, `transport/internal/framing/receive.go:138`) and C (`SIZE_MAX` check, `netipc_uds.c:14`) were already correct.

## Validation

Acceptance criteria evidence:

- Duplication: jscpd `--min-tokens 50` on the Rust transport files (tests included) went from 49 clones, 587 duplicated lines (12.8%, `posix.rs` + `windows.rs`, 4578 lines) to 29 clones, 338 duplicated lines (7.9%, `posix.rs` + `windows.rs` + `framing.rs`, 4289 lines). Remaining clones are the per-transport parts the design keeps (config types, session fields and accessors, handshake wrappers, `Hello` literal, session construction) and Windows test fixtures.
- No behavior change except the two recorded fixes (Windows receive gains the `u32::MAX` total check; 32-bit overflow fixed on both transports). Error variants and messages are unchanged.
- Codacy duplicated-files percentage: measured after push (tracked in SOW-0050's post-push Codacy re-check).

Tests and equivalent validation:

- Linux: `cargo test` 389 passed (stable and 1.91.0); CTest 32/32 (`test_shm_full_backing_store` skipped without root); `interop_codec.sh`, `test_uds_interop.sh` 18/18, `test_shm_interop.sh` 9/9, `test_service_interop.sh` 18/18.
- Linux targets (`run-linux-target.sh <target> rust interop`): x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl all pass after the overflow fix (i386-gnu and i386-musl failed before it).
- FreeBSD 14.3: CTest 24/24, `cargo test` 319 passed.
- Windows: `cargo test` 273 passed (1.97.1 and MSRV 1.91.0-msvc), Go ok, CTest 20/20 (both suites).
- Gates: `cargo fmt --check` with stable and 1.91.0; clippy `-D clippy::correctness -D clippy::suspicious` on Linux (stable and 1.91.0) and `--target x86_64-pc-windows-gnu`, no new warnings in the changed files.
- Mutant: disabling the total-length guard fails `test_receive_rejects_unrepresentable_total_length` (it then reserved the ~4 GiB declared payload).

Real-use evidence:

- The interop suites run real Rust clients and servers against C and Go over UDS, SHM and Named Pipes on Linux and Windows.

Performance assessment:

- Per-request path changed: Rust send/receive on UDS and Named Pipe now run in `framing::send_message`/`receive_message`, generic over the I/O closures (monomorphized as before). Benchmarked in the pre-vendoring run together with the other SOWs (user ordering).

Reviewer findings:

- No external review requested for this SOW. The self-review found the 32-bit overflow (via the i386 target) and fixed it.

Same-failure search:

- Searched the Rust crate for `+ <u32> as usize` sums over peer data. The codecs check bounds with `u64` sums before slicing (`protocol/mod.rs:412`, `protocol/cgroups_snapshot.rs:214,225`); the `service/raw/server_session_*.rs` buffer sizes are capped by the server's own config. No other instance.

Sensitive data gate:

- Code, docs and SOW text only; hosts are named by role.

Artifact maintenance gate:

- `AGENTS.md`: no workflow change.
- Runtime project skills: none affected (vendoring skill unchanged; the new file vendors with the crate directory).
- Specs: no protocol, wire or API change; `.agents/sow/specs/` does not describe Rust module layout.
- End-user/operator docs: `docs/code-organization.md` lists `transport/framing.rs`.
- End-user/operator skills: `docs/netipc-integrator-skill.md` covers public APIs only, unchanged.
- SOW lifecycle: completed and moved to `done/`; follow-up SOW-0060 created.

Spec update: none needed (internal reorganization; the two fixes match the documented C and Go behavior).

Project skill update: none needed (no new workflow knowledge beyond the targets run already documented).

Lessons:

- Running a new boundary-value test on 32-bit targets exposed overflow in an expression that 64-bit runs never trip. Unit tests at `u32::MAX` boundaries belong in the Linux-target runs, which already include them.

Follow-up mapping:

- Next duplication pair: SOW-0060 (pending).
- Codacy re-measure after push: SOW-0050 post-push check.
- Benchmark: the pre-vendoring run (SOW-0050 / SOW-0021 ordering).

## Outcome

The Rust UDS and Named Pipe transports share one framing module, like Go's `transport/internal/framing`; duplicated lines in the pair fell from 587 to 338. Windows receive now rejects unrepresentable total lengths, and a 32-bit overflow in the receive path is fixed on both transports.

## Lessons Extracted

See Validation, Lessons.

## Followup

- SOW-0060: next duplication pair.

## Regression Log

None yet.
