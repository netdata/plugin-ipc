## Purpose

Make the public L2/L3 APIs OS-transparent for Netdata plugins within each language: plugin source code using C L2/L3 APIs must compile unchanged on Linux and Windows, plugin source code using Rust L2/L3 APIs must compile unchanged on Linux and Windows, and plugin source code using Go L2/L3 APIs must compile unchanged on Linux and Windows. This task is not about making C, Rust, and Go look the same to each other.

## TL;DR

- Costa decided that L2/L3 caller code must not care whether the target OS is POSIX or Windows.
- In C, callers must keep strongly typed caller-allocated structs, including stack allocation. Public `void *`-style heap handles are not acceptable.
- The plan is to unify the public L2/L3 surface across Linux and Windows separately for C, Rust, and Go, while keeping each language natural, lightweight, and high-performance in its own way.

## Decisions

### Made

- Public L2/L3 plugin-facing APIs must compile transparently on Linux and Windows from the same source code within each language.
- C callers must be free to allocate L2/L3 structs themselves, including on the stack.
- Public `void *` handles are rejected for C L2/L3.
- C public struct strategy: keep shared public type names, but allow OS-specific internal field layout behind them for the first implementation milestone.
- OS-specific knobs such as `backlog` must be removed from public L2/L3 configs and kept in L1/internal transport configuration only.
- The task is Linux/Windows API unification within each language, not cross-language API unification between C, Rust, and Go.
- Each language must keep its own native patterns, primitives, and ergonomics.
- Do not emulate C APIs in Rust or Go.
- Do not impose Rust-style or Go-style patterns on C.
- L2 must remain lightweight and close to raw protocol performance for each language.
- There is no migration bridge end-state for L2/L3 tests in this repo: all repo L2/L3 tests must use the correct native public L2/L3 API directly.
- The temporary C `NIPC_INTERNAL_TESTING` compatibility bridge for old L2/L3 transport-shaped config calls has been removed. Repo L2/L3 tests and benchmark callers now have to use the native public service-level API directly.
- Final task close-out must include syncing the verified benchmark artifacts into the tracked repo, committing the task files explicitly, and pushing the result.

### Pending

- None currently. The product-level contract is now clear.

## Analysis

- The repository documentation already states the intended abstraction boundary:
  - `README.md` says Level 2 and Level 3 are transport-agnostic from the caller perspective.
  - `docs/level2-typed-api.md` says callers must not see transports or transport buffers.
  - `docs/level3-snapshot-api.md` says Level 3 must depend exclusively on Level 2.
- The current implementation does not fully meet that contract yet.

### Current public API leaks by language

#### C

- Public L2/L3 signatures still expose transport-specific config types:
  - `nipc_client_init(..., const nipc_np_client_config_t* / const nipc_uds_client_config_t*)`
  - `nipc_server_init_typed(..., const nipc_np_server_config_t* / const nipc_uds_server_config_t*)`
  - `nipc_cgroups_cache_init(..., const nipc_np_client_config_t* / const nipc_uds_client_config_t*)`
- Public L2/L3 structs embed transport internals directly:
  - `nipc_client_ctx_t` embeds `nipc_np_session_t` or `nipc_uds_session_t`, plus OS-specific SHM pointer types.
  - `nipc_session_ctx_t` embeds `HANDLE` on Windows and `pthread_t` on POSIX.
  - `nipc_managed_server_t` embeds OS listener and lock types.
  - `nipc_cgroups_cache_t` embeds `nipc_client_ctx_t`, so the L3 public type also leaks transport internals.
- This means the public C surface is not OS-transparent today, even though the type names are mostly shared.

#### Rust

- The public service layer imports transport config types conditionally:
  - `service/cgroups.rs` imports `transport::posix::{ClientConfig, ServerConfig}` on Unix.
  - `service/cgroups.rs` imports `transport::windows::{ClientConfig, ServerConfig}` on Windows.
- Public type names and method names are already aligned:
  - `CgroupsClient::new()`
  - `ManagedServer::new()`
  - `ManagedServer::with_workers()`
  - `CgroupsCache::new()`
- The remaining leak is config typing and semantics, not the service object model.

#### Go

- Public L2/L3 constructors still expose transport packages:
  - `cgroups.NewClient(..., posix.ClientConfig)` on Unix
  - `cgroups.NewClient(..., windows.ClientConfig)` on Windows
  - `cgroups.NewCache(..., posix.ClientConfig)` on Unix
  - `cgroups.NewCache(..., windows.ClientConfig)` on Windows
- Go also has a real public API mismatch:
  - POSIX exports `cgroups.NewServerWithWorkers(...)`
  - Windows exposes only `cgroups.NewServer(...)`
- So Go currently violates the “same plugin source compiles unchanged” goal more clearly than Rust.

### Current documentation leaks the wrong public contract

- `docs/getting-started.md` uses `nipc_uds_client_config_t` and `nipc_uds_server_config_t` in the public C L2/L3 examples.
- The Go examples import and use `transport/posix` config types directly in the L2 examples.
- The docs currently teach the transport-specific public API shape, so documentation must be updated as part of the same work.

### Current C test and benchmark code is coupled to public internals

- A large amount of C test and benchmark code directly accesses L2 internals such as:
  - `client.state`
  - `client.session`
  - `client.shm`
  - `client.transport_config`
  - `server.worker_count`
  - `server.sessions_lock`
  - `cache.client`
- This matters because unifying the plugin-facing public API is easy; removing or hiding public internals is harder because repo-internal tests and benchmark drivers currently depend on them.
- The repo already has the start of an internal/testing split:
  - `NIPC_INTERNAL_TESTING`
  - `nipc_server_init_raw_for_tests()`
  - test helper headers such as `tests/fixtures/c/test_win_raw_client_helpers.h`
- This suggests the correct direction:
  - public L2/L3 API gets cleaner
  - repo-internal tests/benchmarks use explicit internal-only paths instead of public plugin-facing structs

### Real OS differences that should not leak through L2/L3

- POSIX server config currently exposes `backlog`; Windows server config does not.
- `packet_size = 0` already means different things in POSIX vs Windows transport configs.
- Named pipe accept/stop/wakeup behavior is different from POSIX listen/accept behavior.
- These are real L1 concerns. They should exist internally, but they should not force plugin callers to branch at the L2/L3 source level.

### External precedent

- `libuv` is a useful C precedent:
  - same public type names and init functions on all OSes
  - caller-allocated structs remain valid
  - OS-specific internal fields are compiled into the public structs through platform macros
- This is closer to Costa's requirement than a heap-only opaque-handle model.

### Conclusions from the analysis

- The desired contract is achievable:
  - same plugin source code for L2/L3 on Linux and Windows within each language
  - same public config type names per language
  - same public init/create signatures per language
  - same public callback signatures per language
  - caller-allocated C structs, including stack allocation
- The desired contract does NOT require removing platform-specific implementation code from the library.
- The main architectural work is:
  - move transport-specific config/types below the public L2/L3 boundary
  - create an explicit internal-only path for tests/benchmarks that need transport internals
  - normalize the public config semantics so the same field name means the same thing on both OSes
  - preserve language-native API design instead of forcing cross-language symmetry

## Plan

### Recommended target contract

- Netdata plugins using L2/L3 APIs must compile unchanged on Linux and Windows from the same source code within each language.
- Public L2/L3 APIs must not require transport-specific includes/imports or config types.
- Public L2/L3 callbacks must stay service-level and typed.
- C callers must continue to allocate L2/L3 structs directly, including on the stack.
- OS-specific transport behavior stays below the public L2/L3 boundary.
- C, Rust, and Go are allowed and expected to expose different APIs from each other, as long as each language is internally unified across Linux and Windows.
- Each language must use natural, lightweight, high-performance idioms for that language.

### Staged implementation plan

#### Stage 1: Define the public contract explicitly

- Add a short API contract section to the TODO and docs:
  - L1 may be OS-specific.
  - L2/L3 public APIs must be OS-transparent.
  - Plugin code must not need source-level platform branching for L2/L3.
- Freeze the desired public surface before implementation:
  - C unified `nipc_client_config_t` / `nipc_server_config_t`
  - Rust service-level `ClientConfig` / `ServerConfig`
  - Go service-level `ClientConfig` / `ServerConfig`

### Current implementation status

- Completed:
  - removed the temporary C L2/L3 compatibility shim from `netipc_service.h`
  - updated C L2/L3 tests, interop fixtures, and benchmark drivers to use the native service-level configs on typed entrypoints
  - kept raw dispatch tests on explicit raw/internal APIs only
  - rebuilt the affected POSIX C targets successfully from the normal build tree
  - rebuilt the affected Windows C targets successfully from a Zig-based cross-build tree
  - removed the last Rust typed-test raw transport injection from `service/cgroups_unix_tests.rs`
  - removed the last Go typed-test transport import from `service/cgroups/cgroups_unix_test.go`
  - rechecked the typed docs and typed interop fixtures so they no longer teach or use transport-specific L2/L3 APIs
  - ran the full native Windows test and benchmark suite from a real `win11` shell and captured the resulting benchmark evidence
  - synced the verified POSIX and Windows benchmark artifacts back into the tracked repo

### Proposed public API shape for the first implementation pass

#### C

- Public unified L2/L3 config types in `netipc_service.h`:
  - `nipc_client_config_t`
  - `nipc_server_config_t`
- Recommended public fields for both:
  - `supported_profiles`
  - `preferred_profiles`
  - `max_request_payload_bytes`
  - `max_request_batch_items`
  - `max_response_payload_bytes`
  - `max_response_batch_items`
  - `auth_token`
- Excluded from public L2/L3 configs:
  - `packet_size`
  - `backlog`
  - any future transport-only tuning
- Recommended signature set:
  - `void nipc_client_init(nipc_client_ctx_t *ctx, const char *run_dir, const char *service_name, const nipc_client_config_t *config);`
  - `nipc_error_t nipc_server_init_typed(nipc_managed_server_t *server, const char *run_dir, const char *service_name, const nipc_server_config_t *config, int worker_count, const nipc_cgroups_service_handler_t *service_handler);`
  - `void nipc_cgroups_cache_init(nipc_cgroups_cache_t *cache, const char *run_dir, const char *service_name, const nipc_client_config_t *config);`
- Recommended caller contract:
  - callers may allocate the structs on the stack or inside larger typed objects
  - plugin-facing code should not need to inspect transport/session internals directly

#### Rust

- Define service-level config types in the public Rust `service` layer:
  - `ClientConfig`
  - `ServerConfig`
- Re-export them from typed service modules for ergonomic use:
  - `netipc::service::cgroups::ClientConfig`
  - `netipc::service::cgroups::ServerConfig`
- Keep the existing service object names:
  - `CgroupsClient`
  - `ManagedServer`
  - `CgroupsCache`
- Keep the existing constructor names, but change them to use service-level configs only.
- Exclude transport-only fields from public service configs.
- Preserve Rust-native ownership, borrowing, `Result`, and builder patterns.

#### Go

- Define service-level config types in the public Go service layer.
- Re-export them from typed service packages for ergonomic use:
  - `cgroups.ClientConfig`
  - `cgroups.ServerConfig`
- Public typed package target:
  - callers import only `service/cgroups` and protocol types if needed
  - callers do not import `transport/posix` or `transport/windows` for L2/L3
- Required constructor set on both OSes:
  - `NewClient`
  - `NewServer`
  - `NewServerWithWorkers`
  - `NewCache`
- Exclude transport-only fields from public service configs.
- Preserve Go-native package structure, value semantics, and error handling.

#### Stage 2: Unify public config types

- C:
  - Introduce public L2/L3 service-level config typedefs in `netipc_service.h`.
  - Stop exposing `nipc_uds_*_config_t` and `nipc_np_*_config_t` in L2/L3 signatures.
  - Internally translate the unified config to transport-specific config structs.
- Rust:
  - Introduce service-layer config types under `service` instead of re-exporting transport configs.
  - Convert those service configs internally to `transport::posix` / `transport::windows` configs.
- Go:
  - Introduce service-layer config types in `service/raw` and `service/cgroups`, or a shared service config package.
  - Stop requiring `transport/posix` or `transport/windows` imports in L2/L3 caller code.

#### Stage 3: Normalize public constructor/init signatures

- C:
  - Make `nipc_client_init()`, `nipc_server_init_typed()`, and `nipc_cgroups_cache_init()` take the same config types on both OSes.
- Rust:
  - Keep the same constructors, but make them take service-layer config types with identical semantics on all OSes.
- Go:
  - Make Windows and POSIX export the exact same L2/L3 constructor set.
  - Add the missing Windows `NewServerWithWorkers()` to match POSIX.

#### Stage 4: Separate public plugin API from internal privileged API

- Keep public L2/L3 plugin-facing API simple and OS-transparent.
- Introduce or expand explicit internal-only paths for:
  - transport inspection
  - benchmark special paths
  - raw malformed-message tests
  - direct session/SHM manipulation
- Prefer:
  - internal headers/APIs guarded by `NIPC_INTERNAL_TESTING` in C
  - internal packages/modules for Rust and Go tests/benchmarks
- Goal:
  - benchmarks and repo tests can still do privileged things
  - plugins cannot accidentally depend on transport internals

#### Stage 5: Revisit public C struct exposure

- Do not switch to heap-only opaque handles.
- Keep caller-allocated typed structs.
- Short-term recommended path:
  - keep shared public type names (`nipc_client_ctx_t`, `nipc_managed_server_t`, `nipc_cgroups_cache_t`)
  - allow internal field layout to differ by OS
  - stop documenting or encouraging direct field access in plugin-facing docs
- Optional stronger follow-up:
  - make the visible public field set identical across OS using reserved/internal storage fields
  - only if there is a strong need for stricter ABI/API symmetry

#### Stage 6: Documentation and example cleanup

- Rewrite public examples so they use unified L2/L3 config types only.
- Remove transport-specific imports/includes from L2/L3 examples.
- Add one explicit note:
  - transport-specific knobs belong to L1, not L2/L3 public plugin APIs

#### Stage 7: Verification

- Build and test one identical plugin-facing example per language on Linux and Windows:
  - same source file
  - no caller-side platform branch
- Re-run affected unit/integration suites for C, Rust, and Go.
- Re-run benchmark drivers if any API moved for internal-only use.

### Implementation progress as of 2026-03-27

- C public L2/L3 first pass is implemented:
  - `netipc_service.h` now exposes unified public `nipc_client_config_t` and `nipc_server_config_t`.
  - Public L2/L3 signatures now take those unified config types on both POSIX and Windows.
  - Service implementations translate the public configs into `nipc_uds_*` / `nipc_np_*` transport configs internally.
  - Public C examples in `docs/getting-started.md` were updated to the service-level config names.
  - `tests/fixtures/c/test_win_service.c` was updated to the public service-level config names.
- The temporary C `NIPC_INTERNAL_TESTING` typed compatibility bridge is gone:
  - repo L2/L3 tests, interop fixtures, and benchmark drivers now use the native public service-level typed config API directly
  - only explicit raw/internal-only hooks remain for raw malformed-message and transport-specific test paths
- Rust public L2/L3 first pass is implemented:
  - `src/crates/netipc/src/service/cgroups.rs` now defines public service-level `ClientConfig` and `ServerConfig`.
  - `CgroupsClient`, `ManagedServer`, and `CgroupsCache` now consume those service-level configs instead of platform-selected transport configs.
  - Rust typed cgroups tests, interop binaries, and Rust benchmark typed snapshot clients were updated to use the service-level configs.
- Go public L2/L3 first pass is implemented:
  - `src/go/pkg/netipc/service/cgroups` now defines public service-level `ClientConfig` and `ServerConfig`.
  - `NewClient`, `NewServer`, and `NewCache` now consume those service-level configs on both Unix and Windows.
  - Windows now also exports `NewServerWithWorkers()` in `service/cgroups`, matching Unix.
  - Go typed cgroups tests, Go interop binaries, Go benchmark typed snapshot callers, and the public Go getting-started example were updated to use `cgroups.ClientConfig` / `cgroups.ServerConfig`.
- Go raw Windows support was extended so the public Windows typed API is real, not a stub:
  - `src/go/pkg/netipc/service/raw/client_windows.go` now implements `NewServerWithWorkers()` with bounded concurrent session handling, mirroring the existing Unix raw server shape closely enough for the typed Windows API to rely on it.

### Verification completed so far

- C:
  - `cmake --build build --target test_service test_cache interop_service_c interop_cache_c bench_posix_c -j4` passed.
  - A public non-internal smoke compile using `nipc_client_config_t` and `nipc_client_init()` passed.
  - `ctest` is still not usable in this environment because the local wrapper fails with `ModuleNotFoundError: No module named 'cmake'`.
- Rust:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml cgroups_ --lib` passed.
  - `cargo check --manifest-path src/crates/netipc/Cargo.toml --all-targets` passed.
- Go:
  - `go test ./pkg/netipc/service/cgroups` passed in `src/go`.
  - `GOOS=windows GOARCH=amd64 go test -c -o /tmp/netipc-go-cgroups-windows.test ./pkg/netipc/service/cgroups` passed.
  - `go build ./cmd/interop_service ./cmd/interop_cache` passed in `tests/fixtures/go`.
  - `GOOS=windows GOARCH=amd64 go build ./cmd/interop_service_win ./cmd/interop_cache_win` passed in `tests/fixtures/go`.
  - `go build .` and `GOOS=windows GOARCH=amd64 go build .` both passed in `bench/drivers/go`.

### Remaining work after this first implementation pass

- No remaining implementation work is required for this task after the explicit commit/push.
- Follow-up work, if desired, is performance investigation only:
  - explain the stable Windows `lookup` language/runtime delta
  - explain the stable Go-server tail latency in low-rate Windows SHM batch rows

### Documentation status after the first pass

- `docs/getting-started.md` was updated where it taught transport-specific public L2/L3 config usage.
- A focused search across `README.md`, `docs/level2-typed-api.md`, and `docs/level3-snapshot-api.md` did not find any remaining transport-specific L2/L3 config examples or constructor signatures to fix in this pass.

### Exact implementation map for the first pass

#### C

- Public header:
  - `src/libnetdata/netipc/include/netipc/netipc_service.h`
  - Changes:
    - add unified public `nipc_client_config_t`
    - add unified public `nipc_server_config_t`
    - change `nipc_client_init()` signature to use `nipc_client_config_t`
    - change `nipc_server_init_typed()` signature to use `nipc_server_config_t`
    - change `nipc_cgroups_cache_init()` signature to use `nipc_client_config_t`
    - remove transport-specific config types from L2/L3 public signatures
- POSIX service implementation:
  - `src/libnetdata/netipc/src/service/netipc_service.c`
  - Changes:
    - add translation from `nipc_client_config_t` to `nipc_uds_client_config_t`
    - add translation from `nipc_server_config_t` to `nipc_uds_server_config_t`
    - keep transport-specific `packet_size` / `backlog` internal
- Windows service implementation:
  - `src/libnetdata/netipc/src/service/netipc_service_win.c`
  - Changes:
    - add translation from `nipc_client_config_t` to `nipc_np_client_config_t`
    - add translation from `nipc_server_config_t` to `nipc_np_server_config_t`
    - keep transport-specific `packet_size` internal
- Public docs/examples:
  - `docs/getting-started.md`
  - `README.md`
  - `docs/level2-typed-api.md`
  - `docs/level3-snapshot-api.md`
- Internal-only follow-up needed:
  - tests/bench code that reads public internals must migrate to explicit internal-only helpers where needed

#### Rust

- Public typed service API:
  - `src/crates/netipc/src/service/cgroups.rs`
  - Changes:
    - stop importing `transport::posix::{ClientConfig, ServerConfig}` and `transport::windows::{ClientConfig, ServerConfig}` directly into the public API
    - expose service-level config types instead
- New or expanded service config definition point:
  - likely `src/crates/netipc/src/service/` module
  - Changes:
    - define Rust-native service `ClientConfig`
    - define Rust-native service `ServerConfig`
    - preserve `Default`, ownership, borrowing, and `Result` patterns
- Raw/service adapter layer:
  - `src/crates/netipc/src/service/raw.rs`
  - Changes:
    - convert service configs to transport configs internally
- Tests/docs:
  - update typed service tests and examples to import service-level configs only

#### Go

- Public typed service package:
  - `src/go/pkg/netipc/service/cgroups/client.go`
  - `src/go/pkg/netipc/service/cgroups/client_windows.go`
  - `src/go/pkg/netipc/service/cgroups/cache.go`
  - `src/go/pkg/netipc/service/cgroups/cache_windows.go`
  - Changes:
    - stop requiring `transport/posix` or `transport/windows` config imports in public L2/L3 callers
    - expose service-level `ClientConfig` and `ServerConfig`
    - add `NewServerWithWorkers()` on Windows to match POSIX
- Public service config definition point:
  - likely `src/go/pkg/netipc/service/cgroups` or `src/go/pkg/netipc/service/raw`
  - Changes:
    - define Go-native service config types
    - preserve package ergonomics, value semantics, and native error style
- Raw adapter layer:
  - `src/go/pkg/netipc/service/raw/client.go`
  - `src/go/pkg/netipc/service/raw/client_windows.go`
  - `src/go/pkg/netipc/service/raw/cache.go`
  - `src/go/pkg/netipc/service/raw/cache_windows.go`
  - Changes:
    - convert service-level configs to transport configs internally
- Docs/tests:
  - remove transport package imports from L2/L3 examples and caller-facing tests

### Pending design decisions

#### 1. C public struct strategy

Context:
- Costa requires caller-allocated strongly typed structs, including stack allocation.
- Public `void *` handles are rejected.

Options:
- A. Keep shared public struct names, but allow OS-specific internal field layout behind the same typedef names.
  - Pros:
    - simplest migration
    - preserves stack allocation
    - matches proven cross-platform C practice (for example, libuv-style design)
    - minimizes ABI churn
  - Cons:
    - public headers still contain some internal details
    - exact visible field set may still differ by OS
  - Risks:
    - tests/benchmarks may continue to abuse public internals unless an internal boundary is enforced
- B. Keep caller-allocated typed structs, but make the visible public field set identical across OS via reserved/private inline storage.
  - Pros:
    - stricter API symmetry
    - cleaner long-term caller contract
  - Cons:
    - more invasive
    - size/alignment/versioning become trickier
    - harder future evolution
  - Risks:
    - incorrect sizing/alignment bugs
    - larger ABI churn

Decision made:
- A.

Reason:
- it achieves the actual product goal (same plugin source code on both OSes)
- it preserves stack allocation
- it avoids over-design before the public config/signature cleanup is complete

#### 2. How to treat OS-specific knobs such as `backlog`

Context:
- POSIX exposes server `backlog`; Windows does not have the same public concept at L2/L3.

Options:
- A. Remove OS-specific knobs from L2/L3 public configs and keep them in L1 or internal transport config only.
  - Pros:
    - clean L2/L3 contract
    - same field names have same meaning on all OSes
    - fewer fake/no-op public settings
  - Cons:
    - some tuning moves below L2/L3
  - Risks:
    - tests/bench code may need a separate internal path for transport tuning
- B. Keep a superset unified L2/L3 config and ignore/no-op unsupported fields on some OSes.
  - Pros:
    - fewer immediate removals
    - can be easier short-term
  - Cons:
    - weaker API truthfulness
    - same field name may not mean the same thing on both OSes
  - Risks:
    - hidden caller confusion
    - future semantic drift

Decision made:
- A.

Reason:
- a transparent API must be semantically honest, not just syntactically identical

## Implied decisions

- Any OS-specific transport tuning that cannot be given identical public semantics should move below the L2/L3 public boundary.
- Public docs and examples are part of the contract and must be updated together with the code.
- Repo-internal tests and benchmark drivers are allowed to keep privileged access, but that access should move to explicit internal-only APIs instead of remaining in the public plugin-facing surface.
- Cross-language visual/API symmetry is not a goal by itself.
- Performance and language-natural ergonomics take priority over superficial sameness between C, Rust, and Go.

## Testing requirements

- C:
  - Compile identical plugin-facing L2 client/server/cache examples on Linux and Windows from the same source file.
  - Add tests that use unified public config types only.
  - Keep transport-aware tests behind internal-only APIs/macros.
- Rust:
  - Compile the same public `service::cgroups` client/server/cache code on Linux and Windows without `cfg` at the call site.
  - Add tests for unified service-level config conversions to transport configs.
- Go:
  - Compile the same `service/cgroups` caller code on Linux and Windows without importing `transport/posix` or `transport/windows`.
  - Add tests for the now-matching constructor set, including `NewServerWithWorkers()` on Windows.
- Cross-language:
  - Re-run existing interop suites after the public API boundary changes.
  - Ensure benchmark drivers still compile, using internal-only hooks if needed.
- Documentation:
  - Add one example per language that is intentionally identical across OS at the source level.

### Verified so far

- POSIX C verification:
  - `cmake --build build --target test_service test_cache test_multi_server test_ping_pong test_hardening test_stress interop_service_c interop_cache_c bench_posix_c -j4`
- Windows C verification:
  - `cmake -E env CC='zig cc -target x86_64-windows-gnu' cmake -S . -B /tmp/plugin-ipc-build-win2 -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_RC_COMPILER=llvm-rc`
  - `cmake --build /tmp/plugin-ipc-build-win2 --target test_win_service test_win_service_extra test_win_service_guards test_win_service_guards_extra test_win_stress interop_service_win_c interop_cache_win_c bench_windows_c -j4`
- Rust typed-layer verification:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml cgroups_ --lib`
- Go typed-layer verification:
  - `go test ./pkg/netipc/service/cgroups`
  - `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/cgroups`
- Full POSIX suite verification:
  - `/usr/bin/ctest --test-dir build --output-on-failure -j4`
    - first run: `36/37` passed, with one transient `test_service_shm_interop` failure on the `Rust server, Rust client` SHM pair (`server exited before socket bind`)
    - isolated reruns:
      - `bash tests/test_service_shm_interop.sh` passed
      - `/usr/bin/ctest --test-dir build --output-on-failure -R test_service_shm_interop --repeat until-pass:5` passed
    - second full rerun: `37/37` passed
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `298` Rust tests passed
  - `go test ./...`
    - full Go package suite passed
  - `bash tests/run-posix-bench.sh /tmp/plugin-ipc-final-verify-20260327/benchmarks-posix.csv 5`
    - complete matrix: `201` measurements
  - `bash tests/generate-benchmarks-posix.sh /tmp/plugin-ipc-final-verify-20260327/benchmarks-posix.csv /tmp/plugin-ipc-final-verify-20260327/benchmarks-posix.md`
    - all POSIX performance floors met
- `git diff --check` passed for all files touched in this no-bridge cleanup.
- Full native `win11` verification from an isolated tree at `~/src/plugin-ipc-verify-20260327-l2l3`:
  - native Windows build:
    - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
    - `cmake --build build -j4`
    - passed
  - native Windows `ctest`:
    - `ctest --test-dir build --output-on-failure -j4`
    - `28/28` passed
  - native Windows Rust suite:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `177` passed, `0` failed, `1` ignored
    - ignored test:
      - `service::raw::windows_tests::test_retry_on_failure_windows`
  - native Windows Go suite:
    - `cd src/go && go test ./...`
    - passed
  - native Windows strict benchmark suite:
    - `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_windows`
    - `bash tests/run-windows-bench.sh benchmarks-windows.csv 5`
    - exited `0`
    - `201` data rows, `202` CSV lines total
  - native Windows benchmark report generation:
    - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md`
    - `All performance floors met`
  - notable benchmark trust findings from the native `win11` one-shot run:
    - the old `shm-ping-pong rust->c @ max` collapse did not reproduce
    - `shm-ping-pong rust->c @ max = 2450286`, `stable_ratio = 1.014143`
    - one trimmed-warning row remained publishable:
      - `shm-ping-pong rust->rust @ 10000/s`
      - `raw_min = 6880`, `raw_max = 9999`, `stable_ratio = 1.000000`
    - the strongest real Windows language/runtime deltas were:
      - local cache lookup:
        - `rust = 84747056`
        - `c = 52857656`
        - `go = 38995304`
      - `shm-batch-ping-pong @ max`:
        - high band:
          - `c->c = 55301833`
          - `c->rust = 53390579`
          - `rust->c = 52301787`
        - low Go-server band:
          - `c->go = 44114002`
          - `rust->go = 40136280`
          - `go->go = 39735030`
      - `np-pipeline-batch-d16 @ max`:
        - strongest:
          - `c->c = 37228878`
          - `go->rust = 35700043`
        - weakest:
          - `go->c = 26662511`
          - `c->rust = 27633290`
          - `c->go = 29120853`

## Documentation updates required

- `README.md`
  - clarify that L2/L3 plugin-facing APIs are OS-transparent by contract
- `docs/level2-typed-api.md`
  - make the OS-transparent public API rule explicit
- `docs/level3-snapshot-api.md`
  - clarify that Level 3 callers must not import transport config types
- `docs/getting-started.md`
  - replace transport-specific L2/L3 examples with unified service-level config examples
- Language-specific public package/module docs
  - ensure examples and signatures reflect the unified API
