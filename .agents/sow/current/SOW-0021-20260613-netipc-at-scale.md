# SOW-0021 - NetIPC At Scale

## Status

Status: in-progress

Sub-state: implementation active; supersedes SOW-0007 and owns the full C/Rust/Go, POSIX/Windows, interop, coverage, benchmark, review, vendoring, and downstream PR scope. Server learned-capacity cap governance is implemented and validated; downstream topology-containers post-vendor validation passed; remaining review focuses on any broader adversarial matrix gaps before the SOW can close.

## Requirements

### Purpose

NetIPC Level 2 APIs must be dead simple and scale-safe for consumers. A caller should provide typed semantic work, whether small, big, or huge, and the library should handle transport caps, batching, splitting, stitching, retries, timeouts, aborts, and response ownership details internally.

The project must be fit for Netdata-scale deployments, including hosts and clusters with tens of thousands of processes or cgroups. This includes memory-constrained IoT/RPi-class systems, large HPC systems, and large Kubernetes fleets. NetIPC must not fail a valid typed lookup merely because one encoded request or response exceeds a per-message transport cap.

NetIPC and Netdata semantics must stay aligned. Level 2 must not classify something as "impossible" if it is normal or expected for Netdata consumers. If one item contains pathological or oversized metadata, that item may fail to be enriched, but it must not invalidate the rest of the logical batch.

### User Request

The user clarified that it is equally bad whether the SDK spins or returns failure when a valid large lookup cannot complete. The first real consumer, topology-containers, can enqueue an `8192` PID APPS_LOOKUP batch, and a valid response can exceed the current default per-message payload budget. NetIPC must work at scale; Level 2 API consumers should not care about transport mechanics.

The user requested a new SOW for "NetIPC at scale" and asked that the scalability patterns become prominent in docs, skills, and related project guidance so this pattern does not regress. The user also clarified that strange, unrealistic-looking, overflow, oversized, and corner cases must be tested because Netdata runs across a very wide deployment range.

The user further clarified the distinction between failures and valid scale cases:

- Missing endpoints, dead plugins, corrupt messages, malformed payloads, incompatible protocols, auth failures, timeouts, and aborts are failures and should fail fast.
- A lot of valid data is not a failure and should be handled by Level 2.
- A single problematic item must not poison the whole batch. For example, a container with a huge label may fail to be enriched, but other containers in the same logical request must still be enriched.
- All corner cases must be fully tested in all supported SDK languages. The project should know the behavior works in C, Rust, and Go; it must not rely on hope or one-language inference.

### Assistant Understanding

Facts:

- The current implementation and docs use a fixed transport-level payload cap in places, but SOW-0021 changes the contract: payload ceilings must be configured at initialization or derived from documented overrideable defaults, not scattered hardcoded literals.
- Level 2 typed lookup consumers should not have to know encoded request or response sizes.
- The existing pending `SOW-0007 - Transparent Lookup Batch Splitting` covers the narrower request-side oversized semantic batch problem.
- The topology-containers integration exposed a broader response-side scale problem: an `8192` PID APPS_LOOKUP request can be valid, but its response can exceed the per-message cap depending on returned metadata size.
- Netdata target deployments can include very large process and cgroup cardinality, including HPC-like environments.
- Lookup codecs already carry per-item statuses, but the current status vocabulary may need an explicit item-level "omitted / too large / not enrichable" outcome instead of overloading an unrelated status.

Inferences:

- The correct contract is logical-call completion, not single-message completion.
- Level 2 should treat transport caps as internal mechanics and produce one logical response from as many bounded transport calls as needed.
- Lookup split/stitch should be a reusable Level 2 scalability pattern, not an application-specific workaround.
- The existing `SOW-0007` should be merged into or superseded by this broader SOW before implementation starts, to avoid two overlapping SOWs for the same API contract.

Resolved implementation decisions:

- SOW-0021 supersedes SOW-0007 so there is one authoritative scale contract.
- Oversized valid metadata uses explicit item-level outcome semantics, not `UNKNOWN` or `UNKNOWN_PERMANENT`.
- Level 2 preserves the existing simple consumer-facing view shape by assembling client-owned stitched response storage.
- Split/stitch uses proactive request sizing plus conservative response sizing with adaptive retry-on-valid-overflow.
- Response-cap overflow is represented as item-level `PAYLOAD_EXCEEDED` outcomes when the server can return a valid partial response. The Level 2 client consumes those outcomes internally and transparently repeats the call only for those items.
- A single item that cannot fit in one maximum-size payload is represented as item-level `OVERSIZED_ITEM`, not as a retriable condition.
- Mixed generations across subcalls fail the logical call; NetIPC must not return one stitched view that silently mixes snapshots.
- Mixed generation rejection is a global NetIPC rule, not a per-SOW option. Plugins/providers and clients must match the documented method, layout, status, and generation contract exactly; any mismatch is rejected.
- Logical calls have explicit memory, item-count, payload-budget, and subcall ceilings. These ceilings come from initialization config or documented defaults that consumers can override for their deployment size. Exceeding a ceiling is a clear whole-call error.
- Internal ceilings must not be hardcoded inside the NetIPC libraries. Implementations may ship defaults, but consumers must be able to override them during client/server initialization so small systems can use small buffers and large systems can opt into larger payload budgets.
- `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM` are standard lookup response outcomes and must have the same semantics in every C, Rust, and Go implementation.
- Mixed generations are globally unsupported. NetIPC has no backward-compatibility, forward-compatibility, or best-effort compatibility mode for provider/client contract drift; any method, layout, generation, echoed-key, or status mismatch rejects the affected call.
- C, Rust, and Go are all in scope. The SOW cannot close with a staged language gap.

### Acceptance Criteria

- Level 2 typed lookup APIs in C transparently complete valid large semantic APPS_LOOKUP and CGROUPS_LOOKUP calls, even when the full request or response cannot fit in one transport message.
- C, Rust, and Go typed lookup APIs provide the same consumer-facing scale contract before this SOW can complete.
- Logical response order and item count match the caller's semantic request order.
- Per-item statuses are preserved across split/stitch boundaries.
- No silent partial enrichment is possible: every requested item returns either an enrichment result or an explicit item-level status explaining why that item was not enriched.
- Oversized or otherwise not-enrichable single items do not fail the logical batch when the request itself is valid. They produce explicit per-item outcomes and allow other items to succeed.
- Server-side response-cap pressure returns explicit `PAYLOAD_EXCEEDED` item outcomes for the first item that could not fit and for all remaining unprocessed items in that request. Level 2 clients must hide this from API consumers by issuing follow-up requests for those items and stitching the final logical response.
- `PAYLOAD_EXCEEDED` is retriable by the Level 2 client only. It is not an instruction for application callers to manually retry.
- `OVERSIZED_ITEM` means the single item cannot fit alone in a maximum-size payload and is not retriable by Level 2. The item remains in the final logical response as not enriched while later items continue normally when possible.
- Whole-call failures are reserved for true call/session/protocol failures: endpoint unavailable, peer failure, timeout, abort, auth failure, incompatibility, malformed/corrupt request or response, allocation failure, or an input that cannot be represented as a valid request at all.
- Timeout and abort apply to the whole logical call, not only one transport fragment.
- Reconnect and overflow-recovery behavior remains bounded and does not hide repeated permanent failures.
- Documentation prominently states that Level 2 consumers do not manage transport payload caps, request splitting, response stitching, or buffer sizing for normal typed calls.
- `docs/netipc-integrator-skill.md` and any relevant project runtime skill guidance are updated so future integrations preserve the scale-safe Level 2 contract.
- Tests cover small, boundary, large, and huge calls, including response-overflow cases that can only succeed through split/stitch.
- Tests cover adversarial and strange cases, including exact cap boundaries, cap plus/minus one byte, oversized single items, huge item counts, duplicate keys, zero-item calls, mixed per-item statuses, malformed server responses, mismatched response counts, repeated overflow, no capacity-growth overflow, reconnect during a logical call, timeout during a logical call, abort during a logical call, and memory-constrained operation.
- The full adversarial test matrix exists and passes in C, Rust, and Go. A test in one language is not evidence for another language.
- Cross-language interop tests cover large and adversarial lookup cases where a client in one language talks to a server in another language.
- The server contract is explicit: lookup split/stitch is client-side Level 2 orchestration using multiple complete request/response cycles; servers see ordinary independent lookup requests and do not need continuation/cursor state.
- Lookup split/stitch does not use `NIPC_FLAG_BATCH`; it uses method-internal lookup item ordering and ordinary Level 2 calls.
- The stitched response generation rule is specified and tested, including generation changes between subcalls.
- Mixed-generation stitched responses are globally unsupported and rejected in all languages.
- Logical response memory, payload-budget, item-count, and subcall ceilings are specified, configurable at initialization or via documented defaults, enforced, and tested. Exceeding a logical-call ceiling returns a clear error without OOM, unbounded recursion, or silent partial results.
- Timeout is an absolute logical-call deadline across all subcalls. Abort is checked before each subcall and partial results are discarded on abort.
- Mid-logical-call peer failure, malformed subresponse, or corrupt data fails the whole logical call and discards partial stitched results. These failures are not repaired, guessed around, or silently retried as scale handling.
- Coverage scripts include all touched lookup protocol/service files and packages in C, Rust, and Go.
- Benchmark/stress scenarios include the new scale cardinalities and define measurable floors or analysis gates before the SOW can close.
- Public specs are corrected for lookup method IDs, Level 2 logical-call splitting, and the difference between Level 1 per-message caps and Level 2 logical calls.
- Benchmarks or equivalent scale validation prove the implementation remains practical for tens of thousands of lookup items.

## Analysis

Sources checked:

- `.agents/sow/pending/SOW-0007-20260528-transparent-lookup-batch-splitting.md`
- `docs/level2-typed-api.md`
- `docs/netipc-integrator-skill.md`
- `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
- `src/libnetdata/netipc/src/service/netipc_service_common.c`
- topology-containers integration evidence from `network-viewer` APPS_LOOKUP and `apps.plugin` APPS_LOOKUP usage

Current state:

- `docs/level2-typed-api.md` documents bounded reconnect-on-overflow as a fallback safety net, not the primary sizing strategy.
- The existing SDK can grow negotiated capacities and retry, but it cannot make one response larger than the effective transport payload ceiling fit.
- The current public typed lookup surface can therefore expose a transport cap as caller-visible failure for valid large semantic calls.
- `SOW-0007` already recognizes that callers cannot reasonably know request payload byte sizes, but it does not fully promote response overflow and logical-call scale behavior as the central Level 2 contract.
- Existing codec tests already cover many malformed lookup layouts and integer-overflow guards, especially in Go and Rust, but they do not prove Level 2 logical-call split/stitch behavior.
- Existing cross-language `interop_service` fixtures exercise `CGROUPS_SNAPSHOT`, not APPS_LOOKUP or CGROUPS_LOOKUP at scale.
- Existing codec interop fixtures include lookup messages, but only fixed small examples; they do not cover large logical calls, response splitting, item-level oversized outcomes, or malformed stitched responses.
- Existing coverage scripts need updates so new lookup scale files and split source files are included in the measured surface. For example, Go coverage currently focuses on protocol, POSIX transport, and cgroups snapshot packages.
- `docs/level1-wire-envelope.md` needs review for method table drift because the current protocol defines lookup methods in addition to the earlier method examples.
- `docs/level2-typed-api.md` has to keep Level 1 batch behavior distinct from lookup logical-call partitioning so future work does not reintroduce a false one-transport-message assumption for lookup APIs.

Risks:

- Stitching borrowed C views incorrectly can create lifetime bugs.
- Rust and Go parity mistakes can create different production behavior by language.
- Adaptive splitting can accidentally amplify calls, latency, or server load if not bounded and measured.
- Application-side workarounds would spread transport knowledge into consumers and make future APIs harder to maintain.
- Silent partial results would be worse than explicit failure because topology enrichment would look correct while being incomplete.
- Treating unrealistic-looking cases as impossible without a proven invariant would leave Netdata exposed on unusual systems. Every rejected case needs either a test or a documented invariant with a test proving the invariant boundary.
- Overloading an existing per-item status to mean "too large" can mislead consumers if the status has a different business meaning, such as unknown or permanently missing.
- Adding a new wire status without layout/version/compatibility planning would break older decoders because current lookup specs require rejecting unknown status values.
- Rebuilding one stitched wire response in service code would violate the existing codec/service layer boundary unless the response merge/assemble operation is implemented as a codec-layer primitive.
- A logical response without configurable memory/subcall ceilings can become a denial-of-service vector on memory-constrained systems.
- Hardcoded scale ceilings would make the library wrong for both tiny systems and large-memory systems. Defaults must exist, but consumers must be able to override them during initialization.
- Adaptive response splitting can be expensive because overflow responses close the current session; proactive sizing quality and reconnect counts must be measured.

## Test And Implementation Planning

### Current Evidence From The Repository

- C typed lookup clients currently encode one request, perform one raw Level 2 call, and decode one response:
  - `src/libnetdata/netipc/src/service/netipc_service_apps_lookup.c`
  - `src/libnetdata/netipc/src/service/netipc_service_cgroups_lookup.c`
- Rust typed lookup clients have the same one-request/one-response shape:
  - `src/crates/netipc/src/service/raw/apps_lookup.rs`
  - `src/crates/netipc/src/service/raw/cgroups_lookup.rs`
- Go typed lookup clients have the same one-request/one-response shape:
  - `src/go/pkg/netipc/service/raw/apps_lookup.go`
  - `src/go/pkg/netipc/service/raw/cgroups_lookup.go`
- C service tests already cover payload-limit and overflow recovery patterns for existing services:
  - `tests/fixtures/c/test_service.c`
  - `tests/fixtures/c/test_service_payload_limits.c`
  - `tests/fixtures/c/test_service_method_limits.c`
- Rust and Go service tests cover raw/service behavior, but lookup scale needs dedicated typed logical-call tests:
  - `src/crates/netipc/src/service/raw_unix_tests.rs`
  - `src/crates/netipc/src/service/raw_windows_tests.rs`
  - `src/go/pkg/netipc/service/raw/lookup_unix_test.go`
  - `src/go/pkg/netipc/service/raw/more_windows_test.go`
- POSIX service interop currently uses snapshot fixtures:
  - `tests/fixtures/c/interop_service.c`
  - `tests/fixtures/rust/src/bin/interop_service.rs`
  - `tests/fixtures/go/cmd/interop_service/main.go`
  - `tests/test_service_interop.sh`
  - `tests/test_service_shm_interop.sh`
- Windows service interop has the same snapshot-oriented pattern:
  - `tests/fixtures/c/interop_service_win.c`
  - `tests/fixtures/rust/src/bin/interop_service_win.rs`
  - `tests/fixtures/go/cmd/interop_service_win/main.go`
  - `tests/test_service_win_interop.sh`
  - `tests/test_service_win_shm_interop.sh`

### Implementation Surfaces

- Protocol/status contract:
  - `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
  - `docs/codec-apps-lookup.md`
  - `docs/codec-cgroups-lookup.md`
  - C/Rust/Go lookup codec constants, status validators, and any response stitch/merge primitive needed to preserve layer boundaries.
- C Level 2:
  - `src/libnetdata/netipc/src/service/netipc_service_apps_lookup.c`
  - `src/libnetdata/netipc/src/service/netipc_service_cgroups_lookup.c`
  - optional shared lookup split/stitch helper under `src/libnetdata/netipc/src/service/` if it reduces duplication without crossing codec ownership boundaries.
- Rust Level 2:
  - `src/crates/netipc/src/service/raw/apps_lookup.rs`
  - `src/crates/netipc/src/service/raw/cgroups_lookup.rs`
  - public facades in `src/crates/netipc/src/service/apps_lookup.rs` and `src/crates/netipc/src/service/cgroups_lookup.rs` if API docs or status exposure change.
- Go Level 2:
  - `src/go/pkg/netipc/service/raw/apps_lookup.go`
  - `src/go/pkg/netipc/service/raw/cgroups_lookup.go`
  - public packages under `src/go/pkg/netipc/service/apps_lookup/` and `src/go/pkg/netipc/service/cgroups_lookup/` if API docs or status exposure change.
- Docs and operator guidance:
  - `docs/level2-typed-api.md`
  - `docs/level1-wire-envelope.md` only if wording must clarify transport cap versus Level 2 logical calls.
  - `docs/netipc-integrator-skill.md`
  - `.agents/sow/specs/` durable project memory for Level 2 scale invariants.

### Planned New Or Updated Tests

- C unit/integration tests:
  - Add a focused POSIX fixture such as `tests/fixtures/c/test_lookup_scale.c` instead of burying all cases in already-large service tests.
  - Add a Windows counterpart such as `tests/fixtures/c/test_win_lookup_scale.c`.
  - Register both in `CMakeLists.txt`, coverage scripts, sanitizer scripts where applicable, and Windows validation scripts.
- Rust tests:
  - Add dedicated lookup scale test modules/files rather than expanding already-large raw service test files indefinitely.
  - Cover POSIX and Windows with `#[cfg(unix)]` / `#[cfg(windows)]` slices or separate files matching the existing test organization.
- Go tests:
  - Add dedicated lookup scale tests under `src/go/pkg/netipc/service/raw/` and public facade packages as needed.
  - Update `tests/run-coverage-go.sh` and `tests/run-coverage-go-windows.sh` so lookup service packages are measured.
- Codec interop:
  - Extend `tests/interop_codec.sh` and C/Rust/Go codec fixtures with new per-item oversized/not-enriched status samples if the protocol status vocabulary changes.
  - If a new wire status is selected, update all C/Rust/Go codec constants, docs, byte-identical fixtures, and cross-decode fixtures in one change.
- Service interop:
  - Add lookup-scale interop binaries:
    - `tests/fixtures/c/interop_lookup_scale.c`
    - `tests/fixtures/rust/src/bin/interop_lookup_scale.rs`
    - `tests/fixtures/go/cmd/interop_lookup_scale/main.go`
    - Windows equivalents if separate binaries are needed by platform.
  - Add scripts:
    - `tests/test_lookup_scale_interop.sh`
    - `tests/test_lookup_scale_shm_interop.sh`
    - `tests/test_lookup_scale_win_interop.sh`
    - `tests/test_lookup_scale_win_shm_interop.sh`
  - Run the full 3x3 directed client/server matrix for C, Rust, and Go.

### Required Test Matrix

Every row below must have coverage in C, Rust, and Go. Cross-language rows must also run every directed client/server pair supported by the platform.

1. Fast-fail cases:
   - endpoint absent before call
   - endpoint disappears before first subcall
   - endpoint disappears after partial logical progress
   - auth failure
   - incompatible method/profile/layout
   - malformed or corrupt response
   - timeout before completion
   - caller abort before and during completion
   Expected result: explicit failure, partial subcall results discarded, no wait-for-plugin-to-return loop, no repair heuristics, and no retry for malformed/corrupt responses.

2. Valid scale cases:
   - zero items
   - one item
   - duplicate keys
   - sorted and unsorted input order
   - exact request payload boundary
   - request payload boundary plus/minus one byte
   - exact response payload boundary
   - response payload boundary plus/minus one byte
   - `8192` items matching topology-containers pressure
   - at least `32768` items for large-host/HPC coverage
   - APPS_LOOKUP request-overflow boundary above the fixed-size request threshold, which is much higher than `8192`
   Expected result: one logical response, same item count, same order, correct per-item statuses.

3. Request-side split cases:
   - APPS_LOOKUP PID count requires multiple requests.
   - CGROUPS_LOOKUP path list requires multiple requests.
   - one request key is too large to encode in a transport message.
   Expected result: valid partitioning for representable items; oversized request-key handling must follow the chosen item-level outcome contract instead of poisoning the whole batch. The SOW must distinguish current APPS_LOOKUP fixed-size keys from CGROUPS_LOOKUP variable-length path keys and future variable-key codecs.

4. Response-side split cases:
   - known APPS_LOOKUP responses with large cgroup paths/names/labels
   - known CGROUPS_LOOKUP responses with large names/labels
   - mixed small and large items
   - one item with huge valid metadata, such as a very large label
   - server marks the first item that does not fit the current response and all following items as `PAYLOAD_EXCEEDED`
   - Level 2 client repeats only the `PAYLOAD_EXCEEDED` suffix until it has a complete logical response
   - a single item that still does not fit alone at maximum payload capacity becomes `OVERSIZED_ITEM`
   Expected result: split/stitch succeeds for representable items; `PAYLOAD_EXCEEDED` is consumed internally by Level 2; one oversized item returns an explicit non-retriable item-level outcome and later items still succeed when possible.

5. Per-item status cases:
   - known
   - unknown/retry-later where applicable
   - unknown/permanent where applicable
   - host-root where applicable
   - `PAYLOAD_EXCEEDED`
   - `OVERSIZED_ITEM`
   - mixed statuses across split boundaries
   Expected result: status semantics are preserved and not overloaded with misleading business meaning. `PAYLOAD_EXCEEDED` is a Level 2-internal retriable item outcome. `OVERSIZED_ITEM` is a non-retriable item outcome. `HOST_ROOT` applies only where the codec actually defines it, such as APPS_LOOKUP cgroup status.

6. Malformed stitched-response cases:
   - wrong item count
   - response item key does not match requested key
   - reordered response items
   - duplicate response items
   - truncated response
   - unknown status enum
   - invalid status-dependent fields
   - invalid label table layout
   - valid response for subcall 1 followed by malformed response for subcall 2
   Expected result: whole logical call fails clearly, partial results are discarded, and NetIPC must not guess or repair corrupt data.

7. Retry and capacity cases:
   - normal capacity growth succeeds
   - repeated overflow with no capacity growth fails boundedly
   - overflow after some subcalls succeeds by splitting smaller
   - proactive split estimate is too large, subcall N overflows, adaptive split reduces only that unresolved range
   - server-side partial response marks the overflow suffix as `PAYLOAD_EXCEEDED`
   - Level 2 follow-up call starts at the first `PAYLOAD_EXCEEDED` item
   - overflow after reaching single-item floor becomes `OVERSIZED_ITEM` only when the item is valid but too large to fit alone at maximum payload capacity
   - overflow caused by corrupt/malformed server output remains whole-call failure
   Expected result: reconnect count per logical call is bounded and recorded because `LIMIT_EXCEEDED` closes the current session.

8. Resource and memory cases:
   - constrained response capacity
   - constrained request capacity
   - bounded stitch-buffer growth
   - allocation failure where the language/test harness can inject or simulate it
   - no unbounded recursion or loop
   Expected result: either success under bounded memory or explicit failure with no partial silent result.

9. Transport/platform cases:
   - POSIX baseline
   - POSIX SHM profile where supported
   - Windows Named Pipe
   - Windows SHM profile where supported
   - fallback from failed local SHM attach remains limited to existing documented behavior and does not become a generic retry policy.

10. Performance and scale cases:
    - split count is bounded and explainable
    - no quadratic stitch algorithm for normal large calls
    - no per-item connection setup in valid large calls
   - benchmark or stress rows for `8192`, `32768`, and a heavier stress-only cardinality appropriate for tens-of-thousands deployment claims
   - benchmark output records subcall count, reconnect count, item count, payload bytes, elapsed time, and throughput where practical.
   - benchmark floors or parity analysis gates are defined before close; informational benchmarks alone are not sufficient for performance claims.

### Validation Gates

- Fast CI gate:
  - deterministic boundary and representative large tests in C, Rust, and Go
  - codec interop when protocol status vocabulary changes
  - POSIX baseline and SHM interop matrix
- Windows gate:
  - Windows C/Rust/Go lookup scale tests
  - Named Pipe and Windows SHM interop matrix where supported
  - MSYS validation script updated to include lookup scale targets
- Stress gate:
  - heavier cardinality runs outside the fastest CI slice
  - memory-constrained runs
  - benchmark/stress result artifacts summarized in the SOW before close
- Coverage gate:
  - C coverage source list updated for split protocol/service files touched by this SOW
  - Rust coverage includes lookup service code touched by this SOW
  - Go coverage includes lookup protocol, raw service, and public typed packages touched by this SOW
  - Minimum explicit Go package list includes `./pkg/netipc/service/raw/`, `./pkg/netipc/service/apps_lookup/`, and `./pkg/netipc/service/cgroups_lookup/`.
  - C coverage source list includes `netipc_service_apps_lookup.c`, `netipc_service_cgroups_lookup.c`, and any new split/stitch helper or codec merge files.

### Non-Goals And Guardrails

- Do not add retries for absent endpoints. A missing provider is failure for that call.
- Do not wait for a dead plugin to come back.
- Do not repair malformed or corrupt payloads.
- Do not add checksums, cryptographic hashes, payload hashing, full-payload
  integrity scans, or heuristic corruption recovery to the hot path. Corrupt
  or malformed data detection is limited to structural validation required for
  safe decoding: bounds, declared lengths, alignment, layout/version/status,
  item counts, echoed keys, required NUL terminators, and strict generation
  matching.
- Do not hide item omissions. Every non-enriched item needs an explicit item-level outcome.
- Do not move transport payload limits into application callers.
- Do not accept C-only, Rust-only, or Go-only proof for a shared Level 2 contract.
- Do not put wire-format response assembly logic in service modules. If one stitched wire response is returned, response assembly must live in the codec layer and be orchestrated by service code.

### Reviewer-Discovered Blockers To Resolve Before Implementation

Five external reviewers returned usable read-only findings. One requested reviewer did not return usable output after two attempts and was stopped. The following blockers and gaps must be resolved before implementation starts:

1. Oversized item outcome mechanism:
   - Current wire status enums do not include exact `PAYLOAD_EXCEEDED` or `OVERSIZED_ITEM` statuses.
   - Adding a wire status affects C/Rust/Go decoders because unknown statuses are currently rejected.
   - The server library should detect response payload pressure and emit `PAYLOAD_EXCEEDED` for the first item that does not fit and every following item in that response.
   - The server library should emit `OVERSIZED_ITEM` when it can determine a single item cannot fit alone in the maximum payload.
   - If a server cannot determine that an item is oversized until it is retried alone, it may first emit `PAYLOAD_EXCEEDED`; the Level 2 client then retries from that item, allowing the server to emit `OVERSIZED_ITEM` on the single-item attempt.

2. SOW-0007 contradiction:
   - `SOW-0007` says oversized single items fail with overflow/all-or-error.
   - This SOW replaces that with item-level outcome semantics.
   - `SOW-0007` must be closed or explicitly marked superseded before this SOW moves to implementation.

3. Generation consistency:
   - APPS_LOOKUP and CGROUPS_LOOKUP responses carry generation fields.
   - Split subresponses can carry different generations if the provider changes during a logical call.
   - Mixed generations always fail the logical call. NetIPC must not select or invent one generation for a mixed snapshot.

4. Client/server subcall contract:
   - Split/stitch should be defined as multiple complete independent request/response cycles.
   - Servers do not receive a continuation marker and do not know a request is part of a larger logical call.
   - Mid-call peer failure must have a defined outcome. Current recommendation: fail the whole logical call and discard partial results.

5. Server-side overflow classification:
   - C common server code maps `NIPC_ERR_OVERFLOW` to `LIMIT_EXCEEDED`, but every language must preserve that distinction.
   - Handler failure, malformed data, and corrupt data must not be treated as valid scale overflow.
   - Tests must prove builder overflow stays overflow and handler failure stays failure in C, Rust, and Go.

6. Stitching memory model and layer boundary:
   - Returning the existing borrowed view shape requires a stable client-owned stitched response.
   - If the implementation assembles a wire response, the merge operation belongs in codec code, not service code.
   - Go view lifetime and C/Rust borrowed-view lifetime must be specified, including reuse on the next typed call.

7. Logical-call limits:
   - The SOW must define maximum logical response bytes, maximum logical item count, maximum payload budgets, and maximum subcall count.
   - These limits must be initialization-configurable, with documented defaults. They must not be hardcoded literals in C, Rust, or Go implementation paths.
   - Exceeding these limits is a clear whole-call error, not OOM or silent truncation.

8. Deadline and abort:
   - Timeout is one absolute deadline for the logical call.
   - Each subcall gets only the remaining budget.
   - Abort is checked before each subcall and partial results are discarded.

9. Split heuristic and reconnect cost:
   - Request split uses exact encoded request size and negotiated request cap.
   - Response split is primarily driven by server-emitted `PAYLOAD_EXCEEDED` suffixes because the server discovers the true encoded response size while building it.
   - Adaptive overflow can kill the current session; reconnect count must be bounded and measured.

10. Test precision:
    - `8192` and `32768` APPS_LOOKUP requests stress response size, not request overflow, because APPS_LOOKUP request items are fixed-size.
    - CGROUPS_LOOKUP variable path keys and future variable-key codecs need separate request-side oversized-key tests.
    - `HOST_ROOT` must only be tested where defined by the codec.

11. Spec drift:
    - `docs/level1-wire-envelope.md` must list the current lookup method IDs.
    - `docs/level2-typed-api.md` must clarify that large logical lookup calls may take multiple Level 1 messages even though each message stays under the per-message cap.
    - The existing "Batch splitting (planned)" section must be disambiguated from lookup split/stitch, which does not use `NIPC_FLAG_BATCH`.

12. Benchmarks and CI:
    - Existing lookup benchmarks cover small item counts such as 16 and 256, not topology-scale logical calls.
    - New 8192/32768/stress scenarios need calibrated floors or written parity/performance analysis gates.
    - Coverage scripts must be updated to measure the new code, not only run tests.

## Pre-Implementation Gate

Status: ready-for-implementation

Problem / root-cause model:

- Level 2 currently exposes a single logical typed lookup as one transport request/response pair plus bounded reconnect-on-overflow. That works for normal requests and for capacity-growth recovery, but it fails when the logical response is valid yet too large for one configured message payload budget.
- The root cause is that Level 2 does not yet own logical-call partitioning. The transport cap remains visible to consumers through `NIPC_ERR_OVERFLOW` / equivalent errors.
- For Netdata-scale deployments, a valid APPS_LOOKUP or CGROUPS_LOOKUP request may need multiple transport messages even though the caller should see one typed call.
- A single oversized item's metadata is not the same kind of failure as a corrupt message or missing endpoint. It is an item-level enrichment outcome and must not become a whole-call error when the rest of the batch is valid.

Evidence reviewed:

- `SOW-0007` already records that typed APIs should own oversized semantic batching for request payloads.
- `docs/level2-typed-api.md` previously blurred per-call wire buffer sizing with initialization-time payload-budget policy; this SOW clarifies that typed callers do not pass per-call buffers, but consumers may configure payload ceilings during initialization.
- `docs/level1-wire-envelope.md` and `netipc_protocol.h` currently preserve a fixed payload cap/default in places; this SOW must replace fixed-cap wording and hardcoded call-path literals with configured or documented-default ceilings.
- topology-containers `network-viewer` can send `8192` PIDs in one APPS_LOOKUP call.
- topology-containers `apps.plugin` accepts `8192` PIDs per request.
- APPS_LOOKUP responses include variable-size fields, so valid known PID metadata can exceed one configured payload budget while the request itself remains valid.
- APPS_LOOKUP and CGROUPS_LOOKUP wire formats already include per-item status fields, but the current enums need review before using them for oversized-item outcomes.

Affected contracts and surfaces:

- C Level 2 APPS_LOOKUP and CGROUPS_LOOKUP client APIs.
- Rust Level 2 APPS_LOOKUP and CGROUPS_LOOKUP client APIs.
- Go Level 2 APPS_LOOKUP and CGROUPS_LOOKUP client APIs.
- Lookup response view lifetime and ownership contracts.
- Per-item status enum semantics and possible wire-compatible or wire-versioned extensions.
- Timeout, abort, reconnect, and overflow semantics.
- `docs/level2-typed-api.md`
- `docs/level1-wire-envelope.md` if wording needs to clarify transport cap versus logical Level 2 calls.
- `docs/code-organization.md` if scalability helpers affect ownership boundaries.
- `docs/netipc-integrator-skill.md`
- Project SOW specs under `.agents/sow/specs/` if the rule becomes durable project memory.
- Tests, interop tests, benchmarks, and downstream vendor guidance.

Existing patterns to reuse:

- Existing lookup request encoders know exact encoded request size.
- Existing lookup response codecs preserve ordered item directories.
- Existing lookup responses already carry per-item status fields that can preserve batch-level completion while reporting item-specific outcomes.
- Existing service call retry path already handles bounded overflow recovery.
- Existing borrowed response views already define client-owned reusable response storage.
- Existing C/Rust/Go test families cover lookup codecs, services, raw transport, and cross-language interop.

Risk and blast radius:

- High API contract impact: this defines what Level 2 promises for all current and future typed service APIs.
- Medium implementation risk in C due to borrowed view lifetime and stitched response buffer ownership.
- Medium parity risk across Rust and Go.
- Medium performance risk if huge calls create too many subcalls or allocate excessive stitch buffers.
- Low wire compatibility risk if split/stitch is implemented above Level 1 using existing wire messages.
- Operational risk if defaults allow unbounded memory growth; logical calls still need explicit configurable ceilings, timeout behavior, and clear failure modes.
- Extreme-case test risk: huge synthetic tests can be slow or memory-heavy. The test suite needs a tiered design so fast CI covers boundaries and representative large cases, while heavier stress/benchmark jobs cover huge deployment-scale scenarios.
- Wire/API compatibility risk if a new per-item status is required. This must be handled deliberately instead of silently changing the meaning of existing statuses.
- Test coverage risk: if a corner case is only tested in one language, language-specific codec, allocation, integer-width, lifetime, timeout, or transport differences can still break production.

Sensitive data handling plan:

- Use synthetic PIDs, cgroup paths, names, labels, service names, and generated payloads only.
- Do not record live process lists, container identifiers, customer names, personal data, private endpoints, secrets, tokens, SNMP communities, or proprietary incident details in SOWs, specs, docs, skills, instructions, tests, or code comments.
- If topology-containers or Netdata evidence is needed, cite file paths, constants, and behavior only; do not paste live runtime data.

Implementation plan:

1. Consolidate scope by updating or closing `SOW-0007` as superseded/merged, so this SOW is the only active scale contract implementation track.
2. Update docs/specs first to define the Level 2 scale contract: logical calls, transport caps hidden from consumers, split/stitch, item-level outcomes, timeout/abort over the complete logical call, and fail-fast behavior for true call/session/protocol failures.
3. Decide and document the per-item outcome vocabulary for oversized valid metadata before implementation, keeping NetIPC and Netdata business semantics aligned.
4. Add fail-first tests for C APPS_LOOKUP and CGROUPS_LOOKUP response overflow, request overflow, large logical calls, item order preservation, per-item status preservation, timeout, abort, and oversized single-item item-level outcomes.
5. Add an adversarial test matrix before implementation: exact payload boundaries, one-byte-over boundaries, zero items, duplicate items, enormous item counts that exercise integer overflow guards, oversized dynamic strings/labels, malformed partial responses, mismatched response counts, mixed statuses, reconnects after partial progress, repeated overflow without capacity growth, timeout across multiple subcalls, abort across multiple subcalls, and memory-pressure paths.
6. Design and implement C lookup split/stitch using internal bounded subcalls and one caller-visible logical response view.
7. Implement the same scale behavior in Rust and Go.
8. Add cross-language interop and scale benchmarks for small, boundary, large, and huge lookup calls.
9. Update integration guidance, output/reference skills, and any runtime project skill needed to preserve this pattern for future services.

Validation plan:

- Unit tests for request-side and response-side split/stitch in C, Rust, and Go.
- Cross-language lookup interop tests where C, Rust, and Go clients call C, Rust, and Go servers with large logical requests.
- Timeout and abort tests over multi-subcall logical calls in C, Rust, and Go.
- Same-failure searches for direct `NIPC_ERR_OVERFLOW` propagation from Level 2 lookup callers.
- Benchmark or scale test with at least `8192` and `32768` synthetic items and large variable metadata.
- Boundary and negative tests in C, Rust, and Go for exact configured payload ceilings, ceiling minus one byte, ceiling plus one byte, default ceilings, override ceilings, oversized single-item payloads, count multiplication overflow, directory offset overflow, malformed stitched responses, duplicate request keys, zero-item calls, repeated overflow, no-growth overflow, and memory allocation pressure.
- Tests in C, Rust, and Go where one item has oversized valid metadata, such as a huge label, and the logical response still returns successful enrichment for other items plus an explicit item-level not-enriched outcome for that item.
- Tests in C, Rust, and Go proving corrupt/malformed/faulty responses fail the whole call rather than being repaired or guessed around.
- Tiered scale validation so fast CI covers deterministic boundary cases and representative large calls, while stress/benchmark jobs cover huge synthetic workloads appropriate for Netdata's deployment range.
- POSIX and Windows validation for the same logical contract in every supported language/runtime combination that the repository supports.

Artifact impact plan:

- AGENTS.md: likely update only if the Level 2 scale contract becomes a project-wide workflow guardrail for future work.
- Runtime project skills: create or update a `project-*` skill only if there is reusable implementation/review workflow guidance; no runtime project skill exists today.
- Specs: update Level 2 typed API docs and SOW specs with the durable scale contract.
- End-user/operator docs: update public integration guidance where Level 2 consumer expectations are documented.
- End-user/operator skills: update `docs/netipc-integrator-skill.md` prominently.
- SOW lifecycle: merge/supersede `SOW-0007` before implementation or record why both remain separate without overlap.

Open-source reference evidence:

- None checked for this SOW creation. This is primarily an internal API contract caused by NetIPC's own transport cap and typed lookup semantics. Implementation may still inspect local mirrored projects for batching/stitching patterns before coding.

Resolved decisions:

1. SOW consolidation:
   - Decision: SOW-0021 supersedes `SOW-0007`, and `SOW-0007` is closed before implementation.
   - Reason: the topology evidence proves the old SOW is too narrow, and two overlapping SOWs would increase risk.
2. Initial implementation scope:
   - Decision: C, Rust, and Go are all in scope for this SOW. The SOW cannot complete with a staged language gap for the corner-case matrix.
3. Stitching model:
   - Decision: assemble one internal response buffer and return the existing borrowed view shape.
   - Reason: this preserves the dead-simple consumer API while keeping transport mechanics inside Level 2.
4. Split strategy:
   - Decision: use proactive split from encoded request and conservative estimated response budget, with adaptive retry-on-valid-overflow.
   - Reason: proactive splitting reduces avoidable overflow, while adaptive retry handles dynamic response sizes.
5. Oversized single-item outcome:
   - Decision: add explicit per-item status values for `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM`.
   - Reason: existing unknown statuses do not mean response-cap retry or valid-but-too-large, and using them would create misleading business semantics.
   - `PAYLOAD_EXCEEDED` is retriable only inside the Level 2 client; application callers never manage it.
   - `OVERSIZED_ITEM` is non-retriable and remains the final item outcome.
6. Generation consistency:
   - Decision: fail the logical call if split subresponses have different generations.
   - Reason: mixed generations are globally unsupported. Silently stitching mixed snapshots would make the response look consistent when it is not.
7. Logical-call ceilings:
   - Decision: define and enforce memory, item-count, payload-budget, and subcall ceilings through initialization config and documented defaults.
   - Reason: this protects memory-constrained systems and lets large-memory deployments scale without changing source code. The public API remains simple because callers configure policy once; they do not split calls or pass scratch buffers per request.

## Implications And Decisions

Implementation-shape decisions have been recorded. No staged language gaps or deferred SOW-0021 requirements are allowed.

Recorded user decisions:

1. Level 2 consumers should not manage scale mechanics; the library must handle small, big, and huge valid calls.
2. Missing endpoints, dead plugins, corrupt messages, malformed payloads, incompatible protocols, auth failures, timeouts, and aborts are failures and should fail fast.
3. Valid large data is not failure and must be handled as normal flow.
4. A single problematic or oversized item must not invalidate the entire batch. It must produce an item-level outcome while other items continue to be enriched.
5. All corner cases must be fully tested in C, Rust, and Go. One-language coverage or inference is not sufficient.
6. SOW-0021 supersedes SOW-0007 and is the only active implementation track for lookup scale behavior.
7. Long-term-best choices are required: explicit item-level outcome semantics, existing consumer-facing view shape, server-guided response overflow retry, strict generation consistency, and configurable bounded logical calls.
8. Do not introduce functionality outside SOW-0021's required scale, correctness, testing, benchmark, documentation, vendoring, and PR scope.
9. When a response fills before all requested items are encoded, the server marks the first unfit item and all remaining request items as `PAYLOAD_EXCEEDED`; the Level 2 client retries from that item transparently.
10. When a single item cannot fit by itself in a maximum-size payload, the server marks it as `OVERSIZED_ITEM`; the Level 2 client keeps that item outcome and continues enriching later items when possible.
11. Logical call ceilings and payload budgets must be configurable at client/server initialization or through documented defaults; no C/Rust/Go implementation path may rely on scattered hardcoded numeric limits.
12. `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM` are standard response outcomes for every lookup implementation in every supported language.
13. Mixed generations are globally unsupported. Plugins/providers and clients must match the documented NetIPC contract exactly. There is no backward-compatibility, forward-compatibility, or best-effort compatibility mode for method/layout/status/generation drift.
14. Internal scale ceilings must come from initialization config or named overrideable defaults. Consumers must be able to tune request and response payload budgets for the deployment class, from small IoT systems to large-memory HPC systems.
15. Mixed-generation and provider/client compatibility policy is not a per-feature decision. Future work must treat exact contract matching and mixed-generation rejection as global NetIPC rules unless the global specs are explicitly revised.
16. Named payload defaults are not protocol hard limits. Explicit client/server initialization config is the deployment authority, and implementation paths must not embed deployment-size literals as hidden ceilings.
17. Corruption detection must not hurt IPC performance. NetIPC must not add checksums, cryptographic hashes, payload hashing, full-payload integrity scans, or heuristic repair. The allowed checks are only structural validation needed to safely decode and reject bad data.

## Plan

1. Record the scale contract in docs/specs before coding so implementation has a stable target.
2. Merge/supersede `SOW-0007` to avoid duplicate lookup split/stitch tracks.
3. Decide the item-level status contract for oversized valid metadata without drifting from Netdata business semantics.
4. Build fail-first tests around topology-scale lookup responses that exceed one transport message.
5. Build the adversarial matrix for overflow, oversized, malformed, timeout, abort, retry, reconnect, memory-pressure, and integer-boundary cases before implementation.
6. Implement C split/stitch with bounded subcalls and unchanged consumer API.
7. Implement and validate the same contract in Rust and Go.
8. Add scale benchmarks and interop validation.
9. Update integrator guidance and skills so future Level 2 APIs follow the same scale pattern.

## Execution Log

### 2026-06-13

- Created SOW after topology-containers exposed that valid large APPS_LOOKUP responses can exceed one transport message and should be handled by Level 2 rather than by consumers.
- Added the requirement that strange, oversized, overflow, malformed, and deployment-extreme cases are first-class tests, not optional hardening.
- Recorded the item-level failure rule: oversized or otherwise not-enrichable single items must not poison the whole logical batch, and NetIPC's item outcomes must stay aligned with Netdata business semantics.
- Recorded the all-language validation rule: every corner case must be tested in C, Rust, and Go before the SOW can complete.
- Moved SOW-0021 to implementation after recording the required long-term-best decisions and closing SOW-0007 as superseded.
- Promoted the clarified global rules into docs and implementation guidance: standard lookup oversized-response outcomes in all languages, no mixed-generation support, no compatibility mode for provider/client drift, and no hidden hardcoded deployment ceilings.
- Recorded the response-overflow item semantics: `PAYLOAD_EXCEEDED` drives transparent Level 2 follow-up calls, while `OVERSIZED_ITEM` is the non-retriable final outcome for a single item that cannot fit alone.
- Recorded that mixed generations are globally unsupported and always reject the logical call.
- Recorded that scale ceilings and payload budgets must come from initialization config or documented defaults, not hardcoded literals.
- Recorded the stronger contract that lookup scale statuses are standard across all C/Rust/Go implementations, mixed generations and compatibility drift are globally rejected, and payload ceilings must be consumer-configurable or named defaults.
- Updated the public docs and integrator skill wording so strict provider/client contract matching, no mixed-generation compatibility, standard lookup scale statuses, and initialization-tunable ceilings are visible outside the SOW.
- Implemented and documented the variable-length request-key oversized case for `CGROUPS_LOOKUP`: a valid path that cannot fit into one configured request payload is represented as an item-level `OVERSIZED_ITEM`, the oversized key is not sent to the server, and later keys continue normally.
- Reworked lookup retry scope so logical overflows do not use the generic transport retry wrapper. The wrapper is now used only for raw transport subcalls; logical item/subcall/stitched-response ceilings fail directly.
- Added a request-capacity preflight path for stale or artificially small session payload caps. If the configured request budget can fit one lookup item, the client reconnects before classifying the item as oversized. If the configured budget cannot fit the one item, `CGROUPS_LOOKUP` produces `OVERSIZED_ITEM`.
- Fixed the cross-language distinction between "session cap too small" and "configured cap too small" in C, Go, and Rust.
- Moved logical subcall accounting to the actual raw request point. Capacity preflight reconnects and local `OVERSIZED_ITEM` synthesis do not consume subcall budget because no service request was sent.
- Validated the focused POSIX and Windows lookup paths after the retry-scope and request-capacity changes.
- Updated POSIX C, Windows C, POSIX Go, and Windows Go coverage scripts so the new lookup protocol/service files and typed lookup packages are measured.
- Added Go tests for raw lookup response stitching, public typed lookup wrapper coverage, typed transport-config mapping, POSIX receive timeout/abort paths, lookup request-boundary helpers, and `PAYLOAD_EXCEEDED` suffix builder behavior.
- Measured initial coverage gates after script updates. At that point, coverage was still below the configured 90% gates:
  - Go POSIX coverage: `85.4%` total; weakest remaining relevant files include `service/raw/apps_lookup.go` at `75.7%`, `service/raw/cgroups_lookup.go` at `78.4%`, and lookup protocol files at about `82.9-84.9%`.
  - C POSIX coverage: `87.6%` total; weakest remaining relevant files are `netipc_service_apps_lookup.c` at `77.5%` and `netipc_service_cgroups_lookup.c` at `78.6%`.
  - Rust POSIX coverage: `89.87%` total; weakest remaining relevant files include public typed wrappers `service/apps_lookup.rs` at `61.93%`, `service/cgroups_lookup.rs` at `61.58%`, and raw client/call paths.
  - These are real test gaps, not script noise; SOW-0021 remains open until the coverage gates and planned adversarial matrix pass.
- Added Rust public facade lookup coverage for APPS_LOOKUP and CGROUPS_LOOKUP on Unix, including successful round trips, not-ready/abort public methods, logical item limits, and managed-server constructor paths.
- Re-ran Rust coverage after the public facade tests. `bash tests/run-coverage-rust.sh` now passes with `91.54%` total line coverage. Rust coverage is no longer a coverage-gate blocker, but the broader adversarial matrix still requires Rust parity coverage for every new scale case.
- Recorded the performance guardrail for corruption detection: validation is structural only and limited to safe decode checks. NetIPC must not add checksums, cryptographic hashes, payload hashing, full-payload integrity scans, or repair heuristics to this high-performance memory-to-memory IPC path.
- Added Go branch coverage for structural decode guards, configurable listener payload limit mutation, snapshot dispatch failures, and client timeout/abort/capacity helpers.
- Re-ran Go coverage after additional protocol/raw/POSIX guard tests. `bash tests/run-coverage-go.sh` still fails the configured 90% gate, but improved to `89.3%` total coverage (`3223/3611` statements). Go tests pass; Go coverage remains a blocker by 27 covered statements.
- Re-ran C coverage after adding the typed lookup zero-item test. `bash tests/run-coverage-c.sh` still fails the configured 90% gate at `87.9%` total coverage (`2239/2547` lines). All C tests pass; the largest real remaining gaps are `netipc_service_apps_lookup.c` at `79.2%`, `netipc_service_cgroups_lookup.c` at `79.9%`, and `netipc_shm.c` at `86.9%`.
- Added more Go coverage for lookup protocol guard branches, Windows lookup request-capacity reconnect paths, bounded Windows overflow retry behavior, Windows named-pipe timeout/readiness behavior, and Windows listener defensive error branches.
- Removed an unused Windows Go handshake-local `sendRejection()` helper after confirming the shared framing layer owns rejection ACK sending.
- Re-ran Windows Go coverage in an isolated `win11` temp checkout. `MSYSTEM=MSYS bash tests/run-coverage-go-windows.sh` now passes the configured 90% gate with `90.0%` total statement coverage (`3299/3665`).
- Re-ran POSIX Go coverage. `bash tests/run-coverage-go.sh` now passes the configured 90% gate with `90.0%` total statement coverage (`3251/3611`).
- Go coverage is no longer a coverage-gate blocker. At this point in the log, C coverage remained open, and the broader adversarial matrix, interop, benchmarks, and downstream validation remained open.
- Added C common-service helper coverage in `test_service_extra` for safe C-string copying, `uint32_t` header-length overflow, timeout default resolution, batch response-header propagation, response status mapping, SHM request overflow, and dispatch-result limit handling.
- Re-ran C coverage. `bash tests/run-coverage-c.sh` now passes the configured 90% gate with all measured files at or above 90%; `netipc_service_common.c` improved to `93.3%`, and total C coverage is `91.8%` (`2329/2537`).
- C, Rust, POSIX Go, and Windows Go coverage gates now pass. SOW-0021 remains open for the broader adversarial matrix and downstream topology-containers validation.
- Added explicit large logical-call tests for both APPS_LOOKUP and CGROUPS_LOOKUP at `8192` and `32768` items:
  - POSIX C: `tests/fixtures/c/test_service.c::test_lookup_large_logical_calls`
  - Windows C: `tests/fixtures/c/test_win_service_extra.c::test_lookup_large_logical_calls`
  - POSIX Go: `src/go/pkg/netipc/service/raw/lookup_unix_test.go::TestLookupLargeLogicalCalls`
  - Windows Go: `src/go/pkg/netipc/service/raw/lookup_windows_test.go::TestWinLookupLargeLogicalCalls`
  - POSIX Rust: `src/crates/netipc/src/service/raw_unix_tests.rs::test_lookup_large_logical_calls`
  - Windows Rust: `src/crates/netipc/src/service/raw_windows_tests.rs::test_lookup_large_logical_calls_windows`
- The new large logical-call tests use synthetic ordered data, an `8192` byte request payload cap to force internal request fragmentation, full response item/order verification, generation checks, and assertions that each logical call used multiple request fragments rather than one transport message.
- Added POSIX lookup-scale cross-language interop coverage by extending the existing C/Rust/Go `interop_service` fixtures with lookup modes and adding `tests/test_lookup_scale_interop.sh`.
- Registered `test_lookup_scale_interop` and `test_lookup_scale_shm_interop` in CMake. Each script run covers APPS_LOOKUP and CGROUPS_LOOKUP across all 9 directed C/Rust/Go client/server pairs at `8192` items, for 18 directed checks per transport profile.
- Extended the POSIX lookup method benchmark matrix from 8 to 12 scenarios by adding all-known APPS_LOOKUP and CGROUPS_LOOKUP rows at `8192` and `32768` items.
- Updated the C, Go, and Rust POSIX benchmark drivers so lookup-method benchmarks allocate request/response buffers from the scenario item count instead of assuming the previous 256-item maximum.
- Updated the POSIX benchmark generator expected row count from `297` to `345`. At that point the existing checked-in `benchmarks-posix.csv` and `benchmarks-posix.md` were known-stale; they were regenerated in the 2026-06-14 validation pass below.
- Added Windows lookup-scale interop modes to the C, Go, and Rust `interop_service_win` fixtures for APPS_LOOKUP and CGROUPS_LOOKUP.
- Added `tests/test_lookup_scale_win_interop.sh` and `tests/test_lookup_scale_win_shm_interop.sh`, and registered both through CMake. Each Windows script covers APPS_LOOKUP and CGROUPS_LOOKUP across all 9 directed C/Rust/Go client/server pairs at `8192` items.
- Added Rust POSIX and Windows typed lookup malformed-response parity tests covering APPS_LOOKUP and CGROUPS_LOOKUP truncated responses, wrong item counts, echoed-key mismatches, all-`PAYLOAD_EXCEEDED` first-fragment overflow, and malformed `PAYLOAD_EXCEEDED` suffixes.

### 2026-06-14

- Continued benchmark validation after the lookup-scale implementation and found a POSIX benchmark interop configuration bug: Rust POSIX benchmark servers used the transport default request payload budget while C and Go benchmark clients initialized typed calls with a `4096` byte request budget. C/Go clients talking to a Rust benchmark server were rejected during HELLO/HELLO_ACK with `LIMIT_EXCEEDED`. Fixed the Rust POSIX benchmark client/server config so request and response batch/payload limits match the C and Go benchmark drivers.
- Fixed benchmark server-stop accounting problems that caused missing `SERVER_CPU_SEC` instead of usable rows:
  - Rust POSIX benchmark servers now wake the UDS accept loop after setting the running flag false.
  - Go benchmark servers now use a shared stop/wake helper.
  - Go POSIX raw service `Stop()` now closes the listener to unblock `Accept()`, matching the existing Windows listener-close pattern.
- Fixed POSIX benchmark readiness for Rust servers by waiting for both `READY` and the service socket path. Rust prints `READY` before the managed server binds; readiness based only on stdout could race the first client connection.
- Increased POSIX floor-retry diagnostics from `3` to `7` samples and taught the POSIX report generator to treat stable recovered retry evidence as `PASS (retry)` instead of a false floor failure. The runner now clears the matching retry diagnostics CSV at the start of each run so stale retry rows cannot pollute current evidence.
- Regenerated the full POSIX benchmark matrix with `timeout 7200 bash tests/run-posix-bench.sh benchmarks-posix.csv`. Result: exit code `0`, `345` data rows plus header in `benchmarks-posix.csv`.
- Regenerated the POSIX markdown report with `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md`. Result: exit code `0`; generator reported `All performance floors met`.
- Current POSIX retry evidence is limited to three stable 7-sample recoveries:
  - `snapshot-baseline` Go client to C server: original `92647`, floor `100000`, recovered median `107500`, stable ratio `1.110499`.
  - `snapshot-baseline` Go client to Go server: original `93605`, floor `100000`, recovered median `104212`, stable ratio `1.079027`.
  - `apps-lookup-mixed-16` Go client to Go server: original `296177`, floor `350000`, recovered median `377175`, stable ratio `1.163240`.
- Re-ran focused validation after the benchmark/lifecycle fixes:
  - `bash -n tests/run-posix-bench.sh tests/generate-benchmarks-posix.sh` passed.
  - `go test ./pkg/netipc/service/raw` passed in `98.265s`.
  - `go test ./pkg/netipc/service/internal/transportconfig ./pkg/netipc/transport/posix` passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml transport::posix --lib` passed with `62` tests.
  - `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_posix` passed.
- Updated `vendor-to-netdata.sh` post-vendor guidance so the optional Go import-path rewrite uses `sed -i.bak`, shows the diff, and then removes generated backup files after review. `bash -n vendor-to-netdata.sh tests/run-posix-bench.sh tests/generate-benchmarks-posix.sh` passed after the script edits.
- Ran full default Windows benchmark validation on `win11` from an isolated synced copy at `/tmp/plugin-ipc-sow0021-20260614052244`, excluding `.git`, `.env`, and build artifacts from the sync.
- The first Windows full-matrix attempt exposed a benchmark-driver config mismatch: Go Windows `cgroups_snapshot` typed configs did not set `MaxRequestPayloadBytes`, while C/Rust raw snapshot benchmark configs used `4096`. The focused failing row was `snapshot-baseline c->go @ max`, where the Go server died before `READY` because the warmup client could not complete the handshake.
- Fixed the Windows benchmark-driver parity issue by setting `MaxRequestPayloadBytes: 4096` in Go typed snapshot client/server configs and `max_request_payload_bytes: 4096` in the Rust typed snapshot client config. Kept benchmark warmup retry hardening limited to the explicit `not ready after retries` case.
- Re-ran the focused failing Windows row: `NIPC_BENCH_SCENARIOS=snapshot-baseline NIPC_BENCH_CLIENTS=c NIPC_BENCH_SERVERS=go NIPC_BENCH_TARGETS=0 timeout 900 bash tests/run-windows-bench.sh /tmp/snapshot-c-go.csv` passed with median throughput `101600` and stable ratio `1.107466`.
- Re-ran the full Windows benchmark matrix: `timeout 18000 bash tests/run-windows-bench.sh benchmarks-windows.csv` exited `0` and produced `201` measurements.
- Generated the Windows benchmark report: `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md` exited `0`, reported `All performance floors met`, and produced `benchmarks-windows.csv` with `202` lines plus `benchmarks-windows.md` with `393` lines.
- Added heavier stress-only interop evidence above the representative `32768` item tests:
  - POSIX baseline: `INTEROP_SVC_C=build-coverage/bin/interop_service_c INTEROP_SVC_GO=build-coverage/bin/interop_service_go INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service NIPC_LOOKUP_SCALE_ITEMS=65536 timeout 900 bash tests/test_lookup_scale_interop.sh` passed with `18 passed, 0 failed, 0 skipped`.
  - POSIX SHM: same command with `NIPC_PROFILE=shm` passed with `18 passed, 0 failed, 0 skipped`.
  - Windows Named Pipe on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260614052244`: `NIPC_LOOKUP_SCALE_ITEMS=65536 timeout 900 bash tests/test_lookup_scale_win_interop.sh` passed with `18 passed, 0 failed, 0 skipped`.
  - Windows SHM on the same copy: `NIPC_LOOKUP_SCALE_ITEMS=65536 timeout 900 bash tests/test_lookup_scale_win_shm_interop.sh` passed with `18 passed, 0 failed, 0 skipped`.
- The first Windows `65536` stress attempt exposed a test-harness cleanup bug, not a protocol failure. `tests/test_lookup_scale_win_interop.sh` used MSYS `kill` and then `taskkill.exe /PID` against an MSYS PID; native Windows C server processes could remain alive and stall the harness after a successful client call. Fixed the lookup-scale, service, and cache Windows interop scripts to resolve the native Windows PID via `ps -W` and call `taskkill.exe //PID`.
- Validated the Windows harness cleanup fix:
  - `bash -n tests/test_lookup_scale_win_interop.sh tests/test_lookup_scale_win_shm_interop.sh tests/test_service_win_interop.sh tests/test_cache_win_interop.sh` passed.
  - On `win11`, `timeout 600 bash tests/test_service_win_interop.sh` passed with `9 passed, 0 failed, 0 skipped`.
  - On `win11`, `timeout 600 bash tests/test_cache_win_interop.sh` passed with `9 passed, 0 failed, 0 skipped`.
- Added focused mid-logical-call timeout and abort tests after successful partial lookup progress:
  - Go POSIX and Windows raw lookup tests now force a `PAYLOAD_EXCEEDED` first subcall, delay the follow-up subcall, and verify timeout/abort applies to the whole logical call.
  - Rust POSIX and Windows raw lookup tests cover the same follow-up timeout/abort cases for APPS_LOOKUP and CGROUPS_LOOKUP.
  - C POSIX and Windows lookup scale handlers now support a controlled slow second subcall, and the fixtures verify timeout/abort after the second subcall is reached.
- Validated the new mid-logical timeout/abort tests:
  - POSIX Go focused tests passed: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run 'TestLookup(Timeout|Abort)DuringFollowupSubcall'`.
  - Windows Go focused tests passed on `win11`: `cd src/go && CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run "TestWinLookup(Timeout|Abort)DuringFollowupSubcall"`.
  - POSIX Rust focused lookup tests passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_ -- --nocapture`.
  - Windows Rust focused lookup tests passed on `win11`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_ -- --nocapture`.
  - POSIX C full service fixture passed: `timeout 900 build-coverage/bin/test_service` with `532 passed, 0 failed`.
  - Windows C focused follow-up filter passed on `win11`: `NIPC_TEST_FILTER=followup timeout 600 build-windows-focused/bin/test_win_service_extra.exe` with `38 passed, 0 failed`.
  - Windows C full focused-extra fixture passed on `win11`: `timeout 900 build-windows-focused/bin/test_win_service_extra.exe` with `404 passed, 0 failed`.
- Added Rust lookup-specific request-capacity reconnect parity tests. C and Go already had lookup-call tests where a stale negotiated request payload cap forces reconnect before a valid lookup request; Rust now covers the same APPS_LOOKUP and CGROUPS_LOOKUP paths on POSIX and Windows.
- Validated Rust lookup reconnect parity:
  - POSIX focused test passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_request_capacity_reconnects -- --nocapture`.
  - POSIX broader lookup filter passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_ -- --nocapture` with `7 passed, 0 failed`.
  - Windows focused test passed on `win11`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_request_capacity_reconnects_windows -- --nocapture`.
  - Windows broader lookup filter passed on `win11`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_ -- --nocapture` with `7 passed, 0 failed`.
- Tightened exact request-payload boundary coverage across C, Rust, and Go:
  - APPS_LOOKUP proactive request split tests now prove a `64` byte request payload cap carries exactly `3` items per request fragment.
  - CGROUPS_LOOKUP proactive request split tests now prove a `48` byte request payload cap carries exactly `2` seven-byte path items per request fragment.
  - These are exact-fit request boundary checks, not only "does not exceed cap" checks.
- Validated the exact request-boundary checks:
  - POSIX Go focused test passed: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupProactiveRequestSplit$'`.
  - POSIX Rust focused test passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split -- --nocapture`.
  - POSIX C full service fixture passed: `cmake --build build-coverage --target test_service -j12 && timeout 900 build-coverage/bin/test_service` with `532 passed, 0 failed`.
  - Windows Go focused test passed on `win11`: `cd src/go && CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupProactiveRequestSplitAndLimits$'`.
  - Windows Rust focused test passed on `win11`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split_windows -- --nocapture`.
  - Windows C focused proactive filter passed on `win11`: `NIPC_TEST_FILTER=proactive timeout 600 build-windows-focused/bin/test_win_service_extra.exe` with `22 passed, 0 failed`.
- Added remaining low-risk no-progress and bounded-memory parity tests:
  - Rust POSIX and Windows lookup logical-limit tests now cover APPS_LOOKUP and CGROUPS_LOOKUP logical response-byte ceilings, not only item/subcall ceilings.
  - Windows C now covers no-progress `PAYLOAD_EXCEEDED` responses where the first response item is retriable overflow. These fail with `NIPC_ERR_OVERFLOW` instead of spinning.
  - Windows C now covers APPS_LOOKUP and CGROUPS_LOOKUP logical response-byte ceilings plus APPS_LOOKUP and CGROUPS_LOOKUP subcall ceilings.
- Validated the new parity tests:
  - POSIX Rust focused test passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_logical_limits -- --nocapture`.
  - POSIX C full service fixture passed after the local changes: `cmake --build build-coverage --target test_service -j12 && timeout 900 build-coverage/bin/test_service` with `532 passed, 0 failed`.
  - Windows Rust focused test passed on `win11`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_logical_limits_windows -- --nocapture`.
  - Windows C focused logical-limit filter passed on `win11`: `NIPC_TEST_FILTER=logical_limits timeout 600 build-windows-focused/bin/test_win_service_extra.exe` with `40 passed, 0 failed`.
  - Windows C focused no-progress filter passed on `win11`: `NIPC_TEST_FILTER=no_progress timeout 600 build-windows-focused/bin/test_win_service_extra.exe` with `14 passed, 0 failed`.
  - Windows C full focused-extra fixture passed on `win11`: `timeout 900 build-windows-focused/bin/test_win_service_extra.exe` with `448 passed, 0 failed`.
- Added exact response-payload boundary coverage in C, Rust, and Go protocol builder tests:
  - APPS_LOOKUP and CGROUPS_LOOKUP responses now prove exact-size and exact-plus-one buffers keep all items known.
  - APPS_LOOKUP and CGROUPS_LOOKUP responses now prove an exact-minus-one buffer returns a decodable response whose overflow item is marked `PAYLOAD_EXCEEDED`.
- Validated the exact response-boundary tests:
  - Go focused protocol test passed: `cd src/go && go test -count=1 ./pkg/netipc/protocol -run TestLookupResponseExactAndShortBoundary`.
  - Rust focused protocol tests passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml lookup_response_exact_and_short_boundary -- --nocapture` with `2 passed, 0 failed`.
  - C protocol fixture passed: `cmake --build build-coverage --target test_protocol -j12 && timeout 600 build-coverage/bin/test_protocol` with `514 passed, 0 failed`. The run emitted a gcov profile checksum warning from rebuilding an existing coverage object, but the executable completed successfully.
- Ran baseline downstream topology-containers NetIPC-facing tests before vendoring any new SDK changes:
  - `timeout 600 build/apps-lookup-protocol-test` passed.
  - `timeout 600 build/cgroup-lookup-netipc-test` passed.
  - `timeout 600 build/network-viewer-apps-lookup-client-test` passed.
  - This proves the current downstream branch is healthy before vendoring, but it does not yet prove the new SOW-0021 SDK behavior in the downstream tree.
- Added Rust raw-call no-growth overflow retry parity tests:
  - POSIX helper `start_raw_overflow_no_growth_server()` accepts the reconnect but keeps negotiated response capacity unchanged, proving the client returns `NipcError::Overflow` instead of looping or issuing another request.
  - Windows helper does the same with `NpListener`.
  - Focused POSIX validation passed: `cargo test --manifest-path src/crates/netipc/Cargo.toml raw_call_overflow_no_growth -- --nocapture`.
  - Focused Windows validation passed on `win11` isolated copy `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml raw_call_overflow_no_growth -- --nocapture`.
- While adding the Rust no-growth tests, found a related cap-governance item: Rust managed server overflow handling doubled learned response capacity from the current negotiated session value without checking a configured maximum ceiling (`src/crates/netipc/src/service/raw/server_session_unix.rs:201` and `src/crates/netipc/src/service/raw/server_session_windows.rs:170`). The follow-on review found equivalent C and Go governance paths, so the fix below covers all three languages.
- Resolved the cap-governance issue across C, Go, and Rust, not only Rust:
  - C managed servers now store request/response payload growth ceilings from initialization config and cap `nipc_service_common_server_note_request_capacity()` / `nipc_service_common_server_note_response_capacity()` growth against those ceilings.
  - Go raw servers now store request/response payload growth ceilings and pass them through `serverNotePayloadCapacity()` for request learning, successful response learning, and overflow response learning.
  - Rust managed servers now store request/response payload growth ceilings and pass them into POSIX and Windows session handlers so overflow-triggered learned response growth cannot exceed the configured ceiling.
  - Windows C tests that still expected explicit tiny request/response caps to grow were corrected to assert `NIPC_ERR_OVERFLOW`, bounded negotiated caps, and broken client state.
  - A Windows C raw probe in `test_win_service_guards_extra.c` was corrected to use the typed snapshot server's default request cap, keeping that test focused on missing-handler dispatch instead of unrelated handshake limit negotiation.
- Validated cap-governance and no-growth behavior after the fix:
  - C POSIX: `cmake --build build-coverage --target test_service_payload_limits -j12 && timeout 600 build-coverage/bin/test_service_payload_limits` passed with `58 passed, 0 failed`.
  - C POSIX: `cmake --build build-coverage --target test_service -j12 && timeout 900 build-coverage/bin/test_service` passed with `532 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw` passed.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --nocapture` passed with `361 passed, 0 failed`.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml --lib -- --nocapture` passed with `243 passed, 0 failed`.
  - Windows C on `/tmp/plugin-ipc-sow0021-20260614052244`: focused service executables passed cleanly: `test_win_service_payload_limits` `22 passed, 0 failed`; `test_win_service_guards` `238 passed, 0 failed`; `test_win_service_guards_extra` `145 passed, 0 failed`; `test_win_service_extra` `448 passed, 0 failed`; `test_win_service` `110 passed, 0 failed`.
- Validated the first real consumer after vendoring the current SDK into the topology-containers checkout:
  - `network-viewer-apps-lookup-client.c` now treats `NIPC_PID_LOOKUP_OVERSIZED_ITEM` as an explicit negative/not-enriched PID outcome and keeps escaped `NIPC_PID_LOOKUP_PAYLOAD_EXCEEDED` out of successful enrichment.
  - `apps-cgroups-lookup-client.c` now maps `NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM` to the existing final negative cgroup cache state and treats escaped `NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED` as a Level 2 contract failure.
  - `cgroup-lookup-test.c` now prints names for `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM`.
  - Added focused consumer tests for APPS_LOOKUP and CGROUPS_LOOKUP `OVERSIZED_ITEM` handling.
  - Initial post-vendor validation caught a downstream hardening regression: the vendored POSIX UDS lifecycle file had overwritten owner-only socket permissions and pre-auth handshake receive timeout from the topology branch. Restored those branch-specific hardening changes before accepting the validation result.
  - Focused normal build passed: `cmake --build .local/build-sow45 --target netdata apps.plugin network-viewer.plugin apps-cgroups-lookup-client-abort-test network-viewer-apps-lookup-client-test cgroup-lookup-netipc-test cgroup-orchestrator-test network-viewer-topology-containers-test -j "$(nproc)"`.
  - Focused normal runtime validation passed: `apps-cgroups-lookup-client-abort-test`, `network-viewer-apps-lookup-client-test`, `cgroup-lookup-netipc-test`, `cgroup-orchestrator-test`, `network-viewer-topology-containers-test`, and the topology fixture validator with `validated 4 topology container fixtures`.
  - Focused ASAN build and runtime validation passed for the same target/test surface with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`.
- Closed a remaining request-payload boundary gap in the adversarial matrix:
  - Existing APPS_LOOKUP and CGROUPS_LOOKUP proactive request-split tests proved only exact-fit request fragments.
  - Updated C, Rust, and Go POSIX/Windows tests to cover cap-minus-one, exact-cap, and cap-plus-one request payload budgets.
  - APPS_LOOKUP test budgets `63`, `64`, and `65` bytes now prove maximum request-fragment sizes `2`, `3`, and `3` items.
  - CGROUPS_LOOKUP test budgets `47`, `48`, and `49` bytes now prove maximum request-fragment sizes `1`, `2`, and `2` seven-byte path items.
- Validated the expanded request-boundary tests:
  - C POSIX: `cmake --build build-coverage --target test_service -j12 && timeout 900 build-coverage/bin/test_service` passed with `560 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupProactiveRequestSplit$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `NIPC_TEST_FILTER=proactive timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `66 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupProactiveRequestSplitAndLimits$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split_windows -- --nocapture` passed.
- Closed the malformed-follow-up response adversarial matrix gap:
  - Existing tests covered malformed first lookup responses, malformed `PAYLOAD_EXCEEDED` suffixes, mixed-generation follow-up subcalls, and timeout/abort during follow-up subcalls.
  - Added C, Rust, and Go POSIX/Windows tests where APPS_LOOKUP and CGROUPS_LOOKUP first receive a valid partial response with a `PAYLOAD_EXCEEDED` suffix, then receive a structurally invalid follow-up response with a wrong echoed key.
  - Expected result in every language/platform: the whole logical call fails with `BAD_LAYOUT`/`ErrBadLayout`/`NipcError::BadLayout`, and the partial first-subcall result is not returned as a stitched response.
- Validated malformed-follow-up response tests:
  - C POSIX: `cmake --build build-coverage --target test_service -j12` passed; `timeout 900 build-coverage/bin/test_service` passed with `568 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupRejectsMalformedFollowupResponse$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml malformed_followup -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `NIPC_TEST_FILTER=malformed_followup timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `16 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupRejectsMalformedFollowupResponse$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml malformed_followup -- --nocapture` passed.
- Closed the duplicate-key and unsorted input-order request-splitting gap:
  - Strengthened the APPS_LOOKUP and CGROUPS_LOOKUP proactive request-boundary tests in C, Rust, and Go on POSIX and Windows to use duplicate, unsorted request keys.
  - The tests now assert the stitched logical response preserves original input order and duplicate entries while still proving cap-minus-one, exact-cap, and cap-plus-one request-fragment sizes.
- Validated duplicate-key and unsorted-order request splitting:
  - C POSIX: `cmake --build build-coverage --target test_service -j12` passed; `timeout 900 build-coverage/bin/test_service` passed with `574 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupProactiveRequestSplit$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `NIPC_TEST_FILTER=proactive timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `72 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupProactiveRequestSplitAndLimits$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_proactive_request_split_windows -- --nocapture` passed.
- Closed the endpoint-disappears-after-partial-progress fast-fail gap:
  - Added C, Rust, and Go POSIX/Windows tests where a raw APPS_LOOKUP or CGROUPS_LOOKUP peer sends one valid partial response with a `PAYLOAD_EXCEEDED` suffix and then closes before the Level 2 client can complete the follow-up request.
  - Expected result in every language/platform: the whole logical call fails, the already received partial result is discarded, and NetIPC does not wait for the endpoint to return or loop indefinitely.
- Validated endpoint-disappears-after-partial-progress tests:
  - C POSIX: `cmake --build build-coverage --target test_service -j12` passed; `timeout 900 build-coverage/bin/test_service` passed with `582 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupEndpointGoneAfterPartialProgress$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml endpoint_gone_after_partial -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4 && NIPC_TEST_FILTER=endpoint_gone timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `16 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupEndpointGoneAfterPartialProgress$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml endpoint_gone_after_partial -- --nocapture` passed.
- Closed the zero-item typed lookup parity gap:
  - C POSIX and Go POSIX already had end-to-end APPS_LOOKUP and CGROUPS_LOOKUP zero-item typed-call coverage.
  - Added missing zero-item typed-call coverage for Rust POSIX plus C, Go, and Rust Windows.
  - The tests prove zero-item logical calls still validate the endpoint through a real typed request/response and return an empty logical response instead of a local fake success.
- Validated zero-item typed lookup tests:
  - C POSIX: latest full `test_service` evidence includes `test_lookup_zero_item_calls` and passed with `582 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupZeroItemCalls$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_zero_item_calls -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4 && NIPC_TEST_FILTER=zero_item timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `16 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupZeroItemCalls$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_zero_item_calls_windows -- --nocapture` passed.
- Closed the endpoint-disappears-before-first-subcall fast-fail gap:
  - Added C, Rust, and Go POSIX/Windows tests where an APPS_LOOKUP or CGROUPS_LOOKUP client reaches `READY`, the lookup provider disappears before the first typed request, and the logical call is attempted with a short timeout.
  - Expected result in every language/platform: the logical call fails clearly, no local fake success is returned, no partial result exists, and NetIPC does not wait for the endpoint to come back.
- Validated endpoint-disappears-before-first-subcall tests:
  - C POSIX: `timeout 900 build-coverage/bin/test_service` passed with `588 passed, 0 failed`.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupEndpointGoneBeforeFirstSubcall$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml endpoint_gone_before_first -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4 && NIPC_TEST_FILTER=before_first timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `14 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupEndpointGoneBeforeFirstSubcall$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml endpoint_gone_before_first -- --nocapture` passed.
- Closed the endpoint-absent-before-call fast-fail gap:
  - Added C, Rust, and Go POSIX/Windows tests where no APPS_LOOKUP or CGROUPS_LOOKUP provider is present, the client refreshes to `NOT_FOUND`, and the typed logical call fails immediately.
  - Expected result in every language/platform: the call fails as not-ready/bad-layout before any scale handling, retry loop, or wait-for-provider behavior can start.
- Validated endpoint-absent-before-call tests:
  - C POSIX: `cmake --build build-coverage --target test_service -j12 && timeout 900 build-coverage/bin/test_service` passed with `594 passed, 0 failed`. The run emitted a gcov checksum warning from rebuilding an existing coverage object, but the executable completed successfully.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupEndpointAbsentBeforeCall$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml endpoint_absent_before_call -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4 && NIPC_TEST_FILTER=absent timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `6 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `CGO_ENABLED=0 "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupEndpointAbsentBeforeCall$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `MSYSTEM=MSYS /c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml endpoint_absent_before_call -- --nocapture` passed.
- Closed the mixed-status cross-language lookup interop gap:
  - Extended the C, Rust, and Go POSIX and Windows `interop_service` fixtures with APPS_LOOKUP and CGROUPS_LOOKUP mixed-status server/client modes.
  - APPS_LOOKUP mixed interop covers: known item with label, host-root cgroup status, unknown PID, `OVERSIZED_ITEM`, and known PID with cgroup retry-later.
  - CGROUPS_LOOKUP mixed interop covers: known item with label, retry-later unknown, permanent unknown, `OVERSIZED_ITEM`, and known item without labels.
  - Updated POSIX and Windows lookup interop scripts so each transport profile runs all-known scale plus mixed-status directed matrices: 4 method/mode matrices x 9 C/Rust/Go directed client/server pairs = 36 checks per profile.
- Validated mixed-status lookup interop:
  - POSIX fixture builds passed: `cmake --build build-coverage --target interop_service_c interop_service_go -j12`; `cargo build --manifest-path src/crates/netipc/Cargo.toml --bin interop_service`.
  - POSIX Go package sanity passed: `cd src/go && go test -count=1 ./pkg/netipc/...`.
  - POSIX baseline passed: `INTEROP_SVC_C=build-coverage/bin/interop_service_c INTEROP_SVC_GO=build-coverage/bin/interop_service_go INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service NIPC_LOOKUP_SCALE_ITEMS=8192 timeout 900 bash tests/test_lookup_scale_interop.sh` with `36 passed, 0 failed, 0 skipped`.
  - POSIX SHM passed with the same binary environment and `NIPC_PROFILE=shm`: `36 passed, 0 failed, 0 skipped`.
  - Windows fixture build passed on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target interop_service_win_c interop_service_win_go interop_service_win_rs -j4`.
  - Windows Named Pipe passed: `INTEROP_SVC_C=build-windows-focused/bin/interop_service_win_c.exe INTEROP_SVC_GO=build-windows-focused/bin/interop_service_win_go.exe INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service_win.exe NIPC_LOOKUP_SCALE_ITEMS=8192 timeout 2400 bash tests/test_lookup_scale_win_interop.sh` with `36 passed, 0 failed, 0 skipped`.
  - Windows SHM passed with the same binary environment: `timeout 2400 bash tests/test_lookup_scale_win_shm_interop.sh` with `36 passed, 0 failed, 0 skipped`.
- Closed the lookup status codec interop gap:
  - Added C, Rust, and Go codec interop fixtures for APPS_LOOKUP `PAYLOAD_EXCEEDED`, APPS_LOOKUP `OVERSIZED_ITEM`, CGROUPS_LOOKUP `PAYLOAD_EXCEEDED`, and CGROUPS_LOOKUP `OVERSIZED_ITEM`.
  - Added those generated files to the byte-identical comparison list in `tests/interop_codec.sh`.
  - Validated with `timeout 900 bash tests/interop_codec.sh`: Rust decoded C and Go output, Go decoded C and Rust output, C decoded Rust and Go output, and all C/Rust/Go generated files matched byte-for-byte. The final result was `ALL INTEROP TESTS PASSED`.
- Closed the explicit reordered/duplicate response-item gap in the malformed stitched-response matrix:
  - Added APPS_LOOKUP and CGROUPS_LOOKUP tests where the server returns response items in the wrong order.
  - Added APPS_LOOKUP and CGROUPS_LOOKUP tests where the server returns a duplicate echoed key and omits the requested key at that position.
  - The production invariant being tested is strict per-position echoed-key validation: reordered and duplicate response items are corrupt responses, not inputs to repair or infer.
- Validated reordered/duplicate response-item rejection:
  - C POSIX: `cmake --build build-coverage --target test_service -j12` passed and `/usr/bin/ctest --test-dir build-coverage --output-on-failure -R '^test_service$'` passed.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=180s ./pkg/netipc/service/raw -run '^TestLookupRejectsMalformedServerResponses$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_unix -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4` passed and `NIPC_TEST_FILTER=malformed_first_response timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `42 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cd src/go && "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=180s ./pkg/netipc/service/raw -run '^TestWin(Apps|Cgroups)LookupRejectsMalformedTypedResponses$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_windows -- --nocapture` passed.
- Closed the remaining malformed first-response status/table gap in the Level 2 client matrix:
  - Added APPS_LOOKUP and CGROUPS_LOOKUP tests for unknown status enum values.
  - Added APPS_LOOKUP and CGROUPS_LOOKUP tests where a known response item is corrupted into a non-known status without clearing status-dependent fields.
  - Added APPS_LOOKUP and CGROUPS_LOOKUP tests where a known response item advertises more labels than its encoded item contains.
  - Extended C, Rust, and Go POSIX/Windows malformed typed-response tests so these corruptions are rejected before a stitched logical response can reach a Level 2 consumer.
  - Corrected the Windows C wrong-echo malformed-response fixture to return one response item per request item and corrupt only the first echoed key, so it tests echoed-key rejection rather than item-count rejection.
- Validated invalid-status/status-fields/label-table malformed-response rejection:
  - C POSIX: `cmake --build build-coverage --target test_service -j12` passed and `/usr/bin/ctest --test-dir build-coverage --output-on-failure -R '^test_service$'` passed.
  - Go POSIX: `cd src/go && go test -count=1 -timeout=180s ./pkg/netipc/service/raw -run '^TestLookupRejectsMalformedServerResponses$'` passed.
  - Rust POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_unix -- --nocapture` passed.
  - C Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cmake --build build-windows-focused --target test_win_service_extra -j4` passed and `NIPC_TEST_FILTER=malformed_first_response timeout 600 build-windows-focused/bin/test_win_service_extra.exe` passed with `84 passed, 0 failed`.
  - Go Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `cd src/go && "/c/Program Files/Go/bin/go.exe" test -count=1 -timeout=180s ./pkg/netipc/service/raw -run '^TestWin(Apps|Cgroups)LookupRejectsMalformedTypedResponses$'` passed.
  - Rust Windows on `/tmp/plugin-ipc-sow0021-20260614052244`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_windows -- --nocapture` passed.

## Validation

Acceptance criteria evidence:

- Implemented standard `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM` lookup outcomes in C, Rust, and Go codec/status surfaces.
- Implemented transparent Level 2 split/stitch for APPS_LOOKUP and CGROUPS_LOOKUP in C, Rust, and Go.
- Implemented strict generation consistency checks in all three languages; mixed subresponse generations reject the logical call.
- Implemented logical lookup item, subcall, and stitched-response-byte ceilings in public client configuration/defaults.
- Implemented cgroups variable request-key oversized handling as an item-level `OVERSIZED_ITEM` outcome after confirming the configured request payload budget cannot fit the key.

Tests or equivalent validation:

- POSIX local validation:
  - `cmake --build build -j12` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R '^test_service$'` passed.
  - `cd src/go && go test ./...` passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml --no-fail-fast` passed.
- Windows validation on `win11` using `/tmp/plugin-ipc-win-20260613193339`:
  - Configured and built `test_win_service_extra` with MSYS GCC/Ninja.
  - `/usr/bin/ctest --output-on-failure -R '^test_win_service_extra$'` passed.
  - `cd src/go && CGO_ENABLED=0 go test ./pkg/netipc/service/raw -run 'Win.*Lookup|Lookup' -count=1 -timeout=180s` passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml lookup --no-fail-fast` passed.
- Focused POSIX lookup validation before the full suites:
  - `/usr/bin/ctest --test-dir build --output-on-failure -R '^test_service$'` passed.
  - `cd src/go && go test ./pkg/netipc/service/raw -run 'Lookup|AppsLookup|CgroupsLookup'` passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml lookup --no-fail-fast` passed.
- Focused large-cardinality validation:
  - POSIX Go: `cd src/go && go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestLookupLargeLogicalCalls$'` passed.
  - POSIX Rust: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_large_logical_calls -- --nocapture` passed.
  - POSIX C: `cmake --build build-coverage --target test_service -j12 && /usr/bin/ctest --test-dir build-coverage --output-on-failure -R '^test_service$'` passed.
  - Windows Go on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260613234940`: `cd src/go && CGO_ENABLED=0 go test -count=1 -timeout=300s ./pkg/netipc/service/raw -run '^TestWinLookupLargeLogicalCalls$'` passed.
  - Windows Rust on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260613234940`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_large_logical_calls_windows -- --nocapture` passed.
  - Windows C on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260613234940`: configured `build-windows-focused`, built `test_win_service_extra`, and ran `NIPC_TEST_FILTER=test_lookup_large_logical_calls timeout 600 build-windows-focused/bin/test_win_service_extra.exe`; result was `44 passed, 0 failed`.
- POSIX lookup-scale interop validation:
  - Latest direct baseline script run with `INTEROP_SVC_C=build-coverage/bin/interop_service_c`, `INTEROP_SVC_GO=build-coverage/bin/interop_service_go`, and `INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service` passed `36 passed, 0 failed, 0 skipped` at `8192` scale items plus mixed-status lookup cases.
  - Latest direct SHM script run with the same binary environment and `NIPC_PROFILE=shm` passed `36 passed, 0 failed, 0 skipped` at `8192` scale items plus mixed-status lookup cases.
  - Earlier CTest registration validation passed before the mixed-status expansion: `cmake -S . -B build-coverage -DNETIPC_COVERAGE=ON && /usr/bin/ctest --test-dir build-coverage --output-on-failure -R '^test_lookup_scale(_shm)?_interop$'`.
  - Heavier stress-only baseline validation passed at `65536` items across APPS_LOOKUP and CGROUPS_LOOKUP, covering all 9 directed C/Rust/Go client/server pairs per method: result `18 passed, 0 failed, 0 skipped`.
  - Heavier stress-only SHM validation passed at `65536` items with the same method and language-pair matrix: result `18 passed, 0 failed, 0 skipped`.
- POSIX lookup-scale benchmark/stress validation:
  - `cmake --build build-coverage --target bench_posix_c bench_posix_go -j12` passed after the benchmark driver changes.
  - `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_posix` passed after the Rust benchmark driver change.
  - Short focused one-second rows for `cgroups-lookup-known-8192`, `cgroups-lookup-known-32768`, `apps-lookup-known-8192`, and `apps-lookup-known-32768` passed in C, Go, and Rust at targets `0`, `100000`, `10000`, and `1000`, producing `/tmp/plugin-ipc-sow0021-lookup-scale-bench.csv` with `48` data rows and no zero-throughput rows.
  - Observed one-second scale-row throughput ranges were positive across all rows: `8192` item rows ranged from `366` to `1182` logical calls/s; `32768` item rows ranged from `79` to `289` logical calls/s. These are smoke/stress evidence, not publication-quality performance floors.
  - Full POSIX benchmark regeneration now passes with the new `345`-row matrix: `timeout 7200 bash tests/run-posix-bench.sh benchmarks-posix.csv` exited `0`, and `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md` exited `0` with `All performance floors met`.
  - The regenerated POSIX retry diagnostics contain only stable current-run recoveries for three noisy rows: two `snapshot-baseline` Go rows and one `apps-lookup-mixed-16` Go row. Each recovery used `7` retry samples and a max/min stable ratio below `1.35`.
- Windows lookup-scale interop validation on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260613234940`:
  - Built Windows C, Go, and Rust `interop_service_win` binaries with `cmake --build build-windows-focused --target interop_service_win_c interop_service_win_go interop_service_win_rs -j4`.
  - Direct baseline script run passed: `INTEROP_SVC_C=build-windows-focused/bin/interop_service_win_c.exe INTEROP_SVC_GO=build-windows-focused/bin/interop_service_win_go.exe INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service_win.exe NIPC_LOOKUP_SCALE_ITEMS=8192 timeout 600 bash tests/test_lookup_scale_win_interop.sh`; result was `18 passed, 0 failed, 0 skipped`.
  - Direct Win SHM script run passed with the same binary environment: `timeout 600 bash tests/test_lookup_scale_win_shm_interop.sh`; result was `18 passed, 0 failed, 0 skipped`.
  - CTest registration validation passed: `/usr/bin/ctest --test-dir build-windows-focused --output-on-failure -R '^test_lookup_scale_win(_shm)?_interop$'`; `test_lookup_scale_win_interop` passed in `13.15s` and `test_lookup_scale_win_shm_interop` passed in `12.24s`.
- Windows heavier stress-only lookup-scale interop validation on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260614052244`:
  - Rebuilt Windows C, Go, and Rust `interop_service_win` binaries with explicit `GO_EXECUTABLE=/c/Program Files/Go/bin/go.exe` and `CARGO_EXECUTABLE=/c/Users/costa/.cargo/bin/cargo.exe` because that SSH shell did not expose Go or Cargo on `PATH`.
  - Latest Named Pipe validation with mixed-status coverage passed at `8192` scale items: `INTEROP_SVC_C=build-windows-focused/bin/interop_service_win_c.exe INTEROP_SVC_GO=build-windows-focused/bin/interop_service_win_go.exe INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service_win.exe NIPC_LOOKUP_SCALE_ITEMS=8192 timeout 2400 bash tests/test_lookup_scale_win_interop.sh`; result was `36 passed, 0 failed, 0 skipped`.
  - Latest Windows SHM validation with mixed-status coverage passed at `8192` scale items with the same binary environment: `timeout 2400 bash tests/test_lookup_scale_win_shm_interop.sh`; result was `36 passed, 0 failed, 0 skipped`.
  - Named Pipe validation passed at `65536` items: `INTEROP_SVC_C=build-windows-focused/bin/interop_service_win_c.exe INTEROP_SVC_GO=build-windows-focused/bin/interop_service_win_go.exe INTEROP_SVC_RS=src/crates/netipc/target/debug/interop_service_win.exe NIPC_LOOKUP_SCALE_ITEMS=65536 timeout 900 bash tests/test_lookup_scale_win_interop.sh`; result was `18 passed, 0 failed, 0 skipped`.
  - Windows SHM validation passed at `65536` items with the same binary environment: `timeout 900 bash tests/test_lookup_scale_win_shm_interop.sh`; result was `18 passed, 0 failed, 0 skipped`.
  - The same cleanup patch was validated against existing Windows service/cache interop harnesses: `test_service_win_interop.sh` passed `9 passed, 0 failed, 0 skipped`; `test_cache_win_interop.sh` passed `9 passed, 0 failed, 0 skipped`.
- Windows benchmark validation on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260614052244`:
  - Focused regression row passed after config parity fix: `snapshot-baseline c->go @ max` produced median throughput `101600` and stable ratio `1.107466`.
  - Full matrix passed: `timeout 18000 bash tests/run-windows-bench.sh benchmarks-windows.csv` exited `0` and produced `201` measurements.
  - Report generation passed: `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md` exited `0` with `All performance floors met`.
  - Generated artifact sizes: `benchmarks-windows.csv` has `202` lines including header; `benchmarks-windows.md` has `393` lines.
- Rust malformed typed lookup response validation:
  - POSIX: `cargo test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_unix -- --nocapture` passed.
  - Windows on isolated `win11` copy `/tmp/plugin-ipc-sow0021-20260613234940`: `/c/Users/costa/.cargo/bin/cargo.exe test --manifest-path src/crates/netipc/Cargo.toml test_lookup_rejects_malformed_typed_responses_windows -- --nocapture` passed.
- Coverage validation:
  - `bash tests/run-coverage-go.sh` runs the expanded POSIX Go coverage package set and now passes the configured 90% gate with `90.0%` total coverage. The latest measured total was `3251/3611` statements covered.
  - `MSYSTEM=MSYS bash tests/run-coverage-go-windows.sh` runs the expanded Windows Go coverage package set on `win11` and now passes the configured 90% gate with `90.0%` total coverage. The latest measured total was `3299/3665` statements covered.
  - `bash tests/run-coverage-c.sh` runs the expanded C source list and now passes the configured 90% gate with `91.8%` total coverage. The latest measured total was `2329/2537` lines covered; all measured files are at or above 90%.
  - `bash tests/run-coverage-rust.sh` runs the Rust library coverage suite and now passes the configured 90% gate with `91.54%` total line coverage after adding public facade lookup tests.
  - Coverage gates are no longer SOW blockers; the remaining blocker is the broader adversarial matrix review.
- Mid-logical timeout/abort validation:
  - POSIX Go validates APPS_LOOKUP and CGROUPS_LOOKUP timeout and abort after a first successful partial response and a delayed follow-up subcall.
  - Windows Go validates the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up timeout/abort cases.
  - POSIX Rust validates the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up timeout/abort cases.
  - Windows Rust validates the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up timeout/abort cases.
  - POSIX C validates the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up timeout/abort cases in the full service fixture; latest focused evidence is `532 passed, 0 failed`.
  - Windows C validates the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up timeout/abort cases in the focused-extra fixture; latest full-fixture evidence is `404 passed, 0 failed`.
- Request-capacity reconnect validation:
  - C POSIX and Go POSIX already validate that artificially stale session request payload caps trigger reconnect during lookup calls instead of item-level oversized classification.
  - Rust POSIX now validates the same APPS_LOOKUP and CGROUPS_LOOKUP lookup-call reconnect path; latest lookup-filter evidence is `7 passed, 0 failed`.
  - Rust Windows now validates the same APPS_LOOKUP and CGROUPS_LOOKUP lookup-call reconnect path; latest lookup-filter evidence is `7 passed, 0 failed`.
- Exact request-payload boundary validation:
  - C, Rust, and Go POSIX tests validate cap-minus-one, exact-cap, and cap-plus-one APPS_LOOKUP and CGROUPS_LOOKUP request-fragment boundaries using configured request payload caps.
  - C, Rust, and Go Windows tests validate the same cap-minus-one, exact-cap, and cap-plus-one APPS_LOOKUP and CGROUPS_LOOKUP request-fragment boundaries.
  - APPS_LOOKUP `63`/`64`/`65` byte request payload budgets produce max request fragments of `2`/`3`/`3` items; CGROUPS_LOOKUP `47`/`48`/`49` byte budgets produce max request fragments of `1`/`2`/`2` seven-byte path items.
  - The same tests now use duplicate, unsorted request keys and assert that the final stitched logical response preserves the original item order and duplicate entries.
  - Response exact-fit and plus/minus-one response boundaries are covered separately at the protocol builder layer in C, Rust, and Go.
- Malformed follow-up response validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a valid first partial response is followed by a follow-up response with the wrong echoed key.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP follow-up malformed-response case.
  - Latest focused evidence: C POSIX `568 passed, 0 failed`; C Windows focused filter `16 passed, 0 failed`; Go/Rust POSIX and Windows focused tests passed.
- Reordered/duplicate response-item validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a server returns response items in a different order than requested.
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a server returns a duplicate echoed key and omits the expected key at that position.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP reordered and duplicate response-item cases.
  - Latest focused evidence: C POSIX `ctest` `test_service` passed; C Windows focused filter `42 passed, 0 failed`; Go/Rust POSIX and Windows focused malformed-response tests passed.
- Invalid status/status-fields/label-table malformed-response validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a response item uses an unknown status enum value.
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a response item status is corrupted without matching that status's required field semantics.
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a response item advertises more labels than its encoded item contains.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP malformed status/table cases.
  - Latest focused evidence: C POSIX `ctest` `test_service` passed; C Windows focused filter `84 passed, 0 failed`; Go/Rust POSIX and Windows focused malformed-response tests passed.
- Endpoint-disappears-after-partial-progress validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the whole logical call when a valid first partial response is followed by endpoint disappearance before follow-up completion.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP endpoint-disappears-after-partial-progress case.
  - Latest focused evidence: C POSIX `582 passed, 0 failed`; C Windows focused filter `16 passed, 0 failed`; Go/Rust POSIX and Windows focused tests passed.
- Endpoint-disappears-before-first-subcall validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail the logical call when the client reaches `READY` but the provider disappears before the first typed subcall is sent.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP provider-disappears-before-first-subcall case.
  - Latest focused evidence: C POSIX `588 passed, 0 failed`; C Windows focused filter `14 passed, 0 failed`; Go/Rust POSIX and Windows focused tests passed.
- Endpoint-absent-before-call validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP fail immediately when no provider exists and the client is in `NOT_FOUND`.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP absent-provider case.
  - Latest focused evidence: C POSIX `594 passed, 0 failed`; C Windows focused filter `6 passed, 0 failed`; Go/Rust POSIX and Windows focused tests passed.
- Zero-item typed lookup validation:
  - C, Rust, and Go POSIX tests validate APPS_LOOKUP and CGROUPS_LOOKUP zero-item typed calls return empty logical responses.
  - C, Rust, and Go Windows tests validate the same APPS_LOOKUP and CGROUPS_LOOKUP zero-item typed-call behavior.
  - Latest focused evidence: C POSIX `582 passed, 0 failed`; C Windows focused filter `16 passed, 0 failed`; Go/Rust POSIX and Windows focused tests passed.
- Exact response-payload boundary validation:
  - C, Rust, and Go protocol tests validate exact-size, exact-plus-one, and exact-minus-one APPS_LOOKUP and CGROUPS_LOOKUP response buffers.
  - Exact-size and exact-plus-one buffers keep the second response item known.
  - Exact-minus-one buffers force a valid `PAYLOAD_EXCEEDED` second item instead of corrupting or failing the whole response.
- Lookup status codec interop validation:
  - C, Rust, and Go codec fixtures now include byte-identical APPS_LOOKUP and CGROUPS_LOOKUP samples for `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM`.
  - `timeout 900 bash tests/interop_codec.sh` passed. Cross-decode counts were Rust `105/0`, Go `106/0`, and C `121/0` for each applicable source set, and the byte-identical comparison matched the new status files across C, Rust, and Go.
- Raw no-growth overflow validation:
  - C POSIX and Windows already have typed no-growth overflow coverage in `tests/fixtures/c/test_service_payload_limits.c` and `tests/fixtures/c/test_win_service_guards.c`.
  - Go POSIX and Windows already have raw no-growth overflow coverage in `src/go/pkg/netipc/service/raw/more_unix_test.go` and `src/go/pkg/netipc/service/raw/more_windows_test.go`.
  - Rust POSIX now validates the same client-side bounded retry behavior with `test_raw_call_overflow_no_growth_stops_bounded`.
  - Rust Windows now validates the same client-side bounded retry behavior with `test_raw_call_overflow_no_growth_stops_bounded_windows`.
  - Server learned-capacity cap governance is now validated in C, Go, and Rust. Explicitly configured request/response payload caps remain ceilings; overflow learning cannot grow past them.
- No-progress overflow and bounded response-memory validation:
  - C POSIX, Rust POSIX/Windows, and Go POSIX/Windows already reject a typed lookup response whose first item is `PAYLOAD_EXCEEDED`.
  - C Windows now validates the same no-progress `PAYLOAD_EXCEEDED` rejection for APPS_LOOKUP and CGROUPS_LOOKUP.
  - Go POSIX/Windows, Rust POSIX/Windows, and C POSIX/Windows validate logical response-byte ceilings for APPS_LOOKUP and CGROUPS_LOOKUP.
  - C POSIX also validates a final stitched response-buffer allocation fault; Go and Rust use explicit logical response-byte ceilings as the deterministic memory-pressure simulation available in those test harnesses.
- Note: `ctest` on `$PATH` resolved to a broken local Python wrapper missing the `cmake` module; validation used `/usr/bin/ctest`.

Real-use evidence:

- The topology-containers pressure case remains the concrete motivating real-use path: a Level 2 APPS_LOOKUP logical request may include `8192` PIDs and produce a response larger than one transport payload budget.
- Synthetic C/Rust/Go POSIX and Windows tests now cover `8192` and `32768` item logical APPS_LOOKUP and CGROUPS_LOOKUP calls with internal fragmentation and full order verification.
- Direct downstream topology-containers baseline validation passed for the current downstream branch before vendoring: `apps-lookup-protocol-test`, `cgroup-lookup-netipc-test`, and `network-viewer-apps-lookup-client-test` all exited `0`.
- Direct downstream topology-containers post-vendor validation passed after adapting the consumer-owned status handling and restoring the branch-specific POSIX UDS hardening overwritten by the vendor copy. Focused normal and ASAN builds/tests passed for `netdata`, `apps.plugin`, `network-viewer.plugin`, APPS_LOOKUP/CGROUPS_LOOKUP consumer tests, cgroup tests, and the topology fixture validator.

Reviewer findings:

- External reviewer finding: generic retry wrappers could turn logical overflow into reconnect/retry behavior. Handled by moving APPS_LOOKUP and CGROUPS_LOOKUP outer logical loops outside generic retry and using the retry wrapper only around raw transport subcalls.
- External reviewer finding: local oversized request-key synthesis could run before proving the client is connected. Handled by enforcing `READY` before logical lookup work in C, Go, and Rust.
- External reviewer finding: cgroups all-local oversized handling could skip the zero-item probe path. Handled by continuing after a local oversized item only when more request items remain; otherwise the client sends the zero-item request and validates endpoint/generation behavior.
- External reviewer concern: hidden fixed payload ceilings would contradict initialization-tunable budgets. Reviewed against code and docs. `NIPC_MAX_PAYLOAD_CAP` / `MaxPayloadCap` remains a named default growth ceiling; request/response payload budgets are exposed through initialization config. Server learned-capacity growth now also honors those configured ceilings.
- External reviewer finding: coverage scripts and broader adversarial matrix still need updates before SOW completion. Coverage script expansion is now implemented; C/Rust/Go coverage gates pass; representative `8192` and `32768` logical-call tests now pass in C/Rust/Go on POSIX and Windows; POSIX and Windows baseline/SHM lookup-scale interop now pass across all C/Rust/Go directed pairs, including mixed-status lookup cases and heavier `65536` stress-only runs; lookup status codec interop now proves `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM` wire parity across C/Rust/Go; Rust malformed typed lookup response parity is now covered on POSIX and Windows; malformed follow-up responses after partial progress are now covered in C/Rust/Go on POSIX and Windows; reordered and duplicate response-item corruption is now covered in C/Rust/Go on POSIX and Windows; invalid status enum, invalid status-dependent field, and invalid label-table corruption are now covered in C/Rust/Go on POSIX and Windows; endpoint absence before call, endpoint disappearance after partial progress, and endpoint disappearance before the first subcall are now covered in C/Rust/Go on POSIX and Windows; zero-item typed lookup calls are now covered in C/Rust/Go on POSIX and Windows; duplicate and unsorted request keys under request splitting are now covered in C/Rust/Go on POSIX and Windows; full POSIX and Windows benchmark regenerations now pass; downstream topology-containers post-vendor validation now passes. Broader adversarial matrix review remains open.

Same-failure scan:

- `rg` scan for `call_with_retry`, `callWithRetry`, `raw_call_with_retry`, `OVERSIZED_ITEM`, and request-capacity helpers confirms:
  - C lookup raw retries are limited to `apps_lookup_do_raw_call_with_retry()` and `cgroups_lookup_do_raw_call_with_retry()`.
  - C lookup request-size preflight uses `nipc_service_platform_ensure_request_capacity()` before `CGROUPS_LOOKUP` emits local `OVERSIZED_ITEM`.
  - Go lookup request-size preflight uses `ensureLookupRequestCapacity()` before `CGROUPS_LOOKUP` emits local `CgroupLookupOversizedItem`.
  - Rust lookup request-size preflight uses `ensure_lookup_request_capacity()` before `CGROUPS_LOOKUP` emits local `CGROUP_LOOKUP_OVERSIZED_ITEM`.
  - Non-lookup typed calls still use the existing generic retry wrapper unchanged.
  - C, Go, and Rust server learned-capacity helpers now cap growth against initialization-derived request/response payload ceilings.

Sensitive data gate:

- No raw sensitive data was added to SOW, docs, skills, code comments, or tests.
- Durable artifacts use only code paths, command names, status names, and sanitized deployment-class examples.

Artifact maintenance gate:

- AGENTS.md: no workflow or project-wide guardrail change required by this implementation pass.
- Runtime project skills: no runtime `project-*` skills exist in this repo; no update required in this pass.
- Specs: public docs under `docs/` were updated for strict compatibility, lookup scale statuses, response overflow behavior, and configurable ceilings.
- End-user/operator docs: `README.md`, `docs/README.md`, `docs/level1-wire-envelope.md`, `docs/level2-typed-api.md`, `docs/codec-apps-lookup.md`, `docs/codec-cgroups-lookup.md`, and `docs/getting-started.md` were updated.
- End-user/operator skills: `docs/netipc-integrator-skill.md` was updated for the scale contract, strict matching, and tunable ceilings.
- SOW lifecycle: SOW-0021 remains `in-progress`; SOW-0007 is superseded and moved to done. Coverage gates, heavier `65536` stress-only interop runs, full POSIX and Windows benchmark regenerations, server learned-capacity cap-governance validation, and downstream topology-containers post-vendor validation now pass, but the SOW cannot close until remaining broader adversarial matrix gaps are handled.

Specs update:

- Public docs now state:
  - no backward-compatible, forward-compatible, or best-effort compatibility mode for method/layout/status/generation drift;
  - mixed-generation stitched lookup responses are globally unsupported;
  - `PAYLOAD_EXCEEDED` is a Level 2 internal retry outcome;
  - `OVERSIZED_ITEM` is a final per-item outcome;
  - payload budgets and logical lookup ceilings come from initialization config or documented defaults.

Project skills update:

- No runtime project skill exists.
- `docs/netipc-integrator-skill.md` was updated because it is an output/reference skill for downstream integrators.

End-user/operator docs update:

- Updated as listed in the artifact maintenance gate.

End-user/operator skills update:

- Updated `docs/netipc-integrator-skill.md`.

Lessons:

- The logical lookup retry loop must never reuse the generic transport retry wrapper for whole logical failures. Only raw transport subcalls should use reconnect-on-overflow behavior.
- `OVERSIZED_ITEM` must be decided against the configured/effective maximum payload budget, not merely the current session cap. A stale/small session cap is a reconnect case, not an oversized item.
- Explicitly configured payload budgets are ceilings, not growth hints. Tests should not expect reconnect-on-overflow to grow past a consumer-provided request or response cap.
- Logical subcall ceilings count service request/response cycles, not local preflight work or locally synthesized item outcomes.
- Windows SSH sessions may not expose Cargo through `$HOME/.cargo/bin`; use the Cargo path under `$USERPROFILE` in validation commands.
- Windows MSYS cleanup code must not pass MSYS PIDs or unescaped `/PID` switches directly to `taskkill.exe`. Resolve the native Windows PID with `ps -W` and use `taskkill.exe //PID` for exact-process cleanup.

Follow-up mapping:

- Still open inside this active SOW:
  - add any remaining broader adversarial tests from the planned matrix beyond the now-covered representative `8192`/`32768` logical-call cases, now-covered mid-logical timeout/abort cases, now-covered malformed follow-up responses after partial progress, now-covered reordered/duplicate response-item corruption, now-covered invalid status/status-dependent/label-table response corruption, now-covered endpoint absence before call, now-covered endpoint disappearance after partial progress, now-covered endpoint disappearance before the first subcall, now-covered zero-item typed lookup calls, now-covered stale request-capacity reconnect cases, now-covered duplicate/unsorted request keys under request splitting, now-covered request cap-minus-one/exact/plus-one boundaries, now-covered exact response-fit plus/minus-one boundaries, now-covered no-progress overflow cases, now-covered raw no-growth overflow cases, now-covered logical response-byte ceilings, now-covered mixed-status cross-language interop cases, and now-covered lookup status codec interop cases;
  - keep lookup-scale interop green across POSIX baseline, POSIX SHM, Windows Named Pipe, and Windows SHM; all four profiles now cover both all-known scale and mixed-status C/Rust/Go directed tests.

## Downstream Vendoring Plan

Purpose:

- Vendor the completed NetIPC SDK changes into a Netdata checkout only after the plugin-ipc validation gates are green.
- Preserve the Level 2 contract: Netdata consumers should keep issuing simple typed logical calls and must not implement caller-side split/stitch workarounds.

Evidence from the first consumer branch:

- `src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.c:724` initializes an APPS_LOOKUP client with profiles, auth token, and timeout only. Zero-valued new SDK ceilings currently select library defaults.
- `src/collectors/apps.plugin/apps-lookup-netipc.c:315` initializes the APPS_LOOKUP server with profiles and auth token only.
- `src/collectors/apps.plugin/apps-cgroups-lookup-client.c:475` initializes a CGROUPS_LOOKUP client with profiles, auth token, and timeout only.
- `src/collectors/cgroups.plugin/cgroup-lookup-netipc.c:269` initializes the CGROUPS_LOOKUP server with profiles and auth token only.
- `src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.c:503` handles `NIPC_PID_LOOKUP_UNKNOWN`, but does not yet explicitly handle the new `NIPC_PID_LOOKUP_OVERSIZED_ITEM` outcome. After vendoring, this must not be treated as a known PID or inserted as successful cache data.
- `src/collectors/apps.plugin/apps-cgroups-lookup-client.c:378` stores the returned CGROUPS_LOOKUP item status. `NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM` should remain a final non-retry item state, while an escaped `NIPC_CGROUP_LOOKUP_PAYLOAD_EXCEEDED` would indicate an SDK contract violation because Level 2 should consume it internally.
- `src/collectors/cgroups.plugin/cgroup-lookup-test/cgroup-lookup-test.c:7` prints cgroup lookup status names and needs names for `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM` after vendoring.
- The Netdata CMake netipc source list already matches the current plugin-ipc split-file shape for C protocol/service files in the checked topology-containers branch, but it still must be checked after vendoring because `vendor-to-netdata.sh` copies source files and intentionally does not edit build-system files.

Planned downstream steps:

1. Finish remaining plugin-ipc validation gaps: broader adversarial tests and downstream topology-containers validation.
2. Commit and push plugin-ipc only after validation evidence is recorded and the active SOW is ready for the requested checkpoint.
3. In the target Netdata checkout, read its local `AGENTS.md`, create or reuse the appropriate Netdata SOW, and record the clean end state before editing.
4. Run `vendor-to-netdata.sh` against the Netdata checkout, excluding secrets and preserving Netdata-specific wrapper files.
5. Rewrite Go netipc import paths if the destination Go module differs from `github.com/netdata/plugin-ipc/go`; review the diff before deleting generated `.bak` files.
6. Check and update Netdata CMake source lists if vendoring introduced any new C source files.
7. Adapt Netdata C APPS_LOOKUP and CGROUPS_LOOKUP consumers for the new item statuses:
   - treat `NIPC_PID_LOOKUP_OVERSIZED_ITEM` as an explicit not-enriched item in network-viewer, not as a successful cache entry;
   - ensure an escaped `PAYLOAD_EXCEEDED` is handled as a contract failure or retriable SDK bug, not as successful data;
   - keep `NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM` as a final non-retry cgroup state unless Netdata product semantics require a different explicit user decision;
   - update test/debug output status names.
8. Decide, with evidence from build/runtime tests, whether Netdata should rely on SDK defaults for logical lookup ceilings or set explicit APPS_LOOKUP/CGROUPS_LOOKUP budgets in each consumer. The default-safe starting point is to use SDK defaults unless tests show Netdata needs larger per-message budgets for realistic label/path payloads.
9. Run focused Netdata builds/tests for:
   - netipc library build;
   - network-viewer APPS_LOOKUP client tests;
   - apps.plugin APPS_LOOKUP server tests;
   - apps.plugin CGROUPS_LOOKUP client tests;
   - cgroups.plugin CGROUPS_LOOKUP server/test-client tests.
10. Run broader Netdata validation required by the Netdata SOW, then open the PR from the Netdata checkout.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
