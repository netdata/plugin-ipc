# TODO: Pending Items from Rewrite

## Purpose

Restore a fully clean validation state after the rewrite and the repo-wide Rust formatting:

- full cross-language evidence, not partial spot checks
- fail-closed validation when benchmark runs are incomplete or stale
- generated reports that reflect the actual matrix being executed today
- both Linux and Windows reruns green for the supported test/benchmark entrypoints
- commit the verified fixes and refreshed artifacts only after all known current rerun failures are addressed

## TL;DR

Two categories of incomplete work remaining from the plugin-ipc rewrite:

1. **Test coverage**: Target is 100% line coverage across all 3 languages on both POSIX and Windows. Exceptions allowed only if justified and documented.
2. **Benchmark/stress-test matrix**: Must cover all scenarios × all language pairs × both transports × both platforms, including batching and pipelining.

## Review Update (2026-03-16)

Verified facts from the current codebase:

- `tests/run-posix-bench.sh` already runs:
  - C/Rust/Go (full 3×3 pairs)
  - baseline + SHM ping-pong
  - baseline + SHM batch ping-pong
  - snapshot baseline + snapshot SHM
  - local lookup
  - pipeline + pipeline+batch
  - `target_rps` tiers `0`, `100000`, `10000`, `1000` for ping-pong and batch
- `tests/run-windows-bench.sh` already runs the same scenario families and rate tiers, and includes Rust when `bench_windows.exe` exists.
- `CMakeLists.txt` already defines `bench_windows_rs`, so the TODO note saying Windows Rust benchmarking does not exist is stale.
- `tests/run-posix-bench.sh` previously duplicated Rust→Rust baseline/SHM rows under the normal scenario names for a negotiated-profile appendix; this polluted CSV counts and has now been removed.
- Benchmark CSV output now includes explicit `target_rps` metadata:
  - `scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct`
- `tests/generate-benchmarks-posix.sh` and `tests/generate-benchmarks-windows.sh` now:
  - validate exact scenario/rate counts
  - reject stale/incomplete CSV inputs
  - generate batch and pipeline+batch sections
  - accept LF and CRLF CSV inputs
- The committed CSV artifacts in the repo are not reliable proof of completeness:
  - they still use the old header without `target_rps`
  - they were generated before the duplicate-row cleanup / stricter validation
  - implication: they must be regenerated from fresh benchmark runs before they can be used again

## Documentation Review Update (2026-03-17)

Verified facts from `docs/`:

- The top-level docs declare the specifications authoritative:
  - `docs/README.md`
  - "When code and spec disagree, the spec is right unless explicitly revised."
- The Codec docs are clear about borrowed view semantics:
  - `docs/codec.md`
  - decode returns ephemeral typed views
  - zero-allocation decode is the default path
  - library calls/callbacks may aggressively reuse buffers
- The Level 2 docs are partly clear, but internally inconsistent:
  - `docs/level2-typed-api.md` says:
    - Level 2 callers interact only with typed request/response structures
    - server callbacks are strongly typed and single-item
    - typed handlers should not see transport details or wire format
  - the same document later specifies:
    - public managed server uses a raw-byte handler:
      - `handler(method_code, request_payload_bytes, response_buffer) -> success/failure`
    - client examples/signatures still expose caller-managed response buffers for typed calls
- `docs/getting-started.md` reinforces the weaker/rawer L2 shape in examples:
  - C/Rust/Go client examples allocate and pass `resp_buf` / `responseBuf` into public L2 calls
  - C server example uses a raw top-level handler that dispatches to typed codec helpers
  - Rust and Go server examples also expose raw request bytes and raw response buffers / owned response bytes
- `docs/level3-snapshot-api.md` matches the borrowed-view model better:
  - Level 3 consumes ephemeral L2 views and materializes owned cache data

Working conclusion from the docs:

- This is not only implementation drift.
- The authoritative docs already contain two competing L2 models:
  - a stronger model in the principles:
    - L2 is typed and transport-hidden
  - a weaker model in the handler/signature/examples:
    - L2 still exposes raw request bytes and caller-managed response buffers
- Therefore the next code changes must be preceded by a doc correction/normalization pass for Level 2, so implementation is aligned with one explicit model instead of a mixed one.

## Implementation / Validation Update (2026-03-17)

Verified facts from the current code and reruns:

- The public Level 2 spec/docs are now normalized around the typed-only model:
  - `docs/level2-typed-api.md`
  - `docs/getting-started.md`
  - `docs/codec.md`
- The C public header and implementation are now aligned with that typed L2 contract for clients.
- Internal/raw managed-server entrypoints are still available only for internal tests/benchmarks under `NIPC_INTERNAL_TESTING`:
  - public spec remains typed-only
  - internal malformed-response tests and raw benchmark drivers still work
- C call sites that used the old public L2 buffer-based API were updated:
  - fixtures, stress/ping-pong/service tests, and C benchmark drivers now use typed client calls
- Go L1 borrowed-payload test expectations were fixed:
  - `src/go/pkg/netipc/transport/posix/uds_test.go`
  - payloads that must outlive a receive are now copied in the test
- CTest scheduling for Go POSIX transport tests is now serialized with a `RESOURCE_LOCK`:
  - avoids `test_uds_go` vs `test_shm_go` racing on the same fixed SHM path / service names

Linux rerun status:

- `cmake --build build -j4`: passes
- `/usr/bin/ctest --test-dir build --output-on-failure -j4`: `36/36` passed
- Focused SHM transport tests all pass after the latest fix:
  - C: `build/bin/test_shm`
  - Go: `go test ./pkg/netipc/transport/posix -run TestShm -count=1`
  - Rust: `cargo test --manifest-path src/crates/netipc/Cargo.toml transport::shm -- --nocapture`

New root cause found and fixed on 2026-03-17:

- POSIX SHM `client_attach()` in all 3 languages could accept a partially initialized region header.
- Concrete failure mode:
  - `magic`, `version`, and `header_len` were already visible
  - `request_offset`, `request_capacity`, `response_offset`, `response_capacity` could still be zero
  - client attach succeeded with cached capacities `0/0`
  - the first SHM send then failed with `message exceeds SHM area capacity`
- Evidence:
  - reproducible intermittent failure in `shm-batch-ping-pong rust->c @ 1000/s`
  - Rust diagnostic run showed:
    - `batch client: shm send failed: message exceeds SHM area capacity`
    - with a normal request size and cached SHM capacities `0/0`
- Fix implemented:
  - C:
    - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
  - Rust:
    - `src/crates/netipc/src/transport/shm.rs`
  - Go:
    - `src/go/pkg/netipc/transport/posix/shm_linux.go`
  - client attach now treats zero / not-yet-valid area geometry as `NotReady`, so the existing retry loops handle the race correctly
- Regression tests added:
  - C:
    - `tests/fixtures/c/test_shm.c`
  - Rust:
    - `src/crates/netipc/src/transport/shm.rs`
  - Go:
    - `src/go/pkg/netipc/transport/posix/shm_linux_test.go`

Current POSIX benchmark status after the SHM attach fix:

- `tests/run-posix-bench.sh benchmarks-posix.csv 5`: passes, complete `201`-row matrix
- `tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md`: generates the report and fails only on configured floors
- Verified remaining floor violations are limited to `snapshot-shm` max-rate rows:
  - `c -> c`: `624516` vs min `1000000`
  - `rust -> c`: `625029` vs min `1000000`
  - `go -> c`: `595669` vs min `800000`
  - `c -> rust`: `450596` vs min `1000000`
  - `rust -> rust`: `445944` vs min `1000000`
  - `go -> rust`: `462507` vs min `800000`
  - `rust -> go`: `674303` vs min `800000`
- Verified improvement:
  - previous `snapshot-baseline` floor failures are gone in the current rerun
  - the previous functional blocker in `shm-batch-ping-pong rust->c @ 1000/s` is gone

## Decisions

### Made

- Benchmark report generation will be treated as an enforcement layer, not a best-effort pretty-printer.
- Internal benchmark CSVs may evolve when required for correctness of validation/reporting.
- Windows execution environment for this repo is:
  - `ssh win11`
  - `cd ~/src/plugin-ipc.git/`
  - work under `MSYSTEM=MSYS`
  - local workflow is: develop locally, commit, push, then pull/build/test on `win11`
- User-approved exception:
  - the `win11` clone of this repo is disposable for this task
  - it may be cleaned/reset there before pull/build/test if needed
  - this approval does **not** apply to the local Linux worktree
- User direction on 2026-03-17 after the POSIX reruns:
  - commit the validated functional fixes first
  - do a proper root-cause review of the remaining `snapshot-shm` underperformance after that commit
  - implication:
    - the remaining performance-floor failures are not to be “blindly fixed” in this phase without evidence

### Pending

- Current task is to fix the remaining verified rerun gaps and commit:
  - POSIX benchmark floor failures
  - failing Linux coverage gate
  - Windows default build entrypoint
- Decision required: what public L2 server surface C and Rust should expose after the Level 2 spec normalization.
  - evidence collected on 2026-03-17:
    - Go has already been moved to a typed public handler surface:
      - `src/go/pkg/netipc/service/cgroups/types.go`
      - `type Handlers struct { OnIncrement ... OnStringReverse ... OnSnapshot ... SnapshotMaxItems ... }`
    - C still exposes a raw public managed-server callback:
      - `src/libnetdata/netipc/include/netipc/netipc_service.h`
      - `typedef bool (*nipc_server_handler_fn)(void *user, uint16_t method_code, const uint8_t *request_payload, size_t request_len, uint8_t *response_buf, size_t response_buf_size, size_t *response_len_out);`
    - Rust still exposes a raw public managed-server callback returning owned response bytes:
      - `src/crates/netipc/src/service/cgroups.rs`
      - `pub type HandlerFn = Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync>;`
      - `ManagedServer::new(..., _response_buf_size, handler)`
    - both C and Rust public L2 clients still expose caller-managed response buffers:
      - C:
        - `nipc_client_call_cgroups_snapshot(... request_buf, response_buf, ...)`
        - `nipc_client_call_increment(... response_buf, ...)`
        - `nipc_client_call_string_reverse(... response_buf, ...)`
      - Rust:
        - `call_snapshot(&mut response_buf)`
        - `call_string_reverse()` and `call_increment_batch()` still allocate per call internally
    - implication:
      - the remaining refactor is no longer just “make buffers internal”
      - C and Rust need a public typed L2 server registration surface that matches the normalized spec, or they remain out of contract even if internal scratch reuse is fixed
  - concrete design fork:
    - either replace the current public raw managed-server API in C/Rust with a typed cgroups-specific handler surface
    - or keep the raw managed-server public and add a typed wrapper beside it
  - recommendation:
    - replace the current public raw L2 server surface with a typed cgroups handler surface, and keep the raw dispatch/session loop as an internal implementation detail
    - reason:
      - this matches the corrected L2 spec directly
      - it keeps the public surface minimal
      - it avoids having two competing public L2 models again
  - follow-up doc verification on 2026-03-17:
    - the normalized spec is already explicit here, so this is no longer a real design choice:
      - `docs/level2-typed-api.md`
        - "Public Level 2 APIs are strongly typed and single-item by default"
        - server side:
          - "Registers typed callbacks per supported method"
          - "Never exposes raw payload bytes, raw response buffers, or outer transport metadata to the callback"
      - `docs/getting-started.md`
        - "Servers register typed callbacks only"
        - "User code never switches on method_code and never touches raw payload bytes or raw response buffers"
    - implication:
      - option `A` is the spec-aligned implementation
      - options that keep a raw public L2 server API beside the typed one would contradict the current normalized specification
- Decision required: how far to change the Go/Rust L2 server APIs to achieve zero-allocation request handling.
  - evidence collected on 2026-03-17:
    - Go handler API returns owned response bytes:
      - `src/go/pkg/netipc/service/cgroups/types.go`
      - `type HandlerFunc func(methodCode uint16, request []byte) ([]byte, bool)`
    - Rust handler API returns owned response bytes:
      - `src/crates/netipc/src/service/cgroups.rs`
      - `pub type HandlerFn = Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync>`
    - Go server session path copies request payloads into owned slices and allocates response buffers per request:
      - `src/go/pkg/netipc/service/cgroups/client.go`
      - `src/go/pkg/netipc/transport/posix/uds.go`
    - Rust server session path does the same:
      - `src/crates/netipc/src/service/cgroups.rs`
      - `src/crates/netipc/src/transport/posix.rs`
    - C is closer to the desired model already:
      - handler writes into caller-provided response buffer
      - remaining per-request allocations are mostly SHM message assembly and some batch scratch buffers
  - implication:
    - eliminating client/transport scratch allocations is straightforward and should be done
    - eliminating all server-side request allocations in Go/Rust may require changing the public handler API to a caller-buffer/write-into-scratch model closer to C
  - user decision on 2026-03-17:
    - chosen option: `B`
    - change Go/Rust L2 server APIs to match the C-style write-into-caller-buffer model
    - compatibility wrappers are not the chosen path for this task
  - superseded by later API-boundary clarification on 2026-03-17:
    - L1 may expose caller-managed buffers / borrowed payload views
    - L2 must remain strongly typed and must not require caller-side buffer management
    - implication:
      - the library must own and reuse any scratch/response buffers needed internally for L2
      - public L2 APIs should return typed values/views, not require `responseBuf` arguments
      - internal L2 handlers/session loops may still use reusable per-session scratch buffers behind the API
- Decision required: how to handle the Go/Rust L1 `Receive()` API semantics while removing per-request allocations.
  - evidence collected on 2026-03-17:
    - C L1 already exposes borrowed payload views via caller/internal buffers
    - Go and Rust L1 currently return owned payload buffers from `Receive()`
    - after changing Go `Receive()` to return borrowed slices, the existing Go pipelining test starts failing because it stores payloads across later receives:
      - `src/go/pkg/netipc/transport/posix/uds_test.go`
      - the test appends `rPayload` into a request list / response map and compares it after additional receives
    - implication:
    - borrowed-slice `Receive()` aligns Go/Rust with C and supports zero-allocation hot paths
    - but it changes the semantics of a public L1 API that currently behaves like ownership transfer
  - user clarification on 2026-03-17 narrows this decision:
    - caller-managed scratch is acceptable at L1
    - implication:
      - borrowed payload views are acceptable for L1 as long as the lifetime is documented clearly
      - the public-API ownership debate now primarily belongs to L2, not L1
- Decision required: what public ownership/lifetime model L2 should expose once buffer ownership moves inside the library.
  - evidence collected on 2026-03-17:
    - current public L2 client APIs still expose response buffers directly:
      - C: `nipc_client_call_cgroups_snapshot(... request_buf, response_buf, ..., &view_out)`
      - Rust: `call_snapshot(&mut response_buf) -> Result<CgroupsResponseView>`
      - Go: `CallSnapshot(responseBuf) -> (*CgroupsResponseView, error)`
    - current public L2 server APIs in Go and Rust are also not strongly typed yet:
      - Go `HandlerFunc func(methodCode uint16, request []byte, responseBuf []byte) (int, bool)`
      - Rust `HandlerFn = Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync>`
    - C already has protocol-level typed dispatch helpers that match the desired layering better:
      - `nipc_dispatch_increment()`
      - `nipc_dispatch_string_reverse()`
      - `nipc_dispatch_cgroups_snapshot()`
      - these decode raw requests, call typed business handlers, and encode into a provided response buffer
    - L3 caches in Go and Rust already demonstrate the desired ownership boundary:
      - the library owns reusable response buffers internally and exposes typed cache operations to callers
  - implication:
    - removing `responseBuf` from public L2 client calls is not enough by itself
    - public L2 server callbacks should also stop exposing raw transport/request-response buffers if L2 is to remain truly typed
    - some methods need an explicit lifetime model for returned data:
      - snapshot can naturally return an ephemeral typed view backed by internal reusable storage
      - variable-size string/batch results can either return ephemeral typed views, owned results, or both APIs

### User Decisions

- 2026-03-17:
  - Windows `np-pipeline-batch-d16` will be fixed by changing the benchmark clients, not by imposing a lower global limit.
  - Chosen direction: keep logical depth `16`, but add proper client-side flow control so the clients drain responses while maintaining a bounded in-flight window.
  - Scope:
    - Windows benchmark clients in C, Go, and Rust
    - no transport-wide limit reduction
    - no benchmark-only downgrade to depth `8`
- 2026-03-17:
  - User approved running Rust formatting repo-wide.
  - After formatting, rerun all tests and benchmark suites again.
  - Scope:
    - format all Rust code in the repo with the standard formatter
    - rerun the available local Linux test matrix
    - rerun POSIX benchmarks locally
    - rerun Windows tests and benchmarks on `win11`
- 2026-03-17:
  - User wants all currently known rerun gaps fixed and committed.
  - Scope for this phase:
    - make the Windows default build usable on Windows
    - eliminate the remaining current POSIX benchmark floor failures
    - eliminate the current Linux coverage gate failure
    - rerun the affected test and benchmark suites
    - commit only the relevant fixes and refreshed artifacts
- 2026-03-17:
  - User decision for the POSIX snapshot underperformance:
    - do not apply a blind benchmark-only semantic change yet
    - commit the currently verified rerun state first
    - then perform a proper root-cause review of the underperforming implementations
    - benchmark floors/semantics are not to be changed before the root cause is understood
  - User concern to investigate explicitly:
    - the snapshot path is L2 and should likely perform substantially better
    - possible causes may be outside the transport layer
    - L3 cache lookup is only a hypothesis and must be verified from code, not assumed
- 2026-03-17:
  - User decision after root-cause review:
    - precompute the benchmark snapshot payload inputs instead of formatting strings on every request
    - fix the library so L1/L2 request paths do not allocate per request in production
  - implications:
    - benchmark handlers may cache prebuilt names/paths and, if needed, the serialized snapshot payload template
    - library clients/transports need reusable scratch buffers owned by the client/session context, not fresh heap allocations on each call
    - this is a real production-path performance requirement, not a benchmark-only workaround
  - risks to control:
    - reusable buffers must remain per-client/per-session, never shared unsafely across concurrent requests
    - cached snapshot benchmark payloads must keep wire correctness when the generation field changes
    - changes must preserve zero-allocation behavior for both baseline and SHM paths where practical, not just one language or one transport
- 2026-03-17:
  - User clarified the API boundary explicitly:
    - L1 is the bits-and-pieces layer, so caller-managed buffer lifetimes are acceptable there
    - L2 is the strongly typed layer, so the caller should only work with its own typed structures/views
    - L2 must not require the caller to provide scratch/response buffers or manage transport buffers
    - the library must ensure zero-copy / reusable-buffer behavior internally for L2 hot paths
    - L2 contract clarified further:
      - each L2 method owns its typed request encoder and typed response decoder
      - L2 client callers provide typed request objects/values, not transport buffers
      - L2 server callers register typed callbacks and receive typed request objects/values, not raw wire payloads
      - typed response shape determines whether zero-allocation decode is possible via views or requires owned materialization
      - apart from method-specific encode/decode constraints, the rest of the L2 hot path must remain zero-copy / zero-allocation
  - implications:
    - previous direction to expose C-style caller response buffers directly in Go/Rust L2 is not acceptable
    - Go/Rust/C L2 public APIs must converge toward internal buffer ownership with typed return values/views
    - benchmark and transport work must separate internal hot-path scratch management from public L2 caller semantics
    - method-specific codec design now becomes the primary place where allocation policy is decided:
      - fixed-size scalars should decode with no heap allocation
      - structured variable-size payloads should decode to ephemeral typed views over internal reusable buffers where possible
      - owned result materialization should happen only when the typed API explicitly promises ownership
- 2026-03-17:
  - User decision for the documentation/spec work:
    - fix the specifications so Level 2 is explicit and unambiguous
    - L2 must expose the absolute minimum requirements from clients and servers
    - public L2 APIs must be typed and must not leak transport buffers, raw payload bytes, or method-code dispatch to callers
  - scope for this documentation pass:
    - normalize `docs/level2-typed-api.md`
    - normalize `docs/getting-started.md`
    - update any related docs that still reinforce the weaker/raw-byte L2 model
  - expected outcome:
    - one consistent L2 contract across principles, examples, and terminology
    - explicit separation between:
      - L1 caller-managed scratch / payload buffers
      - Codec typed encode/decode + typed builders/views
      - L2 typed convenience API with internal reusable buffers
  - risks to control:
    - typed views returned by L2 need a clearly documented lifetime model
    - any internal reusable buffers must be per-client/per-session to preserve concurrency safety
    - moving buffer ownership inside L2 may require API adjustments in all three languages, not only Go/Rust

### Current Implementation Chunk

- [x] Add explicit run metadata to benchmark CSV output, starting with `target_rps`.
- [x] Update both benchmark generators to validate the full matrix from explicit CSV data and fail on incomplete inputs.
- [ ] Bring the C Level 2 implementation back in sync with the normalized typed public API.
  - Verified on 2026-03-17:
    - `src/libnetdata/netipc/include/netipc/netipc_service.h` already exposes the new typed public L2 contract.
    - `src/libnetdata/netipc/src/service/netipc_service.c` and `src/libnetdata/netipc/src/service/netipc_service_win.c` still implement the old caller-buffer API.
    - the tree does not build until those sources and the C call sites are migrated.

## Plan

1. [x] Update this TODO with verified current-state facts and stale assumptions.
2. [x] Add explicit rate-tier metadata to benchmark CSV rows emitted by the runner scripts.
3. [x] Rewrite the POSIX and Windows markdown generators to:
   - validate expected scenario counts
   - validate expected rate tiers
   - fail on partial/stale CSVs
   - generate sections for batch and pipeline+batch scenarios
4. [x] Verify the scripts using the repo CSVs plus synthetic complete fixtures.
5. [x] Re-run real POSIX and Windows benchmark suites to regenerate CSV + markdown artifacts with the new schema.
   - POSIX rerun completed on 2026-03-16: `benchmarks-posix.csv` now has 201 rows and `benchmarks-posix.md` was regenerated from it.
   - POSIX generator exited non-zero because the measured floors currently fail on real data:
     - `snapshot-baseline`: `c->go 71009`, `rust->go 71400`, `go->go 65838` vs required `>= 100000`
     - `snapshot-shm` C/Rust pairs: `471413` to `624506` vs required `>= 1000000`
     - `snapshot-shm` Go pairs: `106397` to `175814` vs required `>= 800000`
   - First Windows rerun on 2026-03-16 exposed real runtime benchmark failures behind otherwise complete CSV output:
     - `shm-batch-ping-pong`: all 12 rows with `client=c` were emitted with `throughput=0`
     - `np-pipeline-batch-d16`: all 9 language pairs were emitted with `throughput=0`
   - Follow-up Windows rerun on 2026-03-17 completed cleanly on `win11` after the SHM and pipeline+batch client fixes:
     - `benchmarks-windows.csv` and `benchmarks-windows.md` regenerated successfully
     - generator exited zero and reported all configured floors met
     - copied back to the local repo from `win11`
6. [ ] Commit the currently verified rerun state before deeper snapshot-performance changes.
7. [ ] Analyze the remaining rerun gaps with concrete evidence before editing code:
   - Windows default build still broken because `netipc_rust` in `ALL` builds POSIX-only Rust bins on Windows
   - POSIX snapshot benchmark floors still fail on current code
   - Linux C coverage threshold still fails on current code
8. [ ] Complete the allocation audit for L1/L2 request paths across C, Go, and Rust:
   - identify every per-request heap allocation in baseline and SHM client paths
   - separate benchmark-only rebuild costs from production library costs
   - confirm which allocations are unavoidable API-return allocations versus reusable transport scratch buffers
   - redesign public L2 APIs so buffer reuse stays internal to the library, while L1 may continue exposing caller-managed scratch semantics
9. [ ] Implement the required fixes.
   - precompute snapshot benchmark payload inputs to remove per-request string formatting/rebuild cost from the synthetic handlers
   - eliminate per-request production-path allocations in L1/L2 client/service paths by reusing owned scratch buffers
   - keep public L2 APIs strongly typed, with internal buffer ownership/reuse hidden from callers
   - make the Windows default build usable on Windows
   - close the Linux C coverage gap
10. [ ] Rerun the affected Linux and Windows validation paths.
11. [ ] Commit the verified fixes and refreshed artifacts with explicit file selection.

## Fresh Benchmark Evidence (2026-03-16)

- Real POSIX benchmark suite completed successfully with the new runner schema:
  - output file: `benchmarks-posix.csv`
  - generated report: `benchmarks-posix.md`
  - measurement count: `201`
- Real POSIX run confirms that the report path is now trustworthy enough to surface real floor failures instead of silently accepting stale/partial data.
- Concrete floor violations observed in the generated report:
  - `snapshot-baseline` below floor for all `* -> go` pairs:
    - `c -> go`: `71009`
    - `rust -> go`: `71400`
    - `go -> go`: `65838`
  - `snapshot-shm` below floor for all pairs:
    - C/Rust-server pairs ranged from `471413` to `624506` against a `>= 1000000` requirement
    - Go-involved pairs ranged from `106397` to `175814` against a `>= 800000` requirement
- Working conclusion:
  - benchmark reporting/validation bug: **fixed**
  - benchmark floor compliance: **still failing on current code**

- First real Windows benchmark suite on 2026-03-16 completed successfully enough to generate artifacts:
  - output file: `benchmarks-windows.csv`
  - generated report: `benchmarks-windows.md`
  - measurement count: `201`
- First Windows run confirmed:
  - Win SHM ping-pong floor currently passes on `win11`
    - `c -> c`: `2433943`
    - `rust -> c`: `2237817`
    - `c -> rust`: `2299190`
    - `rust -> rust`: `1922360`
  - local lookup floor currently passes on `win11`
    - `c`: `113500082`
    - `rust`: `148968543`
    - `go`: `111807264`
  - the fail-closed story is still incomplete for Windows runtime benchmark errors:
    - `shm-batch-ping-pong` emitted 12 zero-throughput rows for all `c -> *` pairs
    - `np-pipeline-batch-d16` emitted 9 zero-throughput rows for all pairs
    - the current generator accepts these rows because it validates row presence and selected floors, not semantic non-zero throughput for scenarios that should never report zero on a healthy run

- Follow-up debug/update after `bench: fail closed on invalid benchmark runs` (`b6aa295`):
  - both benchmark generators now reject any row with `throughput <= 0`
  - both runner scripts now treat client non-zero exit / missing output / unparsable output / zero throughput as benchmark failure instead of fabricating zero rows
  - Windows C `shm-batch-ping-pong-client` had a real buffer overflow in `bench/drivers/c/bench_windows.c`
    - bug: it assembled `NIPC_HEADER_LEN + req_len` bytes into a fixed `NIPC_HEADER_LEN + 8` stack buffer on the Win SHM batch path
    - fix: reuse the preallocated heap response buffer as the contiguous SHM send buffer
    - verification on `win11`: manual C `shm-batch-ping-pong-client` probe now succeeds (`throughput=25741586`, exit `0`) instead of segfaulting
  - after fail-closed validation was enabled, additional hidden Windows benchmark correctness failures surfaced during a `duration=1` smoke run:
    - `shm-ping-pong` max-rate Go clients failed against C/Rust/Go servers with counter-chain mismatches
    - `shm-ping-pong` also showed non-zero correctness failures for some `c -> go` and `rust -> go` runs
    - implication: the old benchmark path was masking real Windows SHM correctness bugs by accepting client failures as usable measurements
  - concrete root cause identified for the Windows SHM correctness failures:
    - file: `src/go/pkg/netipc/transport/windows/shm.go`
    - issue: the Go HYBRID receive path performed only one `WaitForSingleObject()` wake and then copied the shared-memory payload without looping until `seq >= expected_seq`
    - evidence:
      - C reference implementation in `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c` already uses a deadline-based retry loop
      - Rust reference implementation in `src/crates/netipc/src/transport/win_shm.rs` already uses the same retry loop
      - Microsoft event-object documentation states that auto-reset events remain signaled until a waiter consumes them, so a wake is not equivalent to “new payload is present”
    - implication: a stale event wake could make Go read the previous payload and advance the local sequence, which matches the observed `counter chain broken: expected X, got X-1` failures
    - implemented fix:
      - Go `WinShmReceive()` now mirrors the C/Rust deadline-based retry loop
      - Windows-only regression tests were added to force a spurious wake and verify the second receive returns the new payload, not the stale one
    - verification on `win11`:
      - `go test ./pkg/netipc/transport/windows` passes
      - previously failing SHM pairs now exit cleanly in direct probes:
        - `go -> c`
        - `go -> rust`
        - `go -> go`
        - `c -> go`
        - `rust -> go`
      - a fresh `duration=1` smoke rerun now clears all of:
        - named pipe ping-pong
        - Win SHM ping-pong
        - snapshot baseline
        - snapshot SHM
        - named pipe batch ping-pong
        - Win SHM batch ping-pong
        - local lookup
        - named pipe pipeline
  - `np-pipeline-batch-d16` still has a real runtime bug on Windows:
    - after the Go SHM fix, the remaining failure is now isolated to this scenario
    - fresh `duration=1` smoke rerun on `win11` fails all 9 pairs with client timeout `exit 124`:
      - `c -> c`, `rust -> c`, `go -> c`
      - `c -> rust`, `rust -> rust`, `go -> rust`
      - `c -> go`, `rust -> go`, `go -> go`
    - implication:
      - the old mixed `pipeline-batch client: 1 errors` symptom was at least partly contaminated by the Windows SHM receive bug
      - the current remaining bug is specific to the Windows named-pipe pipeline+batch path itself
  - follow-up fix implemented for Windows `np-pipeline-batch-d16` benchmark clients:
    - files:
      - `bench/drivers/c/bench_windows.c`
      - `bench/drivers/go/main_windows.go`
      - `bench/drivers/rust/src/bench_windows.rs`
    - change:
      - keep logical depth `16`
      - replace the send-all-then-read-all client pattern with bounded in-flight flow control
      - each client now drains responses by `message_id` before in-flight bytes exceed a conservative duplex named-pipe budget
    - targeted verification on `win11`:
      - `c -> c`: exit `0`, throughput `17276276`
      - `go -> go`: exit `0`, throughput `9948091`
      - `rust -> rust`: exit `0`, throughput `12352267`
    - full Windows smoke verification on `win11`:
      - command: `bash tests/run-windows-bench.sh /tmp/benchmarks-windows-smoke.csv 1`
      - result: exit `0`
      - measurement count: `201`
      - `np-pipeline-batch-d16` now passes for all 9 language pairs:
        - `c -> c`: `27039526`
        - `rust -> c`: `27386522`
        - `go -> c`: `23804745`
        - `c -> rust`: `14222790`
        - `rust -> rust`: `14781565`
        - `go -> rust`: `13780909`
        - `c -> go`: `15801053`
        - `rust -> go`: `14520914`
        - `go -> go`: `15431067`
    - working conclusion:
      - the Windows `np-pipeline-batch-d16` failure was caused by the benchmark client flooding a blocking full-duplex named pipe without draining responses
      - no evidence was found for a transport-wide rule that Windows named pipes must cap pipeline depth below `16`
  - second full Windows rerun completed on 2026-03-17 after both Windows fixes:
    - command sequence on `win11`:
      - `cmake --build build --target bench_windows_c bench_windows_go bench_windows_rs -j4`
      - `bash tests/run-windows-bench.sh benchmarks-windows.csv 5`
      - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md`
    - result:
      - benchmark runner exit `0`
      - generator exit `0`
      - measurement count `201`
      - all configured Windows floors met again
    - artifacts copied back locally:
      - `benchmarks-windows.csv`
      - `benchmarks-windows.md`
    - concrete `np-pipeline-batch-d16` results from the real 5-second run:
      - `c -> c`: `23841905`
      - `rust -> c`: `25975022`
      - `go -> c`: `25377574`
      - `c -> rust`: `13494555`
      - `rust -> rust`: `14761094`
      - `go -> rust`: `15518546`
      - `c -> go`: `16791728`
      - `rust -> go`: `15856614`
      - `go -> go`: `13537047`
    - concrete verification that the prior Windows benchmark failures are gone:
      - no zero-throughput `shm-batch-ping-pong` rows
      - no zero-throughput `np-pipeline-batch-d16` rows
      - local parse-only verification on Linux also passes:
        - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv /tmp/benchmarks-windows-verify.md`

## Validation Update (2026-03-17)

- Repo-wide Rust formatting was run via:
  - `cargo fmt --all --manifest-path src/crates/netipc/Cargo.toml`
- Local Linux validation results after formatting:
  - `/usr/bin/ctest --test-dir build --output-on-failure`: `36/36` passed
  - `bash tests/run-go-race.sh`: passed
  - `bash tests/run-sanitizer-asan.sh`: passed
  - `bash tests/run-sanitizer-tsan.sh`: passed
  - `bash tests/run-valgrind.sh`: passed
  - `bash tests/run-extended-fuzz.sh`: passed
- Coverage results after formatting:
  - `bash tests/run-coverage-c.sh`: failed threshold
    - total `90.3%`
    - `netipc_uds.c`: `88.7%`
    - `netipc_service.c`: `86.7%`
  - `bash tests/run-coverage-go.sh`: report completed
    - total `86.4%`
    - lowest major files:
      - `service/cgroups/client.go`: `82.2%`
      - `transport/posix/shm_linux.go`: `78.6%`
      - `transport/posix/uds.go`: `83.7%`
  - `bash tests/run-coverage-rust.sh`: completed with tarpaulin
    - total `86.72%` (`1561/1800`)
- POSIX benchmark rerun after formatting:
  - first full `bash tests/run-posix-bench.sh benchmarks-posix.csv 5` run failed closed on one runtime error:
    - `shm-batch-ping-pong`
    - `rust -> c`
    - `10000/s`
    - stderr: `batch client: 1 errors out of 0 items`
  - targeted repro probe for that exact pair passed cleanly
  - second full `bash tests/run-posix-bench.sh benchmarks-posix.csv 5` run completed successfully with `201` rows
  - `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md` still exits non-zero on the same configured floor violations:
    - `snapshot-baseline` `* -> go`
    - all `snapshot-shm` pairs
- Windows validation and benchmark rerun after formatting:
  - targeted Windows-only rebuild on `win11` completed successfully after `git reset --hard origin/main` and `git clean -fd`
  - the default `cmake --build build` path is still not a valid Windows entrypoint:
    - `CMakeLists.txt` keeps `netipc_rust` in `ALL`
    - that target runs plain `cargo build` and tries to compile POSIX-only Rust bins (`bench_posix`, `interop_uds`, `interop_service`, `interop_cache`) on Windows
    - implication: Windows validation currently requires the explicit Windows-only target list used in this rerun
  - one temporary false failure in `bash tests/test_named_pipe_interop.sh` was traced to a leaked old debug process from `2026-03-15`, not to the current code:
    - stale process: `build/bin/interop_named_pipe_c.exe server /tmp np-debug`
    - effect: Rust server reported `pipe name already in use by live server` and the shell script surfaced that as `server exited early`
    - resolution: kill the exact leaked PIDs on `win11`, rerun the script, confirm all `9/9` Named Pipe interop pairs pass
  - Windows test results after cleanup:
    - `./build/bin/test_protocol.exe`: `245 passed, 0 failed`
    - `./build/bin/fuzz_protocol.exe`: passed
    - `cargo test --lib -- --test-threads=1` in `src/crates/netipc`: `137 passed, 0 failed`
    - `go test ./pkg/netipc/protocol ./pkg/netipc/transport/windows`: passed
    - `bash tests/interop_codec.sh`: passed
    - `./build/bin/test_named_pipe.exe`: `26 passed, 0 failed`
    - `bash tests/test_named_pipe_interop.sh`: `9 passed, 0 failed`
    - `./build/bin/test_win_shm.exe`: `25 passed, 0 failed`
    - `bash tests/test_win_shm_interop.sh`: `9 passed, 0 failed`
    - `bash tests/test_service_win_interop.sh`: `9 passed, 0 failed`
    - `bash tests/test_service_win_shm_interop.sh`: `9 passed, 0 failed`
    - `bash tests/test_cache_win_interop.sh`: `9 passed, 0 failed`
    - `bash tests/test_cache_win_shm_interop.sh`: `9 passed, 0 failed`
  - full Windows benchmark rerun:
    - `bash tests/run-windows-bench.sh benchmarks-windows.csv 5`: completed successfully with `201` rows
    - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md`: exited zero
    - generator result: all configured Windows performance floors met

## Implied Decisions

- A Windows benchmark run without Rust is still useful for local debugging, but it is not sufficient for the final generated report required by this TODO.
- Completeness validation must rely on explicit CSV metadata, not row ordering.
- Generated markdown should cover the matrix already produced by the runners before adding new benchmark features.

## Testing Requirements

- Script-level verification that incomplete CSV inputs fail with a clear error.
- Script-level verification that complete CSV inputs generate markdown successfully.
- Validation that new CSV headers remain accepted by both generator scripts.

## Documentation Updates Required

- Update benchmark generator output format expectations in this TODO.
- Regenerate `benchmarks-posix.md` and `benchmarks-windows.md` only from complete CSV runs after the generators are fixed.

---

## 1. Test Coverage — Target: 100%

### 1.1 Current State

Coverage is measured on **Linux only** via 3 scripts:

| Script | Tool | Files Measured |
|--------|------|----------------|
| `tests/run-coverage-c.sh` | gcov (GCC) | 4 C files: protocol, uds, shm, service |
| `tests/run-coverage-rust.sh` | cargo-llvm-cov or cargo-tarpaulin | netipc crate library code |
| `tests/run-coverage-go.sh` | `go test -coverprofile` | 3 packages: protocol, transport/posix, service/cgroups |

Current threshold: 90%. **New target: 100%.**

Windows: **no instrumented coverage**. Only pass/fail unit tests and interop scripts.

### 1.2 Gaps to Close

#### A. Coverage Infrastructure

- [ ] **Raise threshold from 90% to 100% in all 3 scripts**
  - `run-coverage-c.sh`: change `THRESHOLD=${1:-90}` → `THRESHOLD=${1:-100}`
  - `run-coverage-rust.sh` and `run-coverage-go.sh`: add enforcement (currently report-only)
- [ ] **Add Windows coverage measurement** or document why it's impossible per-tool:
  - C: gcov works with MinGW/GCC on Windows. Feasible.
  - Rust: cargo-tarpaulin is Linux-only. cargo-llvm-cov works on Windows with MSVC toolchain but we cross-compile with `x86_64-pc-windows-gnu`. **Needs investigation**: does llvm-cov work with windows-gnu target?
  - Go: `go test -coverprofile` works natively on Windows. Feasible.
- [ ] **Add coverage to CI** (no CI pipeline exists today — everything is manual)

#### B. Untested Code Paths (POSIX)

- [ ] **Identify and test all uncovered lines** — run coverage scripts, review `.gcov` / tarpaulin / go cover output, add tests for every untested branch
- [ ] **Document justified exclusions** — for each line that cannot reach 100%, document WHY in a `COVERAGE-EXCLUSIONS.md`:
  - Unreachable defensive code (e.g., malloc failure paths in C)
  - Platform-specific branches (e.g., `#ifdef __APPLE__` on Linux-only builds)
  - Each exclusion must state: file, line range, reason, and whether it's tested on another platform

#### C. Untested Code Paths (Windows)

- [ ] **Rust L2 service tests are unix-only** (`#[cfg(all(test, unix))]` at `cgroups.rs:1579`)
  - The entire Rust L2/L3 service module has ZERO unit tests on Windows
  - Only coverage: interop shell scripts (pass/fail, no line-level measurement)
  - **Action**: Add `#[cfg(all(test, windows))]` test module, or make existing tests cross-platform
- [ ] **Go coverage script excludes Windows packages**
  - `run-coverage-go.sh` only measures `transport/posix/`, not `transport/windows/`
  - `pipe_test.go` and `ping_pong_windows_test.go` exist but are not in coverage
  - **Action**: Create `run-coverage-go-windows.sh` that measures Windows packages on Windows
- [ ] **C Windows files not in coverage**
  - `netipc_service_win.c`, `netipc_named_pipe.c`, `netipc_win_shm.c` have no gcov measurement
  - Unit tests exist (`test_named_pipe.c`, `test_win_shm.c`) but coverage not collected
  - **Action**: Create `run-coverage-c-windows.sh` using gcov with MinGW

#### D. Missing Test Types

- [ ] **Fuzz tests on Windows** — currently fuzz tests exist only for POSIX (C: `run-extended-fuzz.sh`, Go: `fuzz_test.go`, Rust: protocol fuzz). Verify they run on Windows or document why not.
- [ ] **Windows stress tests** — `test_stress.c` is POSIX-only (uses pthreads, UDS). No Windows equivalent.

---

## 2. Benchmarking / Stress-Testing

### 2.1 Current State

#### What Exists

**POSIX** (`tests/run-posix-bench.sh`):
- 3 languages: C, Rust, Go
- Full 9-pair cross-language matrix (3×3)
- Scenarios implemented:
  - UDS ping-pong
  - SHM ping-pong
  - snapshot-baseline
  - snapshot-SHM
  - UDS batch ping-pong
  - SHM batch ping-pong
  - local cache lookup
  - UDS pipeline (depth 16)
  - UDS pipeline+batch (depth 16)
- Rate tiers implemented:
  - ping-pong + batch: `0`, `100000`, `10000`, `1000`
  - snapshot: `0`, `1000`
- Current gap is no longer scenario implementation here; it is trustworthy reporting/validation of these runs.

**Windows** (`tests/run-windows-bench.sh`):
- Runner supports C, Rust, Go when the Rust benchmark binary exists
- `CMakeLists.txt` defines `bench_windows_rs`; Cargo also defines the `bench_windows` binary
- Scenarios implemented:
  - NP ping-pong
  - Win SHM ping-pong
  - snapshot-baseline
  - snapshot-SHM
  - NP batch ping-pong
  - Win SHM batch ping-pong
  - local cache lookup
  - NP pipeline (depth 16)
  - NP pipeline+batch (depth 16)
- Rate tiers implemented:
  - ping-pong + batch: `0`, `100000`, `10000`, `1000`
  - snapshot: `0`, `1000`
- Remaining issue: the runner still allows partial 2-language local runs when the Rust binary is absent, but the final report path must reject those as incomplete.

**Stress tests** (separate from benchmarks):
- C: `test_stress.c` — POSIX only (1000/5000 items, 10/50 concurrent clients, rapid connect/disconnect, 60s stability, SHM lifecycle, mixed transport)
- Rust: 6 stress test functions in `cgroups.rs` — POSIX only (`#[cfg(all(test, unix))]`)
- Go: `stress_test.go` — POSIX only (`//go:build unix`)
- **No Windows stress tests in any language**

**Report generation**:
- `tests/generate-benchmarks-posix.sh` → `benchmarks-posix.md`
- `tests/generate-benchmarks-windows.sh` → `benchmarks-windows.md`
- Generators now validate the full matrix from explicit `target_rps` metadata and reject stale/incomplete CSVs.
- The checked-in CSV/markdown artifacts still need regeneration with the new runner output.

### 2.2 Required Full Matrix

#### Scenarios (6 benchmark types)

| # | Scenario | Transport | Description |
|---|----------|-----------|-------------|
| 1 | Ping-pong | Baseline (UDS / Named Pipe) | Single request-response, measures raw latency |
| 2 | Ping-pong | SHM | Single request-response over SHM transport |
| 3 | Ping-pong + batching | Baseline | Batch of random 1–1000 items per request-response |
| 4 | Ping-pong + batching | SHM | Batch of random 1–1000 items per request-response over SHM |
| 5 | Pipelining | Baseline only | Multiple in-flight requests (depth=N), no SHM |
| 6 | Pipelining + batching | Baseline only | Pipelined requests where each carries a random 1–1000 batch |

*Note: Pipelining over SHM is not applicable — SHM is a single shared region, inherently serialized.*

#### Rate Tiers

All scenarios must be tested at **max throughput** (rate=0).

Ping-pong scenarios (1–4) must additionally be tested at:
- 1,000 req/s
- 10,000 req/s
- 100,000 req/s

#### Language Matrix

**Both platforms**: C, Rust, Go — **9 directed pairs** (3×3), full cross-language interop.

#### Full Count

| Platform | Scenario | Pairs | Rate tiers | Subtotal |
|----------|----------|-------|------------|----------|
| POSIX | Ping-pong baseline | 9 | 4 (max, 100k, 10k, 1k) | 36 |
| POSIX | Ping-pong SHM | 9 | 4 | 36 |
| POSIX | Ping-pong + batch baseline | 9 | 4 | 36 |
| POSIX | Ping-pong + batch SHM | 9 | 4 | 36 |
| POSIX | Pipelining baseline | 9 | 1 (max) | 9 |
| POSIX | Pipelining + batch baseline | 9 | 1 (max) | 9 |
| Windows | Ping-pong baseline | 9 | 4 | 36 |
| Windows | Ping-pong SHM | 9 | 4 | 36 |
| Windows | Ping-pong + batch baseline | 9 | 4 | 36 |
| Windows | Ping-pong + batch SHM | 9 | 4 | 36 |
| Windows | Pipelining baseline | 9 | 1 (max) | 9 |
| Windows | Pipelining + batch baseline | 9 | 1 (max) | 9 |
| **Total** | | | | **324** |

*(Plus local cache lookup: 3 languages × 2 platforms = 6 additional)*

### 2.3 Gaps to Close

#### A. Missing Scenarios

- [x] Ping-pong + batching (baseline + SHM) exists in the benchmark drivers and runner scripts on both platforms.
- [x] Pipelining + batching exists in the runner scripts and benchmark drivers.
- [x] Rust and Go pipelining clients exist.
- [ ] What remains is regenerating real benchmark artifacts with the new runner/generator path.

#### B. Missing Language Coverage

- [x] Rust bench driver on Windows exists.
- [x] Final report generation now enforces the full 3×3 Windows matrix and rejects 2×2 local-debug runs.

#### C. Missing Platform Coverage

- [x] Windows pipelining exists in the benchmark runner path.
- [ ] **Windows stress tests** — none exist
  - C stress tests use pthreads/UDS (POSIX-only)
  - Need Windows equivalents using Win32 threads + Named Pipes

#### D. Missing Rate Tiers

- [x] `1k/s` is already present in both benchmark runner scripts.
- [x] Explicit `target_rps` metadata is now part of benchmark CSV output.

#### E. Benchmark Infrastructure

- [x] Batch mode exists in the bench drivers and runner scripts.
- [x] Run scripts already include batch, pipeline+batch, Rust/Go pipelining, and `1k/s`.
- [x] **CSV schema now includes explicit tier metadata**
  - `target_rps` added to benchmark CSV output
  - lookup rows emitted as `target_rps=0`
  - generators now use explicit rate metadata instead of positional slicing
- [x] **Report generators rewritten**
  - validate exact expected counts for complete runs
  - validate the presence of all expected tiers
  - reject incomplete/stale CSVs
  - generate sections for batch and pipeline+batch scenarios
  - stop assuming Windows is always 2×2 or pipeline is C-only
- [ ] **Real artifacts must be regenerated**
  - `benchmarks-posix.csv` / `benchmarks-posix.md`
  - `benchmarks-windows.csv` / `benchmarks-windows.md`

#### F. Benchmark Reporting Requirements (from TODO-rewrite.md, TODO-hardening.md)

Each benchmark run must report (CSV columns already partially exist):
- [ ] **Actual throughput** (req/s) — already present
- [ ] **Server CPU utilization** (% of one core) — already present (from `SERVER_CPU_SEC`)
- [ ] **Client CPU utilization** (% of one core) — already present
- [ ] **Total CPU utilization** (client + server) — already present
- [ ] **Latency percentiles**: p50, p95, p99 (µs) — already present
- [ ] **Correctness verification** — counter chain / payload verification per run (from TODO-rewrite.md: "Each benchmark validates correctness (counter chain verification)")

Additional reporting requirements from TODO-rewrite.md Phase 13:
- [ ] **Performance floors must be enforced** (below = bug, not acceptable):
  - SHM ping-pong max: ≥ 1M req/s for all pairs
  - SHM snapshot refresh max: ≥ 1M req/s for C/Rust pairs, ≥ 800k for Go pairs
  - UDS ping-pong max: ≥ 150k req/s for all pairs
  - UDS snapshot refresh max: ≥ 100k req/s for all pairs
  - Local cache lookup max: ≥ 10M lookups/s for all languages
  - Windows SHM max: ≥ 1M req/s for C/Rust pairs
- [ ] **Reference targets** (match or exceed old implementation):
  - SHM ping-pong: C→C ~3.2M, Rust→Rust ~2.9M, Go→Go ~1.2M
  - UDS ping-pong: C→C ~220k, Rust→Rust ~240k, Go→Go ~164k
  - Local lookup: C ~25M (now ~79M), Rust ~23M (now ~185M), Go ~13M (now ~109M)
- [ ] **Benchmark docs generated from complete runs, never hand-edited**:
  - `benchmarks-posix.md`
  - `benchmarks-windows.md`

---

## 2b. Requirements Extracted from Other TODOs

### From TODO-plugin-ipc.md (decisions 42-43)

> "The level 1 / level 2 / level 3 specs must explicitly require:
> 100% testing coverage, fuzz testing / fuzziness coverage,
> explicit corner-case and abnormal-path coverage, no exceptions."
>
> "Nothing is acceptable for Netdata integration unless the specs
> and implementation together provide enough coverage to make crashes
> from malformed IPC, corner cases, and abnormal situations
> unacceptable by design."

### From TODO-rewrite.md (quality rules)

> "100% test coverage (line + branch) for all library code."
> "Fuzz testing for all decode/parse paths."
> "Cross-language interop tests for all wire contracts."
> "Abnormal path coverage for all failure modes."
> "No exceptions. Nothing integrates into Netdata without this."

### From TODO-hardening.md (completion criteria, unchecked items)

- [ ] 100% line coverage proven in all 3 languages
- [ ] benchmarks-posix.md generated from current code
- [ ] benchmarks-windows.md generated from current code
- [ ] All performance floors met
- [ ] All 4 external reviewers agree: production-ready
- [ ] Costa approves

### From TODO-spec-compliance.md (unchecked items)

- [ ] 1. L2 typed call spec — update docs to match returned-view API
- [ ] 2. L3 status fields — ~~done in previous session~~
- [ ] 3. L2 batch client calls — ~~done in previous session~~
- [ ] 4. L2 server batch dispatch — ~~done in previous session~~
- [ ] 5. SHM interop tests — ~~done in previous session~~
- [ ] 6. Windows service/cache test coverage — still open (Rust service tests unix-only)
- [ ] 7. Coverage tooling — 100% target, not 90%
- [ ] 8. File size discipline — large files still exist (cgroups.rs ~3000 lines)

### From TODO-production-readiness-review.md (blocking findings)

These were identified as blocking for production deployment:
- [ ] SHM session isolation — ~~fixed in SHM redesign~~
- [ ] Protocol violation handling — service layers skip unexpected non-request messages instead of terminating session
- [ ] Client typed-call paths don't validate received header kind/code/message_id before decoding
- [ ] Codec decoders accept overlapping name/path string regions (spec forbids it)
- [ ] No test for unexpected message kinds/codes terminating session
- [ ] No test for rejection of overlapping variable-length fields in cgroups response items

### From TODO-hardening.md Phase H5

> "C MAX_INFLIGHT from 64 to 128" — **now FIXED to unbounded dynamic array**

Pipeline benchmark results from that phase (C→C only):
- depth=1: 175k, depth=4: 395k, depth=8: 533k, depth=16: 629k, depth=32: 656k
- **3.7x throughput improvement at depth 32**
- "Pipelining at Rust and Go bench drivers" was never implemented

### From TODO-plugin-ipc.md (Phase I: Hardening revalidation)

Coverage required for hardening revalidation that is NOT yet done:
- [ ] Disconnect with multiple in-flight requests (pipelining)
- [ ] Reconnect semantics at level 2 and level 3
- [ ] Out-of-order replies under concurrent workers

---

## 7. Feature / Architecture / Decision Gaps (Non-Benchmark)

### 7.1 Pending User Decisions (from TODO-plugin-ipc.md decisions 44-46) — NOW RESOLVED

**Decision 44 — L2 managed server API layering: RESOLVED**
> Yes, expose a generic managed server surface. The current implementation already
> does this: `nipc_server_init()` takes a generic `nipc_server_handler_fn(method_code,
> request_payload)`. Rust `CgroupsServer` and Go `NewServer` similarly accept generic
> handler callbacks. The naming (`CgroupsServer`) is misleading — it's actually generic.
>
> **Action**: Rename `CgroupsServer` → `Server` (or `ManagedServer`) in Rust and Go
> to reflect that it is the generic L2 managed server, not cgroups-specific.

**Decision 45 — Managed server worker-count: RESOLVED**
> `worker_count` 0 or 1 = single-threaded server (no worker pool, inline dispatch).
>
> **Action**: Verify current code handles worker_count=0 and worker_count=1 identically
> (single-threaded). Currently `worker_count` in C controls max concurrent sessions,
> not a separate worker pool. Need to verify behavior at 0 and 1.

**Decision 46 — Managed server shutdown: RESOLVED — Abort is primary, keep drain as optional convenience**
> `stop()` = abort (primary). `drain(timeout_ms)` = optional graceful shutdown with
> deadline, degrades to abort on timeout expiry.
>
> **Action**: Keep both. Document that `stop()` is the primary shutdown path and
> `drain()` is an optional convenience for graceful transitions (config reload,
> clean test teardown). No code changes needed — current implementation is correct.

### 7.2 Protocol Violation Handling (from TODO-production-readiness-review.md)

**VERIFIED** — mostly fixed, one remaining issue:

- [x] ~~Service layers skip unexpected non-request messages~~ — **FIXED**: All 6 implementations (C/Rust/Go × POSIX/Windows) now break out of the session loop when `kind != REQUEST`, terminating the session.
- [x] ~~Client typed-call paths don't validate response headers~~ — **FIXED in single-call paths**: All 6 implementations validate kind, code, message_id, and transport_status in `do_raw_call()`.
- [ ] **C batch path missing message_id validation** — `do_increment_batch_attempt()` in `netipc_service.c` and `netipc_service_win.c` checks kind, code, transport_status, item_count but **NOT message_id**. Rust and Go batch paths DO check message_id. This is a cross-language inconsistency and a correctness bug.
- [ ] No test verifies that unexpected message kinds/codes terminate the session (test gap, not code gap).

### 7.3 Codec Overlap Validation (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 3 languages already implement overlap rejection:
- C: `netipc_protocol.c:529-537` — explicit `name_start < path_end && path_start < name_end` check
- Rust: `cgroups.rs:231-240` — identical logic
- Go: `cgroups.go:241-250` — identical logic

The spec (`docs/codec-cgroups-snapshot.md`) does NOT explicitly mention overlap rejection in its validation rules. **Action**: Add overlap rejection to the spec's validation rules section to document existing behavior.

### 7.4 L2 Handler Shape (from TODO-shm-redesign.md finding #3)

**VERIFIED — consistent**: The server dispatch is generic `(method_code, raw_payload) → raw_response`. Typed dispatch helpers exist at the codec layer (dispatch_increment, dispatch_string_reverse, dispatch_cgroups_snapshot) for use INSIDE handlers, but the server itself is method-agnostic. This is correct and consistent.

### 7.5 Server Naming (from Decision 44 verification)

**VERIFIED — naming is misleading**: `CgroupsServer` (Rust), `package cgroups` (Go) contain zero cgroups-specific logic. The managed server is 100% generic: accepts any method_code, passes raw bytes to the handler. Only the typed CLIENT calls and L3 cache are cgroups-specific.

**Action**: Rename and/or restructure:
- Rust: `CgroupsServer` → `ManagedServer` or move to `service/mod.rs`
- Go: Move generic server to `service/server.go`, keep cgroups client/cache in `service/cgroups/`
- C: Already generic (`nipc_managed_server_t`) — no rename needed

### 7.6 File Size Discipline (from TODO-spec-compliance.md #8, TODO-hardening.md)

Large files still exist. TODO-rewrite.md says "Small files, small functions, single purpose":
- `cgroups.rs`: ~3000 lines
- `posix.rs`: ~2081 lines
- `netipc_service.c`: ~1482 lines
- `netipc_service_win.c`: ~1300 lines
- `client.go`: ~829 lines

Decision from H8: "DEFERRED: file splitting after integration." This needs a decision from Costa.

### 7.7 Rust/Go L3 Cache Lookup Performance (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 3 languages now use O(1) hash lookup:
- C: Open-addressing hash table (Phase H8)
- Rust: `HashMap<(u32, String), usize>` at `cgroups.rs:1419`, lookup at `cgroups.rs:1541`
- Go: `map[cacheKey]int` at `cache.go:25`, lookup at `cache.go:110`

No action needed.

### 7.8 L3 Response Buffer Sizing (from TODO-production-readiness-review.md)

**VERIFIED — ALREADY FIXED**: The TODO finding was stale. All 4 implementations derive the buffer from config:
- If `max_response_payload_bytes > 0`: uses `header_size + max_response_payload_bytes`
- Otherwise: falls back to 65536 default

This is correct. The 64KB fallback is only when no explicit config is provided.

### 7.9 L2 Typed Call Spec (from TODO-spec-compliance.md #1)

> `level2-typed-api.md` describes callback-based delivery but code
> returns decoded views/values directly.

**Action**: Update `docs/level2-typed-api.md` to match the actual returned-view API pattern. Low priority — documentation alignment only.

### 7.10 Windows SHM Interop Failures (from TODO-plugin-ipc.md current status)

Documented failing live pairs (pre-SHM redesign). **Need to verify on win11** whether still failing after per-session SHM redesign.

### 7.11 SEQPACKET First-Packet Truncation (from TODO-shm-redesign.md finding #5)

> "If caller buffer < first packet, kernel truncates silently. Mitigated
> by dynamic buffer allocation but edge case exists."

No test covers this edge case.

### 7.12 C Helper Negative Test Gap (from TODO-hardening.md Phase H2)

> "The C helper still relies more on live/integration coverage than on
> small deterministic helper-only tests."

Rust and Go have deterministic helper-level negative tests. C does not.

### 7.13 Remaining SHM Coverage Gaps (from TODO-hardening.md Phase H2)

- [ ] SHM-mode L2 service test (client+server with SHM profile negotiation)
- [ ] Server session capacity and growth paths
- [ ] Cache build failure paths (malloc failure injection)

### 7.14 Worker Count Semantics (from Decision 45 verification)

**VERIFIED — inconsistent across languages:**

| Language | worker_count=0 | worker_count=1 | Mechanism |
|----------|---------------|----------------|-----------|
| C (POSIX/Windows) | **REJECTED** (`ERR_BAD_LAYOUT`) | 1 concurrent session (thread per session) | Counter check at accept |
| Rust | Silently clamped → 1 | 1 concurrent session (thread per session) | Thread handle tracking |
| Go POSIX | Silently clamped → 1 | 1 concurrent session (goroutine) | Buffered channel semaphore |
| Go Windows | N/A (no parameter) | Always serial (inline) | No concurrency |

**Issues:**
- ~~C rejects 0~~ — **FIXED**: C now clamps `worker_count < 1` → 1, matching Rust/Go.
- None of the implementations have true "inline dispatch" — worker_count=1 still spawns a thread/goroutine for the single session. This is fine functionally.
- Go Windows has no worker_count parameter at all — always serial.

---

## 8. Consolidated Priority List (Post-Analysis)

### BLOCKING for production (must fix before Netdata integration)

1. ~~Protocol violation handling~~ — **VERIFIED FIXED** (session terminates on bad kind). ~~C batch path missing message_id check~~ — **NOW FIXED** in both `netipc_service.c` and `netipc_service_win.c`.
2. ~~Client response validation~~ — **VERIFIED FIXED** in all paths (single + batch).
3. ~~Codec overlap rejection~~ — **VERIFIED ALREADY IMPLEMENTED**. Add to spec docs.
4. 100% test coverage across all 3 languages (current: 70-98% depending on file)
5. ~~Rust/Go L3 cache O(1)~~ — **VERIFIED ALREADY IMPLEMENTED** (HashMap/map).

### HIGH priority (spec compliance / performance)

6. Complete benchmark matrix (324 runs + cache lookups)
7. Rust Windows bench driver (does not exist)
8. Pipelining for Rust and Go bench drivers (only C has it)
9. Batching benchmarks (random 1-1000, no bench driver supports batching today)
10. Windows stress tests (none exist in any language)
11. Rust L2 service unit tests on Windows (`#[cfg(all(test, unix))]` — zero Windows tests)
12. ~~Resolve pending decisions (44, 45, 46)~~ — **ALL RESOLVED** (see section 7.1)
13. Verify Windows SHM interop post-redesign (need to test on win11)
14. ~~Fix C `worker_count=0` rejection~~ — **NOW FIXED** (clamps to 1, matching Rust/Go)
15. Rename `CgroupsServer` → generic name in Rust/Go (misleading naming)

### MEDIUM priority (quality / completeness)

16. ~~L3 response buffer sizing~~ — **VERIFIED ALREADY CORRECT** (derived from config)
17. File size discipline — large files need splitting (Costa decision needed)
18. L2 typed call spec document alignment (`level2-typed-api.md`)
19. Add codec overlap rejection to spec (`codec-cgroups-snapshot.md`)
20. SEQPACKET first-packet truncation test
21. C helper deterministic negative tests
22. SHM-mode L2 service test (L2 client+server with SHM negotiation)
23. Test: unexpected message kind terminates session
24. Performance floors enforcement in CI

---

## 3. Decisions (Resolved)

1. **Windows Rust coverage**: Documented exclusion. Rust Windows coverage relies on interop tests + code review. Justified: cargo-tarpaulin is Linux-only, llvm-cov doesn't work with windows-gnu target.
2. **Pipelining on Windows**: Implement and test. Named Pipes should support pipelining — investigate any claim otherwise.
3. **Batch sizes for benchmarks**: Random 1–1000 per request.

---

## 4. Implementation Plan

### Phase A: Quick Fixes
Small, independent, low-risk items.

1. Rename `CgroupsServer` → `ManagedServer` in Rust; restructure Go server naming
2. Add overlap rejection rule to `docs/codec-cgroups-snapshot.md`
3. Update `docs/level2-typed-api.md` to match returned-view API
4. Add 1k/s rate tier to both run scripts (one-line each)
5. Add test: unexpected message kind terminates session (C, Rust, Go)
6. Add test: SEQPACKET first-packet truncation edge case
7. Add C helper deterministic negative tests (matching Rust/Go pattern)
8. Add SHM-mode L2 service test (client+server with SHM profile negotiation)
9. Commit, push, verify on win11

### Phase B: POSIX Coverage to 100%
1. Run all 3 coverage scripts, record baseline numbers
2. Analyze every uncovered line — categorize: testable vs exclusion
3. Write targeted tests for all testable uncovered lines
4. Create `COVERAGE-EXCLUSIONS.md` for genuinely untestable paths
5. Raise thresholds to 100%
6. Verify all 3 scripts pass at 100% (with exclusion list)
7. Commit, push

### Phase C: Windows Coverage
1. Create `run-coverage-c-windows.sh` (gcov with MinGW)
2. Create `run-coverage-go-windows.sh` (`go test -coverprofile` for Windows packages)
3. Document Rust Windows coverage exclusion
4. Add Rust L2 service tests for Windows
5. ssh win11, run coverage, identify gaps, write tests
6. Commit, push

### Phase D: Benchmark Drivers — Batching + Pipelining

**D1: POSIX batching** (C, Rust, Go):
- Add batch ping-pong subcmds (baseline + SHM)
- Random 1–1000 batch size, negotiate batch_items=1000
- Server per-item dispatch + reassemble

**D2: POSIX pipelining for Rust + Go**:
- Add `uds-pipeline-client` to Rust and Go bench drivers

**D3: POSIX pipelining + batching** (C, Rust, Go):
- Add `uds-pipeline-batch-client` combining D1 + D2

**D4: Rust Windows bench driver**:
- Create `bench_windows.rs` + CMake target
- Mirror C/Go Windows driver subcmds

**D5: Windows batching + pipelining** (C, Rust, Go):
- Port D1/D2/D3 to Windows bench drivers
- Test on win11

### Phase E: Benchmark Run Scripts + Reports
1. Update `run-posix-bench.sh` for full 162-run matrix
2. Update `run-windows-bench.sh` for full 162-run matrix
3. Update both report generators for new scenarios + rate tiers
4. Run full POSIX matrix, generate `benchmarks-posix.md`
5. ssh win11, run Windows matrix, generate `benchmarks-windows.md`
6. Verify all performance floors
7. Commit, push

### Phase F: Stress Test Parity
1. Port C stress tests to Windows (`test_stress_win.c`)
2. Add Windows stress tests to Rust and Go
3. Add batch stress tests (random 1–1000 under concurrent load)
4. Test on win11
5. Commit, push

### Phase G: Windows SHM Verification
1. ssh win11, run full SHM interop matrix
2. Verify the 4 previously-failing pairs
3. Fix if still broken
4. Commit, push

### Phase H: Final Validation + Multi-Agent Review
1. Run all tests on Linux and Windows
2. Run full benchmark matrices on both platforms
3. Run 4 external reviewers
4. Fix findings, re-review until clean
5. Costa final review

### Decisions
- File splitting: **DEFERRED** to post-integration (Costa decision 2026-03-16)

### Progress (2026-03-16)

| Phase | Status | Notes |
|-------|--------|-------|
| A: Quick fixes | **DONE** | Rename, tests, spec docs, rate tier |
| B: POSIX coverage | **DONE** | C: 90.5%, Rust: 87.0%, Go: 86.2% (298 new tests total) |
| C: Windows coverage | Not started | Scripts + gap tests for C and Go on Windows |
| D: All bench drivers | **DONE** | C/Rust/Go × POSIX/Windows, all 6 scenarios |
| E: POSIX benchmarks | **DONE** | 325 measurements, full matrix |
| E: Windows benchmarks | **NEEDS RE-RUN** | Previous run used pre-QPC-fix binaries |
| F: Stress parity | Not started | Windows stress tests (C/Rust/Go) |
| G: Windows SHM verify | Not started | Verify 4 previously-failing interop pairs |
| H: Final validation | Not started | Multi-agent review |

### Bugs found and fixed during implementation (10 total)
1. Server batch dispatch: `item_count > 1` → `>= 1` (all 5 implementations)
2. C batch bench: goto-based error handling caused protocol desync
3. Server batch builder buffer overlap with per-item scratch
4. Rust SHM batch client missing SHM attach
5. C inflight caps (128/64) replaced with unbounded dynamic arrays
6. C batch response missing message_id validation
7. C worker_count=0 rejected instead of clamped
8. C Windows server missing batch dispatch entirely
9. C/Rust Windows SHM spurious wake bug (deadline-based retry)
10. C/Rust Windows bench QPC overhead: 72k→2.5M (C), 53k→1.6M (Rust), lookup 3.7M→125-150M

### Current Windows benchmark results (after QPC fix)

| Scenario | C | Rust | Go | Historical |
|----------|---|------|----|------------|
| SHM ping-pong | **2.5M** | **1.6M** | **2.7M** | C 3.2M, Rust 4.5M |
| NP ping-pong | 17k | 20k | 18k | — |
| Cache lookup | **125M** | **150M** | **114M** | — |

### Remaining work

1. [ ] **Re-run full Windows benchmark matrix** with QPC-fixed binaries (all 3 languages)
2. [ ] **Windows stress tests** (Phase F) — none exist in any language
   - Port C test_stress.c to Windows (Win32 threads + Named Pipes)
   - Add Rust/Go Windows stress tests
   - Add batch stress tests
3. [ ] **Windows SHM interop verification** (Phase G) — verify 4 previously-failing pairs
4. [ ] **Windows coverage scripts** (Phase C) — run-coverage-c-windows.sh, run-coverage-go-windows.sh
5. [ ] **Rust SHM performance** — 1.6M vs 4.5M historical (investigate)
6. [ ] **Go snapshot baseline** — 50k vs C 121k (bench driver per-request allocation)
7. [ ] **Regenerate benchmark reports** (benchmarks-posix.md, benchmarks-windows.md)
8. [ ] **Final multi-agent review** (Phase H) — 4 external reviewers, fix findings, re-review

### Dependencies
- A, B, D, F, G: independent — can run in parallel
- C depends on B (methodology)
- E depends on D (bench drivers must exist)
- H depends on all others

---

## 5. Size Limitations Analysis

Costa asked: "Are there any limitations in packet size, message size, batch size, etc.? There shouldn't be any apart from numeric overflow."

### 5.1 Wire Format Field Types — The Real Constraints

All wire format fields that govern sizes are **u32** (4 bytes unsigned):

| Field | Type | In Header | Max Theoretical | Constrains |
|-------|------|-----------|-----------------|------------|
| `payload_len` | u32 | Outer header | 4,294,967,295 bytes (~4GB) | Max payload per message |
| `item_count` | u32 | Outer header | 4,294,967,295 | Max batch items per message |
| `total_message_len` | u32 | Chunk header | 4,294,967,295 bytes (~4GB) | Max reassembled chunked message |
| `chunk_count` | u32 | Chunk header | 4,294,967,295 | Max chunks per message |
| Batch `offset` | u32 | Batch dir entry | 4,294,967,295 | Max offset within payload |
| Batch `length` | u32 | Batch dir entry | 4,294,967,295 | Max single batch item size |
| SHM `request_capacity` | u32 | SHM region header | 4,294,967,295 | Max SHM request area |
| SHM `response_capacity` | u32 | SHM region header | 4,294,967,295 | Max SHM response area |

**Verdict**: No arbitrary limits. All size fields are u32 — the practical ceiling is ~4GB per message, which is the u32 numeric overflow boundary. This is by design.

### 5.2 Negotiated Limits — Runtime, Not Hardcoded

All directional limits are negotiated per-session during handshake:

| Negotiated Field | Type | Default | Meaning |
|------------------|------|---------|---------|
| `max_request_payload_bytes` | u32 | 1024 | Per-direction payload ceiling |
| `max_response_payload_bytes` | u32 | 1024 | Per-direction payload ceiling |
| `max_request_batch_items` | u32 | 1 | Per-direction batch ceiling |
| `max_response_batch_items` | u32 | 1 | Per-direction batch ceiling |
| `packet_size` | u32 | SO_SNDBUF or 65536 | Triggers chunking, not a message limit |

**Negotiation rule**: `min(client_value, server_value)` per field.

Defaults are conservative (1024 bytes, 1 batch item) but fully configurable up to u32 max by both client and server. This is correct — no artificial caps.

### 5.3 Actual Bugs / Issues Found

These are theoretical u32 overflow issues at ~4GB boundaries. Costa confirmed: "u32 limits are ok. We will never pass that many data in a single message or batch." No action needed.

### 5.4 Hardcoded Limits That Are NOT Size-Related

| Limit | Value | Where | Why |
|-------|-------|-------|-----|
| Inflight IDs (C) | Unbounded | Dynamic `realloc` array | Grows from 16, doubling (FIXED) |
| Inflight IDs (Rust) | Unbounded | `HashSet<u64>` | No limit |
| Inflight IDs (Go) | Unbounded | `map[uint64]struct{}` | No limit |
| `run_dir` buffer | 256 chars | C service structs | Fixed char array |
| `service_name` buffer | 128 chars | C service structs | Fixed char array |
| `sun_path` (UDS) | 108 chars | OS (POSIX) | Kernel AF_UNIX limit |
| `NIPC_NP_MAX_PIPE_NAME` | 256 chars | C Named Pipe | Pipe name buffer |
| `NIPC_WIN_SHM_MAX_NAME` | 256 chars | C Win SHM | Kernel object name |

**Cross-language inconsistency**: ~~C had hard inflight caps (128/64) via fixed arrays.~~ **FIXED**: C inflight tracking now uses dynamically grown arrays (realloc, starting at 16, doubling), matching Rust (HashSet) and Go (map) — no cap.

### 5.5 Summary

**Costa's expectation is correct**: there are no arbitrary size limitations beyond the u32 field types (~4GB ceiling). The protocol is designed with:
- Negotiated limits (configurable up to u32 max)
- Chunking (transparent, handles payloads larger than packet_size)
- No hardcoded payload caps

**Two correctness bugs** exist at the u32 overflow boundary (SHM region layout, send truncation), but they're not practical concerns.

**One cross-language inconsistency**: C inflight caps (128/64) vs Rust/Go unbounded. This matters for pipelining benchmarks.

---

## 6. Summary of What's Missing (Coverage + Benchmarks)

| Area | POSIX | Windows |
|------|-------|---------|
| Coverage measurement | C, Rust, Go (at 90%) | None |
| Coverage target | 90% → 100% | N/A → 100% |
| Ping-pong bench (baseline) | C, Rust, Go — 9 pairs, 3 rates | C, Go — 4 pairs, 3 rates (no Rust, no 1k/s) |
| Ping-pong bench (SHM) | C, Rust, Go — 9 pairs, 3 rates | C, Go — 4 pairs, 3 rates (no Rust, no 1k/s) |
| Ping-pong + batch bench | **Missing entirely** | **Missing entirely** |
| Pipelining bench | **C only** (no Rust, no Go) | **Missing entirely** |
| Pipelining + batch bench | **Missing entirely** | **Missing entirely** |
| Rate tiers | max, 100k, 10k (missing 1k) | max, 100k, 10k (missing 1k) |
| Rust bench driver | ✓ (POSIX only) | **Missing** |
| Stress tests | C, Rust, Go ✓ | **Missing entirely** |
| Rust service unit tests | ✓ (unix) | **Missing** (`#[cfg(all(test, unix))]`) |
