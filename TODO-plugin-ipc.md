# TODO-plugin-ipc

## Purpose
- Fit-for-purpose goal:
  - make `plugin-ipc` robust enough for real Netdata integration
  - replace the current ad-hoc `cgroups.plugin -> ebpf.plugin` communication with a generic, typed, cross-language IPC library
  - do not attempt Netdata integration until the library is hardened against edge cases, abnormal situations, and operational mistakes

## TL;DR
- The protocol/API/transport work is largely implemented in this repo for C, Rust, and Go across Linux and Windows.
- The current active work is **pre-Netdata hardening**, not integration.
- New requirement:
  - rework the current prototype into the originally intended architecture before Netdata integration:
    - level 1 low-level API must support async progression and pipelining
    - servers must support multiple clients and multiple workers
    - request/response matching must remain correct under pipelined and chunked traffic
- The historical transcript was moved to [TODO-plugin-ipc.history.md](/home/costa/src/plugin-ipc.git/TODO-plugin-ipc.history.md). This file is now the active specification and execution plan only.

## Current Status (Fact-Based)
- Protocol core:
  - variable-length outer envelope implemented in C, Rust, and Go
  - directional handshake implemented:
    - request payload limit
    - request batch-item limit
    - response payload limit
    - response batch-item limit
- Baseline transports:
  - POSIX `UDS_SEQPACKET`
  - Windows `Named Pipe`
  - transparent transport-level chunking is implemented for packet-limited baseline sends/receives:
    - one negotiated `packet_size` per connection
    - continuation chunks carry a small chunk header
    - mid-stream failure is handled like any other send/receive failure
  - first level-1 client-side split is now implemented for POSIX baseline:
    - explicit client send/receive primitives exist instead of only blocking `call_*`
    - C POSIX has live pipelining coverage proving:
      - two independent requests can be sent before receiving responses
      - replies may arrive out of order and be matched by `request_id`
    - Rust and Go POSIX now expose the same client-side split primitives for baseline
  - Linux SHM client-side split is now implemented too:
    - raw SHM clients in C, Rust, and Go now expose explicit send/receive primitives
    - existing `call_*` helpers remain as wrappers on top
    - negotiated POSIX clients no longer reject SHM on `send_*` / `receive_*`; they delegate to the split SHM client path
    - validation now proves:
      - raw SHM split round-trip in Rust and Go unit tests
      - negotiated SHM split round-trip in Rust and Go unit tests
      - direct live SHM C/Rust matrix still passes after switching the C fixture to the split SHM client path
  - C POSIX server-side refactor is now underway:
    - unmanaged listener/session primitives exist in:
      - C POSIX
      - Rust POSIX
      - Go POSIX
    - legacy one-session server wrappers still exist on top in all three
    - validation now proves:
      - one listener can accept two concurrent sessions in C, Rust, and Go POSIX
      - both sessions can be served independently without collapsing back to a one-connection server object
    - C has live multi-client coverage
    - Rust and Go have direct transport tests for the same listener/session shape
    - managed C POSIX proof now exists on top of the unmanaged layer:
      - one acceptor
      - per-session readers
      - shared worker queue
      - per-session write arbitration
      - validated with:
        - one pipelined client
        - one normal client
        - out-of-order worker replies still matched correctly by `request_id`
    - Rust and Go POSIX now also prove level-1 out-of-order pipelining in direct transport tests
    - next step is managed server mode:
      - lift the managed model from C proof code into a reusable public/server surface
      - then propagate the same model to Rust and Go POSIX
      - then return to Windows baseline/SHM parity on the new level-1/session model
- SHM transports:
  - Linux:
    - unified SHM model implemented for C, Rust, and Go
    - synchronization backend is Linux-only futex-based
    - macOS / FreeBSD fall back to UDS
  - Windows:
    - unified SHM model implemented for:
      - `c-native`
      - `c-msys`
      - `rust-native`
      - `go-native`
- High-level methodology:
  - callback-based zero-copy `...View` API
  - cache-backed helper pattern implemented on top of the generic core
  - first concrete helper/service family is the fake `cgroups` snapshot service
  - integration-facing `cgroups` helper config is now strict:
    - explicit `auth_token` required
    - explicit `supported_profiles` required
    - explicit `preferred_profiles` required
    - explicit `max_response_payload_bytes` required
    - explicit `max_response_batch_items` required
  - production-shaped deterministic fake `cgroups` corpus is implemented across fixtures/tests:
    - 16 varied items
    - preserved lookup anchors:
      - `123 / system.slice-nginx`
      - `456 / docker-1234`
    - added long-name / long-path entries and mixed systemd/container/user/machine-style items
- Live validation already achieved:
  - Linux:
    - full repo tests green
    - negotiated baseline + SHM matrices green for `C/Rust/Go`
    - fake snapshot/cache baseline + SHM matrices green for `C/Rust/Go`
    - baseline large-message chunking is validated in the live fake `cgroups` path
    - direct live SHM C/Rust matrix is green after the client-side split refactor
  - Windows:
    - `tests/smoke-win.sh` green
    - `tests/run-live-cgroups-win.sh` green
    - baseline + SHM snapshot methodology validated across:
      - `c-native`
      - `c-msys`
      - `rust-native`
      - `go-native`
- Benchmark status:
  - `benchmarks-posix.md` now shows the complete Linux picture:
    - baseline UDS
    - direct/negotiated SHM
    - snapshot/cache baseline
    - snapshot/cache SHM
    - local cache lookup
  - fast path is still fast:
    - classic POSIX SHM remains in the multi-million req/s range for C/Rust
    - Go SHM snapshot refresh is now in the ~1.16M req/s range after the Linux spin fix
- Netdata integration:
  - **not started**
  - explicitly blocked until the hardening gate below is closed

## Final Design

### Service Model
- Discovery is by:
  - `service_namespace`
  - `service_name`
- One endpoint per service.
- One client context per service.
- One plugin process may expose multiple services.

### Protocol Model
- One fixed 32-byte outer header.
- Outer header carries:
  - message kind
  - flags
  - method / control code
  - transport/protocol status only
  - payload length
  - item count
  - message id
- Single messages:
  - one self-contained method/control payload
- Batch messages:
  - one outer header
  - one item directory
  - packed self-contained item payloads
- Batch semantics:
  - ordered
  - homogeneous per service/method
  - per-item business status lives inside each response payload, not in the outer header

### Method Payload Model
- Each method payload is self-contained.
- Variable fields use:
  - `offset + length`
  - trailing `\0` for string compatibility
- Decode returns **ephemeral views**, not owned data.
- Views are valid only during the current callback / library call.
- Naming must scream this:
  - `...View`
  - short comments must explicitly warn they are borrowed and ephemeral

### API Model
- Low-level API:
  - transport/session primitives
  - raw message send/receive
  - method payload encode/decode
- High-level API:
  - fixed per-service client contexts
  - callback-based zero-copy calls
  - optional explicit copy/materialize helpers
- Cache-backed helper layer:
  - sits strictly on top of the generic core
  - supports patterns like:
    - `refresh_cgroups()`
    - `lookup_cgroup()`

### Transport Model
- Baseline transports are first-class:
  - POSIX `UDS_SEQPACKET`
  - Windows `Named Pipe`
- SHM is one unified subsystem:
  - not separate “RPC SHM” and “snapshot SHM” products
  - must satisfy both:
    - request/response
    - server-owned snapshot publication
- Linux SHM:
  - Linux-specific optimized backend
  - BSD/macOS fallback to UDS

### Security / Handshake Model
- Auth token is caller-supplied.
- Handshake negotiates directional ceilings:
  - request payload bytes
  - request batch items
  - response payload bytes
  - response batch items
- Batch bytes are derived per direction.

## Made Decisions
1. Do not integrate into Netdata until the robustness findings are addressed.
2. Keep the active TODO as a short specification/plan and archive the historical transcript separately.
3. Use one service endpoint per service, discovered by `service_namespace + service_name`.
4. Keep the wire format as a custom typed binary protocol with explicit encode/decode.
5. Keep decoded request/response objects as ephemeral zero-copy views.
6. Make the zero-copy high-level API callback-based.
7. Support cache-backed helper patterns on top of the generic core.
8. Make the fake `cgroups` snapshot service suitable for later real `cgroups/ebpf` integration.
9. Apply the same snapshot/cache methodology on Windows too.
10. Keep Linux SHM as the real Unix SHM implementation; BSD/macOS fall back to UDS.
11. Keep one unified SHM subsystem that supports both RPC and snapshot publication.
12. Deliver work in strict phases, each validated before the next.
13. Make the high-level helper configuration strict by default for real integration.
14. Require explicit helper inputs for:
    - `auth_token`
    - `supported_profiles`
    - `preferred_profiles`
    - `max_response_payload_bytes`
    - `max_response_batch_items`
15. Baseline transport must support the snapshot/cache methodology too; Linux SHM may be preferred for scale/performance, but the methodology cannot depend on SHM-only support because FreeBSD/macOS fall back to baseline UDS.
16. Packet-limited transports must support transparent transport-level chunking for all messages; message size must not be artificially restricted to one packet, and mid-stream failure must be handled the same way as any other transport send/receive failure.
17. Continuation chunks should carry a small chunk header so receivers can detect wrong packet order, wrong message, or accidental multiplexing/desync.
18. Packet-size negotiation should stay simple: the client advertises one transport packet size in the handshake, the server replies with the negotiated `min(client, server)` packet size, and both request and response chunking use that single negotiated packet size for the whole connection.
19. The real `cgroups` service is a canonical ownership service:
    - `cgroups.plugin` is the sole owner of cgroup-to-container/service naming and identity data
    - this data will have many consumers, not just `ebpf.plugin`
    - therefore the first real integration cannot be designed as a one-off pairwise link per consumer
    - the library must support one producer with multiple consumers for this service family
20. The core library interaction model remains request -> response:
    - ping-pong, batch, and snapshot request/response must remain first-class forever
    - some datasets are too large to transfer wholesale and must stay query-based
    - some smaller datasets can be transferred as full snapshots/batches
    - this is a permanent requirement, not a temporary migration constraint
21. A second model may be added on top for canonical owned datasets:
    - producer maintains the latest snapshot
    - consumers can fetch the current snapshot on connect/startup
    - producer may later broadcast deltas/updates so consumers do not need to re-fetch full snapshots all the time
    - this is a different model from request/response and is still only a design direction, not an approved implementation decision yet
22. Keep request/response as the permanent core, and allow any future snapshot+delta push/update model only as an optional layer on top of it.
23. Level 1 low-level API must be async-capable and support pipelining:
    - callers must be able to send multiple requests without waiting for each response first
    - callers must be able to integrate progress into their own event loops
    - chunking must not collapse level 1 into a blocking request->response API
    - if pipelining is supported on one connection, chunk framing and transport state must support correct request/response matching under multiplexed in-flight traffic
24. The real server model must support:
    - multiple clients
    - multiple workers
    - request/response matching under pipelining
    - this is a required capability, not a later nice-to-have
25. The three API levels are now fixed as:
    - level 1:
      - low-level transport/session API
      - async-capable
      - event-loop friendly
      - supports pipelining
      - chunking may block only within one logical send progression or one logical receive progression, not across the whole request->response transaction
    - level 2:
      - blocking high-level callback/request-response API
      - convenience wrapper on top of level 1
    - level 3:
      - blocking snapshot/cache API
      - built on top of the generic core
26. The same 3-level split applies to servers too:
    - server level 1:
      - unmanaged listener/session primitives
      - event-loop friendly
      - caller-owned accept/read/write progression
    - server level 2:
      - managed callback-based request/response server
      - library-owned acceptor, worker threads, and per-session write arbitration
    - server level 3:
      - managed snapshot/cache publication helpers built on top of level 2
27. Level 1 must be redesigned away from the current blocking `call_message()` transport shape:
    - low-level send/receive/progress primitives must exist independently
    - blocking `call_message()` semantics belong to level 2 wrappers, not to level 1
28. Pipelining on one connection must support many in-flight logical messages, but each logical message remains atomic on the wire per direction:
    - do not interleave chunks from different logical messages on the same connection
    - chunking is still transparent at the transport level
    - request/response matching is done with `message_id`
29. The server concurrency model must be:
    - multi-client
    - multi-worker
    - one acceptor/session layer per connection
    - one per-connection write arbiter that serializes complete logical response messages back to the wire
30. Responses may be emitted out of request order on a connection and must be matched by `message_id`:
    - out-of-order replies are part of real pipelining
    - if a caller needs ordered dependent work, it should use batch instead of pipelining
    - independent work may use either:
      - batch
      - pipelining
31. Level-1 client/session API shape is now frozen for implementation:
    - separate request submission from response reception
    - expose native wait objects for caller-owned event loops:
      - POSIX: file descriptor
      - Windows: handle
    - expose non-blocking/progress-oriented operations for:
      - connect/accept completion
      - send progression
      - receive progression
      - completed response retrieval by `message_id`
    - low-level API may keep small blocking helpers for one logical send or one logical receive progression, but not a blocking request->response transaction
32. Server/session/worker API shape is now frozen for implementation:
    - provide unmanaged low-level listener/session primitives
    - provide a managed worker-pool server mode on top of them
    - per-connection writer arbitration is internal to the managed server mode
    - unmanaged mode still exposes enough primitives for callers with custom event loops / worker models
    - managed servers start with a fixed configured worker count at initialization
    - worker count does not change at runtime
33. SHM implications are now frozen for implementation:
    - the current request/response SHM path remains per-session / per-connection
    - this redesign does not require a separate canonical snapshot publication SHM path yet
    - any future shared publication/delta model remains a higher-level extension, not a blocker for the current redesign
34. Retry / reconnect semantics are now frozen for implementation:
    - level 1:
      - no automatic retry
      - if a connection fails, all in-flight requests on that session fail and are reported back to the caller
    - level 2:
      - may reconnect and retry the last request once where the wrapper already provides that guarantee
    - level 3:
      - may reconnect and retry refresh once
      - on failure it must preserve the previously valid cache
35. Migration order is now fixed for implementation:
    - first reference implementation:
      - C POSIX baseline
    - then parity propagation:
      - Rust POSIX baseline
      - Go POSIX baseline
      - Windows baseline
      - Linux SHM
      - Windows SHM
    - rationale:
      - Netdata integration depends primarily on the C API
      - POSIX baseline is the smallest clean reference transport
      - the design must be proven there before propagating it elsewhere
36. Before any further architectural implementation/rewrite, write four specification guides and treat them as the project authority:
    - level 1 transport API guide:
      - full client and server design
      - async progression
      - pipelining
      - multi-client / multi-worker expectations
    - level 2 strongly typed API guide:
      - built exclusively on level 1
      - full client and server design
      - ping-pong
      - batching
    - level 3 snapshot API guide:
      - built exclusively on level 2
      - full client and server design
      - snapshot refresh / cache / lookup semantics
    - code organization guide:
      - how to add transports
      - how to add strongly typed message families
      - how to add level-3 snapshot helpers
      - how to preserve separation of concerns, clarity, maintainability, and isolation between levels
37. These four guides are the project “GOD”:
    - implementation must align with them without exceptions
    - gap analysis should be performed against them before more architectural work continues
38. Create a dedicated spec-authoring task:
    - create `TODO-specs.md`
    - create `docs/`
    - derive four authoritative specification documents from:
      - `TODO-plugin-ipc.md`
      - `TODO-plugin-ipc.history.md`
    - latest decisions override earlier ones when conflicts exist
39. The spec-authoring pass must try to close the architecture fully:
    - specs should not intentionally leave open questions
    - if genuine unanswered questions are discovered while drafting:
      - stop treating the affected spec as normative
      - surface the exact questions
      - resolve them before finalizing the specs
40. Spec authoring order is now fixed:
    - first close specs for:
      - level 1 transport API
      - level 2 strongly typed API
      - level 3 snapshot API
    - only after those exist:
      - derive and discuss the code-organization guide
    - do not try to finalize spec 4 in parallel with 1-3
41. When TODO/history wording is ambiguous, extract Costa's prior responses verbatim and use those exact statements to disambiguate the normative specs
42. The level 1 / level 2 / level 3 specs must explicitly require:
    - 100% testing coverage
    - fuzz testing / fuzziness coverage
    - explicit corner-case and abnormal-path coverage
    - no exceptions
43. Nothing is acceptable for Netdata integration unless the specs and implementation together provide enough coverage to make crashes from malformed IPC, corner cases, and abnormal situations unacceptable by design

## Pending User Decisions
44. Managed server API layering:
    - user challenged whether level 2 needs any generic handler registry at all
    - open question is now:
      - should level 2 be only strongly typed service wrappers on top of level 1
      - or should the library also expose one generic managed callback/request-response server surface beneath those typed wrappers
45. Managed server worker-count configuration:
    - fixed worker count is configured at initialization time
    - no runtime scaling or worker-count changes are allowed
    - remaining question is whether the public API requires an explicit `worker_count > 0`, or accepts a sentinel such as `0` during initialization
46. Managed server shutdown semantics:
    - choose whether the public API exposes:
      - graceful drain only
      - abort only
      - both graceful drain and immediate abort

## Hardening Gate Before Netdata Integration
- Netdata integration is blocked until these findings are addressed.

### 0. Architectural layering audit
- Fact:
  - user challenged whether the codebase actually follows the agreed 3-level model consistently, or whether it has drifted into a mixed API/utility soup
- Findings:
  - the codebase is not random soup:
    - there is a coarse top-level split into:
      - `protocol`
      - `transport`
      - `service`
    - Rust crate root exposes exactly those three modules in `src/crates/netipc/src/lib.rs`
  - however, the agreed 3 encapsulating API levels are not reflected consistently in the public surface:
    - level 1 and level 2 are still mixed together in the transport APIs
    - level 3 service helpers still depend directly on blocking transport `call_message()` style interfaces
    - reusable managed server level-2/3 code is still mostly absent from the library and exists only as proof code in test fixtures
  - transport files are too monolithic for the intended separation of concerns:
    - Go POSIX transport is `1228` lines in `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - Rust POSIX transport is `2949` lines in `src/crates/netipc/src/transport/posix.rs`
    - the same files currently mix:
      - listener/session types
      - wrapper server types
      - client types
      - handshake/chunking internals
      - blocking `call_*` helpers
      - tests in Rust
  - public transport headers/types still expose both low-level split send/receive and higher-level blocking `call_*` wrappers side by side:
    - C UDS public header mixes:
      - server wrapper
      - unmanaged listener/session
      - client split send/receive
      - blocking `client_call_*`
      - all in `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
    - Go POSIX `Client` exposes:
      - `ReceiveMessage()`
      - `SendMessage()`
      - `CallMessage()`
      - all on the same public type in `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - Rust POSIX `UdsSeqpacketClient` exposes:
      - `receive_message()`
      - `send_message()`
      - `call_message()`
      - all in `src/crates/netipc/src/transport/posix.rs`
  - service wrappers are still thin and transport-aware instead of sitting on a clearer level-2 typed client layer:
    - Go `cgroupssnapshot` client defines its transport dependency as:
      - `CallMessage(...)`
      - `Close()`
      - in `src/go/pkg/netipc/service/cgroupssnapshot/client_unix.go`
    - C `netipc_cgroups_snapshot_client_refresh()` manually:
      - builds message headers
      - encodes payload
      - calls transport `transport_call_message()`
      - validates raw response envelope
      - in `src/libnetdata/netipc/src/service/netipc_cgroups_snapshot.c`
  - managed server mode is only partially implemented as test proof, not as reusable public library API:
    - the current worker queue / reader threads / worker threads / per-session writer proof lives in:
      - `tests/fixtures/c/netipc_live_posix_c.c`
  - current tracked repo size is not enormous overall, but the implementation is still much larger than the intended shape:
    - tracked repo total:
      - `101` files
      - `39,612` lines
    - code-like tracked files:
      - `94` files
      - `37,711` lines
    - implementation-only tracked source under `src/` plus `CMakeLists.txt`, excluding docs and tests:
      - `39` files
      - `21,765` lines
    - implementation-only by language:
      - C:
        - `12` files
        - `8,756` lines
      - Rust:
        - `7` files
        - `7,743` lines
      - Go:
        - `15` files
        - `5,225` lines
  - the size concentration is dominated by transport and protocol state machines, not by the typed helper layer:
    - largest implementation files include:
      - `src/crates/netipc/src/transport/posix.rs` (`2949` lines)
      - `src/crates/netipc/src/transport/windows.rs` (`2569` lines)
      - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c` (`2365` lines)
      - `src/go/pkg/netipc/transport/windows/pipe.go` (`2104` lines)
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c` (`1670` lines)
      - `src/crates/netipc/src/protocol.rs` (`1378` lines)
      - `src/libnetdata/netipc/src/protocol/netipc_schema.c` (`975` lines)
  - working theory:
    - the implementation size appears to have grown from a combination of:
      - real cross-language / cross-platform scope
      - transport complexity added during handshake, SHM, chunking, and pipelining work
      - architectural drift that mixed level 1 and level 2 concerns into the same modules and public APIs
      - compatibility wrappers and transitional surfaces retained while requirements were still moving
- Required action:
  - audit the public API and module boundaries across C, Rust, and Go
  - verify whether level 1 / level 2 / level 3 concerns are clearly separated
  - treat any mismatch as a pre-integration blocker

### 1. Server model mismatch
- Fact:
  - the current transport/server layer is still effectively single-connected in several implementations.
  - POSIX baseline now has the first unmanaged listener/session split in place across:
    - C
    - Rust
    - Go
  - POSIX baseline multi-client acceptance is now validated:
    - C live script
    - Rust unit/integration test
    - Go unit/integration test
  - POSIX baseline managed-server semantics are now partially validated:
    - C live script proves:
      - multi-client acceptance
      - shared worker queue
      - per-session writer serialization
      - out-of-order reply correctness under pipelining
    - Rust and Go currently prove the same low-level out-of-order matching at the transport layer, but not yet a reusable managed-server surface
  - Windows baseline and SHM still need the same unmanaged multi-client/session progression.
  - Windows baseline unmanaged listener/session parity is the next concrete transport task.
- Evidence:
  - C POSIX server keeps one `conn_fd` in `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
  - Rust POSIX server keeps one `conn_fd` in `src/crates/netipc/src/transport/posix.rs`
  - Go POSIX server keeps one `conn` in `src/go/pkg/netipc/transport/posix/seqpacket.go`
  - Go Windows server keeps one connected pipe in `src/go/pkg/netipc/transport/windows/pipe.go`
- Required action:
  - implement an integration-safe one-producer / multi-consumer topology for the canonical `cgroups` service

### 1b. API layering mismatch
- Fact:
  - the current transport layer still mixes the low-level transport/session API with higher-level blocking convenience calls, and it does not yet satisfy the required pipelined level-1 model
- Evidence:
  - C transport headers expose blocking `*_client_call_message()`:
    - `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
  - Rust transports expose `call_message()` directly on transport clients:
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
  - Go transports expose blocking `CallMessage()` directly on transport clients:
    - `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
- Required action:
  - decide and implement the exact split between:
    - level 1 async-capable transport/session operations
    - level 2 blocking callback/request-response convenience operations
    - level 3 snapshot/cache helpers
  - redesign level 1 around:
    - pipelined request submission
    - response matching
    - multi-client servers
    - multi-worker servers
  - revisit the current transport chunking assumption:
    - today one chunked logical message assumes exclusive use of the stream until completion
    - that assumption is not sufficient for a pipelined/multiplexed level-1 API

### 2. Production-shaped snapshot corpus
- Status:
  - implemented
- Fact:
  - the fake `cgroups` snapshot corpus is now production-shaped enough for pre-integration hardening:
    - 16 deterministic items
    - long names
    - long paths
    - varied options
    - varied systemd/container/user/machine-style entries
- Evidence:
  - C fixture: `tests/fixtures/c/netipc_cgroups_live.c`
  - C codec tool: `tests/fixtures/c/netipc_codec_tool.c`
  - Rust fixture and tests:
    - `tests/fixtures/rust/src/bin/netipc_codec_rs.rs`
    - `src/crates/netipc/src/protocol.rs`
    - `src/crates/netipc/src/service/cgroups_snapshot.rs`
  - Go fixture and tests:
    - `tests/fixtures/go/main.go`
    - `src/go/pkg/netipc/protocol/frame_test.go`
    - `src/go/pkg/netipc/service/cgroupssnapshot/client_unix_test.go`
  - Live assertions:
    - `tests/run-interop.sh`
    - `tests/run-live-cgroups-baseline.sh`
    - `tests/run-live-cgroups-shm.sh`
    - `tests/run-live-cgroups-win.sh`
- Remaining limitation:
  - these are still deterministic fake fixtures, not real captured Netdata artifacts

### 3. Real-scale limit handling
- Status:
  - transport-level chunking is implemented for packet-limited baseline transports
- Fact:
  - helper default response batch limit is `1000`
  - current Netdata `cgroup_root_max` default is also `1000`, but it is configurable
- Evidence:
  - library defaults:
    - `src/libnetdata/netipc/include/netipc/netipc_cgroups_snapshot.h`
    - `src/crates/netipc/src/service/cgroups_snapshot.rs`
    - `src/go/pkg/netipc/service/cgroupssnapshot/client.go`
  - Netdata config:
    - `/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.c`
- Required action:
  - prove behavior above the default
  - document and test limit override requirements clearly
- New evidence from the real-scale hardening slice:
  - deterministic fake `cgroups` corpus size is now configurable via `NETIPC_CGROUPS_ITEM_COUNT`
  - Linux baseline UDS failed at the current default-scale `1000`-item corpus with:
    - server-side `Message too long`
    - client-side `Connection reset by peer`
  - Linux SHM passed the full `C/Rust/Go` producer/client snapshot matrix at:
    - `NETIPC_CGROUPS_ITEM_COUNT=1200`
    - `NETIPC_MAX_RESPONSE_BATCH_ITEMS=1200`
  - Windows baseline Named Pipe and Windows SHM both passed the full `c-native/c-msys/rust-native/go-native` snapshot matrix at the same `1200`-item scale
  - Linux baseline UDS failed at the same `1200`-item scale with:
    - server-side `Message too long`
    - client-side `Connection reset by peer`
- Implication:
  - production-shaped snapshot refresh already exceeds Linux baseline UDS at the current default-scale corpus
  - large snapshot refresh already has a concrete transport ceiling on Linux baseline UDS
  - because baseline remains mandatory for FreeBSD/macOS, snapshot refresh had to be made robust at the generic transport layer
- Implemented behavior:
  - chunking is implemented in the generic transport send/receive path, not as a service-specific snapshot loop
  - packet-limited baseline transports now support transparent chunking for all messages
  - sender:
    - if a message exceeds the negotiated per-connection `packet_size`, keep the operation blocking
    - reuse offsets over the original message buffer
    - emit consecutive packets until the full logical message is sent
  - receiver:
    - reads the first packet
    - allocates the full logical message buffer once from the first-chunk metadata
    - receives the remaining packets immediately and appends them by offset
  - continuation chunks carry a small chunk header so receivers can validate message identity and ordering
  - no multiplexing is assumed during one chunked send/receive operation
  - mid-stream failure is handled exactly like any other transport send/receive failure:
    - discard partial reception
    - close the stream
    - reconnect for clients
    - retry the last transaction once where the high-level contract already permits it
- Validation:
  - Linux:
    - `go test ./...`
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `/usr/bin/ctest --test-dir build --output-on-failure`
    - production-shaped fake snapshot baseline:
      - `NETIPC_CGROUPS_ITEM_COUNT=1000 NETIPC_MAX_RESPONSE_BATCH_ITEMS=1000 bash tests/run-live-cgroups-baseline.sh`
  - Windows:
    - `bash tests/smoke-win.sh` on `win11` -> `32 passed, 0 failed`
    - `bash tests/run-live-cgroups-win.sh` on `win11` -> `64 passed, 0 failed`

### 4. Abnormal-path test coverage
- Status:
  - helper-layer negative/recovery coverage is now materially stronger
- Fact:
  - helper cache-preservation and refresh-failure coverage now exists in both:
    - transport-backed live tests
    - deterministic helper-level tests
- Evidence:
  - Go Unix helper tests:
    - `src/go/pkg/netipc/service/cgroupssnapshot/client_unix_test.go`
  - Go cross-platform helper tests:
    - `src/go/pkg/netipc/service/cgroupssnapshot/client_test.go`
  - Rust Unix helper tests:
    - `src/crates/netipc/src/service/cgroups_snapshot.rs`
  - repo-level live validation:
    - `tests/run-live-cgroups-baseline.sh`
    - `tests/run-live-cgroups-shm.sh`
    - `tests/run-live-cgroups-win.sh`
- Required action:
  - keep the remaining gap explicit:
    - the C helper still relies more on live/integration coverage than on small deterministic helper-only tests
- New evidence from this hardening slice:
  - Go helper now has platform-neutral deterministic tests for:
    - strict constructor rejection of implicit insecure defaults
    - refresh failure after a ready transport preserving the cache and disconnecting the transport
    - reconnect-after-ready failure rebuilding a fresh transport and updating the cache
    - malformed-but-transport-valid responses preserving the previous cache and disconnecting
    - non-OK transport status envelopes preserving the previous cache and disconnecting
  - existing Unix helper tests still cover:
    - baseline refresh failure preserving the cache
    - SHM refresh failure preserving the cache
    - malformed SHM responses preserving the cache
  - Rust helper tests still cover the same failure classes on the Rust path

### 5. Parser hardening
- Status:
  - partially implemented
- Fact:
  - parser hardening now exists in the Go and Rust test suites, but dedicated C-side fuzz/property coverage is still missing
- Evidence:
  - Go protocol fuzz targets:
    - `src/go/pkg/netipc/protocol/frame_test.go`
    - covers:
      - `DecodeMessageHeader()`
      - `DecodeChunkHeader()`
      - `DecodeCgroupsSnapshotRequestView()`
      - `DecodeCgroupsSnapshotView()`
  - Rust protocol hardening:
    - `src/crates/netipc/src/protocol.rs`
    - `cgroups_snapshot_decode_never_panics_on_corrupted_payloads`
- Required action:
  - keep the remaining gap explicit:
    - no dedicated C-side fuzz/property harness exists yet for:
      - malformed outer headers
      - malformed chunk headers
      - malformed item directories
      - malformed method-local offsets/lengths
      - missing trailing `\0`
  - decide whether pre-integration confidence requires:
    - a dedicated C harness
    - or the current Go/Rust parser hardening plus live C interop/negative tests is sufficient

## Plan

### Phase A: TODO cleanup
- Archive the historical transcript.
- Keep this file as the active specification only.

### Phase B: Operational hardening
- Make auth/profile/limit expectations explicit in helper-facing APIs.
- Status:
  - completed

### Phase C: Production-shaped fixtures
- Build a larger realistic fake `cgroups` snapshot corpus.
- Validate baseline and SHM paths against it on Linux and Windows.
- Status:
  - completed

### Phase D: Abnormal-path coverage
- Expand helper negative tests.
- Add restart/reconnect/cache-preservation cases.
- Add malformed payload coverage and fuzz/property tests.
- Status:
  - partially completed
- Remaining:
  - equivalent Windows/helper negative coverage where applicable
  - wider parser/property coverage beyond the first Go+Rust slice

### Phase E: Real-scale validation
- Test larger snapshot counts and larger payloads.
- Re-run benchmark generation and verify that performance remains acceptable.
- Status:
  - partially completed
- Remaining:
  - keep revalidating behavior and performance while the transport/session redesign lands

### Phase F: Level-1 transport/session redesign
- Replace the current blocking transport-level `call_message()` shape with:
  - independent request submission
  - independent send progression
  - independent receive progression
  - completion retrieval by `message_id`
  - native wait object exposure for caller-owned event loops
- Preserve existing blocking wrappers only as level-2 convenience APIs.
- Success criteria:
  - one client session can pipeline multiple independent requests without waiting for each response first
  - responses are matched correctly by `message_id`
  - no chunk interleaving is required on the wire
- Status:
  - started
- Completed in the first slice:
  - C POSIX baseline client-side split:
    - explicit client send/receive message/frame/increment primitives
    - existing blocking `call_*` wrappers now sit on top for baseline
  - live C POSIX pipelining test:
    - two requests sent before any response read
    - server replies in reverse order
    - client validates by `request_id`
  - Rust and Go POSIX client-side split primitives added for baseline parity
- Remaining:
  - native wait-object accessors
  - server/session split
  - multi-client accept/session model
  - multi-worker dispatch
  - SHM level-1 client/session redesign

### Phase G: Multi-client / multi-worker server redesign
- Replace the current single-connected server/session model with:
  - multi-client listener
  - per-connection sessions
  - shared worker pool
  - per-connection write arbiter for complete logical responses
- Provide:
  - unmanaged low-level listener/session primitives
  - managed worker-pool server mode
- Success criteria:
  - one canonical service endpoint can serve many concurrent clients
  - multiple workers can process requests concurrently
  - out-of-order replies are delivered correctly by `message_id`

### Phase H: Propagation across transports and languages
- Land the redesigned level-1/session/server model consistently across:
  - C
  - Rust
  - Go
  - POSIX baseline
  - Windows baseline
  - Linux SHM
  - Windows SHM
- Keep level-2 blocking wrappers and level-3 snapshot/cache helpers working on top.

### Phase I: Hardening revalidation
- Re-run:
  - unit tests
  - interop tests
  - fake snapshot baseline + SHM live matrices
  - abnormal-path tests
  - real-scale tests
  - benchmark generation
- Add coverage for:
  - multiple clients
  - pipelined requests
  - out-of-order replies
  - disconnect with multiple in-flight requests
  - reconnect semantics at level 2 and level 3

### Phase J: Netdata integration
- Only after the hardening gate closes.
- Replace the current ad-hoc `cgroups.plugin -> ebpf.plugin` path with the new helper/service path.
- Keep a temporary fallback path during migration if needed.

## Implied Decisions
- The fake `cgroups` service remains the pre-integration proving ground.
- The current benchmark and validation docs remain authoritative until superseded by regenerated results.
- Historical discussion is preserved in [TODO-plugin-ipc.history.md](/home/costa/src/plugin-ipc.git/TODO-plugin-ipc.history.md), not here.

## Testing Requirements
- Protocol:
  - encode/decode roundtrips
  - handshake success/failure
  - directional limit enforcement
  - malformed envelope rejection
- Method layer:
  - builder correctness
  - offset/length validation
  - trailing `\0` validation
  - ephemeral view lifetime documentation checks
- Live matrices:
  - Linux baseline and SHM for `C/Rust/Go`
  - Windows baseline and SHM for `c-native`, `c-msys`, `rust-native`, `go-native`
- Snapshot/cache:
  - refresh correctness
  - cache rebuild correctness
  - lookup correctness
  - refresh-failure preserves old cache
  - reconnect after previously-ready failure
- Hardening:
  - large snapshots
  - high item counts
  - long strings
  - malformed payload fuzz/property tests
- Performance:
  - regenerate `benchmarks-posix.md`
  - regenerate `benchmarks-windows.md`
  - verify fast path remains fast after hardening changes

## Documentation Updates Required
- README:
  - active transport support matrix
  - Linux SHM vs BSD/macOS fallback behavior
  - helper configuration expectations
- Protocol spec:
  - outer header
  - directional handshake limits
  - batch/item layout
  - transport status vs business status
- Developer docs:
  - ephemeral `...View` lifetime rules
  - callback-based zero-copy API shape
  - cache-backed helper model
- Integration docs:
  - first Netdata migration target
  - required auth/profile/limit wiring
  - fallback/rollback notes for migration
