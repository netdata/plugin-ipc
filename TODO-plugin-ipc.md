# TODO-plugin-ipc

## Purpose
- Switch the local repository to `origin/windows-transports-rust-go` without risking unrelated work.
- Open a GitHub pull request for the Windows transport branch against `main`.
- Keep the clean PR head branch in sync with the latest commits on `windows-transports-rust-go`.
- Pull the repository after the PR merge and move the local worktree back to `main`.
- Perform a production-readiness review of the full library across Linux and Windows, for C, Rust, and Go, and decide whether it is ready to merge into Netdata.

## Session Handoff (2026-03-11)
- User decision (2026-03-11, after remediation):
  - commit the current fixes and push them
  - use `ssh win11` for real Windows validation before finalizing the commit
  - use `/c/Users/costa/src/plugin-ipc-win.git` on `win11` for validation
  - reset that Windows test clone before applying the current patch and running tests
- Fact: local repository `/home/costa/src/plugin-ipc.git` is currently clean on branch `main` (`git status --short --branch` showed `## main...origin/main`).
- Fact: remote branch `origin/windows-transports-rust-go` exists and was fetched successfully on 2026-03-11.
- Fact: local branch `windows-transports-rust-go` now tracks `origin/windows-transports-rust-go`.
- Fact: `origin/windows-transports-rust-go` is 2 commits ahead of `origin/main`:
  - `d9246b0 Add Rust/Go Windows transports, fix SHM store-load reordering race`
  - `9f050c7 Fix Rust bench: use RDTSC timing instead of QPC per-iteration`
- Fact: current diff versus `origin/main` touches 24 files with 4897 insertions and 97 deletions, including Windows transport code for C, Rust, and Go, Windows benchmark drivers/docs, and Windows smoke/live bench scripts.
- Fact: `origin` remote HEAD branch is `main`.
- Fact: GitHub currently has no open or closed PR with head `netdata:windows-transports-rust-go`.
- Fact: both branch commits currently include `Co-Authored-By` trailers, so opening a PR directly from that head would carry disallowed attribution text into the PR commit list.
- Fact: clean branch `windows-transports-rust-go-pr` was created from `origin/main`, replayed with sanitized commit messages, pushed to `origin`, and used to open PR `#1`.
- Fact: PR URL is `https://github.com/netdata/plugin-ipc/pull/1`.
- Fact: after `windows-transports-rust-go` advanced with `43ee635` and `d910ab0`, the clean PR branch was synced by replaying those commits with sanitized messages and pushing:
  - `b95b8f5 Update benchmark docs and README with post-RDTSC Rust performance`
  - `965879e Fill Rust rate-limited 100K rps benchmark row`
- Fact: the synced `windows-transports-rust-go-pr` branch now matches the tree of `origin/windows-transports-rust-go`.
- Fact: local worktree is now back on `main`, and `main` was fast-forwarded to merged commit `b2bdcd5 Add Windows IPC transports and benchmark coverage (#1)`.
- Fact: the local `TODO-plugin-ipc.md` note was stashed for the pull and then reapplied cleanly on top of `main`.
- Review findings (2026-03-11):
  - `cmake --build build --target test` fails on Linux because the Rust bench-driver manifest always includes the Windows binary and CMake builds the manifest without selecting a single bin; `netipc-live-uds-interop` and `netipc-uds-negotiation-negative` both fail for this reason.
  - `GOOS=windows GOARCH=amd64 go test ./...` fails because the Go POSIX transport package is not build-tagged out on Windows while its syscall helper definitions are unix-only.
  - POSIX SHM stale-endpoint takeover in both C and Rust only checks `owner_pid` even though the shared region stores an `owner_generation`; PID reuse can therefore make stale endpoints appear live.
  - Windows validation scripts remain workstation-specific (`/c/Users/costa/...`, hard-coded PATH/GOROOT), so the documented Windows validation path is not portable or CI-ready.
  - README still documents current limitations: Go/Rust rely on helper binaries for validated live paths, and Netdata integration wiring is explicitly out of scope in this repository phase.
- Current remediation plan:
  - gate the Rust Windows bench binary behind an explicit feature and make CMake build explicit per-bin targets
  - add proper unix build tags to the Go POSIX transport sources/tests
  - strengthen POSIX SHM ownership metadata so stale-endpoint reclaim is resilient to PID reuse
  - make Windows helper scripts path-configurable instead of Costa-workstation-specific
  - rerun Linux validation and cross-build checks after the fixes
  - run the smoke and live Windows checks on `win11` before committing
- Current immediate request:
  - switch locally to `origin/windows-transports-rust-go`
  - create a PR from `windows-transports-rust-go` to `main`
  - sync the clean PR branch to the latest `origin/windows-transports-rust-go`
  - pull the repository and check out `main`
  - review the full codebase for production readiness and Netdata merge suitability
  - commit and push the remediation changes after Windows validation on `win11`
- Current execution plan:
  - keep the local repository on tracking branch `windows-transports-rust-go`
  - keep the temporary clean PR branch available on `origin` as the PR head
  - leave the current repository on `windows-transports-rust-go`
  - replay any new commits from `windows-transports-rust-go` onto the clean PR branch with sanitized commit messages
  - verify the current worktree state after the interrupted turn
  - update local refs
  - switch the main worktree to `main`
  - fast-forward local `main` to `origin/main`
  - audit repository structure, build system, and test coverage
  - inspect Linux and Windows transports in C, Rust, and Go
  - run Linux-side validation that is possible in this environment
  - validate the same changes on `win11` with the real Windows runtime
  - if that validation passes, commit the specific files changed in this task and push `main`
  - produce a review with concrete findings, risks, and a readiness recommendation
- Windows validation results on `win11` (2026-03-11):
  - Fact: `/c/Users/costa/src/plugin-ipc-win.git` was reset to `origin/main`, cleaned, and used as the Windows validation clone.
  - Fact: the current local remediation patch was applied there cleanly with `git apply`.
  - Fact: `go test ./...` under `src/go` passed on Windows when using the official Go installation.
  - Fact: `cargo check --manifest-path bench/drivers/rust/Cargo.toml --features windows-driver --bin netipc_live_win_rs` passed on Windows.
  - Fact: the first smoke-script attempt failed because `win11` has two Go installations and CMake selected `/mingw64/bin/go.exe`, which is broken on that host unless `GOROOT` is set and its stdlib/tool versions match.
  - Fact: `tests/smoke-win.sh` and `tests/run-live-win-bench.sh` were then updated to auto-prefer `/c/Program Files/Go/bin/go.exe` when present and to pass that executable into CMake configuration.
  - Fact: after that script fix, `NETIPC_CMAKE_BUILD_DIR=build-mingw-auto bash tests/smoke-win.sh` passed on `win11` with `18 passed, 0 failed` and no manual Go override.
  - Fact: after that script fix, `NETIPC_SKIP_BUILD=1 NETIPC_CMAKE_BUILD_DIR=build-mingw-auto bash tests/run-live-win-bench.sh` completed successfully on `win11`.

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
  - New active task:
    - pull the latest pushed Windows version from GitHub
    - inspect the Windows implementation changes
    - investigate the reported Windows throughput regression (`~16k req/s`)
  - Current immediate request:
    - pull the latest pushed code locally for inspection without disrupting in-progress local notes

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
       - the deleted prototype benchmark binary
       - `libnetipc.a`
       - `netipc-codec-c`
       - the deleted SHM client demo binary
       - the deleted SHM server demo binary
       - the deleted UDS client demo binary
       - the deleted UDS server demo binary
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

64. Pause local Windows transport optimization work while another assistant provides a fast-path attempt.
   - Source: user direction "Wait. Don't do that. the other assistant is working to provide a fast path."
   - Fact:
     - current pushed native Windows C named-pipe transport was built and smoke-tested successfully on `win11`
     - reproduced max-throughput benchmark on `win11` with the current fixture:
       - `mode=c-npipe`
       - `duration_sec=5`
       - `responses=79024`
       - `throughput_rps=15804.65`
       - `p50_us=43.30`
       - `p95_us=105.60`
       - `p99_us=180.30`
       - `client_cpu_cores=0.428`
   - Implication:
     - do not continue optimizing or redesigning the Windows path in this session until the alternate fast-path proposal is available

65. Benchmarking must use only the shipped library implementations; the deleted prototype benchmark binary should be replaced and then deleted.
   - Source: user decision "proceed. All A" for Decision 1.
   - Implication:
     - private benchmark transport implementations are no longer authoritative
     - benchmark orchestration code may remain, but transport behavior must live only in the library

66. The authoritative benchmark transport set must include only real library transports:
   - POSIX: `uds-seqpacket`, `shm-hybrid`
   - Windows: `named-pipe`
   - Source: user decision "proceed. All A" for Decision 2.
   - Implication:
     - `stream`, `dgram`, `shm-spin`, and `shm-sem` must be removed from benchmark acceptance/reporting paths

67. There must be no separate spin benchmark/product variant; keep only one SHM product path, `shm-hybrid`.
   - Source: user decision "proceed. All A" for Decision 3.
   - Implication:
     - any spin behavior that remains is only an internal implementation detail of `shm-hybrid`
     - `shm-spin` must be removed as a named benchmark transport

68. CPU accounting must stay out of the library itself; benchmark/helper executables may collect their own CPU usage internally and report it on exit, instead of depending on external scripts.
   - Source: user clarification "The library should not measure its cpu. But the implementations using the library could by theselves open proc and do the work, on exit, without relying on external scripts."
   - Implication:
     - library APIs remain transport-only
     - benchmark/live helper binaries may own Linux `/proc` sampling and process-lifecycle accounting
   - Constraint discovered during refactor:
     - script-side `/proc/<pid>` sampling is unreliable for UDS server benchmarks because the server may exit on client disconnect before the script reads `/proc`
     - helper-owned final CPU reporting requires a clean benchmark shutdown/reporting path in the helper executable

69. Benchmark result ownership after removing the deleted prototype benchmark binary: Option A.
   - Source: user decision "i agree" on the proposed option set.
   - Decision:
     - move benchmark orchestration into the helper executables themselves
     - each helper gets a benchmark command that starts its own server child, runs the client loop, measures child/self CPU, and prints one final result row
   - Implication:
     - shell scripts become thin matrix orchestrators only
     - benchmark correctness no longer depends on shell timing around server exit

70. Scope of the first benchmark-orchestrator conversion: Option A.
   - Source: user decision "i agree" on the proposed option set.
   - Decision:
     - convert all current authoritative POSIX benchmark paths together:
       - `uds-seqpacket` for C, Rust, Go
       - `shm-hybrid` for C
     - keep the same model reserved for later Windows `named-pipe` conversion
   - Implication:
     - Linux benchmark reporting becomes consistent across the active library-backed transports

71. Linux UDS benchmark coverage must include all C/Rust/Go client-server combinations in both directions, at all three benchmark rates:
   - `max`
   - `100k/s`
   - `10k/s`
   - Source: user requirement "For each of the implementations we need: max, 100k/s, 10ks. All combinations tested/benchmarked: c, rust, go interoperability in both directions".
   - Implication:
     - same-language rows alone are not enough
     - benchmark scripts must expand into a cross-language client/server matrix

72. Ping-pong benchmark correctness is mandatory: every helper benchmark must fail on any counter mismatch and verify that the increment chain remains correct through the whole run.
   - Source: user requirement "the ping-pong should be testing incrementing a counter and ensuring that the counter has been incremented properly at the end, or the test should fail."
   - Fact from current code:
     - helper clients already check `response == counter + 1` on every request in C, Go, and Rust
   - Required follow-up:
     - the benchmark scripts must treat any helper-reported mismatch as a hard failure
     - the final row must remain non-authoritative if the helper reports mismatches or inconsistent request/response counts

73. Cross-language benchmark ownership model for the full C/Rust/Go matrix: Option A.
   - Source: user decision "as you recommend".
   - Decision:
     - add helper commands for benchmark client/server roles separately
     - let a thin matrix script compose cross-language client/server pairs
     - each helper remains responsible for its own CPU measurement and correctness checks
   - Implication:
     - benchmark scripts merge helper-produced client/server rows instead of sampling processes externally

74. Remove the remaining legacy C UDS demo dependency from negotiated profile-`2` testing by teaching `netipc-live-c` the same UDS profile override knobs already exposed by the old C demo binaries.

75. Repository cleanup rule (clarified by Costa)
    - This repo is library-only in purpose, but it keeps everything related to the library: source, documentation, unit/integration/stress tests, build systems, benchmarks, and scripts that exercise/validate the library.
    - File retention criterion:
      - Keep any file that is part of library source code.
      - Keep any file that is part of library documentation.
      - Keep any file that is part of unit / stress / integration tests for the library.
      - Keep any file that is part of building / compiling / packaging the library.
      - Keep any file that is part of benchmarking the library.
      - Delete anything else.
    - Mixed files should be cleaned so that only library-related content remains.
    - Implication: validation and benchmark assets stay if they exercise the real library; obsolete prototype-only binaries, demo paths, and unrelated helper code should be removed.

75. Cleanup execution scope for the current pass
75. Cleanup results from the current pass
    - Deleted obsolete private benchmark source the deleted prototype benchmark source.
    - Deleted obsolete C demo sources:
      - the deleted SHM server demo source
      - the deleted SHM client demo source
      - the deleted UDS server demo source
      - the deleted UDS client demo source
    - Switched remaining SHM interop coverage to `netipc-live-c`, so the deleted demo files are no longer needed by tests or build targets.
    - Removed demo targets from `src/libnetdata/netipc/CMakeLists.txt`.
    - Cleaned `README.md` and `.gitignore` so they no longer advertise obsolete demo/prototype artifacts.
    - Full Linux validation after cleanup passed:
      - `./tests/run-interop.sh`
      - `./tests/run-live-interop.sh`
      - `./tests/run-live-uds-interop.sh`
      - `./tests/run-uds-seqpacket.sh`
      - `./tests/run-uds-negotiation-negative.sh`
      - `./tests/run-live-shm-bench.sh`
      - `./tests/run-live-uds-bench.sh`
      - `./tests/run-negotiated-profile-bench.sh`

    - Delete obsolete private benchmark implementation the deleted prototype benchmark source and stop documenting it.
    - Delete obsolete C demo binaries/sources once their remaining SHM interop use is switched to `netipc-live-c`.
    - Keep fixture crates/modules and helper binaries that exercise the real library (`tests/fixtures/*`, `bench/drivers/go`, `bench/drivers/rust`).
    - Keep benchmark and validation scripts in `tests/` because they benchmark/test the library.
    - Keep `Makefile` because it is a build convenience wrapper around CMake and therefore library-build related.
    - Remove generated artifacts and stale local binaries from the working tree (`build/`, Rust `target/` dirs, generated Go helper binary).

75. Repository cleanup request: remove everything not related to the library we are building.
   - Source: user request "cleanup everything that is not related to the library we are building."
   - User clarification:
     - delete anything not related to the library: sources, binaries, scripts, documentation, anything
     - validation harnesses, helper binaries, benchmark drivers, and unrelated docs should not stay just because they were convenient during prototyping
   - Current status:
     - taking this literally means converting the repo from `library + validation` into `library-only`
     - active validation currently still depends on helper binaries, fixtures, and benchmark/test scripts
   - Pending decision:
     - whether any repo-local validation/documentation is still considered part of the deliverable, or whether the repo should contain only the publishable library packages and their native package/build metadata
   - Source: user approval "yes, please do this right" after the remaining-cleanup note.
   - Decision:
     - extend `netipc-live-c` UDS commands to accept optional `supported_profiles`, `preferred_profiles`, and `auth_token` overrides
     - preserve the old positional convention for override values, including the legacy one-shot `iterations=1` placeholder used by the old demo binaries
   - Implication:
     - negotiated C<->Rust SHM tests and UDS negative tests can move fully onto the live helper path
     - the old C UDS demo binaries stop being required for active validation

## Benchmark Refactor Status (2026-03-09)
- Completed:
  - removed the private C benchmark transport source the deleted prototype benchmark source
  - removed the the deleted prototype benchmark binary build target from the CMake build graph
  - added a thin POSIX C live fixture at `tests/fixtures/c/netipc_live_posix_c.c`
    - `uds-server-once`
    - `uds-client-once`
    - `uds-server-loop`
    - `uds-client-bench`
    - `shm-server-once`
    - `shm-client-once`
    - `shm-server-loop`
    - `shm-client-bench`
  - switched `tests/run-live-uds-bench.sh` to benchmark C UDS through `build/bin/netipc-live-c` instead of the deleted prototype benchmark binary
  - updated `README.md` so the repository no longer advertises the deleted prototype benchmark binary as a primary artifact or benchmark path
  - moved authoritative benchmark ownership into the helper executables:
    - C helper now exposes `uds-bench` and `shm-bench`
    - Go helper now exposes `uds-bench`
    - Rust helper now exposes `uds-bench`
  - benchmark helper executables now own server lifecycle and CPU reporting
  - benchmark shell scripts no longer sample `/proc` or manage benchmark server PIDs directly
- Linux validation run after the refactor:
  - `cmake --build build`
  - `./tests/run-live-uds-bench.sh`
  - manual smoke: `build/bin/netipc-live-c shm-server-once` + `build/bin/netipc-live-c shm-client-once`
  - `./tests/run-live-interop.sh`
  - `./tests/run-live-shm-bench.sh`
- Current outcome:
  - authoritative C Linux UDS benchmark data now comes from the public library API path
  - the fake standalone transport benchmark path is gone
  - `shm-spin` no longer exists as a benchmark transport path in the repository
  - authoritative Linux benchmark CPU reporting now comes from the helper executables, not the library and not the shell harness
  - `tests/run-live-uds-bench.sh` now runs the full Linux UDS directed `C/Rust/Go` matrix (`9` client/server pairs) at `max`, `100k/s`, and `10k/s`
  - `tests/run-live-uds-interop.sh` now runs the full directed baseline UDS profile-`1` matrix and keeps the negotiated C<->Rust profile-`2` SHM cases
  - helper benchmarks now fail hard on:
    - any non-OK response status
    - any `response != request + 1` mismatch
    - any `requests != responses` mismatch
    - any final counter-chain mismatch
    - any server handled-count mismatch versus client responses
  - `netipc-live-c` now exposes optional UDS `supported_profiles`, `preferred_profiles`, and `auth_token` overrides for:
    - one-shot commands
    - loop commands
    - bench commands
  - `netipc-live-c` now exposes a repeated-call UDS client mode so the basic seqpacket smoke test can also stay on the live helper path
  - no UDS test script under `tests/*.sh` depends on the old C UDS demo binaries anymore

## Latest Authoritative Benchmark Note (2026-03-09)
- Historical the deleted prototype benchmark binary notes elsewhere in this TODO are prototype history only.
- They are no longer authoritative for acceptance after Decisions `65` through `73`.
- Current authoritative Linux acceptance data must come from:
  - `./tests/run-live-uds-bench.sh`
  - `./tests/run-live-shm-bench.sh`
  - helper binaries that call only the shipped library code paths

## Latest Validation Snapshot (2026-03-09)
- Passed:
  - `./tests/run-uds-seqpacket.sh`
  - `./tests/run-live-uds-bench.sh`
  - `./tests/run-live-uds-interop.sh`
  - `./tests/run-live-shm-bench.sh`
  - `./tests/run-live-interop.sh`
  - `./tests/run-uds-negotiation-negative.sh`
- Linux UDS benchmark matrix (`27` rows, all helper/library-backed, all strict-correctness checked):
  - `max`
    - `c -> c`: ~206.8k req/s, p50 ~4.34us, total CPU ~1.016
    - `c -> rust`: ~220.7k req/s, p50 ~4.02us, total CPU ~0.986
    - `c -> go`: ~198.6k req/s, p50 ~4.54us, total CPU ~1.410
    - `rust -> c`: ~217.1k req/s, p50 ~4.14us, total CPU ~0.996
    - `rust -> rust`: ~236.6k req/s, p50 ~3.81us, total CPU ~0.975
    - `rust -> go`: ~232.0k req/s, p50 ~3.63us, total CPU ~1.450
    - `go -> c`: ~199.9k req/s, p50 ~4.46us, total CPU ~1.392
    - `go -> rust`: ~222.0k req/s, p50 ~4.18us, total CPU ~1.433
    - `go -> go`: ~200.4k req/s, p50 ~4.58us, total CPU ~1.907
  - `100k/s`
    - all `9` directed pairs held ~100k req/s with strict increment validation
    - lowest total CPU in this run: `rust -> rust` at ~0.448 cores
    - highest total CPU in this run: `c -> go` at ~1.271 cores
  - `10k/s`
    - all `9` directed pairs held ~10k req/s with strict increment validation
    - lowest total CPU in this run: `rust -> rust` at ~0.066 cores
    - highest total CPU in this run: `c -> go` at ~1.042 cores
- Linux SHM benchmark (`C shm-hybrid`, helper/library-backed):
  - `max`: ~2.95M req/s, p50 ~0.31us, total CPU ~1.982
  - `100k/s`: ~100.0k req/s, p50 ~3.33us, total CPU ~1.145
  - `10k/s`: ~10.0k req/s, p50 ~3.98us, total CPU ~0.996

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
  - C library now also contains a Windows negotiated `SHM_BUSYWAIT` fast profile backed by shared memory plus pure busy-spin for the single-client low-latency case.
  - Windows fast-path design is intentionally limited to Win32 primitives that can be ported to Rust and pure Go without `cgo`.
- Validated:
  - schema interop: `C <-> Rust <-> Go`
  - live SHM interop: `C <-> Rust`
  - live UDS interop: `C <-> Rust <-> Go`
  - Windows C Named Pipe smoke under MSYS2 `mingw64`: `./tests/run-live-npipe-smoke.sh`
  - Windows C profile comparison under MSYS2 `mingw64`: `./tests/run-live-win-profile-bench.sh`
    - latest local result on `win11`, 5s, 1 client:
      - `c-npipe`: ~15.2k req/s, p50 ~49.0us
      - `c-shm-hybrid` (default spin `1024`): ~84.8k req/s, p50 ~3.7us
      - `c-shm-busywait`: ~84.5k req/s, p50 ~3.7us, lower p99 tail than `SHM_HYBRID`
  - UDS negative negotiation coverage
  - UDS and negotiated-profile benchmark scripts
  - `cargo test -p netipc`
  - `go test ./...` under `src/go`
- Still incomplete:
  - Go package currently implements the reusable POSIX `UDS_SEQPACKET` path only.
  - Go negative-test helper logic for malformed/raw negotiation frames remains local to `bench/drivers/go`; this is fixture-specific coverage, not reusable API.
  - Rust and Go Windows transports remain placeholders.
  - Windows validation is still limited to the C Named Pipe/`SHM_HYBRID`/`SHM_BUSYWAIT` path; cross-language Windows interop and benchmark coverage are still pending.
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
   - the deleted prototype benchmark source
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
  - top-level binaries: `netipc-codec-c`, demo binaries, `libnetipc.a`, and a prototype benchmark executable
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
- Done: Phase 1 POSIX C benchmark harness implemented (the deleted prototype benchmark binary).
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
  - the deleted SHM server demo binary (the deleted SHM server demo source)
  - the deleted SHM client demo binary (the deleted SHM client demo source)
- Done: Cross-language schema interop tools added:
  - Rust codec tool (`interop/rust`)
  - Go codec tool (`interop/go`)
- Done: Automated interop validation script added:
  - `tests/run-interop.sh`
  - Validates C->Rust->C, Rust->Go->Rust, Go->C->Go for typed `increment` schema frames.
- Done: Build system updated (`Makefile`) to produce:
  - `libnetipc.a`
  - `netipc-codec-c`
  - the deleted SHM server demo binary
  - the deleted SHM client demo binary
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
  - the deleted UDS server demo binary (the deleted UDS server demo source)
  - the deleted UDS client demo binary (the deleted UDS client demo source)
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
  - the deleted UDS server demo source: optional `supported_profiles`, `preferred_profiles`, `auth_token`.
  - the deleted UDS client demo source: optional `supported_profiles`, `preferred_profiles`, `auth_token`.
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
  - the deleted prototype benchmark source (`sleep_until_ns`, fixed-rate schedule progression)
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
  - Server: `./the deleted SHM server demo binary /tmp netipc-demo 2`
  - Client: `./the deleted SHM client demo binary /tmp netipc-demo 50 2`
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
  - `./the deleted prototype benchmark binary --transport seqpacket --mode pingpong --clients 1 --payloads 32 --duration 5`
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

## Benchmark Request (2026-03-09)
- User request: run the Linux benchmark for single-client ping-pong performance with no pipelining.
- Scope: execute the existing Linux benchmark path and report the measured results.

## Design Correction (2026-03-09)
- User concern: only one implementation should exist: the library.
- User decision direction: no spin variant.
- User concern: benchmark results are not trustworthy when they measure imaginary/private benchmark transports instead of the reusable library.
- Required outcome: propose how to restructure benchmarking so only library code is measured.

76. Remove all in-tree traces of deleted prototype artifacts
    - Remove remaining references in documentation, TODO/history notes, comments, scripts, and build files to deleted prototype-only artifacts.
    - Scope is the current working tree only; git history is not being rewritten in this pass.
    - Deleted artifact names should disappear from the tree entirely and be replaced with generic wording where historical context must remain.


76. Trace-removal result
    - Removed the deleted artifact names and deleted source paths from active build files, documentation, and TODO/history notes.
    - Updated `Makefile` cleanup to target current generated outputs instead of deleted prototype artifact names.
    - Verified with repo-wide search that the deleted artifact names no longer appear in the working tree.
    - Renamed the Rust benchmark-driver package to remove the residual deleted benchmark substring from package metadata.
    - Verified `make clean` still passes after the cleanup.


77. Build system requirement: the repository must be CMake-based to match Netdata integration.
    - Source: user decision "The build system must be based on cmake, because Netdata uses cmake and it will do the integration easier."
    - Requirement:
      - CMake must remain the top-level and authoritative repository build/test orchestration entrypoint.
      - The repository layout and targets should stay easy to embed into Netdata's CMake build.
    - Fact:
      - Rust crates still require `Cargo.toml`.
      - Go packages/modules still require `go.mod`.
    - Implication:
      - `Cargo.toml` and `go.mod` remain native package metadata, but CMake should drive the repo-level build, validation, and benchmark targets.
      - Avoid introducing parallel non-CMake top-level build systems for repository orchestration.


77. CMake-first execution scope for the current pass
    - Gap found:
      - top-level CMake builds library/helper binaries, but it does not own validation or benchmark workflows yet.
      - shell scripts currently self-configure/self-build, so they are runnable manually but not registered as first-class CMake workflows.
      - generated Rust/Go outputs outside the build directory are currently cleaned only by the wrapper `Makefile`, not by a CMake target.
    - Plan for this pass:
      - register library validation scripts as CMake/CTest workflows.
      - register benchmark scripts as explicit CMake targets.
      - let scripts respect a CMake-driven mode so they can run without recursive configure/build when launched from CMake.
      - add a CMake cleanup target for generated non-build-dir artifacts.
      - update documentation so CMake is the authoritative repo-level interface.


77. CMake-first result
    - Added CMake workflow targets for validation and benchmarks:
      - `netipc-check`
      - `netipc-bench`
      - `netipc-validate-all`
      - `netipc-clean-generated`
    - Registered validation scripts with CTest via CMake build targets.
    - Updated validation/benchmark scripts to support a CMake-driven mode using:
      - `NETIPC_CMAKE_BUILD_DIR`
      - `NETIPC_SKIP_CONFIGURE`
      - `NETIPC_SKIP_BUILD`
    - Validation through CMake passed:
      - `cmake -S . -B build`
      - `cmake --build build --target test`
      - `cmake --build build --target netipc-bench`
      - `cmake --build build --target netipc-clean-generated`
    - Environment note:
      - the plain `ctest` command on this workstation resolves to a broken Python shim in `~/.local/bin/ctest`.
      - `cmake --build build --target test` is therefore the reliable documented path here.


78. Push decision for the cleanup + CMake-first pass
    - Source: user approval "yes push"
    - Decision:
      - push commit `e57e74d` on `main` to `origin`


78. Push blocker after approval
    - Fact:
      - `git push origin main` was rejected with `fetch first` because `origin/main` contains commits not present locally.
    - Implication:
      - local `main` cannot be pushed safely without first integrating the remote changes.


79. Inspect the current `origin/main` in a separate clone under `/tmp`
    - Source: user direction "clone the main to /tmp/ and check it."
    - Goal:
      - inspect remote commit `3d56710` (`Add Windows SHM_HYBRID fast profile`) without modifying the current working tree.


80. Integrate local cleanup/CMake pass on top of current `origin/main`
    - Source: user direction "do it" after inspecting the remote `main` clone.
    - Decision:
      - rebase the local cleanup/CMake commit on top of `origin/main`
      - keep the remote Windows `SHM_HYBRID` fast-profile work
      - resolve overlap in `README.md`, `TODO-plugin-ipc.md`, `src/libnetdata/netipc/CMakeLists.txt`, and `tests/run-live-npipe-smoke.sh`


80. Rebase result
    - Rebase of the local cleanup/CMake commit onto `origin/main` completed cleanly.
    - New local commit id after rebase:
      - `810d8eb`
    - Validation on the rebased tree passed through CMake:
      - `cmake -S . -B build`
      - `cmake --build build --target test`
      - `cmake --build build --target netipc-bench`
      - `cmake --build build --target netipc-clean-generated`


81. Automated benchmark document generation requirement
    - Source: user requirement to automatically generate `benchmarks-posix.md` and `benchmarks-windows.md` from benchmark runs.
    - Requirements:
      - benchmark documents must be regenerated automatically from benchmark execution results.
      - the committed benchmark documents must never contain partial results.
      - generation must be buffered/staged so replacement happens only after a complete successful run.
    - Design constraint:
      - POSIX and Windows benchmark matrices are different, so completeness must be validated separately for each document.
      - cross-platform generation cannot rely on a single machine unless that machine can run both matrices.


81. Automated benchmark document generation decisions
    - Source: user approval "I agree. Do it"
    - Decisions:
      - Source of truth: staged machine-readable benchmark result files, then generate Markdown from them.
      - Trigger model: two explicit scripts, one POSIX and one Windows, independent of CMake orchestration.
    - Implementation plan:
      - benchmark scripts emit machine-readable results when requested.
      - a generator validates completeness for POSIX and Windows independently.
      - Markdown files are rendered to temporary paths and atomically renamed only on full success.
      - generation is owned by two explicit scripts, not by CMake targets.


81. Benchmark-doc trigger clarification
    - Source: user clarification "this does not need to be cmake driven" and "2 scripts: windows, linux"
    - Decision:
      - keep building CMake-based
      - implement benchmark document regeneration as two explicit scripts:
        - one script for POSIX
        - one script for Windows
      - those scripts call the existing benchmark entrypoints and regenerate the markdown docs atomically
    - Implementation status:
      - `tests/generate-benchmarks-posix.sh` implemented
      - `tests/generate-benchmarks-windows.sh` implemented
      - benchmark runner scripts now optionally emit staged machine-readable CSV output via `NETIPC_RESULTS_FILE`
      - `benchmarks-posix.md` is generated from complete staged benchmark runs only
    - Validation:
      - `bash -n tests/generate-benchmarks-posix.sh tests/generate-benchmarks-windows.sh tests/run-live-uds-bench.sh tests/run-live-shm-bench.sh tests/run-negotiated-profile-bench.sh tests/run-live-win-profile-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 ./tests/generate-benchmarks-posix.sh`
    - Notes:
      - the first POSIX generator run failed before publication because of two implementation bugs; no markdown file was created, confirming the buffered replacement behavior
      - `benchmarks-windows.md` is not generated yet in this Linux session; the Windows generator script is implemented and syntax-checked, but runtime validation still needs a Windows run

82. Direct Rust SHM benchmark coverage gap
    - Source: user request "Fix it." after identifying that `benchmarks-posix.md` lacks direct Rust `shm-hybrid` rows.
    - Facts:
      - the Rust library already implements direct POSIX `SHM_HYBRID` transport in `src/crates/netipc/src/transport/posix.rs`.
      - the current Rust benchmark helper only exposes UDS commands, so `tests/run-live-shm-bench.sh` can benchmark only C direct SHM today.
      - `tests/run-negotiated-profile-bench.sh` measures Rust SHM only through negotiated UDS profile switching, not direct SHM helper commands.
    - Requirement:
      - add direct Rust `shm-hybrid` helper commands and include them in the authoritative POSIX SHM benchmark matrix and generated markdown.
    - Plan:
      - inspect the existing Rust `ShmServer` / `ShmClient` library API and current C helper behavior.
      - implement direct Rust SHM helper commands as thin wrappers over the library.
      - extend the POSIX SHM benchmark script and markdown generator to include Rust rows and validate completeness.
      - rerun the POSIX generator and update docs.

82. Direct Rust SHM interop requirement
    - Source: user clarification "It should also have c->rust/rust->c tests".
    - Requirement:
      - direct POSIX `shm-hybrid` coverage must include `c->rust` and `rust->c` validation, not only same-language Rust helper coverage.
    - Plan extension:
      - inspect the current SHM interop script and replace or extend it to use library-backed C and Rust SHM helpers in both directions.
      - keep the authoritative POSIX benchmark markdown focused on benchmark rows, but make sure the direct SHM validation matrix includes cross-language C/Rust directions.

83. Investigate high SHM client CPU at 10k/s
    - Source: user request to explain why `shm-hybrid` shows high client CPU usage at `10k/s` and whether `spin_tries` is set to `20`.
    - Requirement:
      - after fixing direct Rust SHM benchmark coverage, inspect the actual `spin_tries` defaults and the benchmark helper pacing/client loop behavior for `shm-hybrid`.
      - provide an evidence-based explanation, separating facts from any speculation.
    - Implementation status:
      - `tests/fixtures/rust/src/bin/netipc_live_rs.rs` now exposes direct SHM helper roles: `server-once`, `client-once`, `server-loop`, `server-bench`, `client-bench`.
      - `tests/run-live-shm-bench.sh` now runs the authoritative direct POSIX `SHM_HYBRID` matrix for `C/Rust` in all directed pairs.
      - `tests/generate-benchmarks-posix.sh` now validates and renders the SHM section as a `C/Rust` directed matrix instead of a single C-only row.
      - `CMakeLists.txt` now declares `netipc_live_rs` as a dependency of the SHM benchmark workflow.
    - Validation:
      - `bash -n tests/run-live-shm-bench.sh tests/generate-benchmarks-posix.sh tests/run-live-uds-bench.sh`
      - `cargo build --release --manifest-path tests/fixtures/rust/Cargo.toml`
      - `env NETIPC_SKIP_CONFIGURE=1 ./tests/run-live-interop.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 NETIPC_SKIP_BUILD=1 ./tests/run-live-shm-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 ./tests/generate-benchmarks-posix.sh`
    - Important fact:
      - direct SHM `c->rust` and `rust->c` validation already existed in `tests/run-live-interop.sh`; this task fixed the benchmark/helper coverage gap, not a missing interop test path.

83. SHM client CPU investigation findings
    - Facts:
      - the library default spin window is `20` in both C and Rust:
        - `src/libnetdata/netipc/include/netipc/netipc_shm_hybrid.h`: `NETIPC_SHM_DEFAULT_SPIN_TRIES 20u`
        - `src/crates/netipc/src/transport/posix.rs`: `SHM_DEFAULT_SPIN_TRIES: u32 = 20`
      - the C SHM helper uses that default in `tests/fixtures/c/netipc_live_posix_c.c` via `shm_config(...).spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES`.
      - the C benchmark pacing loop uses `sleep_until_ns()`, which intentionally busy-waits / yields close to the send deadline.
      - new direct SHM benchmark evidence shows a strong asymmetry at `10k/s`:
        - `c -> c`: client CPU ~`0.977`, server CPU ~`0.031`
        - `c -> rust`: client CPU ~`0.978`, server CPU ~`0.029`
        - `rust -> c`: client CPU ~`0.039`, server CPU ~`0.034`
        - `rust -> rust`: client CPU ~`0.037`, server CPU ~`0.028`
    - Conclusion:
      - the high `10k/s` client CPU is not explained by `spin_tries = 20` alone.
      - the dominant cause is the C helper's rate-pacing strategy in the benchmark client, which stays active around each scheduled send time.
      - evidence: when the Rust helper drives the same direct SHM transport at `10k/s`, client CPU drops by about `25x` while using the same library transport semantics.

84. Adaptive client pacing for target-rate benchmarks
    - Source: user request "fix the client to use adaptive sleep based on the remaining work. So, make it measure its speed and adapt the sleep interval to achieve the goal rate."
    - Requirement:
      - benchmark clients should not burn CPU by busy-waiting near every send deadline.
      - pacing should adapt based on observed progress versus target throughput.
      - this is benchmark-helper behavior only; it must not change library transport semantics.
    - Working assumption:
      - apply the pacing fix to all benchmark helper clients that implement `target_rps`, so comparisons stay fair across languages.
      - if evidence later shows only the C helper should change, narrow the scope explicitly.
    - Plan:
      - inspect the current pacing loops in C, Rust, and Go benchmark helpers.
      - replace deadline-chasing loops with adaptive sleep based on target progress and backlog.
      - rerun benchmark validation and compare the low-rate CPU numbers.
      - then commit and push the full SHM benchmark fix plus pacing change.

84. Adaptive pacing validation uncovered staged negotiated-profile output bug
    - Facts:
      - `tests/generate-benchmarks-posix.sh` completed the staged UDS and SHM matrix runs successfully, then failed during the negotiated-profile stage.
      - failure mode: `tests/run-negotiated-profile-bench.sh` exited without producing `negotiated.csv`, so the generator aborted without updating `benchmarks-posix.md`.
      - this confirms the buffered publication rule is working as intended: partial benchmark documents are not written.
    - Requirement extension:
      - fix the negotiated-profile benchmark script so it emits staged results consistently when `NETIPC_RESULTS_FILE` is set, then rerun the generator before commit/push.

84. Adaptive pacing validation results
    - Validation:
      - `cmake -S . -B build`
      - `cmake --build build --target netipc-live-c netipc_live_rs netipc_live_uds_rs netipc-live-go`
      - `bash -n tests/generate-benchmarks-posix.sh tests/generate-benchmarks-windows.sh tests/run-live-uds-bench.sh tests/run-live-shm-bench.sh tests/run-negotiated-profile-bench.sh tests/run-live-win-profile-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 NETIPC_SKIP_BUILD=1 ./tests/run-live-shm-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 NETIPC_SKIP_BUILD=1 ./tests/run-live-uds-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 ./tests/generate-benchmarks-posix.sh`
    - Result:
      - the adaptive pacing change removed the low-rate client CPU artifact across the POSIX benchmark helpers.
      - SHM direct `10k/s` client CPU changed from about `0.977-0.978` in the C-driven rows to about `0.034-0.037` after the pacing fix.
      - UDS direct `10k/s` C-client rows are now about `0.038-0.044`, instead of the previous busy-wait behavior around one full core.
    - Additional fixes completed as part of validation:
      - `tests/generate-benchmarks-posix.sh` and `tests/generate-benchmarks-windows.sh` now preserve the real child exit status in their `run()` wrappers.
      - `tests/run-negotiated-profile-bench.sh` now uses a `12s` server timeout instead of `7s`, to avoid staged benchmark flakiness near the `5s` benchmark window.
      - `benchmarks-posix.md` was regenerated successfully from a complete staged run after these fixes.

85. Validate benchmark trustworthiness and counter correctness
    - Source: user concern that the benchmark results may not be trustworthy, specifically whether ping-pong correctness is verified by the final counting, and why direct `rust -> rust` SHM is about `5M req/s` while `c -> c` is about `3.2M req/s`.
    - Requirement:
      - inspect the active benchmark helpers and scripts to verify exactly how request/response correctness is enforced during ping-pong benchmarks.
      - determine whether the current benchmark validation guarantees strict counter-chain correctness or only aggregate counts.
      - inspect the direct SHM C and Rust helper/library paths to explain the large throughput gap, separating facts from working theory.
    - Plan:
      - inspect the C and Rust direct SHM benchmark client/server loops and the benchmark scripts.
      - confirm whether each round-trip validates `response == request + 1`, whether mismatches hard-fail, and whether final handled/request counts are cross-checked.
      - inspect the direct SHM transport/library code and helper hot paths for asymmetries that can explain the result gap.
      - respond with an evidence-based trust assessment and, if needed, propose concrete fixes.

86. Default CMake build type
    - Source: user question "can we make Release the default?"
    - Facts to verify:
      - the repo currently configures CMake with no default `CMAKE_BUILD_TYPE`, so single-config generators build C targets without optimization unless the user sets a build type explicitly.
      - Rust helper builds already use `cargo build --release`, so the current benchmark/build defaults are inconsistent across languages.
    - Requirement:
      - inspect the current root CMake behavior and determine the correct way to default to `Release` without overriding explicit user choices or breaking multi-config generators.
    - Plan:
      - inspect the root `CMakeLists.txt` and current build cache evidence.
      - present the safe options, implications, and recommendation before changing code.

86. Default CMake build type decision
    - User decision: use `RelWithDebInfo` as the default build type for single-config CMake generators.
    - Reasoning:
      - this keeps the repo optimized by default, avoiding the current unfair unoptimized C benchmark baseline.
      - it also keeps debug symbols, which is more practical for crash analysis and transport debugging than pure `Release`.
    - Implementation scope:
      - set the default only when `CMAKE_CONFIGURATION_TYPES` is not used and `CMAKE_BUILD_TYPE` was not provided explicitly.
      - reconfigure the local build, verify the active C flags, rerun validation, and regenerate `benchmarks-posix.md`.

86. Default CMake build type implementation
    - Implementation:
      - root `CMakeLists.txt` now defaults single-config generators to `RelWithDebInfo` when the user did not set `CMAKE_BUILD_TYPE` explicitly.
      - `README.md` now documents the default and how to override it with `-DCMAKE_BUILD_TYPE=Release`.
    - Next validation:
      - reconfigure the existing build directory to update the cached build type.
      - verify that active C flags now include the `RelWithDebInfo` optimization/debug flags.
      - rerun tests and regenerate `benchmarks-posix.md` under the new default.
