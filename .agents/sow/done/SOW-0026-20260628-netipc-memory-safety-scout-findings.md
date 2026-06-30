# SOW-0026 - NetIPC Memory-Safety Scout Findings

## Status

Status: completed

Sub-state: source fixes implemented and validated; Netdata vendor propagation is tracked separately in SOW-0027.

## Requirements

### Purpose

NetIPC is the source of truth for Netdata IPC libraries. Memory-safety, concurrency, liveness, and wire-validation issues found in the vendored Netdata copy must be verified and fixed in `plugin-ipc` first, then propagated to Netdata through the normal vendor/update path.

### User Request

The user stated that any issue related to NetIPC libraries must be added as a new SOW in `plugin-ipc.git` and must not be fixed directly in the Netdata PR because the source of truth is this repository.

### Assistant Understanding

Facts:

- The Netdata memory-corruption scout catalog includes 19 findings whose IDs, titles, or locations point to NetIPC library code.
- The Netdata PR checkout used as scout source is `netdata/netdata` worktree commit `1c65d2c7146db7dd3519904cadc5d6c258251707`.
- This `plugin-ipc` checkout has advanced past the originally recorded `netdata/plugin-ipc` commit `96f5f2962188c2198e621fec5da8a4c90710b46a`; verification is against the current local source after SOW-0025.
- Some NetIPC findings were mis-bucketed by the scout under `src/libnetdata/os`, but their locations point into `src/libnetdata/netipc/...` and therefore belong here.
- The current `plugin-ipc` worktree already has an unrelated local change: `bench/drivers/go/go` modified. This SOW must not disturb it.

Inferences:

- The Netdata PR should mark these findings as delegated to this SOW instead of fixing vendored NetIPC code directly.
- Each finding below was verified in `plugin-ipc` before implementation; scout output was treated as a hypothesis, not proof.

Unknowns:

- The exact Netdata checkout and PR target for vendor propagation remain outside this SOW and are tracked in SOW-0027.

### Acceptance Criteria

- Every finding listed below is verified against `plugin-ipc` source and classified as current bug, potential bug, false positive, already fixed, or decision-required.
- Deterministic source fixes are implemented in `plugin-ipc`, with C/Rust/Go parity where the contract spans languages.
- Findings that require API, protocol, threading, timeout, or compatibility decisions are presented to the user with concrete options before implementation.
- Tests or equivalent validation cover each accepted fix at the correct layer: C library, Rust crate, Go package, POSIX transport, Windows transport, protocol codec, or service API.
- Netdata is not patched directly for these findings; Netdata receives fixes only through the normal plugin-ipc vendor/update flow.
- All false positives and already-fixed findings are recorded with source evidence.

## Analysis

Sources checked:

- Netdata scout catalog: `.local/full-code-review/all-findings.json` from `netdata/netdata` PR commit `1c65d2c7146db7dd3519904cadc5d6c258251707`.
- `plugin-ipc` repository structure and current SOW queue.
- `plugin-ipc` `AGENTS.md` and SOW template.

Current state:

- This SOW is completed in `plugin-ipc`.
- The implementation decisions listed in `## Implications And Decisions` were applied.
- Netdata vendor propagation is tracked by `.agents/sow/pending/SOW-0027-20260629-netdata-vendor-memory-safety-update.md`.

Risks:

- Fixing only the C vendored copy in Netdata would create source-of-truth drift and likely be overwritten by the next vendor update.
- Some issues are cross-language protocol/service contracts; fixing only C may leave Rust and Go behavior inconsistent.
- Concurrency/liveness findings may require design choices rather than surgical edits.
- Windows transport findings require Windows validation before closure.

## Pre-Implementation Gate

Status: implemented and validated; all implementation decisions were resolved before source changes.

Problem / root-cause model:

- NetIPC-related memory-safety scout findings were discovered during a Netdata PR sweep, but NetIPC source ownership is `plugin-ipc`.
- Fixing these in Netdata directly would patch a consumer/vendor copy rather than the source of truth.

Evidence reviewed:

- Netdata scout finding IDs and locations listed below.
- `plugin-ipc` has matching C NetIPC source under `src/libnetdata/netipc/`, plus Rust and Go implementations under `src/crates/netipc/` and `src/go/pkg/netipc/`.

Affected contracts and surfaces:

- C NetIPC protocol, service, POSIX transport, Windows transport, UDS, named-pipe, and SHM code.
- Rust and Go NetIPC protocol/service/transport implementations where behavior is shared.
- Public docs under `docs/` if protocol, timeout, ownership, or threading contracts change.
- Netdata vendor/update flow after plugin-ipc fixes land.

Existing patterns to reuse:

- Existing NetIPC codec/service tests in C, Rust, and Go.
- Existing POSIX and Windows transport tests.
- Existing SOW-0021 scale/contract discipline for cross-language parity.

Risk and blast radius:

- High for transport/service concurrency changes.
- Medium for codec validation changes if they alter public decode/accessor contracts.
- Platform-specific risk on Windows for SHM/named-pipe fixes.
- Performance-sensitive risk for hot transport receive/send paths and lookup accessors.

Sensitive data handling plan:

- No raw secrets, customer data, credentials, or production logs are needed.
- Evidence will use source paths, line references, scout IDs, and sanitized summaries only.

Implementation plan:

1. Verify each finding below against current `plugin-ipc` source and group duplicates or shared root causes.
2. Split into smaller implementation SOWs or ordered phases if needed; do not batch unrelated fixes into one commit.
3. For deterministic fixes, implement surgical source changes with targeted tests.
4. For design decisions, pause with numbered options and concrete file/line evidence.
5. Track Netdata vendor propagation separately rather than manually editing vendored NetIPC code in this SOW.

Validation plan:

- Per finding: source trace, same-failure search, targeted tests, and reviewer pass.
- Cross-language behavior: C/Rust/Go tests where protocol or public service contracts change.
- Platform behavior: POSIX tests for UDS/SHM; Windows tests for named-pipe/Windows SHM/server lifecycle findings.
- Vendor propagation: tracked by SOW-0027, with Netdata build/tests appropriate to affected integrations.

Artifact impact plan:

- AGENTS.md: no expected update unless source-ownership routing needs durable project policy here too.
- Runtime project skills: no expected update unless NetIPC audit workflow needs durable skill guidance.
- Specs: update docs/specs if protocol, timeout, atomic, or threading contracts change.
- End-user/operator docs: likely unaffected unless public integration behavior changes.
- End-user/operator skills: likely unaffected.
- SOW lifecycle: this SOW closes the source fixes; SOW-0027 tracks Netdata vendor propagation.

Open-source reference evidence:

- Primary source is `netdata/plugin-ipc @ 96f5f2962188c2198e621fec5da8a4c90710b46a` and `netdata/netdata @ 1c65d2c7146db7dd3519904cadc5d6c258251707` scout evidence.

Open decisions:

- None. The user selected options 1B, 2A, 3B, 4B, and 5A before implementation.

## Implications And Decisions

1. NetIPC source ownership decision:
   - Selected: track and fix NetIPC findings in `plugin-ipc`, not in the Netdata PR.
   - Reason: `plugin-ipc` is the source of truth; direct Netdata edits would create vendor drift.

2. Managed server lifecycle:
   - Selected: Option B, long-term-best.
   - Decision: make `drain()`/`destroy()` internally wait until the accept loop
     is inactive before freeing server session tracking.

3. Raw L1 same-session thread safety:
   - Selected: Option A, surgical.
   - Decision: raw L1 same-session send/receive remains externally synchronized;
     ordinary integrations should use L2/L3 rather than sharing one raw L1
     session across threads.

4. POSIX abort vs close:
   - Selected: Option B, long-term-best.
   - Decision: add internal synchronization around POSIX abort-pipe lifetime.

5. Raw SHM infinite waits:
   - Selected: Option B, long-term-best.
   - Decision: add liveness/backoff behavior where possible, especially for
     Windows BUSYWAIT CPU safety.

6. UDS stale socket recovery:
   - Selected: Option A, surgical.
   - Decision: accept and document the low same-service concurrent-startup race
     for this SOW; do not redesign UDS stale recovery unless concurrent startup
     of the same service becomes a supported deployment goal.

## Verification - 2026-06-29

### L3 Cache Contract Re-check

The user challenged the decision framing and asked whether the prior L3 cache
design already separated cache maintenance from cache use.

Evidence:

- SOW-0025 selected the long-term-best guarded L3 snapshot-cache API:
  read guards for borrowed access, `get/use` for borrowed immutable views,
  `dup/copy` for owned items, and `free` only for owned items.
- `docs/level3-snapshot-api.md` states that refresh may build a replacement
  snapshot while readers hold guards, but cannot publish and retire the guarded
  snapshot until readers release their guards.
- `docs/netipc-integrator-skill.md` states that L3 cgroups snapshot caches are
  internally synchronized and support concurrent refresh/read access through
  the explicit read-guard API.
- C implementation evidence: `nipc_cgroups_cache_refresh()` serializes writers
  with `writer_lock`, builds a new snapshot privately, swaps under
  `cache_lock`, and frees the old snapshot only after the write lock is
  acquired; read access goes through `nipc_cgroups_cache_read_lock()` /
  `nipc_cgroups_cache_get()` / `nipc_cgroups_cache_read_unlock()`.
- Rust and Go implementations use the same model: a writer mutex plus snapshot
  read/write lock, with guarded borrowed access.

Conclusion:

- The L3 cache contract is not an open SOW-0026 decision.
- Cache maintenance is caller-driven, has no hidden maintenance thread, and is
  internally protected so multiple callers do not refresh/publish concurrently
  on the same cache object.
- Cache use is safe for multiple reader threads only through the read-guard API.
- Borrowed views remain unsafe after guard unlock and must be duplicated if they
  need to survive unlock or cross thread ownership boundaries.
- The remaining thread-safety decision below is about raw L1 transport sessions
  and must not be described as an L3 cache decision.

### Classification Summary

| Finding | Classification | Evidence | Decision |
|---|---|---|---|
| `MEM-netipc-protocol-001` | Current C defensive-public-API bug | C snapshot/lookup/apps public accessors read directory entries or item headers before re-checking `view->_payload_len`: `netipc_protocol_cgroups_snapshot.c:102`, `netipc_protocol_cgroups_lookup.c:155`, `netipc_protocol_apps_lookup.c:222`. Go accessors use checked indexing (`src/go/pkg/netipc/protocol/cgroups_snapshot.go:204`), and Rust lookup views keep payload private plus checked subslices (`src/crates/netipc/src/protocol/lookup/common.rs:145`). | Deterministic fix. Harden C accessors; no Rust/Go source change expected unless parity tests expose a gap. |
| `MEM-netipc-service-002` | Current POSIX lifecycle bug / decision-required | Public docs show `nipc_server_run()` blocking and `nipc_server_drain()` callable from another thread (`docs/getting-started.md:223`). POSIX `drain()` stops running and then frees `server->sessions` and destroys `sessions_lock` (`netipc_service_posix_server.c:273`, `netipc_service_posix_server.c:342`) without accept-loop active tracking; Windows has `accept_loop_active` (`netipc_service_win_server.c:219`). | Decision-required. Need choose public lifecycle semantics and POSIX implementation shape. |
| `MEM-netipc-transport-002` | Contract gap / decision-required | L1 transport spec allows pipelining on one session (`docs/level1-transport.md:75`). C UDS/named-pipe session state has in-flight arrays and receive buffers with no synchronization (`netipc_uds_inflight.c:5`, `netipc_uds_receive.c:88`, `netipc_named_pipe.c:485`, `netipc_named_pipe.c:1135`). Docs do not promise same-session thread safety. | Decision-required. Need choose internal locking vs documented single-owner session contract. |
| `MEM-netipc-service-001` | Current Windows memory bug | `nipc_session_ctx_t.active` is `bool` (`netipc_service.h:328`) but Windows writes/reads it through `Interlocked*` as `LONG *` (`netipc_service_win_server.c:149`, `netipc_service_win_server.c:301`, `netipc_service_win_server_session.c:227`). | Deterministic fix. Use a Windows-sized active field/helper. |
| `MEM-netipc-service-003` | False positive for L2 typed calls | Platform call wrappers pass `timeout_ms` through `nipc_service_common_client_call_timeout_ms()` (`netipc_service_posix_client_call.c:142`, `netipc_service_win_client_call.c:133`), and that maps zero to the context/default timeout (`netipc_service_common.c:185`). Public docs state typed timeout zero means default 30000 ms (`docs/getting-started.md:161`). | No source fix for this finding. Raw L1 infinite waits are covered by `MEM-netipc-transport-004` and `MEM-netipc-transport-005`. |
| `MEM-netipc-service-005` | Potential bug / decision-required | Public API allows `nipc_client_abort()` from another thread (`netipc_service.h:211`). POSIX abort writes `abort_pipe[1]` without synchronization (`netipc_service_posix_client.c:163`) while close tears down both pipe fds unsynchronized (`netipc_service_posix_client.c:38`, `netipc_service_posix_client.c:187`). Integrator guidance says not to call `close()` concurrently with typed methods, but the header does not state abort-vs-close ordering. | Decision-required. Need choose internal abort-pipe lifetime lock vs explicit no-concurrent-close contract. |
| `MEM-netipc-transport-001` | Current Windows portability bug | C Windows SHM load helpers use volatile load plus compiler barrier for all Windows builds (`netipc_win_shm.c:147`) while comments justify only x86-64 TSO. Rust/Go use language atomics for the same fields. | Deterministic fix. Keep x86 fast path if desired, add a correct non-x86 fallback. |
| `MEM-netipc-transport-004` | Raw L1 contract decision | POSIX SHM receive documents `timeout_ms == 0` as infinite (`netipc_shm.h:188`) and implements no deadline (`netipc_shm.c:765`). L2 service code never passes zero to SHM receive; it polls with bounded slices (`netipc_service_posix_client_call.c:87`). | Decision-required. Decide whether raw L1 infinite wait remains valid or should gain liveness escape / peer-dead checks. |
| `MEM-netipc-transport-005` | Raw L1 contract decision / CPU-risk | Windows SHM BUSYWAIT loops without a deadline when `timeout_ms == 0` (`netipc_win_shm.c:867`). L2 service code uses bounded waits (`netipc_service_win_client_call.c:79`). | Decision-required. Decide whether raw busy-wait may be infinite or must require finite timeout/yield/backoff. |
| `MEM-os-windows-010` | Current Windows SHM validation bug, cross-language concern | C client attach validates some offsets/capacities but does not fully cross-check `resp_off + resp_cap` and mapped-size assumptions (`netipc_win_shm.c:548`). Go computes `regionSize := uintptr(respOff + respCap)` without equivalent validation (`src/go/pkg/netipc/transport/windows/shm.go:405`). Rust validates more than Go but also computes region size after header-derived values (`src/crates/netipc/src/transport/win_shm.rs:590`). | Deterministic fix. Harden C and audit/fix Go/Rust attach validation for parity. |
| `MEM-os-windows-011` | Current Windows SHM consistency hardening gap | Receive copies after observing sequence advance and reads length once, but does not verify sequence/length stability after copying (`netipc_win_shm.c:792`, `netipc_win_shm.c:862`). The SHM spec says one in-flight message per direction (`docs/level1-posix-shm.md:181`), but corrupted peers can violate it. Go/Rust use the same pattern. | Deterministic hardening candidate across C/Rust/Go unless rejected as unnecessary under trusted-peer contract. |
| `MEM-os-windows-013` | Current Windows lifecycle bug / overlaps POSIX lifecycle decision | Windows `drain()`/`destroy()` wake the accept loop but immediately proceed to join/free sessions and delete `sessions_lock` (`netipc_service_win_server.c:349`, `netipc_service_win_server.c:412`) while `nipc_server_run()` can later lock the same object (`netipc_service_win_server.c:253`). | Decision-required. Same lifecycle decision as `MEM-netipc-service-002`. |
| `MEM-netipc-transport-007` | Real low TOCTOU / decision-required | POSIX UDS probes an existing path with `connect()` and then unlinks the endpoint name on probe failure (`netipc_uds_lifecycle.c:110`, `netipc_uds_lifecycle.c:137`) before the later `bind()` (`netipc_uds_lifecycle.c:181`). | Decision-required. Need choose acceptable stale recovery risk vs lockfile/bind-retry redesign. |
| `MEM-netipc-protocol-002` | Current protocol parity bug | Docs say STRING_REVERSE `str_offset` is always 8 (`netipc_protocol.h:688`). C only checks bounds (`netipc_protocol_string_reverse.c:39`), Go also only checks bounds (`src/go/pkg/netipc/protocol/string_reverse.go:56`), and Rust rejects offsets below 8 but not offsets above 8 (`src/crates/netipc/src/protocol/string_reverse.rs:46`). | Deterministic cross-language fix. Enforce exact offset in C/Rust/Go. |
| `MEM-netipc-protocol-003` | Current C uninitialized-padding bug | `nipc_hello_t` wire size is 44 but `sizeof` is 48 (`netipc_protocol.h:271`); C decode copies only 44 bytes into caller output without clearing trailing padding (`netipc_protocol.c:323`). Rust/Go structs do not expose this C padding issue. | Deterministic C fix. Zero output before copy. |
| `MEM-netipc-service-004` | Duplicate of `MEM-netipc-protocol-001` | Same C snapshot accessor path (`netipc_protocol_cgroups_snapshot.c:102`). | Fold into `MEM-netipc-protocol-001`. |
| `MEM-netipc-transport-003` | Current integer-overflow bug | C UDS and named-pipe in-flight growth use `capacity * 2` in `uint32_t` (`netipc_uds_inflight.c:12`, `netipc_named_pipe.c:490`). | Deterministic C fix. Add overflow/size checks. |
| `MEM-netipc-transport-006` | Current integer-overflow bug | C UDS and named-pipe batch validators compute `item_count * 8` in `uint32_t` before validation (`netipc_uds_receive.c:103`, `netipc_named_pipe.c:1146`). Go already uses `uint64` for this path (`src/go/pkg/netipc/transport/internal/framing/receive.go:184`). | Deterministic C fix. Use checked `size_t`/`uint64_t` arithmetic before calling codec validator. |
| `MEM-os-windows-014` | False positive | `highest_bit()` intentionally returns the highest selected profile bit from a bitmask; POSIX C has the same helper (`netipc_uds_handshake.c:11`), Go negotiates the same way (`src/go/pkg/netipc/transport/internal/framing/handshake.go:199`), and Rust tests explicitly cover multi-bit masks (`src/crates/netipc/src/transport/windows.rs:1797`). | No fix. Record false positive. |

### Decision Queue

1. Managed server lifecycle, POSIX and Windows.
   - Background: public docs imply `drain()` can be called while `run()` is active, but both platform implementations can destroy session tracking before the accept loop is known to have exited.
   - Option A, surgical: document that callers must stop and join `nipc_server_run()` before `drain()`/`destroy()`.
     - Pros: smallest code change.
     - Cons: contradicts current docs/example and weakens shutdown ergonomics.
     - Risk: maintainers and integrators may keep using the documented pattern and retain the race.
   - Option B, long-term-best: make `drain()`/`destroy()` internally wait until the accept loop is inactive before freeing session tracking.
     - Pros: matches public docs and Windows already has part of the state model.
     - Cons: needs careful wake/wait behavior and tests on POSIX and Windows.
     - Risk: shutdown timing changes; must avoid deadlock if the caller invokes `drain()` from inside the accept loop.
   - Recommendation: B, long-term-best.

2. Raw L1 same-session thread safety. This is not an L3 cache decision.
   - Background: L1 is the low-level transport connection layer under L2/L3. It supports pipelining, meaning more than one message can be in flight on one connection. The C UDS/named-pipe session state is mutable and unlocked.
   - Cache clarification: SOW-0025 already made L3 cgroups snapshot caches internally synchronized for refresh/read through the read-guard API. That L3 decision remains valid and is not reopened here.
   - Option A, surgical: document raw L1 same-session send/receive as externally synchronized; keep raw L1 session objects single-owner.
   - Option B, long-term-best: add internal synchronization for raw L1 in-flight tracking and receive buffer ownership in C UDS/named-pipe.
   - Recommendation: A for SOW-0026 unless a product requirement says raw L1 sessions must be shared directly by multiple threads; L2/L3 remain the recommended public surfaces for ordinary integrations.

3. POSIX abort vs close.
   - Background: abort is explicitly cross-thread, but close is not documented as cross-thread-safe. The current pipe lifetime is unprotected.
   - Option A, surgical: document that `close()` must not race with `abort()`; caller must abort, join call owner, then close.
   - Option B, long-term-best: add internal synchronization around POSIX abort pipe lifetime.
   - Recommendation: B, long-term-best, because the API already exposes abort as a shutdown-thread primitive.

4. Raw SHM infinite waits.
   - Background: L2 typed calls are bounded; raw L1 SHM receive with timeout zero is infinite by contract. POSIX can block in futex, Windows BUSYWAIT can spin.
   - Option A, surgical: keep raw L1 timeout zero as infinite and document that BUSYWAIT requires a finite timeout in production callers.
   - Option B, long-term-best: make raw SHM infinite waits still perform liveness checks/backoff and return peer-dead/disconnected where possible.
   - Recommendation: B for Windows BUSYWAIT CPU safety; POSIX may need a narrower design because server-side peer-death detection is asymmetric.

5. UDS stale socket recovery.
   - Background: current probe-then-unlink can remove an endpoint that becomes live between failed probe and unlink.
   - Option A, surgical: accept the low TOCTOU risk and document the startup race.
   - Option B, long-term-best: redesign stale recovery around a lockfile or bind-retry flow, with tests for concurrent startup.
   - Recommendation: A for this SOW unless concurrent same-service startup is a supported deployment goal.

## Findings To Verify

| Finding | Severity | Confidence | Class | Source locations from scout | Scout title |
|---|---:|---:|---|---|---|
| `MEM-netipc-protocol-001` | High | Potential | oob-read | src/libnetdata/netipc/src/protocol/netipc_protocol_cgroups_snapshot.c:103-128, src/libnetdata/netipc/src/protocol/netipc_protocol_cgroups_lookup.c:156-179, src/libnetdata/netipc/src/protocol/netipc_protocol_cgroups_lookup.c:288-309, src/libnetdata/netipc/src/protocol/netipc_protocol_apps_lookup.c:222-244, src/libnetdata/netipc/src/protocol/netipc_protocol_apps_lookup.c:374-394 | Typed item accessors dereference wire-derived directory offsets without re-validating bounds (OOB read / TOCTOU vs. the validating raw_item siblings) |
| `MEM-netipc-service-002` | High | Potential | lifetime-ownership | src/libnetdata/netipc/src/service/netipc_service_posix_server.c:279-345, src/libnetdata/netipc/src/service/netipc_service_posix_server.c:349-374, src/libnetdata/netipc/src/service/netipc_service_posix_server.c:108-258, src/libnetdata/netipc/include/netipc/netipc_service.h:371-372 | Managed server drain/destroy free the sessions array and destroy sessions_lock without joining or tracking the acceptor thread that still uses them |
| `MEM-netipc-transport-002` | High | Potential | data-race | src/libnetdata/netipc/src/transport/posix/netipc_uds_send.c:26-75, src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c:88-102, 140-170, src/libnetdata/netipc/src/transport/posix/netipc_uds_inflight.c:1-44 | UDS/named-pipe session shared state (inflight_ids, recv_buf) is touched by both send and receive with no synchronization |
| `MEM-netipc-service-001` | Medium | Potential | type-confusion | src/libnetdata/netipc/src/service/netipc_service_win_server.c:301, src/libnetdata/netipc/src/service/netipc_service_win_server.c:149, src/libnetdata/netipc/src/service/netipc_service_win_server.c:368, src/libnetdata/netipc/src/service/netipc_service_win_server_session.c:227, src/libnetdata/netipc/src/service/netipc_service_win_server_session.c:241, src/libnetdata/netipc/include/netipc/netipc_service.h:327-328 | Windows: per-session 'active' is a 1-byte bool but is read/written with 32-bit Interlocked ops (LONG), a type-confusion that touches 3 bytes past the field |
| `MEM-netipc-service-003` | Medium | Potential | livelock | src/libnetdata/netipc/src/service/netipc_service_win_client_call.c:65-95, src/libnetdata/netipc/src/service/netipc_service_posix_client_call.c:73-101 | Client SHM receive loop has no termination when the resolved per-call timeout is 0 (deadline guard is skipped), a latent livelock that matches the reported non-reproducible Windows spin |
| `MEM-netipc-service-005` | Medium | Potential | toctou | src/libnetdata/netipc/src/service/netipc_service_posix_client.c:160-177, src/libnetdata/netipc/src/service/netipc_service_posix_client.c:40-52 | POSIX client abort writes to abort_pipe[1] from a cross-thread abort call with no synchronization against close() tearing the same fd down (fd use-after-close / write to wrong fd) |
| `MEM-netipc-transport-001` | Medium | Potential | memory-ordering | src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:147-160, src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:785-897 | Windows SHM atomic-load helpers use a plain load unconditionally (acquire only valid under x86-TSO) |
| `MEM-netipc-transport-004` | Medium | Potential | livelock | src/libnetdata/netipc/src/transport/posix/netipc_shm.c:774-805 | POSIX SHM receive blocks forever on a dead peer when called with timeout_ms=0 (no liveness escape) |
| `MEM-netipc-transport-005` | Medium | Potential | livelock | src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:868-903 | Windows SHM BUSYWAIT profile can spin at 100% CPU forever (timeout_ms=0, unresponsive peer) |
| `MEM-os-windows-010` | Medium | Potential | oob-read | src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:300-340 | nipc_win_shm client_attach maps the whole file mapping then trusts server-supplied header offsets/capacities with incomplete cross-checks |
| `MEM-os-windows-011` | Medium | Potential | memory-ordering | src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:470-620 | nipc_win_shm_receive observed-path copies data then later re-reads mlen; len/seq consistency not re-validated after wake (potential torn copy) |
| `MEM-os-windows-013` | Medium | Potential | data-race | src/libnetdata/netipc/src/service/netipc_service_win_server.c:305-345 | nipc_server_drain/destroy join+free sessions while dropping the sessions_lock between iterations (use-after-free / double-free window) |
| `MEM-netipc-transport-007` | Low | Potential | toctou | src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c:110-140, 160-170 | Stale UDS socket recovery is a probe-then-unlink TOCTOU |
| `MEM-netipc-protocol-002` | Low | Smell | robustness | src/libnetdata/netipc/src/protocol/netipc_protocol_string_reverse.c:33-46 | nipc_string_reverse_decode does not enforce str_offset == NIPC_STRING_REVERSE_HDR_SIZE |
| `MEM-netipc-protocol-003` | Low | Smell | uninitialized-read | src/libnetdata/netipc/src/protocol/netipc_protocol.c:323-338 | nipc_hello_decode partially initializes the 48-byte struct (44-byte wire leaves 4 trailing padding bytes uninitialized) |
| `MEM-netipc-service-004` | Low | Smell | oob-read | src/libnetdata/netipc/src/protocol/netipc_protocol_cgroups_snapshot.c:102-135 | Public nipc_cgroups_resp_item accessor memcpy's the item header without re-checking the directory offset/length against the payload length (oob-read if decode was bypassed) |
| `MEM-netipc-transport-003` | Low | Smell | integer-overflow | src/libnetdata/netipc/src/transport/posix/netipc_uds_inflight.c:13-19, src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c:491-497 | In-flight capacity growth `capacity * 2` can wrap to a small value (uint32 overflow) |
| `MEM-netipc-transport-006` | Low | Smell | integer-overflow | src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c:105-122, src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c:1146-1165 | validate_batch computes dir_bytes as item_count*8 in uint32 (can overflow) before delegating to the codec |
| `MEM-os-windows-014` | Low | Smell | other-ub | src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c:105-115 | highest_bit profile-selection loop has no guard for a non-power-of-two intersection mask (degenerate but bounded) |

## Plan

1. Verify all findings and split into deterministic-fix, already-fixed, false-positive, and decision-required groups. Completed.
2. Implement deterministic codec, accessor, integer-overflow, lifecycle, abort, and Windows SHM hardening fixes. Completed.
3. Record user decisions for L1 threading, server lifecycle, POSIX abort lifetime, SHM liveness, and UDS stale recovery. Completed.
4. Update affected public specs and the integrator reference skill. Completed.
5. Track Netdata vendor/update propagation separately. Completed as SOW-0027.

## Execution Log

### 2026-06-28

- Created this SOW from the Netdata memory-corruption scout catalog.
- No implementation started.

### 2026-06-29

- Verified the 19 NetIPC-related scout findings against `plugin-ipc` source.
- Re-checked the L3 cgroups snapshot cache contract after the user challenged the earlier framing; confirmed SOW-0025 already made L3 refresh/read access internally synchronized through read guards.
- Recorded user decisions:
  - 1B: managed server `drain()`/`destroy()` wait for accept-loop inactivity.
  - 2A: raw L1 same-session access remains externally synchronized and documented.
  - 3B: POSIX client abort-pipe lifetime gets internal synchronization.
  - 4B: raw Windows SHM BUSYWAIT receives liveness/backoff behavior where possible.
  - 5A: UDS stale socket same-service concurrent-startup race is documented, not redesigned in this SOW.
- Implemented protocol hardening:
  - C/Rust/Go string-reverse decode now requires the canonical string offset.
  - C HELLO decode clears the whole output struct before copying the wire-sized payload.
  - C cgroups/apps lookup and cgroups snapshot accessors revalidate manually constructed public views before dereferencing wire offsets.
- Implemented C transport hardening:
  - UDS and named-pipe in-flight growth now guard capacity and allocation-size overflow.
  - UDS and named-pipe batch validation now uses checked arithmetic before validating lookup directories.
- Implemented managed-service lifecycle and abort fixes:
  - POSIX and Windows managed servers now wait for the accept loop to become inactive before `drain()`/`destroy()` frees session tracking.
  - POSIX client abort-pipe operations are protected by a lifetime lock.
  - Windows session `active` state is a Windows-sized `LONG` field instead of a 1-byte `bool` used with Interlocked APIs.
- Implemented Windows SHM hardening:
  - C/Rust/Go client attach validation now rejects malformed offsets, capacities, overflows, and overlapping request/response regions.
  - C/Rust/Go receive paths now copy only after observing stable sequence/length values.
  - BUSYWAIT paths periodically yield while still checking timeout and peer-close state.
- Updated docs and reference skill:
  - `docs/level1-transport.md`
  - `docs/level1-posix-uds.md`
  - `docs/level1-windows-shm.md`
  - `docs/netipc-integrator-skill.md`

### 2026-06-30

- Used the authorized `win11` host for native Windows C validation.
- The first Windows coverage run passed the functional tests but failed the per-file coverage threshold because new cgroups lookup guard branches lowered `netipc_protocol_cgroups_lookup.c` to 88.7%.
- Added focused C protocol tests for manual cgroups lookup accessor guard paths instead of weakening the coverage gate.
- Reran local and Windows validation; all checks passed.
- Created `.agents/sow/pending/SOW-0027-20260629-netdata-vendor-memory-safety-update.md` to track Netdata vendor propagation.

## Validation

Acceptance criteria evidence:

- Every finding was classified in `## Verification - 2026-06-29`.
- False positives recorded:
  - `MEM-netipc-service-003`: typed L2 timeout path already maps zero to the configured/default timeout.
  - `MEM-os-windows-014`: highest-bit profile selection is intentional and covered by parity evidence.
- Duplicates folded:
  - `MEM-netipc-service-004` folded into `MEM-netipc-protocol-001`.
  - `MEM-os-windows-013` handled with the same managed-server lifecycle decision as `MEM-netipc-service-002`.
- User decisions were recorded before implementation in `## Implications And Decisions`.
- Source fixes were applied in `plugin-ipc`, not directly in Netdata.
- Netdata propagation is tracked by SOW-0027.

Tests or equivalent validation:

- `make` passed on 2026-06-30.
- `/usr/bin/ctest --test-dir build --output-on-failure` passed 48/48 on 2026-06-30, total real time 472.12 sec.
- `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/transport/windows -o /tmp/netipc-transport-windows.test.exe` passed from `src/go`.
- `cargo check --manifest-path src/crates/netipc/Cargo.toml --target x86_64-pc-windows-gnu` passed.
- `git diff --check` passed.
- `bash .agents/sow/audit.sh` passed after moving this SOW to `.agents/sow/done/`; SOW status/directory consistency was clean.
- Native Windows C validation on `win11` passed:
  - Command: `timeout 1800 bash tests/run-coverage-c-windows.sh`
  - Built native Windows C service, named-pipe, SHM, Rust interop, and Go interop binaries.
  - Guard executable: 238 passed, 0 failed.
  - Extra guard executable: 144 passed, 0 failed.
  - Windows service extra executable: 785 passed, 0 failed.
  - Targeted Windows CTest slice passed for protocol, codec interop, named pipe, WinSHM, service, stress, service interop, SHM interop, cache interop, and cache SHM interop.
  - Coverage: all tracked Windows C files met the 90% threshold; total line coverage was 92.1%; `netipc_protocol_cgroups_lookup.c` was 90.8%.
- Earlier default `ctest` invocation hit the local `/home/costa/.local/bin/ctest` Python shim without the `cmake` module; validation used `/usr/bin/ctest`, the working system CTest binary.

Real-use evidence:

- The Windows validation used an isolated clone on `win11` at `/home/costa/src/plugin-ipc-sow26-wincheck-20260630-1041`.
- The clone was fast-forwarded to pushed `main` commit `cf8eaf9fdacc1ce4cc0ece2011728bf96a72b88e`, then the SOW-0026 source/test patch was applied.

Reviewer findings:

- No external AI reviewers were run because the user did not request them for this SOW.
- Local and native Windows validation exposed one real test-coverage gap, fixed by adding focused cgroups lookup accessor tests.

Same-failure scan:

- Searched for old Windows `active` Interlocked casts; source and Windows C fixtures no longer use the unsafe session-active cast pattern.
- Searched for in-flight growth and batch-size multiplication patterns; remaining matches are guarded by explicit overflow checks or are test/helper arithmetic.
- Searched for string-reverse offset handling; C/Rust/Go decoders now reject non-canonical offsets.
- Searched SOW queue for existing Netdata vendor propagation tracking; none existed, so SOW-0027 was created.

Sensitive data gate:

- This SOW contains only source paths, commit hashes, and scout finding summaries. No secrets or customer data are included.

Artifact maintenance gate:

- AGENTS.md: no update needed; SOW workflow, source-ownership, and validation rules already covered this work.
- Runtime project skills: none exist in `.agents/skills/project-*/`.
- Specs: updated `docs/level1-transport.md`, `docs/level1-posix-uds.md`, and `docs/level1-windows-shm.md`.
- End-user/operator docs: updated `docs/netipc-integrator-skill.md` because L1 raw-session access guidance changed.
- End-user/operator skills: updated output/reference skill `docs/netipc-integrator-skill.md`.
- SOW lifecycle: source SOW completed; Netdata vendor propagation tracked by SOW-0027; this SOW moves to `.agents/sow/done/` with the implementation commit.

Specs update:

- `docs/level1-transport.md`: documents raw L1 same-session thread ownership and external synchronization.
- `docs/level1-posix-uds.md`: documents the accepted low concurrent-startup stale-socket race.
- `docs/level1-windows-shm.md`: documents stable observed message copying and BUSYWAIT yielding/liveness behavior.

Project skills update:

- No runtime project skill exists to update.

End-user/operator docs update:

- `docs/netipc-integrator-skill.md` updated to direct ordinary integrations away from shared raw L1 sessions unless each raw session object is single-owner or externally locked.

End-user/operator skills update:

- `docs/netipc-integrator-skill.md` is the affected output/reference skill and was updated.

Lessons:

- Windows C coverage is a useful second validation gate for guard-heavy hardening; new defensive branches can make otherwise valid fixes fail the coverage threshold until tests exercise the rejected shapes directly.

Follow-up mapping:

- Netdata vendor propagation is tracked by `.agents/sow/pending/SOW-0027-20260629-netdata-vendor-memory-safety-update.md`.
- No other follow-up items remain from this SOW.

## Outcome

Completed. NetIPC memory-safety scout findings were verified, fixed or rejected with evidence, validated across local POSIX/C/Rust/Go paths and native Windows C paths, and mapped to a separate Netdata vendor propagation SOW.

## Lessons Extracted

- Treat scout output as hypotheses; several findings were real, several were duplicates, and two were false positives after source tracing.
- Keep raw L1 and L3 cache discussions separate. L3 cache use is internally synchronized through the read-guard API; raw L1 same-session use remains a lower-level externally synchronized contract.
- Do not close Windows-affecting SOWs on Linux validation alone when native Windows validation is available.

## Followup

- SOW-0027 tracks Netdata vendor propagation for these source fixes.

## Regression Log

None yet.
