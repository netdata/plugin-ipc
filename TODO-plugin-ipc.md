# TODO-plugin-ipc

## Session Handoff (2026-03-08)
- Working repository: `/home/costa/src/plugin-ipc.git` (local git repo initialized, branch `main`).
- Port status: source/tests/docs/tooling copied from `/home/costa/src/ipc-test` and validated in-place.
- Validation already run here: `make`, `./tests/run-interop.sh`, `./tests/run-live-uds-interop.sh` (all pass).
- Repository refactor status:
  - approved Netdata-style layout implemented:
    - `src/libnetdata/netipc/`
    - `src/go/pkg/netipc/`
    - `src/crates/netipc/`
  - helper programs moved under:
    - `tests/fixtures/`
    - `bench/drivers/`
  - root `CMakeLists.txt` now orchestrates mixed-language builds.
- Latest full validation after the refactor:
  - `./tests/run-interop.sh`
  - `./tests/run-live-interop.sh`
  - `./tests/run-live-uds-interop.sh`
  - `./tests/run-live-uds-bench.sh`
  - `./tests/run-negotiated-profile-bench.sh`
  - `./tests/run-uds-negotiation-negative.sh`
  - `./tests/run-uds-seqpacket.sh`
  - Result: all pass in the refactored tree.
- Cleanup status after user approval:
  - obsolete legacy `interop/` subtree removed
  - obsolete root `include/` directory removed
- Latest strategic decisions already recorded below:
  - Repo identity: `netdata/plugin-ipc`.
  - Coverage policy: 100% line + 100% branch for library source files.
  - Benchmark CI model: GitHub-hosted cloud VMs (with repetition/noise controls).
  - Windows baseline: Named Pipes, one native Windows library implementation (no separate MSYS2 variant).
  - Windows builds must also work under MSYS2 POSIX emulation during the current Netdata transition.
  - Windows C build mode: compile native Win32 code from MSYS2 `mingw64`/`ucrt64`, not the plain `msys` runtime shell.
- Next starting point for the next session:
  - Continue replacing placeholder Rust/Go library scaffolding with real reusable API implementations.
  - Latest Windows probe findings not yet committed:
    - `win11` repo path used for testing is `/c/Users/costa/src/plugin-ipc-win.git`
    - MSYS2/POSIX C configure+build passes there
    - native `MINGW64` C build currently fails on POSIX-only headers (`arpa/inet.h`, `poll.h`, `sys/mman.h`)
    - Rust on `win11` is currently `x86_64-pc-windows-msvc`, and the crate still tries to compile POSIX transport code on that Windows target
    - Go on `win11` still shows a local toolchain inconsistency during `go test` (`compile: version \"go1.26.1\" does not match go tool version \"go1.26.0\"`)

## TL;DR
- Build cross-language IPC libraries for Netdata plugins in C, Rust, and Go.
- Each plugin can be both IPC server and IPC client.
- Cross-language compatibility is required (C <-> Rust <-> Go).
- Target POSIX (Linux/macOS/FreeBSD) and Windows.
- Use the lightest/highest-performance transport possible.
- Start with IPC transport benchmarks before final protocol/API decisions.

## Requirements (Verbatim)
```text
I have the following task: netdata plugins are independent processes written in various languages (usually: C, Go, Rust).
plugins are usually authoritative for some kind of information, for example cgroups.plugin knows the names of all containers and cgroups, apps.plugin knows everything about processes, ebpf collects everything from the kernel, network-viewer.plugin knows everything about sockets, netflow.plugin knows everything about ip-to-asn and ip-to-country, etc.

I want to develop a library in C, Rust and Go, enabling all plugins to become IPC servers and clients and therefore talk to each other, asking the right plugin to provide information about its domain. This should be cross-language compatible, so that a C plugin can ask a Rust plugin, or a Rust plugin to ask a Go plugin.

I am thinking of a setup that plugins expose a unix domain socket (on posix systems) at the netdata RUN_DIR. So the rust netflow.plugin exposes `ip-asn-geo.sock`. Any other plugin knows that it needs `ip-asn-geo` and just opens the socket and starts communicating without knowing anything about which plugin is the destination, in which language it is written, etc. So, the RUN_DIR of netdata will end up have socket files from various plugins, exposing data enrichment features in a dynamic way.

Ideally, I want 3x libraries: C, Rust, Go, with 2 implementations each: Posix (linux, macos, freebsd), Windows.

Authorization will be done by a shared SALT. So, Netdata already exposes a session id, a UUID which we can use to authorize requests. This means that the plugins will be able to use these services only when spawned by the same netdata.

Another very important requirement is performance. The transport layer of this communication should be the lightest possible. If there is a way for this communication to have smaller cpu impact and latency we must use that communication.

Regarding the internal client/server API, ideally I want these to be strongly typed, so these libraries should accept and return structured data. This may mean that in order to support a new type of message we may have to recompile the libraries. This is acceptable.

If we manage to have an transport layer that supports millions of requests/responses per second, we don't need a batch API. If however the transport is slow, we must then provide a way for the clients to batch multiple requests per message. For this reason I suggest first to implement a test comparing the various forms of IPC, so that we can choose the most performant one.
```

## Analysis (Current Status - Fact Based)
- Fact: Repository path `/home/costa/src/ipc-test` is currently empty (no source files, no tests, no existing TODO file).
- Fact: There is no pre-existing IPC implementation in this repo to extend.
- Fact: There is no existing build layout yet (no CMake/Cargo/go.mod).
- Fact: Host for first benchmark pass is Linux x86_64 (Manjaro, kernel 6.18.12).
- Fact: Toolchain availability confirmed: gcc 15.2.1, rustc 1.91.1, go 1.25.7.
- Fact: External performance tools available: `pidstat`, `mpstat`, `perf`.
- Implication: We should treat this as a greenfield design + benchmark project.
- Risk: Without early benchmark data, choosing protocol/serialization prematurely may lock in suboptimal latency/CPU costs.

## Decisions

### Pending Decisions Requiring User Input
- None at the moment.

### Made Decisions
1. Transport strategy for v1 benchmark candidate set: Option C (benchmark both stream and message-oriented candidates).
   - Source: user decision "1c".

2. Serialization format for strongly typed cross-language messages: Option C (custom binary format, C struct baseline).
   - Source: user decision "2c".
   - User rationale: C struct is wire baseline; Rust and Go map to/from that binary format and native structures.

3. Service discovery model in `RUN_DIR`: Option A (convention-based socket naming only).
   - Source: user decision "3a".
   - User note: plugins are assumed same version; version mismatches can hard-fail on connect/handshake.

4. Authorization handshake: Option A (shared SALT/session UUID in initial auth message).
   - Source: user decision "4a".
   - User rationale: low-risk internal communication; socket OS permissions provide access control.

5. Batching policy: Option C (add only if benchmarks fail the performance target).
   - Source: user decision "5c".
   - Acceptance target: transport should reach 1M+ req/s on target high-end workstation with minimal CPU overhead.

6. Wire binary layout strategy: Option B (explicit field-by-field encode/decode with fixed endianness, preserving C-struct schema baseline).
   - Source: user decision "1B".

7. POSIX transport benchmark breadth: Option C and broader (benchmark multiple POSIX methodologies, including `AF_UNIX` variants and shared memory + spinlock candidates).
   - Source: user decision "2C and even more... what about shared memory + spinlock?".
   - User intent: identify best possible POSIX methodology by measurement, not assumption.

8. Connection lifecycle model for benchmarks: persistent sessions.
   - Source: user clarification "not connect->request->response->disconnect per request; connect once ... disconnect on agent shutdown".
   - Implication: benchmark steady-state request/response over long-lived connections/channels.

9. CPU threshold policy for first pass: no hard threshold upfront.
   - Source: user clarification "I don't know the threshold... want most efficient latency, throughput, cpu utilization".
   - Implication: first report is comparative and multi-metric; batching decision remains data-driven.

10. Shared-memory synchronization benchmark strategy: Option C (benchmark both spinlock-only and blocking/hybrid synchronization).
   - Source: user agreement to proposed options.

11. Benchmark mode scope: Option C (strict ping-pong baseline plus secondary pipelined mode).
   - Source: user agreement to proposed options.

12. CPU measurement method: Option C (collect both external sampling and in-process accounting).
   - Source: user agreement to proposed options.

13. Add and benchmark a shared-memory hybrid synchronization transport.
   - Source: user decision "implement in the test the hybrid you recommend and measure it".
   - Intent: measure if hybrid preserves low latency/high throughput while reducing unnecessary spin behavior.

14. Prioritize single-threaded client ping-pong as the primary optimization target.
   - Source: user clarification about apps.plugin-like enrichment loops.
   - Intent: optimize for one client thread doing many sequential enrichments, then verify scaling with more clients.

15. Add a rate-limited benchmark mode for hypothesis testing at fixed request rate.
   - Source: user request to validate CPU behavior at 100k req/s.
   - Intent: compare `shm-spin` vs `shm-hybrid` CPU utilization under the same fixed throughput target.

16. Tune hybrid spin window from 256 to 64 and re-measure at 100k req/s.
   - Source: user request "spin 64, not 256".
   - Intent: test whether shorter spin window increases blocking and reduces CPU at fixed high request rate.

17. Run a sweep over multiple hybrid spin values to map impact on:
   - CPU utilization at fixed 100k req/s.
   - Maximum throughput at unlimited rate.
   - Source: user request to understand spin-tries effect on these two parameters.

18. Optimize using \"request-rate increase per spin\" as the primary tuning unit for hybrid spin window.
   - Source: user guidance that sweet spot is likely around 8 or 16.
   - Intent: compute marginal throughput gain per added spin and identify diminishing-return point.

19. Set the default shared-memory hybrid spin window to 20 tries.
   - Source: user decision "yes, put 20".
   - Intent: make the benchmark default align with the selected spin/throughput CPU tradeoff.

20. Prefer `shm-hybrid` with spin window 20 as the default method for plugin IPC.
   - Source: user decision "I think 1 is our preferred method."
   - Intent: optimize for lower CPU at target enrichment rates while keeping good throughput and acceptable latency.

21. Execute the next implementation phase now:
   - Freeze POSIX v1 baseline to `shm-hybrid(20)`.
   - Implement first typed request/response schema and C API.
   - Add C/Rust/Go interoperability tests for the typed schema.
   - Source: user decision "yes, do it".

22. Proceed with stale-endpoint recovery and live cross-language transport interoperability validation.
   - Source: user decision "proceed".
   - Phase implementation choice: use Rust/Go FFI shims to the C transport API for live `shm-hybrid` session tests in this iteration.

23. Proceed to native Rust/Go live transport implementations (no dependency on `libnetipc.a`) while preserving the same live interop matrix.
   - Source: user decision "ok, proceed".
   - Intent: validate cross-language `shm-hybrid` behavior with independent Rust/Go transport code paths.

24. Go transport must be pure Go without cgo.
   - Source: user decision "The Go implementation must not need CGO."
   - Constraint: compatibility with `go.d.plugin` pure-Go requirement.
   - Implementation direction for this phase: use pure-Go shared-memory sequencing and make C/Rust waits semaphore-optional for interoperability with pure-Go peers.

25. Current pure-Go polling implementation performance is a blocker and unusable for target plugin IPC.
   - Source: user feedback "1k/s is a blocker. This is unusable."
   - Implication: v1 pure-Go transport strategy must change; current polling approach cannot be accepted.

26. POSIX v1 baseline profile is `UDS_SEQPACKET` for all languages, with optional higher-speed profiles negotiated when both peers support them.
   - Source: user decision "ok, 1A".
   - Context: measured baseline methods already achieve ~260k-330k req/s class, while current pure-Go polling is blocked at ~1.2k req/s.
   - Implication: implement capability negotiation and method/profile selection before request-response starts.

27. Handshake frame format is fixed binary struct (v1).
   - Source: user agreement to recommendation P2:A.
   - Rationale: lower complexity/overhead and aligns with typed fixed-binary IPC model.

28. Profile selection is server-selected from intersection (v1).
   - Source: user agreement to recommendation P3:A.
   - Rationale: deterministic one-round negotiation and simpler state machine.

29. Next implementation scope is negotiation + `UDS_SEQPACKET` baseline only.
   - Source: user agreement to recommendation P4:A.
   - Rationale: fastest unblock path with lower integration risk; optional fast profiles follow in next phase.

30. Implement now the v1 scope from Decisions #26-#29.
   - Source: user agreement "I agree".
   - Scope: C `UDS_SEQPACKET` transport with fixed-binary handshake and server-selected profile.

31. Proceed to native Rust/Go live `UDS_SEQPACKET` implementations with the same fixed-binary negotiation and validate full C<->Rust<->Go live matrix.
   - Source: user direction "proceed".
   - Constraint: Go implementation remains pure Go (no cgo).

32. Proceed with next hardening phase for UDS baseline:
   - Add live UDS benchmark modes for Rust/Go runners with throughput/p50/CPU reporting.
   - Add negative negotiation tests (profile mismatch/auth mismatch/malformed handshake expectations).
   - Source: user direction "proceed".

33. Proceed with optional fast-profile implementation phase:
   - Add negotiated `SHM_HYBRID` profile support for native C and Rust UDS live runners.
   - Keep pure-Go on `UDS_SEQPACKET` baseline (no cgo), with negotiation fallback to profile `1` when peers differ.
   - Add live interop + benchmark coverage to prove negotiated profile selection behavior and performance impact.
   - Source: user direction "proceed".

34. Fix rate-limited benchmark clients to avoid busy-loop pacing and switch to adaptive sleep-based pacing.
   - Apply to C benchmark harness, Rust live UDS bench client, and Go live bench clients.
   - Rerun comparison benchmarks after the pacing fix because previous CPU numbers are polluted by pacing overhead.
   - Source: user direction "Please fix the client that busy loops ... run the comparison again."

35. Remove pure-Go SHM polling path completely.
   - Choice: A (full removal now).
   - Scope: remove pure-go-poll command path, remove its dedicated benchmark/testing references, keep UDS baseline interop/bench and C<->Rust negotiated SHM profile coverage.
   - Source: user decision "A".

36. Start a dedicated new public Netdata repository for this IPC project.
   - Source: user direction "create a new public repo in netdata".

37. New repository must contain 3 libraries (C, Rust, Go), each supporting POSIX and Windows.
   - Source: user direction.

38. New repository must enforce complete automated test coverage for library code (target policy: 100%).
   - Source: user direction "mandatory ... 100% tested ... 100% coverage on the libraries".

39. New repository must include CI benchmark jobs across language role combinations on Linux and Windows.
   - Source: user direction "CI benchmarks for all combinations ... 6 linux + 6 windows".

40. Netdata Agent integration is deferred until this standalone repo is robust, fully tested, and benchmark-validated.
   - Source: user direction "once this is ready ... we will work to integrate it into netdata".

41. New public repository identity and ownership: Option B (`netdata/plugin-ipc`).
   - Source: user decision "1b".

42. Coverage policy for "100% tested": Option A (100% line + 100% branch for library source files only; examples/bench binaries excluded).
   - Source: user decision "2a".

43. Benchmark CI execution model: Option B (all benchmarks run on GitHub-hosted cloud VMs).
   - Source: user decision "3b".
   - User intent: evaluate benchmark behavior on actual cloud VMs.

44. Windows baseline transport: Option A (Named Pipes) for the native Windows path.
   - Source: user decision "4a".
   - Historical note: this originally assumed one native Windows implementation only; Decision #62 supersedes that assumption for the current transition period.

45. Create new repository at `~/src/plugin-ipc.git` and port this IPC project there.
   - Source: user direction "creare the repo in ~/src/plugin-ipc.git and port everything there."
   - Execution intent: move project source, tests, docs, and tooling baseline into the new repository as the working root for next phases.

46. Public project repository path/name is confirmed as `netdata/plugin-ipc`.
   - Source: user clarification "The project will be netdata/plugin-ipc".
   - Note: this matches the earlier repository identity decision and should be treated as fixed.

47. Runtime auth source for plugin IPC should use the `NETDATA_INVOCATION_ID` environment variable carrying the per-agent UUID/session identifier.
   - Source: user clarification "The auth is just an env variable with a UUID, I think NETDATA_INVOCATION_ID".
   - Status: needs verification against `~/src/netdata-ktsaou.git/` before implementation is frozen.

48. "Good-enough" acceptance for this standalone project requires reviewing benchmark results before freezing for Netdata integration.
   - Source: user clarification "We will need to see benchmark results for good-enough acceptance".
   - Implication: benchmark evidence is part of the final go/no-go gate, not just a CI formality.

49. Library scope must stay integration-agnostic: the IPC library API accepts the auth UUID/token value from the caller, and does not read environment variables itself.
   - Source: user clarification "The API of library should just accept it. Focus on the library itself, not the integration".
   - Implication: `NETDATA_INVOCATION_ID` lookup/wiring is deferred to Netdata-side integration code, while `plugin-ipc` defines only the parameterized auth contract.

50. Repository layout needs a redesign before the project grows into 3 libraries x 2 platforms.
   - Source: user feedback "given what we want (a library with 6 implementations), propose the proper file structure. I find the current one a total mess."
   - Status: resolved; repository layout decisions recorded below.

51. Primary repository-layout criterion is simplicity of eventual integration into the Netdata monorepo.
   - Source: user clarification "my primary criteria for the organization is simplicity during integration with netdata."
   - Implication: standalone repository aesthetics are secondary to minimizing future integration churn.

52. Language deliverables must map directly to Netdata integration targets:
   - C implementation must map cleanly into `libnetdata`
   - Go implementation must remain a normal Go package
   - Rust implementation must remain a normal Cargo package
   - Source: user clarification "Somehow, the C library must be in libnetdata, the go version must be a go pkg and the rust version must be a cargo package."

53. Build-system preference is `CMake` as the top-level orchestrator, while preserving native package manifests for Rust and Go.
   - Source: user clarification "ideally all makefiles should be cmake, so that we can keep the option of moving the source into netdata monorepo too."
   - Implication: `Cargo.toml` and `go.mod` remain canonical for their languages; `CMake` orchestrates mixed-language build, tests, fixtures, and benchmarks.

54. Approved standalone repository topology should mirror Netdata's destination structure now:
   - `src/libnetdata/netipc/` for the C implementation
   - `src/go/pkg/netipc/` for the Go package
   - `src/crates/netipc/` for the Rust crate
   - shared repo-level areas for docs/spec, tests, and benchmarks
   - Source: user agreement to the integration-first recommendation.
   - Rationale: avoids a second structural rewrite when moving code into the Netdata monorepo.

55. Reusable library code must stay separated from demos, interop fixtures, and benchmark drivers.
   - Source: user agreement to the proposed organization.
   - Implication:
     - product code lives under `src/libnetdata/netipc/`, `src/go/pkg/netipc/`, and `src/crates/netipc/`
     - helper apps and conformance tools move under repo-level `tests/fixtures/` and `bench/drivers/`
   - Rationale: keeps the library surfaces clean and package-shaped for Netdata integration.

56. Root `CMakeLists.txt` should be the single repo entry point for cross-language orchestration.
   - Source: user agreement to the proposed build-system recommendation.
   - Implication:
     - C code builds directly under `CMake`
     - Rust is imported as a Cargo package/crate
     - Go is built as a Go module/package tree
   - Rationale: matches Netdata's current mixed-language build model.

57. Obsolete legacy prototype paths from the pre-refactor tree should be deleted now.
   - Source: user decision "cleanup yes".
   - Scope:
     - remove legacy generated files under `interop/`
     - remove the old unused root `include/` directory
     - remove empty obsolete `interop/` directories if they become empty after cleanup
   - Rationale: the approved layout is already validated, and keeping stale prototype leftovers would continue to make the tree look partially migrated.
   - Status: completed; the obsolete `interop/` and root `include/` paths have been removed.

58. Stale top-level binary artifacts from the prototype build flow should be removed, and cleanup commands should keep the repository root artifact-free.
   - Source: user report "I see binary artifacts at the root".
   - Evidence:
     - top-level files currently present:
       - `ipc-bench`
       - `libnetipc.a`
       - `netipc-codec-c`
       - `netipc-shm-client-demo`
       - `netipc-shm-server-demo`
       - `netipc-uds-client-demo`
       - `netipc-uds-server-demo`
     - current documented build outputs already point to `build/bin/` and `build/lib/`, not the repository root.
   - Rationale: these are stale leftovers from the earlier prototype layout and should not remain visible after the CMake refactor.
   - Status: completed.
   - Follow-up fix:
     - the root `Makefile` wrapper was adjusted to forward only explicit user-requested targets to CMake.
     - this avoids an accidental attempt by `make` to rebuild `Makefile` itself through the old catch-all `%` rule.

59. `SHM_HYBRID` reusable-library support is required for both C and Rust.
   - Source: user clarification "shm-hybrid needs to be implemented for C and rust, not just rust."
   - Fact:
     - C already has reusable `SHM_HYBRID` support in `src/libnetdata/netipc/`.
     - Rust now has reusable `SHM_HYBRID` support in `src/crates/netipc/`.
   - Implication:
     - the reusable-library gap for direct `SHM_HYBRID` is closed for C and Rust.
     - the remaining Rust gap is negotiated UDS/bench helper deduplication, not direct SHM library support.
     - Go remains `UDS_SEQPACKET`-only in this phase.

60. Windows implementation should be developed against a real Windows machine, and the repo should be pushed to `netdata/plugin-ipc` before that work starts.
   - Source: user proposal to push the repo and provide Windows access over SSH.
   - User decision:
     - approved push to `netdata/plugin-ipc`
     - approved use of `ssh win11`, starting from `msys2`, for Windows development and validation
   - Fact:
     - this repo currently has no configured `git remote`.
     - Rust Windows transport is still a placeholder in `src/crates/netipc/src/transport/windows.rs`.
     - Go Windows transport is still a placeholder package in `src/go/pkg/netipc/transport/windows/`.
     - the C library now has an initial Windows Named Pipe transport under `src/libnetdata/netipc/src/transport/windows/`.
   - Implication:
     - cross-compilation alone is not enough for confidence here; Named Pipe behavior, timeouts, permissions, and benchmark results need real runtime validation on Windows.
     - pushing the repo first will make the Windows work easier to sync, review, and validate across machines.

61. Windows MSYS2 environment should use `MINGW64`, with build packages installed via `pacman`, while Rust should be installed separately via `rustup`.
   - Source: user asked which MSYS2 packages to install and then instructed to ssh and run the install.
   - User decision:
     - approved installation of the recommended MSYS2 package set on `win11`
     - clarified that `ssh win11` lands in `/home/costa` under MSYS2, with repositories available under `/home/costa/src/`
     - clarified that the remaining Windows prerequisites are now installed
   - Package set:
     - `mingw-w64-x86_64-toolchain`
     - `mingw-w64-x86_64-cmake`
     - `mingw-w64-x86_64-ninja`
     - `mingw-w64-x86_64-pkgconf`
   - Implication:
     - Windows builds should run under `MINGW64` paths/toolchain, not the broken plain-`MSYS` `/bin/cmake`.

62. Windows support must cover two build/runtime paths during the Netdata transition:
   - native Windows builds under `MINGW64`
   - MSYS2 POSIX-emulation builds on Windows, because current Netdata still runs there
   - Source: user clarification "the windows library needs to also compile under msys2 (posix emulation), because currently netdata runs with posix emulation and we port it to mingw64 (not done yet)"
   - Implication:
     - the repository cannot treat "Windows" as one build target only
     - native Windows transport work still targets Named Pipes
     - the POSIX transport/library paths also need to compile on Windows under MSYS2 while Netdata remains on the emulation runtime
   - Risk:
     - some transport assumptions are no longer simply "POSIX vs Windows"; build-system and source guards need to distinguish Linux/macOS/FreeBSD, MSYS2-on-Windows, and native Windows carefully

63. Before continuing Windows implementation work, keep the GitHub repo in sync with the latest recorded Windows findings so Linux and Windows work starts from the same visible baseline.
   - Source: user proposal "make sure the repo is synced to github and then I will start you on windows."
   - User decision: approved commit/push of the updated TODO state before Windows work starts (`1a`).
   - Fact:
     - local `HEAD` and `origin/main` currently point to the same commit `1917f75`
     - the only current local modification is `TODO-plugin-ipc.md`
   - Implication:
     - to make the repo fully synced, either the updated TODO must be committed and pushed, or the Windows analysis notes remain local only

## Current Implementation Status (2026-03-08)
- Completed:
  - C library sources moved under `src/libnetdata/netipc/`.
  - Go helper and benchmark code moved under `src/go/`, `tests/fixtures/go/`, and `bench/drivers/go/`.
  - Rust helper and benchmark code moved under `src/crates/`, `tests/fixtures/rust/`, and `bench/drivers/rust/`.
  - Root build entry switched to `CMake`, with helper targets for C, Rust, and Go validation binaries.
  - Test scripts updated to consume artifacts from `build/bin/`.
  - README updated to describe the approved layout and build flow.
  - Obsolete prototype leftovers removed from the repository root (`interop/`, `include/`).
  - Rust crate now contains:
    - reusable frame/protocol encode/decode API
    - first reusable POSIX `UDS_SEQPACKET` client/server API
    - reusable POSIX `SHM_HYBRID` client/server API aligned with the C library state machine
    - negotiated UDS profile-`2` switch that reuses the crate `SHM_HYBRID` API instead of helper-only code
    - crate-level unit tests for protocol and auth/roundtrip transport behavior
  - Go package now contains:
    - reusable frame/protocol encode/decode API
    - first reusable POSIX `UDS_SEQPACKET` client/server API
    - package-level unit tests for protocol and auth/roundtrip transport behavior
  - Schema codec fixtures now consume the reusable Rust crate and Go package instead of embedding private protocol copies.
  - Rust `netipc_live_rs` fixture now consumes the reusable crate `SHM_HYBRID` API instead of embedding private mmap/semaphore code.
  - Rust `netipc_live_uds_rs` helper now consumes the reusable crate UDS transport, including negotiated `SHM_HYBRID`, instead of embedding a separate transport implementation.
  - Go `netipc-live-go` helper now consumes the reusable Go package for normal UDS server/client/bench flows instead of embedding a separate transport implementation.
  - Root `CMakeLists.txt` helper targets now depend on library source trees so fixture/helper rebuilds track library changes.
  - C library now contains an initial Windows Named Pipe transport and Windows live fixture build path for MSYS2 `mingw64`/`ucrt64`.
  - C library now also contains a Windows negotiated `SHM_HYBRID` fast profile backed by shared memory plus named events with bounded spin.
  - Windows fast-path design is intentionally limited to Win32 primitives that can be ported to Rust and pure Go without `cgo`.
- Validated:
  - schema interop: `C <-> Rust <-> Go`
  - live SHM interop: `C <-> Rust`
  - live UDS interop: `C <-> Rust <-> Go`
  - Windows C Named Pipe smoke under MSYS2 `mingw64`: `./tests/run-live-npipe-smoke.sh`
  - Windows C profile comparison under MSYS2 `mingw64`: `./tests/run-live-win-profile-bench.sh`
    - latest local result on `win11`, 5s, 1 client:
      - `c-npipe`: ~16.1k req/s, p50 ~43.6us
      - `c-shm-hybrid` (default spin `1024`): ~82.3k req/s, p50 ~3.8us
  - UDS negative negotiation coverage
  - UDS and negotiated-profile benchmark scripts
  - `cargo test -p netipc`
  - `go test ./...` under `src/go`
- Still incomplete:
  - Go package currently implements the reusable POSIX `UDS_SEQPACKET` path only.
  - Go negative-test helper logic for malformed/raw negotiation frames remains local to `bench/drivers/go`; this is fixture-specific coverage, not reusable API.
  - Rust and Go Windows transports remain placeholders.
  - Windows validation is still limited to the C Named Pipe/`SHM_HYBRID` path; cross-language Windows interop and benchmark coverage are still pending.
  - TODO/history text still contains historical references to the old prototype paths and should be cleaned once the structure is frozen.

## Auth Contract Verification (2026-03-08)
- Fact: `NETDATA_INVOCATION_ID` is already read by the Rust plugin runtime in Netdata:
  - `~/src/netdata-ktsaou.git/src/crates/netdata-plugin/rt/src/netdata_env.rs`
- Fact: Netdata logging initialization checks `NETDATA_INVOCATION_ID`, falls back to systemd `INVOCATION_ID`, and then exports `NETDATA_INVOCATION_ID` back into the environment:
  - `~/src/netdata-ktsaou.git/src/libnetdata/log/nd_log-init.c`
- Fact: Netdata plugin documentation already describes `NETDATA_INVOCATION_ID` as the unique invocation UUID for the current Netdata session:
  - `~/src/netdata-ktsaou.git/src/plugins.d/README.md`
- Fact: Several existing shell/plugin entrypoints pass through `NETDATA_INVOCATION_ID`, so using it for IPC auth aligns with existing Agent behavior.
- Fact: User decision supersedes direct environment lookup inside the library itself; the library should remain integration-agnostic and accept auth as an API input.
- Implication: IPC auth contract should be modeled as caller-provided UUID/token input, with Netdata integration later responsible for supplying `NETDATA_INVOCATION_ID`.
- Risk: test/demo code may still use environment variables for convenience, but the reusable library surface must not depend on that mechanism.

## Plan
1. Create benchmark harness structure for C/Rust/Go with shared test scenarios.
2. Implement primary benchmark scenario first: client-server counter loop.
   - Client sends number N; server increments and returns N+1.
   - Client verifies increment by exactly +1 and sends returned value back.
   - Run duration: 5 seconds per test.
   - Capture: requests sent, responses received, latest counter value, latency per request-response pair, CPU utilization (client and server).
3. Benchmark all transport candidates selected in Decisions #1 and #7 using the same scenario and persistent-session model (Decision #8).
4. For shared-memory transport, benchmark both synchronization approaches from Decision #10.
5. Run both benchmark modes from Decision #11 (strict ping-pong baseline and pipelined secondary mode).
6. Collect CPU metrics using both measurement approaches from Decision #12.
7. Produce decision report with a primary score for single-thread ping-pong and a secondary score for multi-client scaling.
8. Freeze transport + framing + custom binary schema for v1.
9. Implement minimal typed RPC surface in all 3 language libraries.
10. Implement auth handshake using shared SALT/session UUID.
11. Add integration test: C client -> Rust server, Rust client -> Go server, Go client -> C server.
12. Add stress tests to validate stability and tail latency.

## Current Phase Plan (Decision #21)
1. Define and freeze a minimal v1 typed schema for one RPC method (`increment`) with fixed-size binary frame and explicit little-endian encode/decode.
2. Add a C API module for POSIX shared-memory hybrid transport with default spin window 20, targeting single-thread ping-pong first.
3. Add simple C server/client examples using the new C API and schema.
4. Add Rust and Go schema codecs and interoperability fixtures.
5. Add an automated interop test runner to validate:
   - C client framing <-> Rust server framing.
   - Rust client framing <-> Go server framing.
   - Go client framing <-> C server framing.
6. Document baseline and current limitations explicitly.

## Current Phase Plan (Decision #26/#29)
1. Add a POSIX C `UDS_SEQPACKET` transport module as the v1 baseline profile.
2. Add fixed-binary handshake frames for capability negotiation and server-selected profile selection.
3. Keep handshake bitmask-compatible with optional future profiles.
4. Add C server/client demos for persistent session request-response.
5. Add an automated test for handshake correctness plus increment loop behavior.
6. Keep SHM transport and live interop tests intact to avoid regressions in parallel work.

## Current Phase Plan (Decision #33)
1. Extend C UDS transport profile mask to implement `NETIPC_PROFILE_SHM_HYBRID` negotiation candidate.
2. Keep handshake over `UDS_SEQPACKET`, then switch request/response data-plane to shared-memory hybrid when profile `2` is selected.
3. Extend Rust native UDS live runner to support the same negotiated profile switch.
4. Keep Go native UDS live runner baseline-only (`profile 1`) so C/Rust<->Go automatically fall back to UDS.
5. Add/extend live interoperability tests to validate:
   - C<->Rust prefers `SHM_HYBRID` when both advertise it.
   - Any path involving Go negotiates `UDS_SEQPACKET`.
6. Add/extend benchmark scripts to compare negotiated `profile 1` vs `profile 2` in C/Rust paths.

## Current Phase Plan (Decision #34)
1. Replace busy/yield rate pacing loops with adaptive sleep pacing in:
   - `src/ipc_bench.c`
   - `interop/rust/src/bin/netipc_live_uds_rs.rs`
   - `interop/go-live/main.go`
2. Keep full-speed (`target_rps=0`) behavior unchanged.
3. Rebuild and rerun benchmark comparisons:
   - `tests/run-live-uds-bench.sh`
   - `tests/run-negotiated-profile-bench.sh`
4. Update TODO and results tables with post-fix metrics.

## Current Phase Plan (Decision #35)
1. Remove pure-Go SHM polling commands from `interop/go-live/main.go` command surface.
2. Remove dedicated pure-go-poll benchmark script usage (`tests/run-poll-vs-sem-bench.sh`).
3. Remove SHM live interop script dependency on Go polling commands (`tests/run-live-interop.sh`) by narrowing scope to C<->Rust only.
4. Update README and TODO to reflect that Go interop path is UDS-only baseline in this phase.
5. Rebuild and rerun the remaining active test matrix to ensure no regressions.

Status (2026-03-08): completed. Validation reruns passed:
- `make`
- `./tests/run-live-interop.sh`
- `./tests/run-live-uds-interop.sh`
- `./tests/run-live-uds-bench.sh`
- `./tests/run-negotiated-profile-bench.sh`
- `./tests/run-interop.sh`
- `./tests/run-uds-negotiation-negative.sh`

## Next Phase Plan (Decisions #36-#40)
1. Initialize new public repository skeleton (C/Rust/Go library directories, shared protocol spec, CI layout).
2. Port current proven protocol/interop baseline (`UDS_SEQPACKET` + negotiation) into reusable library APIs.
3. Implement Windows transport baseline and align handshake/auth semantics with POSIX.
4. Build exhaustive test matrix per language (unit + integration + cross-language interop) with coverage gates.
5. Build benchmark matrix automation for Linux and Windows role combinations on GitHub-hosted cloud VMs and publish machine-readable artifacts.
6. Add benchmark noise controls for cloud VMs:
   - run each benchmark scenario in multiple repetitions (for example 5)
   - report median plus spread (p95 of run-to-run throughput/latency)
   - capture runner metadata (OS image, vCPU model, memory, kernel/build info)
   - compare combinations primarily by relative ranking, not single-run absolute numbers
7. Freeze acceptance criteria (coverage gate + benchmark sanity gate) before any Netdata integration work.
8. Verify the actual Netdata runtime auth environment contract (`NETDATA_INVOCATION_ID` or replacement) in the Agent codebase and align the IPC handshake implementation with it.
9. Produce benchmark evidence for acceptance review and use that review to decide whether the standalone repo is ready to integrate into Netdata.

## Risks / Constraints In This Phase
- Fact: Current benchmark shared-memory transport is embedded in a benchmark executable and not yet a standalone reusable library.
- Fact: A full production-ready cross-language shared-memory transport API (with lifecycle, reconnect, multi-client fairness, and auth handshake) is larger than this phase.
- Working constraint for this phase:
  - Prioritize protocol/schema interoperability and a minimal C transport API suitable for iterative hardening.

## Repository Layout Analysis (2026-03-08)
- Fact: Top-level currently mixes source artifacts, generated binaries, library code, demos, benchmark executable, and language experiments:
  - top-level binaries: `ipc-bench`, `netipc-codec-c`, `netipc-shm-*`, `netipc-uds-*`, `libnetipc.a`
  - source directories: `src/`, `include/`, `interop/`, `tests/`
- Fact: `interop/` is overloaded:
  - `interop/go` is a schema codec tool
  - `interop/go-live` is a transport/live benchmark runner
  - `interop/rust` contains both codec tool and live runners
- Fact: Current layout does not map cleanly to the intended product boundary of:
  - one C library
  - one Rust library
  - one Go library
  - each with POSIX and Windows implementations
- Risk: if we keep the current layout, adding Windows, CI, coverage, public API docs, examples, and protocol evolution will create repeated ad-hoc structure and unclear ownership of code paths.
- Requirement for the redesign:
  - separate reusable library code from experiments, examples, benchmarks, interop fixtures, and generated build output
  - make transport implementations discoverable by language and platform
  - keep one shared protocol/spec area so wire semantics are not duplicated informally
- Fact: Netdata destination layout already has stable language-specific homes:
  - C core/common code under `src/libnetdata/...`
  - Go packages under `src/go/pkg/...`
  - Rust workspace/crates under `src/crates/...`
- Fact: Netdata top-level `CMakeLists.txt` already:
  - builds `libnetdata`
  - imports Rust crates from `src/crates/...`
  - drives Go build targets rooted at `src/go/...`
- Implication: the standalone `plugin-ipc` repository should mirror these boundaries as closely as possible, otherwise future integration will require a second structural rewrite.

## Implementation Progress
- Done: Phase 1 POSIX C benchmark harness implemented (`ipc-bench`).
- Done: Transport candidates implemented for benchmarking:
- `AF_UNIX/SOCK_STREAM`
- `AF_UNIX/SOCK_SEQPACKET`
- `AF_UNIX/SOCK_DGRAM`
- Shared memory ring with spin synchronization (`shm-spin`)
- Shared memory ring with blocking semaphore synchronization (`shm-sem`)
- Shared memory ring with hybrid synchronization (`shm-hybrid`).
- Done: fixed-rate client pacing support (`--target-rps`, per-client) for hypothesis testing.
- Done: Both benchmark modes implemented:
- strict ping-pong mode
- pipelined mode with configurable depth
- Done: Metrics implemented:
- request/response counters
- mismatch detection
- last counter tracking
- latency histogram with p50/p95/p99
- in-process CPU accounting
- external CPU sampling from `/proc`
- Done: Build and usage docs added in `README.md`.
- Done (historical): POSIX baseline was initially frozen to `shm-hybrid` with default spin window `20` (`NETIPC_SHM_DEFAULT_SPIN_TRIES`), later superseded by Decision #26 (`UDS_SEQPACKET` baseline profile).
- Done: First typed v1 schema implemented (`increment`) with fixed 64-byte frame and explicit little-endian encode/decode:
  - `include/netipc_schema.h`
  - `src/netipc_schema.c`
- Done: First C transport API implemented for shared-memory hybrid path:
  - `include/netipc_shm_hybrid.h`
  - `src/netipc_shm_hybrid.c`
- Done: C tools/examples added:
  - `netipc-codec-c` (`src/netipc_codec_tool.c`)
  - `netipc-shm-server-demo` (`src/netipc_shm_server_demo.c`)
  - `netipc-shm-client-demo` (`src/netipc_shm_client_demo.c`)
- Done: Cross-language schema interop tools added:
  - Rust codec tool (`interop/rust`)
  - Go codec tool (`interop/go`)
- Done: Automated interop validation script added:
  - `tests/run-interop.sh`
  - Validates C->Rust->C, Rust->Go->Rust, Go->C->Go for typed `increment` schema frames.
- Done: Build system updated (`Makefile`) to produce:
  - `libnetipc.a`
  - `netipc-codec-c`
  - `netipc-shm-server-demo`
  - `netipc-shm-client-demo`
- Done: Stale endpoint recovery added to C shared-memory transport:
  - Region ownership PID tracking.
  - Safe takeover rules:
    - owner alive -> `EADDRINUSE`
    - owner dead/invalid region -> unlink stale endpoint and recreate.
- Done: Live transport interop runners added for this phase:
  - Rust live runner: `interop/rust/src/bin/netipc_live_rs.rs`
  - Go live runner: `interop/go-live/main.go`
  - Live interop script: `tests/run-live-interop.sh`
- Done: Rust/Go live runners moved to independent transport implementations (no link dependency on `libnetipc.a`):
  - Rust: native mapping + sequencing + schema encode/decode + semaphore waits/posts (`libc` bindings).
  - Go: native pure-Go UDS seqpacket + schema encode/decode (no cgo).
  - C/Rust waits are semaphore-optional for profile-2 interop when needed.
  - Safety hardening:
    - Rust stale-takeover mapping uses duplicated FDs (no double-close risk).
    - Rust only destroys semaphores after successful semaphore initialization.
- Done: Removed pure-Go SHM polling path after profiling and decision #35:
  - Removed Go SHM polling command handlers from `interop/go-live/main.go`.
  - Removed `tests/run-poll-vs-sem-bench.sh`.
  - Narrowed `tests/run-live-interop.sh` to C<->Rust SHM coverage only.
- Done: POSIX C `UDS_SEQPACKET` transport module added:
  - `include/netipc_uds_seqpacket.h`
  - `src/netipc_uds_seqpacket.c`
- Done: Fixed-binary profile negotiation added to UDS transport:
  - client hello with supported/preferred bitmasks
  - server ack with intersection + selected profile + status
  - deterministic server-selected profile flow
- Done: UDS stale endpoint recovery logic added:
  - active listener -> fail with `EADDRINUSE`
  - stale socket path -> unlink and recreate
- Done: C UDS demos added:
  - `netipc-uds-server-demo` (`src/netipc_uds_server_demo.c`)
  - `netipc-uds-client-demo` (`src/netipc_uds_client_demo.c`)
- Done: Build updated for UDS artifacts (`Makefile`).
- Done: Automated UDS negotiation test added:
  - `tests/run-uds-seqpacket.sh`
- Done: Native Rust live UDS runner added (no `libnetipc.a` link dependency):
  - `interop/rust/src/bin/netipc_live_uds_rs.rs`
  - Supports:
    - `server-once <run_dir> <service>`
    - `client-once <run_dir> <service> <value>`
    - `server-loop <run_dir> <service> <max_requests|0>`
    - `client-bench <run_dir> <service> <duration_sec> <target_rps>`
  - Implements seqpacket + fixed-binary negotiation profile.
- Done: Native pure-Go live UDS commands added to Go runner (`CGO_ENABLED=0`):
  - `interop/go-live/main.go`
  - Supports:
    - `uds-server-once <run_dir> <service>`
    - `uds-client-once <run_dir> <service> <value>`
    - `uds-server-loop <run_dir> <service> <max_requests|0>`
    - `uds-client-bench <run_dir> <service> <duration_sec> <target_rps>`
    - `uds-client-badhello <run_dir> <service>`
    - `uds-client-rawhello <run_dir> <service> <supported_mask> <preferred_mask> <auth_token>`
  - Implements seqpacket + fixed-binary negotiation profile.
- Done: Live cross-language UDS interop test added:
  - `tests/run-live-uds-interop.sh`
  - Validates:
    - C client -> Rust server
    - Rust client -> Go server
    - Go client -> C server
- Done: Live UDS benchmark matrix script added:
  - `tests/run-live-uds-bench.sh`
  - Measures C/Rust/Go UDS paths at max, 100k/s, 10k/s.
  - Reports throughput, p50 latency, client CPU, server CPU, total CPU.
- Done: UDS negotiation negative test script added:
  - `tests/run-uds-negotiation-negative.sh`
  - Validates expected failures for:
    - profile mismatch (`ENOTSUP`)
    - auth mismatch (`EACCES`)
    - malformed hello (`EPROTO`)
- Done: C UDS demos extended with optional negotiation inputs:
  - `src/netipc_uds_server_demo.c`: optional `supported_profiles`, `preferred_profiles`, `auth_token`.
  - `src/netipc_uds_client_demo.c`: optional `supported_profiles`, `preferred_profiles`, `auth_token`.
- Done: C UDS transport now implements negotiated `SHM_HYBRID` data-plane switch (profile `2`) in addition to baseline `UDS_SEQPACKET`:
  - `src/netipc_uds_seqpacket.c`
  - Behavior:
    - handshake remains over UDS seqpacket
    - if negotiated profile is `2`, request/response path switches to shared-memory hybrid API
    - client-side attach includes short retry loop to avoid race with server shared-memory region creation
- Done: Rust native UDS live runner now supports negotiated `SHM_HYBRID` data-plane switch (profile `2`) with env-driven handshake profile masks:
  - `interop/rust/src/bin/netipc_live_uds_rs.rs`
  - Added env controls:
    - `NETIPC_SUPPORTED_PROFILES`
    - `NETIPC_PREFERRED_PROFILES`
    - `NETIPC_AUTH_TOKEN`
  - C/Rust `server-once`, `client-once`, `server-loop`, `client-bench` switch data-plane based on negotiated profile.
- Done: Live UDS interop script extended with negotiated profile assertions:
  - `tests/run-live-uds-interop.sh`
  - Added coverage for:
    - C client -> Rust server with profile preference `2` (expects profile `2`)
    - Rust client -> C server with profile preference `2` (expects profile `2`)
    - baseline/fallback profile checks stay enforced for paths involving Go.
- Done: Added negotiated profile benchmark script:
  - `tests/run-negotiated-profile-bench.sh`
  - Compares profile `1` (`UDS_SEQPACKET`) vs profile `2` (`SHM_HYBRID`) under identical 5s client-bench scenarios.
- Done: Rate-limited benchmark client pacing switched from busy/yield loops to adaptive sleep pacing:
  - `src/ipc_bench.c` (`sleep_until_ns`, fixed-rate schedule progression)
  - `interop/rust/src/bin/netipc_live_uds_rs.rs` (`sleep_until`)
  - `interop/go-live/main.go` (`sleepUntil`)
  - Intent: remove pacing busy-loop CPU pollution from fixed-rate benchmark metrics.

## Validation Results (Phase #21)
- `make clean && make`: pass.
- `./tests/run-interop.sh`: pass.
  - Schema interop validated for:
    - C -> Rust -> C
    - Rust -> Go -> Rust
    - Go -> C -> Go
- C shared-memory API smoke test:
  - Server: `./netipc-shm-server-demo /tmp netipc-demo 2`
  - Client: `./netipc-shm-client-demo /tmp netipc-demo 50 2`
  - Result: pass (`50->51`, `51->52`).
- Live transport interop test:
  - `./tests/run-live-interop.sh`: pass.
  - Validated real `shm-hybrid` sessions for:
    - C client -> Rust server
    - Rust client -> C server
- Native runner validation:
  - Rust live binary (`netipc_live_rs`) builds without linking to `libnetipc.a`.
  - Go live binary (`netipc-live-go`) builds without linking to `libnetipc.a`.
  - `ldd` check confirms no `libnetipc` dependency in either live binary.
  - Go live binary builds and runs with `CGO_ENABLED=0`.
- Regression validation after safety fixes (`cargo fmt`, `gofmt`, full test rerun): pass.
- Stale endpoint recovery test:
  - Forced stale endpoint by terminating a server process before cleanup.
  - New server startup recovered stale endpoint and served request successfully.
- Historical (pre-removal) focused polling-vs-semaphore benchmark (`./tests/run-poll-vs-sem-bench.sh`): pass.
  - latest rerun after pacing fix:
    - `max`:
      - semaphore: throughput ~1,753,540 req/s, p50 ~0.54us, total CPU ~1.966
      - pure-go-poll: throughput ~1,535 req/s, p50 ~1054.35us, total CPU ~0.024
    - `100k/s` target:
      - semaphore: throughput ~99,999 req/s, p50 ~3.20us, total CPU ~0.418
      - pure-go-poll: throughput ~1,049 req/s, p50 ~1054.89us, total CPU ~0.015
    - `10k/s` target:
      - semaphore: throughput ~9,999 req/s, p50 ~3.20us, total CPU ~0.054
      - pure-go-poll: throughput ~1,076 req/s, p50 ~1055.22us, total CPU ~0.018
  - Fact: current pure-Go polling configuration does not reach 10k/s.
- UDS negotiation + increment test (`./tests/run-uds-seqpacket.sh`): pass.
  - Negotiated profile is `1` (`NETIPC_PROFILE_UDS_SEQPACKET`) on both client/server.
  - Verified increment loop for two iterations (`41->42`, `42->43`).
- Live UDS cross-language interop test (`./tests/run-live-uds-interop.sh`): pass.
  - C client -> Rust server: pass, profile `1`.
  - Rust client -> Go server: pass, profile `1`.
  - Go client -> C server: pass, profile `1`.
- UDS negotiation negative tests (`./tests/run-uds-negotiation-negative.sh`): pass.
  - Profile mismatch with raw hello mask `2` -> status `95` (`ENOTSUP`).
  - Auth mismatch (`111` vs `222`) -> status `13` (`EACCES`).
  - Malformed hello frame -> server rejects with `EPROTO`.
- Live UDS benchmark matrix (`./tests/run-live-uds-bench.sh`): pass.
  - latest rerun:
    - `max`:
      - c-uds: throughput ~254,999 req/s, p50 ~3.71us, total CPU ~1.010
      - rust-uds: throughput ~268,966 req/s, p50 ~3.59us, total CPU ~1.001
      - go-uds: throughput ~243,775 req/s, p50 ~3.50us, total CPU ~1.950
    - `100k/s` target:
      - c-uds: throughput ~99,999 req/s, p50 ~4.35us, total CPU ~0.424
      - rust-uds: throughput ~100,000 req/s, p50 ~3.99us, total CPU ~0.397
      - go-uds: throughput ~99,985 req/s, p50 ~3.51us, total CPU ~0.841
    - `10k/s` target:
      - c-uds: throughput ~10,000 req/s, p50 ~4.35us, total CPU ~0.048
      - rust-uds: throughput ~10,000 req/s, p50 ~4.11us, total CPU ~0.048
      - go-uds: throughput ~9,998 req/s, p50 ~4.30us, total CPU ~0.124
- Negotiated profile interop expansion (`./tests/run-live-uds-interop.sh`): pass.
  - Added assertions verify:
    - C<->Rust can negotiate `profile 2` (`SHM_HYBRID`) when both peers advertise/prefer it.
    - Paths involving Go continue to negotiate `profile 1` (`UDS_SEQPACKET`) fallback.
- Negotiated profile benchmark (`./tests/run-negotiated-profile-bench.sh`): pass.
  - Rust server <-> Rust client:
    - `max`:
      - profile1-uds: throughput ~267,448 req/s, p50 ~3.56us, total CPU ~1.000
      - profile2-shm: throughput ~4,353,061 req/s, p50 ~0.20us, total CPU ~2.323
    - `100k/s` target:
      - profile1-uds: throughput ~99,999 req/s, p50 ~4.00us, total CPU ~0.403
      - profile2-shm: throughput ~99,999 req/s, p50 ~0.21us, total CPU ~0.180
    - `10k/s` target:
      - profile1-uds: throughput ~10,000 req/s, p50 ~4.20us, total CPU ~0.048
      - profile2-shm: throughput ~10,000 req/s, p50 ~3.40us, total CPU ~0.056
- Regression checks after UDS implementation:
  - `./tests/run-interop.sh`: pass.
  - `./tests/run-live-interop.sh`: pass.
- Regression checks after extracting reusable Rust `SHM_HYBRID` crate API:
  - `cargo test -p netipc`: pass.
  - `./tests/run-live-interop.sh`: pass, with `netipc_live_rs` now backed by the crate API instead of private SHM code.
- Regression checks after extracting negotiated Rust UDS profile switching into the crate:
  - `cargo test -p netipc`: pass, including negotiated `SHM_HYBRID` roundtrip coverage.
  - `./tests/run-live-uds-interop.sh`: pass, including `profile 2` (`SHM_HYBRID`) negotiation for `C <-> Rust`.
  - `./tests/run-uds-negotiation-negative.sh`: pass.
  - `./tests/run-negotiated-profile-bench.sh`: pass.
  - `./tests/run-live-uds-bench.sh`: pass.
- Regression checks after refactoring Go UDS helper onto the reusable package:
  - `go test ./...` under `src/go`: pass.
  - `./tests/run-live-uds-interop.sh`: pass.
  - `./tests/run-uds-negotiation-negative.sh`: pass.
  - `./tests/run-live-uds-bench.sh`: pass.
- Seqpacket baseline benchmark spot check:
  - `./ipc-bench --transport seqpacket --mode pingpong --clients 1 --payloads 32 --duration 5`
  - Result: throughput ~265,550 req/s, p50 ~3.46us.

## Initial Findings (Smoke, Linux x86_64, 1s, 1 client, payload=40B)
- `uds-stream` pingpong: ~297k req/s, p50 ~2.94us
- `uds-seqpacket` pingpong: ~260k req/s, p50 ~3.46us
- `uds-dgram` pingpong: ~242k req/s, p50 ~3.97us
- `shm-spin` pingpong: ~2.61M req/s, p50 ~0.34us
- `shm-sem` pingpong: ~330k req/s, p50 ~2.94us
- `uds-stream` pipeline depth=16: ~1.01M req/s
- `uds-seqpacket` pipeline depth=16: ~1.05M req/s
- `uds-dgram` pipeline depth=16: ~857k req/s
- `shm-spin` pipeline depth=16: ~6.64M req/s
- `shm-sem` pipeline depth=16: ~2.08M req/s

## Hybrid Findings (5s, pingpong, payload=40B)
- `shm-spin`, 1 client: ~2.77M req/s, p50 ~0.30us, total CPU cores ~1.99
- `shm-hybrid`, 1 client: ~1.96M req/s, p50 ~0.46us, total CPU cores ~1.99
- `shm-sem`, 1 client: ~312k req/s, p50 ~2.94us, total CPU cores ~1.02
- `shm-spin`, 8 clients: ~15.78M req/s, p50 ~0.43us
- `shm-hybrid`, 8 clients: ~4.84M req/s, p50 ~1.60us
- `shm-sem`, 8 clients: ~1.78M req/s, p50 ~4.35us
- Single-thread pingpong winner: `shm-spin`.

## Fixed-Rate Findings (5s, pingpong, 1 client, payload=40B, target_rps=100k)
- `shm-spin`: achieved ~99.0k req/s, p50 ~0.37us, client CPU ~0.995 cores, server CPU ~0.998 cores.
- `shm-hybrid`: achieved ~98.9k req/s, p50 ~0.54us, client CPU ~0.994 cores, server CPU ~0.992 cores.
- `shm-sem`: achieved ~98.3k req/s, p50 ~2.94us, client CPU ~0.833 cores, server CPU ~0.151 cores.
- At 100k/s, `shm-hybrid` CPU is not significantly lower than `shm-spin` in this implementation.

## Fixed-Rate Findings (5s, pingpong, 1 client, payload=40B, target_rps=10k)
- `shm-spin`: achieved ~9.99k req/s, p50 ~0.34us, client CPU ~0.995 cores, server CPU ~0.998 cores.
- `shm-hybrid`: achieved ~9.99k req/s, p50 ~1.86us, client CPU ~0.994 cores, server CPU ~0.111 cores.
- `shm-sem`: achieved ~9.98k req/s, p50 ~3.20us, client CPU ~0.975 cores, server CPU ~0.017 cores.
- At 10k/s, `shm-hybrid` does block/wait more on server side (large server CPU drop vs `shm-spin`), while client CPU remains high due rate pacing strategy.

## Hybrid Spin-Window Tuning (from 256 -> 64, 5s, pingpong, 1 client, payload=40B, target_rps=100k)
- Before (`SHM_HYBRID_SPIN_TRIES=256`): ~98.9k req/s, p50 ~0.54us, client CPU ~0.994, server CPU ~0.992 (total ~1.986).
- After (`SHM_HYBRID_SPIN_TRIES=64`): ~98.5k req/s, p50 ~1.86us, client CPU ~0.987, server CPU ~0.364 (total ~1.351).
- Effect: significant CPU drop (especially server) with moderate latency increase.

## Hybrid Spin Sweep (2 passes average, 5s, pingpong, 1 client, payload=40B)
- Sweep values: 0, 1, 4, 16, 64, 256, 1024.
- Metrics tracked:
  - Max throughput (unlimited mode): req/s, p50, total CPU cores.
  - Fixed-rate 100k target: achieved req/s, p50, total CPU cores.
- Averaged findings:
  - spin=0: max ~336k req/s (p50~2.69us, cpu~1.023), fixed-100k ~98.15k req/s (p50~2.94us, cpu~0.982)
  - spin=1: max ~338k req/s (p50~2.69us, cpu~1.048), fixed-100k ~98.27k req/s (p50~2.94us, cpu~0.989)
  - spin=4: max ~372k req/s (p50~2.81us, cpu~1.142), fixed-100k ~98.09k req/s (p50~2.94us, cpu~1.011)
  - spin=16: max ~1.17M req/s (p50~0.50us, cpu~1.676), fixed-100k ~98.16k req/s (p50~2.94us, cpu~1.101)
  - spin=64: max ~1.88M req/s (p50~0.48us, cpu~1.988), fixed-100k ~98.35k req/s (p50~1.86us, cpu~1.349)
  - spin=256: max ~1.95M req/s (p50~0.46us, cpu~1.989), fixed-100k ~99.00k req/s (p50~0.54us, cpu~1.989)
  - spin=1024: max ~1.82M req/s (p50~0.52us, cpu~1.989), fixed-100k ~99.06k req/s (p50~0.52us, cpu~1.990)
- Key pattern:
  - More spins increase max throughput (up to ~256), but also increase CPU at fixed 100k/s.
  - Low spins reduce fixed-rate CPU but hurt latency and cap max throughput.

## Request-Rate Increase Per Spin (elbow sweep, spins 8..32, 3 reps each)
- Max-throughput means (unlimited mode) and marginal gain per spin:
  - spin 8:  ~526.7k req/s
  - spin 12: ~768.8k req/s,  +60.5k req/s per added spin
  - spin 16: ~1.123M req/s,  +88.6k req/s per added spin
  - spin 20: ~1.840M req/s, +179.3k req/s per added spin
  - spin 24: ~1.815M req/s,   -6.4k req/s per added spin
  - spin 28: ~1.829M req/s,   +3.6k req/s per added spin
  - spin 32: ~1.891M req/s,  +15.4k req/s per added spin
- Fixed-100k CPU for same spins:
  - spin 8: total CPU ~1.048 cores
  - spin 12: total CPU ~1.075 cores
  - spin 16: total CPU ~1.107 cores
  - spin 20: total CPU ~1.126 cores
  - spin 24: total CPU ~1.146 cores
  - spin 28: total CPU ~1.230 cores
  - spin 32: total CPU ~1.230 cores
- Interpretation:
  - Strong gains continue through spin 20.
  - After ~20, throughput gains become small/noisy while fixed-rate CPU keeps climbing.

## Latest Baseline After Setting Default Hybrid Spin=20 (3 reps, 5s, pingpong, 1 client, payload=40B)
- Validation:
  - No failed clients.
  - No mismatches/collisions.
- Max-throughput mode (`target_rps=0`):
  - `shm-spin`: ~2.61M req/s, p50 ~0.34us, total CPU ~1.985 cores.
  - `shm-hybrid`: ~1.79M req/s, p50 ~0.53us, total CPU ~1.962 cores.
  - `shm-sem`: ~335k req/s, p50 ~2.77us, total CPU ~1.021 cores.
  - `uds-stream`: ~305k req/s, p50 ~2.94us, total CPU ~1.239 cores.
  - `uds-seqpacket`: ~265k req/s, p50 ~3.46us, total CPU ~1.000 cores.
  - `uds-dgram`: ~244k req/s, p50 ~3.88us, total CPU ~0.996 cores.
- Fixed-rate mode (`target_rps=100k`):
  - `shm-spin`: ~99.0k req/s, p50 ~0.34us, total CPU ~1.985 cores.
  - `shm-hybrid`: ~98.2k req/s, p50 ~3.20us, total CPU ~1.116 cores.
  - `shm-sem`: ~98.2k req/s, p50 ~2.94us, total CPU ~0.976 cores.
  - `uds-stream`: ~98.1k req/s, p50 ~3.71us, total CPU ~1.086 cores.
  - `uds-seqpacket`: ~98.1k req/s, p50 ~3.97us, total CPU ~0.974 cores.
  - `uds-dgram`: ~98.4k req/s, p50 ~4.22us, total CPU ~0.974 cores.

## Implied Decisions / Assumptions to Validate
- `RUN_DIR` lifecycle and cleanup policy for stale socket files after crashes/restarts.
- Current prototype behavior for stale endpoint files (`*.ipcshm`):
  - owner PID alive -> fail with `EADDRINUSE`.
  - owner PID dead/invalid region -> stale endpoint is unlinked and recreated automatically.
- Pure-Go transport assumptions:
  - Go implementation is UDS-only in this phase (no cgo, no SHM polling path).
- Version mismatch hard-failure behavior (no external registry in v1).
- Timeout/retry/backoff semantics for plugin-to-plugin calls.
- Request correlation and cancellation support.
- Backpressure limits (max in-flight requests per connection/client).
- Security boundary assumptions (local machine, same netdata spawning context).

## Testing Requirements
- Primary benchmark (user-defined):
  - Counter ping-pong loop: request N, response N+1, strict client-side verification.
  - Duration: 5 seconds per run.
  - Required counters: requests sent, responses received, latest counter value, mismatch/collision count.
  - Required metrics: throughput, latency per request/response, CPU utilization on both client and server.
  - Primary ranking axis: single-client single-thread ping-pong quality (latency and CPU/request first, throughput second).
  - Additional hypothesis check: fixed-rate single-client ping-pong at 100k req/s to compare CPU utilization across transports.
- Microbenchmarks:
  - Use persistent connection/channel semantics for request-response loops (no per-request reconnect).
  - Benchmark modes: strict single in-flight ping-pong baseline and secondary pipelined mode.
  - Candidate POSIX methodologies should include at least:
  - `AF_UNIX/SOCK_STREAM` (persistent connection).
  - `AF_UNIX/SOCK_SEQPACKET` (if available on platform).
  - `AF_UNIX/SOCK_DGRAM` (message-oriented baseline).
  - Shared memory request/response channel with spinlock and blocking/hybrid synchronization variants.
  - 1, 8, 64, 256 concurrent clients.
  - Secondary ranking axis: scaling behavior from 1 -> 8 -> 64 -> 256 clients.
  - Payload sizes: tiny (32B), small (256B), medium (4KB), large (64KB).
  - Metrics: req/s, p50/p95/p99 latency, CPU per process, memory footprint.
  - CPU collection: both external process sampling and in-process accounting.
- Compatibility tests:
  - Cross-language round-trips for all pairs C/Rust/Go.
  - POSIX matrix: Linux/macOS/FreeBSD.
  - Windows matrix: Named Pipes implementation.
- Reliability tests:
  - Server restarts, stale endpoints, client reconnect behavior.
  - Invalid auth attempts and replay attempts.
- API stability tests:
  - Schema evolution (backward/forward compatibility checks).

## Documentation Updates Required
- Architecture doc: plugin IPC model, service discovery, auth model.
- Developer guide: how to expose/consume a service from plugins.
- Protocol spec: framing, message schema, error codes, versioning.
- Ops notes: RUN_DIR socket/pipe lifecycle, troubleshooting, perf tuning.
