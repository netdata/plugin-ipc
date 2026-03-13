# TODO-plugin-ipc

## Purpose
- Switch the local repository to `origin/windows-transports-rust-go` without risking unrelated work.
- Open a GitHub pull request for the Windows transport branch against `main`.
- Keep the clean PR head branch in sync with the latest commits on `windows-transports-rust-go`.
- Pull the repository after the PR merge and move the local worktree back to `main`.
- Perform a production-readiness review of the full library across Linux and Windows, for C, Rust, and Go, and decide whether it is ready to merge into Netdata.
- Design the Windows CI wiring needed to validate this library automatically on GitHub Actions before calling it production-ready for Netdata.

## Session Handoff (2026-03-11)
- User decision (2026-03-11, after remediation):
  - commit the current fixes and push them
  - use `ssh win11` for real Windows validation before finalizing the commit
  - use `/c/Users/costa/src/plugin-ipc-win.git` on `win11` for validation
  - reset that Windows test clone before applying the current patch and running tests
  - next step is to wire the same Windows validation into CI
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
  - Fact: after the MSYS/native transition changes and the argument-conversion fix, `bash tests/smoke-win.sh` passed on `win11` with `32 passed, 0 failed`.
  - Fact: the smoke matrix covered the full directed interoperability set across:
    - `c-native`
    - `c-msys`
    - `rust-native`
    - `go-native`
    - under both Windows transport profiles.
  - Fact: `bash tests/run-live-win-bench.sh` completed successfully on `win11` and produced a full 64-row directed benchmark matrix in `/tmp/bench_results_4577.txt`.
  - Fact: benchmark results confirm that `c-msys` stays in the same performance class as `c-native` on the Windows transport.
  - Fact: benchmark results also expose a real client-side asymmetry:
    - C and Rust clients drive SHM HYBRID at about 3.1M to 3.4M rps against C/Rust servers.
    - Go clients top out closer to about 1.1M to 1.7M rps depending on the server implementation.
  - Fact: a Windows-specific SHM spin sweep was executed on `win11` using `rust-native -> rust-native` first, then a representative transition shortlist.
  - Fact: the broad Rust-only sweep over `4, 8, 16, 32, 64, 128, 256, 512, 1024` showed that Windows is not in the same regime as Linux:
    - throughput stayed in named-pipe-class territory up to `64`
    - `128` and `256` improved but were still far from the top-end SHM behavior
    - `512` was the first value that restored full-rate SHM behavior for both max-rate and `100k/s`
    - `1024` added CPU with essentially no throughput gain over `512`
  - Fact: the refined Rust-only sweep around the knee (`320, 384, 448, 512, 640, 768`) showed high Hyper-V noise but still narrowed the useful window to about `384` to `640`.
  - Fact: representative transition validation was then run for spins `384`, `448`, `512`, and `640` on:
    - `rust-native -> rust-native`
    - `c-msys -> rust-native`
    - `rust-native -> c-msys`
    - `rust-native -> go-native`
  - Fact: the representative results did not reveal a single clean Linux-style knee:
    - lower spins such as `384` often reduced CPU, but produced much worse `100k/s` p99 tails on transition pairs
    - higher spins such as `640` improved several max-rate runs, but also raised CPU and were not consistently better across all pairs
    - `512` emerged as the most defensible current default candidate because it restored full-rate SHM behavior broadly, avoided the obvious low-spin collapse, and did not materially underperform `640`

## Current Follow-up Task (2026-03-11)
- Goal: define how to wire Windows validation into CI for this repository.
- Need:
  - inspect existing workflow patterns in this repo
  - verify current official GitHub Actions guidance for Windows + MSYS2 + artifacts/caching
  - recommend a CI structure that matches the validated local/`win11` workflow with low maintenance risk
  - carry forward the full transition matrix requirement rather than collapsing back to same-language-only checks
  - determine the Windows SHM HYBRID spin sweet spot instead of assuming the Linux value applies
  - compare maximum-throughput gain versus CPU/tail-latency cost on `win11`
  - evaluate whether CI can identify and report Linux and Windows SHM spin sweet spots on production-like VMs
  - inspect the existing Netdata agent `ebpf <-> cgroups` plugin-to-plugin communication path as a concrete reference before deciding how much of `plugin-ipc` should mirror established Netdata practice
- User clarification (2026-03-11):
  - the transition requirement is not a POSIX transport on Windows
  - the C library must compile under the MSYS runtime while still using the native Windows IPC transport
  - that MSYS-built C variant must interoperate with native Windows Rust and Go binaries
  - `benchmark-windows.md` must include both `c-native` and `c-msys`
  - benchmark scope should cover all directed implementation pairs, with each timed run capped at 5 seconds
  - Linux already converged on `20` spins as the best throughput-inflection point after testing powers of two and then refining near `16`
  - Windows needs its own sweep because its current SHM HYBRID default is much higher than Linux
  - question to answer now: can CI reliably identify the Linux and Windows sweet spots on real production VMs instead of only local test hosts
  - latest Linux decision: use a safer higher POSIX SHM default of `128` spins even if it increases CPU, because the priority is to avoid under-spinning on production VMs
  - new server-model proposal under discussion:
    - callers should be able to integrate client and server sockets/handles into their own event loops
    - servers must be multi-client; single-client server objects are a blocker
    - the library must also support a managed server mode where the caller registers per-request-type callbacks and asks the library to run a worker pool and own request servicing until explicit shutdown
    - low-level integration surface should expose native wait objects:
      - POSIX: `fd`
      - Windows: `HANDLE`

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
- Fact (2026-03-12): Netdata already has an ad-hoc plugin-to-plugin communication path between `cgroups.plugin` and `ebpf.plugin`.
- Evidence:
  - Shared-memory layout and names are defined in [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h).
  - Producer initializes and populates the shared memory in [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c).
  - Consumer opens, maps, and reads it in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c).
- Fact: this existing path is not a generic RPC protocol.
- Fact: it is Linux/POSIX-only shared memory plus a named semaphore, with a fixed shared struct layout and direct in-process parsing by the consumer.
- Fact: `ebpf.plugin` keeps a local cache from this shared state and uses it widely across multiple modules, so this is effectively a periodically refreshed snapshot feed, not a request-per-lookup interface.
- Evidence:
  - the integration thread periodically maps or parses the shared memory in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c).
  - downstream eBPF modules consume the cached data via `ebpf_cgroup_pids`, for example in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_process.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_process.c).
- Implication: `plugin-ipc` could replace it functionally, but not as a naive 1:1 transport swap. The natural `plugin-ipc` replacement would be a snapshot/batch service with client-side caching, or a future shared-memory backend, not a per-item synchronous lookup in the hot path.
- Implication: it proves the use case and some lifecycle patterns, but it is not a reusable cross-language/cross-OS service bus.
- Fact: Repository path `/home/costa/src/ipc-test` is currently empty (no source files, no tests, no existing TODO file).
- Fact: There is no pre-existing IPC implementation in this repo to extend.
- Fact: There is no existing build layout yet (no CMake/Cargo/go.mod).
- Fact: Host for first benchmark pass is Linux x86_64 (Manjaro, kernel 6.18.12).
- Fact: Toolchain availability confirmed: gcc 15.2.1, rustc 1.91.1, go 1.25.7.
- Fact: External performance tools available: `pidstat`, `mpstat`, `perf`.
- Implication: We should treat this as a greenfield design + benchmark project.
- Risk: Without early benchmark data, choosing protocol/serialization prematurely may lock in suboptimal latency/CPU costs.

## Decisions

### Active Status
- No active user-facing design decisions are currently blocking the protocol/API rewrite.
- The authoritative design is now captured in:
  - `Derived Protocol Sketch`
  - `Derived Public API Sketch`
  - `Derived Method Example: ip_to_asn`
  - `Derived Helper Naming / Builder Surface`
  - `Current Phase Plan: Protocol/API Rewrite`

### Deferred Historical Decisions
- 1-7. Earlier Windows CI / MSYS validation / spin-tuning questions are deferred until after the protocol/API rewrite lands on the baseline transports.
  - Reason:
    - the active rewrite changes the wire envelope, public API shape, method codecs, and batch model
    - wiring CI or final performance policy before that would be premature
- 8-14. These are no longer open user questions.
  - They are resolved by Decisions #44-#56 and by the derived protocol/API sections below.

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

24. Windows CI scope is not ready to proceed with native-only validation.
   - Source: user decision "The MSYS2 (CYGWIN) path must be validated before continuing".
   - Intent: require validation of the POSIX-emulation path on Windows before finalizing CI coverage.

25. The C implementation must also build for the MSYS2 POSIX runtime target and be included in the CI matrix.
   - Source: user decision "the C version must be built for MSYS2 (CYGWIN) target too and it must be added to the matrix."
   - Intent: validate interoperability expectations across native Rust/Go and the POSIX-emulated C build on Windows.

26. Transitional Windows support model:
   - Source: user clarification on 2026-03-11.
   - Fact pattern to validate:
     - the C library must compile as native Windows code under the MSYS runtime, linked with `msys.dll` / POSIX emulation, because this matches how Netdata currently runs on Windows
     - the Rust and Go implementations must remain native Windows binaries without POSIX emulation
     - during the transition to fully native Netdata on Windows, the MSYS-built C implementation must interoperate with the native Windows Rust and Go implementations
   - Implication: the required Windows transition matrix is not "MSYS C talking to POSIX transports"; it is "MSYS-built C using the Windows transport, interoperating with native Rust/Go over the same Windows IPC mechanisms"

27. Windows benchmark reporting scope:
   - Source: user decision on 2026-03-11.
   - Requirement: `benchmark-windows.md` must include results for both `c-native` and `c-msys`.
   - Implication: benchmark tooling and documentation must distinguish the two C runtime environments explicitly instead of reporting a single generic `c` row.

28. Execution workflow for the transition work:
   - Source: user decision on 2026-03-11.
   - Requirement: implement locally in this repo, push the branch, then use `ssh win11` to pull, compile, and run the Windows validation there.

29. Typed-library requirement for all languages:
   - Source: user clarification on 2026-03-11.
   - Requirement:
     - clients and servers must use strongly typed request/response structures in C, Rust, and Go
     - the library, not the caller, must translate between typed structures and the wire frame
     - every new message type therefore requires explicit request and response converters in all supported language implementations
   - Implication: adding a new RPC is a schema + codec change in C, Rust, and Go, not just a transport change.

30. Cross-platform discovery identity:
   - Source: user decision on 2026-03-12.
   - Decision:
     - use `service_namespace` as the public cross-platform namespace term
     - clients resolve services by `service_namespace + service_name`
     - POSIX maps this to `/run/netdata/<service>.sock`
     - Windows maps this to a derived named-pipe name scoped by the namespace
   - Implication:
     - discovery is service-oriented, not plugin-oriented
     - one plugin process may expose multiple services without clients knowing plugin identity

31. Service export model:
   - Source: user decision on 2026-03-12.
   - Decision:
     - use one endpoint per service
     - no separate registry daemon/file; `/run/netdata` is the registry on POSIX
     - Windows uses the same logical identity, but encoded into named-pipe names
   - Implication:
     - service names are the stable public contract
     - plugin/process identity remains an internal deployment detail

32. Server API layering:
   - Source: user decisions on 2026-03-12.
   - Decision:
     - expose both low-level and high-level server APIs
     - the high-level managed server must be a thin wrapper over the low-level transport/session engine
     - low-level server object hierarchy is:
       - `server_host`
       - `service_listener`
       - `session`
     - managed mode is a wrapper over `server_host`
   - Implication:
     - one transport/session implementation is the source of truth
     - high-level server helpers must not re-implement transport, framing, or negotiation logic

33. Service registration lifecycle:
   - Source: user decision on 2026-03-12.
   - Decision:
     - the exported service set is fixed after server startup for v1
   - Implication:
     - adding or removing services requires process restart
     - service lifecycle stays simpler and avoids dynamic registration races in v1

34. Handler contract:
   - Source: user decision on 2026-03-12.
   - Decision:
     - handler shape is `ctx + typed_request -> typed_response`
   - Implication:
     - business code receives typed payloads
     - request/session metadata remains available via the context object

35. High-level client model:
   - Source: user decisions on 2026-03-12.
   - Decision:
     - one persistent high-level client context per service, for example `ctx_ip_to_asn`
     - `initialize()` creates the context but does not require the provider to be up
     - caller owns reconnect cadence by calling `refresh(ctx)` from its normal loop
     - `ready(ctx)` must be a cheap cached predicate
     - `status(ctx)` must expose detailed operational state/counters
   - Implication:
     - no hidden client background thread in v1
     - startup order can remain random
     - plugin hot paths can stay cheap while recovery happens in the outer loop

36. High-level client call semantics:
   - Source: user decisions on 2026-03-12.
   - Decision:
     - if previous state was not `READY`, `call_xxx(ctx, ...)` must fail fast
     - if previous state was `READY`, the call may reconnect once and resend once after failure
     - duplicate requests are acceptable by contract for the high-level API
   - Implication:
     - high-level API is intentionally at-least-once, not exactly-once
     - low-level API remains the exact transport truth without automatic replay

37. High-level convenience return shape:
   - Source: user decisions on 2026-03-12.
   - Decision:
     - C convenience calls use `bool + out response`
     - convenience APIs may return "no response" when unavailable instead of forcing a separate readiness branch at every call site
     - keep both convenience APIs and strict checked APIs
   - Implication:
     - hot-path call sites stay small
     - C avoids heap-allocated response ownership traps
     - detailed diagnostics remain available through the checked API and `status(ctx)`

38. Client status model:
   - Source: user decisions on 2026-03-12.
   - Decision:
     - expose a detailed public status snapshot, not only a boolean
     - `refresh(ctx)` should return `changed` plus optional updated snapshot
     - public state model should include practical states such as:
       - `DISCONNECTED`
       - `CONNECTING`
       - `READY`
       - `NOT_FOUND`
       - `AUTH_FAILED`
       - `INCOMPATIBLE`
       - `BROKEN`
     - status should include reconnect counts and related operational counters
   - Implication:
     - plugins can adapt behavior cheaply via `ready(ctx)` and inspect richer state via `status(ctx)`
     - transition callbacks/logging can later be layered over the same state model

39. Advanced low-level throughput goal:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - one hot client must be able to drive more than one server worker
     - the low-level API must support advanced modes beyond simple ping-pong, including event-loop integration via native wait objects (`fd` on POSIX, `HANDLE` on Windows)
   - Implication:
     - request-level parallelism matters, not just multi-client concurrency
     - a session-sticky "one worker per client" managed-server model is insufficient for the advanced path

40. Protocol direction change:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - the final library protocol cannot remain fixed at 64 bytes
     - requests and responses must support variable payload sizes, including strings
     - move to a fixed header plus variable payload design
   - Implication:
     - the current 64-byte `INCREMENT` frame is only a benchmark scaffold
     - all transport helpers must evolve from exact fixed-size frame I/O to header+payload I/O

41. Advanced-throughput v1 strategy:
   - Source: user decision on 2026-03-12.
   - Decision:
     - prioritize ordered batch request/response support for v1
     - defer general pipelining to a future release if ordered batch is sufficient
     - negotiated connection limits should include both:
       - `max_batch_items`
       - `max_payload_bytes`
   - Implication:
     - v1 can target the main "one client needs more throughput" problem with lower complexity than full general pipeline semantics
     - batch limits become part of handshake/compatibility policy

42. Batch execution model:
   - Source: user decision on 2026-03-12.
   - Decision:
     - the server may split one incoming batch into contiguous parts and hand each part to a worker
     - responses are reassembled in the original request order before sending the batch response
     - preference is for simple deterministic splitting over per-item worker pull/work-stealing for v1
   - User rationale:
     - request handling is expected to be mostly uniform memory-lookup work
     - simple split also degenerates cleanly to the single-worker case, where one worker handles the full batch
   - Implication:
     - ordered batch responses remain deterministic
     - more sophisticated per-item dynamic balancing can be deferred unless measurements show meaningful skew

43. Batch response semantics:
   - Source: user decision on 2026-03-12.
   - Decision:
     - batch responses must carry per-item status/result, not only whole-batch success/failure
   - Implication:
     - one bad item in a batch does not poison the whole batch
     - batch response envelopes must preserve item order and include status for each item

44. Variable-length envelope and ordered homogeneous batches:
   - Source: user decision on 2026-03-12.
   - Decision:
     - use one universal fixed header plus variable payload envelope
     - one batch must contain requests of a single method only
     - ordered homogeneous batch payloads correlate request/response items by array position
   - Implication:
     - single-request and batch messages share one outer protocol
     - batch codecs stay strongly typed and service-specific

45. Outer-envelope status semantics:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - the outer envelope status is transport/protocol status only
     - it means things like "this message was received/validated/responded at the envelope level"
     - it must not be used to represent per-item business outcomes such as "item not found"
   - Implication:
     - per-item or per-method outcomes stay inside the typed response content / batch item response entries
     - outer status stays small, generic, and transport-oriented

46. Batch body layout direction:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - batch items must be variable-size
     - fixed-size negotiated slots are not acceptable because requests/responses may carry strings of very different lengths
     - the active v1 direction is:
       - outer envelope with `item_count`
       - followed by an item directory of offsets and lengths
       - followed by the packed aligned item payloads
   - Implication:
     - batch parsing is a two-step process: directory first, then payload slices
     - item correlation remains by array position, while offsets/lengths provide flexible sizing

47. Self-contained zero-allocation payload contract:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - each request/response payload must be self-contained
     - the outer envelope/directory only identifies the payload byte range for each item
     - payload decoders must be able to interpret the payload in place without heap allocation
     - variable-length fields such as strings should be addressable through offsets inside the payload
     - string data inside payloads should be NUL-terminated so decoders can expose direct pointers/slices to the underlying bytes
   - Implication:
      - typed decode helpers should behave like views over payload bytes, not deep-copy constructors
      - each method payload may need its own internal header/offset table for variable-length members

48. View lifetime contract:
   - Source: user decision on 2026-03-12.
   - Decision:
     - decoded request/response views are highly ephemeral
     - they are valid only within the current library call / current callback invocation
     - callers must either use the view immediately or copy the needed data before the function/callback returns
     - callers must assume the view becomes invalid at the next library call, and in practice treat it as invalid once the current function/callback returns
   - Implication:
     - the library is free to reuse internal buffers aggressively without preserving old decoded views
     - high-level APIs must document this lifetime rule explicitly in C, Rust, and Go

49. Per-method payload layout discipline:
   - Source: user decision on 2026-03-12 (`15.A`).
   - Decision:
     - every method payload uses a small fixed method header for scalar fields plus a method-local offset/length directory for its variable-length members
   - Implication:
     - the outer envelope stays transport-oriented
     - each method payload remains self-contained and decodable in place

50. String field representation inside method payloads:
   - Source: user decision on 2026-03-12 (`16.A`).
   - Decision:
     - each string field is represented by `offset + length`
     - the pointed bytes must also end with `\\0`
   - Implication:
     - C gets cheap direct pointer compatibility
     - Rust and Go get O(1) slicing without scanning for the terminator

51. Decoded payload/view lifetime model:
   - Source: user decision on 2026-03-12 (`17.A`), later tightened by Decision #48.
   - Decision:
     - decoded request/response objects are non-owning views over the underlying message bytes
   - Implication:
     - zero-allocation decoding is the default contract
     - ownership is caller-managed and requires explicit copying outside the library call/callback

52. Ephemeral-view naming and documentation discipline:
   - Source: user decision on 2026-03-12.
   - Decision:
     - public decoded request/response types must be named and documented so their ephemeral lifetime is impossible to miss
     - type names, field names where appropriate, and comments must clearly signal that these are borrowed/view types, not durable owned objects
     - API documentation must state that the data is valid only within the current library call / callback and must be copied immediately if it needs to outlive that scope
   - Implication:
     - naming should prefer explicit view semantics such as `...View`
     - comments and docs must aggressively warn against retaining pointers/slices beyond the allowed lifetime

53. Derived consequence of self-contained response payloads:
   - Source: derived from Decisions #43, #45, #47, #49, #50, and #51 on 2026-03-12.
   - Derived design:
     - batch item descriptors should carry only `offset + length`
     - per-item business/method outcome status belongs inside each response payload's self-contained method-local layout
     - the outer envelope status remains transport/protocol-only
   - Implication:
     - batch request and batch response directories can use the same descriptor shape
     - decoders only need the descriptor to locate each self-contained payload view

## Derived Protocol Sketch (2026-03-12)
- Status: derived from the agreed design decisions above; not a new decision by itself.

- Outer message header (`v1`) should be a single fixed 32-byte envelope:
  - `magic: u32`
  - `version: u16`
  - `header_len: u16`
  - `kind: u16` (`control`, `request`, `response`)
  - `flags: u16` (at least `batch`)
  - `code: u16`
    - request/response: method id
    - control: control opcode
  - `transport_status: u16`
    - envelope-level / transport-level / protocol-level only
    - never business/item-level result
  - `payload_len: u32`
    - bytes after the outer header
  - `item_count: u32`
    - `1` for non-batch messages
    - `N` for batch messages
  - `message_id: u64`
    - request/response correlation id

- Batch item directory:
  - one fixed descriptor shape for both request and response batches:
    - `offset: u32`
    - `length: u32`
  - offsets are relative to the start of the packed item payload area
  - request/response item correlation is by array position

- Batch message payload layout:
  - `[ item_ref[0] ... item_ref[N-1] ][ aligned item payload 0 ][ aligned item payload 1 ] ...`
  - request batch and response batch use the same outer batch layout
  - each item payload is self-contained and method-specific

- Single message payload layout:
  - no directory table
  - one self-contained method/control payload follows the outer header

- Method payload discipline:
  - each method payload has:
    - a small fixed method-local header
    - method-local scalar fields
    - method-local `offset + length` pairs for variable-length members
    - packed variable data area
  - outer envelope never knows inner method field layout

- String fields inside method payloads:
  - represented by `offset + length`
  - pointed bytes must also terminate with `\\0`
  - decoders should expose borrowed views/pointers/slices, not owned copies

- Lifetime model:
  - decoded request/response objects are ephemeral views only
  - they are valid only within the current library call / callback

## Derived Public API Sketch (2026-03-12)
- Status: derived from Decisions #35-#38, #47-#56; intended as the concrete direction for the next implementation phase.

- High-level API split:
  - low-level:
    - transport/session primitives
    - raw envelope send/receive
    - method payload encode/decode to ephemeral `...View` types
  - high-level:
    - fixed per-service client context
    - fixed per-service managed server registration
    - zero-copy request/response handling via callbacks
    - optional explicit copy/materialize helpers for callers that need ownership

- High-level client context model (all languages):
  - one context per service, for example `ip_to_asn`
  - created once
  - refreshed periodically by the caller
  - cheap `ready()` predicate
  - detailed `status()` snapshot
  - zero-copy call path is callback-based

- C shape (derived):
  - owned request input struct, for example:
    - `struct netipc_ip_to_asn_req { const char *ip_text; uint32_t ip_text_len; };`
  - ephemeral response view type:
    - `struct netipc_ip_to_asn_resp_view { ... netipc_str_view as_name_view; ... };`
  - client context:
    - `netipc_ip_to_asn_client_t *`
  - zero-copy call:
    - `bool netipc_ip_to_asn_call_view(..., netipc_ip_to_asn_resp_view_cb cb, void *user);`
  - optional explicit copy helper:
    - copy a response view into a caller-owned output struct

- Rust shape (derived):
  - owned/borrowed request input struct appropriate for encoding, for example:
    - `IpToAsnRequest<'a> { ip_text: &'a str, ... }`
  - ephemeral response view type:
    - `IpToAsnResponseView<'a> { ... as_name_view: StrView<'a>, ... }`
  - client context:
    - `IpToAsnClient`
  - zero-copy call:
    - `call_ip_to_asn_view(&mut self, req, timeout, |view| { ... })`
  - optional explicit materialize helper:
    - convert/copy a view into an owned response struct

- Go shape (derived):
  - owned request input struct for encoding, for example:
    - `type IPToASNRequest struct { IPText string }`
  - ephemeral response view type:
    - `type IPToASNResponseView struct { ... ASNameView CStringView ... }`
  - client context:
    - `type IPToASNClient struct { ... }`
  - zero-copy call:
    - `CallIPToASNView(req, timeout, func(view IPToASNResponseView) bool)`
  - optional explicit materialize helper:
    - copy a view into an owned Go response struct

- Managed server callback shape (derived):
  - request side:
    - callback receives an ephemeral decoded request view
    - request view is valid only during the callback
  - response side:
    - callback writes the response through a method-specific response builder backed by library-managed scratch storage
    - this avoids heap allocation while still allowing variable-length response fields
  - C example shape:
    - `bool netipc_ip_to_asn_handler(void *user, const netipc_ip_to_asn_req_view *req_view, netipc_ip_to_asn_resp_builder_t *resp_builder);`
  - Rust example shape:
    - `FnMut(&IpToAsnRequestView<'_>, &mut IpToAsnResponseBuilder<'_>) -> bool`
  - Go example shape:
    - `func(reqView IPToASNRequestView, resp *IPToASNResponseBuilder) bool`

- Naming/documentation rule:
  - decoded borrowed types should always contain `View` in the public type name
  - docs/comments must explicitly state:
    - borrowed
    - ephemeral
    - valid only during current library call / callback
    - copy immediately if needed later

## Derived Method Example: `ip_to_asn` (2026-03-12)
- Status: derived example only; intended to prove the current design is usable end-to-end.

- Service identity:
  - `service_namespace = /run/netdata` on POSIX
  - `service_name = ip-to-asn`

- Semantic contract:
  - request:
    - caller asks for ASN enrichment for one textual IP address
  - response:
    - returns method/business result code
    - if found, returns ASN and zero-copy string views for metadata

- Method id:
  - fixed service/method-specific code in the outer envelope `code` field

- Request payload example (`ip_to_asn`):
  - method-local fixed header:
    - `layout_version: u16`
    - `flags: u16`
    - `ip_text_off: u32`
    - `ip_text_len: u32`
  - packed variable data:
    - `ip_text` bytes followed by `\\0`
  - total request payload is self-contained and decodable in place

- Response payload example (`ip_to_asn`):
  - method-local fixed header:
    - `layout_version: u16`
    - `result_code: u16`
      - method/business result, for example:
        - found
        - not found
        - invalid input
    - `asn: u32`
    - `as_name_off: u32`
    - `as_name_len: u32`
    - `cc_off: u32`
    - `cc_len: u32`
  - packed variable data:
    - `as_name` bytes followed by `\\0`
    - `cc` bytes followed by `\\0`
  - response payload remains self-contained and decodable in place

- C shape (derived example):
  - request input:
    - `struct netipc_ip_to_asn_req { const char *ip_text; uint32_t ip_text_len; };`
  - string view:
    - `struct netipc_str_view { const char *ptr; uint32_t len; };`
  - response view:
    - `struct netipc_ip_to_asn_resp_view {`
    - `  uint16_t result_code;`
    - `  uint32_t asn;`
    - `  struct netipc_str_view as_name_view;`
    - `  struct netipc_str_view cc_view;`
    - `};`
  - zero-copy client call:
    - `bool netipc_ip_to_asn_call_view(..., netipc_ip_to_asn_resp_view_cb cb, void *user);`
  - managed server callback:
    - `bool netipc_ip_to_asn_handler(void *user, const struct netipc_ip_to_asn_req_view *req_view, netipc_ip_to_asn_resp_builder_t *resp_builder);`
  - copy helper:
    - explicit helper copies a response view into a caller-owned durable output struct

- Rust shape (derived example):
  - request input:
    - `pub struct IpToAsnRequest<'a> { pub ip_text: &'a str }`
  - borrowed string view:
    - `pub struct StrView<'a> { pub bytes: &'a [u8] }`
  - response view:
    - `pub struct IpToAsnResponseView<'a> {`
    - `  pub result_code: u16,`
    - `  pub asn: u32,`
    - `  pub as_name_view: StrView<'a>,`
    - `  pub cc_view: StrView<'a>,`
    - `}`
  - zero-copy client call:
    - `call_ip_to_asn_view(&mut self, req, timeout, |view| { ... })`
  - managed server callback:
    - `FnMut(&IpToAsnRequestView<'_>, &mut IpToAsnResponseBuilder<'_>) -> bool`
  - explicit materialize helper:
    - copy a borrowed response view into an owned Rust response type

- Go shape (derived example):
  - request input:
    - `type IPToASNRequest struct { IPText string }`
  - borrowed string wrapper:
    - `type CStringView struct { ... }`
  - response view:
    - `type IPToASNResponseView struct {`
    - `  ResultCode uint16`
    - `  ASN uint32`
    - `  ASNameView CStringView`
    - `  CCView CStringView`
    - `}`
  - zero-copy client call:
    - `CallIPToASNView(req, timeout, func(view IPToASNResponseView) bool)`
  - managed server callback:
    - `func(reqView IPToASNRequestView, resp *IPToASNResponseBuilder) bool`
  - explicit materialize helper:
    - copy a borrowed response view into an owned Go response struct

- Builder discipline (all languages):
  - response builders should write into library-managed scratch/output buffers
  - handlers should set scalars and append string fields through builder methods
  - builder must guarantee:
    - offset/length bookkeeping
    - trailing `\\0` for string fields
    - alignment/padding of packed variable data

- View lifetime reminder:
  - request and response views in this example are valid only during the current callback / library call
  - callers must not retain pointers/slices/borrowed wrappers after that scope ends

## Derived Helper Naming / Builder Surface (2026-03-12)
- Status: derived from the agreed API and lifetime rules; intended to keep naming and helper behavior consistent across C, Rust, and Go.

- Naming discipline:
  - owned encode input types do not use `View`
  - decoded borrowed request/response types always use `View`
  - response builders always use `Builder`
  - explicit copy helpers should use verbs like:
    - `copy`
    - `materialize`
    - `to_owned`

- C codec/helper naming (derived):
  - encode owned request input into payload bytes:
    - `netipc_ip_to_asn_req_encode(...)`
  - decode request payload into ephemeral request view:
    - `netipc_ip_to_asn_req_decode_view(...)`
  - decode response payload into ephemeral response view:
    - `netipc_ip_to_asn_resp_decode_view(...)`
  - initialize/reset response builder:
    - `netipc_ip_to_asn_resp_builder_init(...)`
    - `netipc_ip_to_asn_resp_builder_reset(...)`
  - set scalar response fields:
    - `netipc_ip_to_asn_resp_builder_set_result_code(...)`
    - `netipc_ip_to_asn_resp_builder_set_asn(...)`
  - append string response fields:
    - `netipc_ip_to_asn_resp_builder_set_as_name(...)`
    - `netipc_ip_to_asn_resp_builder_set_cc(...)`
  - finalize builder into payload bytes:
    - `netipc_ip_to_asn_resp_builder_finish(...)`
  - explicit copy/materialize helper:
    - `netipc_ip_to_asn_resp_view_copy(...)`

- Rust codec/helper naming (derived):
  - encode owned/borrowed request input into payload bytes:
    - `encode_ip_to_asn_request(...)`
  - decode request payload into ephemeral request view:
    - `decode_ip_to_asn_request_view(...)`
  - decode response payload into ephemeral response view:
    - `decode_ip_to_asn_response_view(...)`
  - response builder type and methods:
    - `IpToAsnResponseBuilder`
    - `set_result_code(...)`
    - `set_asn(...)`
    - `set_as_name(...)`
    - `set_cc(...)`
    - `finish(...)`
  - explicit materialize helper:
    - `IpToAsnResponseView::to_owned()`

- Go codec/helper naming (derived):
  - encode owned request input into payload bytes:
    - `EncodeIPToASNRequest(...)`
  - decode request payload into ephemeral request view:
    - `DecodeIPToASNRequestView(...)`
  - decode response payload into ephemeral response view:
    - `DecodeIPToASNResponseView(...)`
  - response builder type and methods:
    - `IPToASNResponseBuilder`
    - `SetResultCode(...)`
    - `SetASN(...)`
    - `SetASName(...)`
    - `SetCC(...)`
    - `Finish(...)`
  - explicit materialize helper:
    - `func (v IPToASNResponseView) ToOwned() IPToASNResponse`

- Builder behavior rules:
  - builder methods must never expose raw offset bookkeeping to handlers
  - builder owns:
    - packed variable-data placement
    - offset/length assignment
    - trailing `\\0` insertion for strings
    - alignment/padding
  - `finish(...)` returns one self-contained method payload
  - handlers should only set semantic fields, not wire-layout details

- Validation rules for decode helpers:
  - reject out-of-bounds offsets/lengths
  - reject overlapping/invalid field regions when the method schema forbids them
  - reject string fields missing the required trailing `\\0`
  - reject payloads shorter than the fixed method-local header

## Current Phase Plan: Protocol/API Rewrite (2026-03-12)
- Goal:
  - replace the current fixed 64-byte benchmark protocol and typed-return transport APIs with the agreed variable-length, self-contained, zero-allocation view model
  - freeze the directional handshake contract before more implementation continues
  - deliver the project in strict phases, with each phase tested and documented before the next begins
  - validate the first snapshot/cache-backed service with a fake producer and dummy data inside this repository before touching the Netdata repository

- Critical implementation constraint (fact-based):
  - the current SHM transports are single-slot and fixed-frame:
    - POSIX SHM region still stores one request frame and one response frame
    - Windows SHM region still stores one request frame and one response frame
  - implication:
    - the current SHM design cannot carry the new variable-length ordered-batch protocol without a separate redesign
  - therefore:
    - phase 1 of this protocol/API rewrite should target the multiplexable baseline transports first:
      - POSIX `UDS_SEQPACKET`
      - Windows `Named Pipe`
    - SHM redesign should follow as a separate phase after the new envelope/method API is stable

- Phase 1: directional handshake contract rewrite (all languages)
  - Replace the current symmetric hello / hello-ack contract with the approved directional model:
    - request direction:
      - `max_request_payload_bytes`
      - `max_request_batch_items`
    - response direction:
      - `max_response_payload_bytes`
      - `max_response_batch_items`
    - batch bytes derived per direction
  - Update:
    - C:
      - `src/libnetdata/netipc/include/netipc/netipc_schema.h`
      - `src/libnetdata/netipc/src/protocol/netipc_schema.c`
    - Rust:
      - `src/crates/netipc/src/protocol.rs`
      - `src/crates/netipc/src/lib.rs`
    - Go:
      - `src/go/pkg/netipc/protocol/frame.go`
  - Add protocol tests for:
    - encode/decode roundtrips
    - negotiation success/failure
    - limit mismatch rejection

- Phase 2: baseline transport migration to the new protocol core
  - Migrate baseline transports first:
    - POSIX `UDS_SEQPACKET`
    - Windows `Named Pipe`
  - Replace fixed-frame / `increment`-shaped paths with generic variable-length message handling
  - Execution detail for this phase:
    - first add generic message send/receive/call primitives on the baseline transports
    - keep the existing fixed-frame `increment` helpers as compatibility wrappers on top during migration
    - do not force SHM onto the new generic message path in this phase; SHM stays on the fixed-frame compatibility path until its dedicated redesign phase
  - Preserve:
    - control handshake
    - single-message request/response
    - ordered homogeneous batch request/response
  - Target files:
    - POSIX C:
      - `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
      - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
    - Windows C:
      - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - Rust:
      - `src/crates/netipc/src/transport/posix.rs`
      - `src/crates/netipc/src/transport/windows.rs`
    - Go:
      - `src/go/pkg/netipc/transport/posix/seqpacket.go`
      - `src/go/pkg/netipc/transport/windows/pipe.go`

- Phase 3: first real method family and helper foundation
  - Implement the first real method family as the approved fake `cgroups`-style snapshot service
  - Introduce:
    - owned request encode input
    - ephemeral request/response view decode helpers
    - response/snapshot builders
    - explicit copy/materialize helpers
    - Go borrowed string wrapper type
  - Keep the service contract cleaned, not a direct copy of current Netdata SHM structs

- Phase 4: fake producer + cache-backed helper layer
  - Build a fake producer with dummy cgroup/container records inside this repository
  - Build the higher-level cache-backed helper layer strictly on top of the generic client/service core
  - Validate flows like:
    - `refresh_cgroups()`
    - `lookup_cgroup()`
  - No Netdata-repo integration in this phase

- Phase 5: full tests and interop for the fake snapshot service
  - Protocol tests:
    - handshake
    - envelope validation
    - directional limit enforcement
  - Method tests:
    - snapshot payload decode
    - builder correctness
    - lifetime/documentation-sensitive behavior
  - Interop tests:
    - C <-> Rust <-> Go for the fake snapshot service
  - Reliability tests:
    - reconnect after previously-READY failure
    - refresh/cache rebuild correctness
    - malformed offsets/lengths rejection

- Phase 6: performance coverage on baseline transports
  - Benchmark:
    - ping-pong baseline
    - ordered-batch request/response
    - snapshot refresh path
    - local cache lookup hot path
  - Validate:
    - throughput
    - p50/p95/p99
    - correctness
    - CPU impact

- Phase 7: SHM redesign for the real protocol
  - Redesign POSIX and Windows SHM paths to support the real variable-length protocol and snapshot publication model
  - Then run the same fake snapshot/cache service and performance coverage on SHM

- Phase 8: real Netdata integration
  - Only after the fake producer/service is fully validated in this repository
  - Add adaptation from current Netdata producer/consumer data shapes to the cleaned `plugin-ipc` service contract

- Phase 9: documentation rewrite
  - Update:
    - architecture doc
    - protocol spec
    - client/server developer guide
    - cache-backed helper usage
    - lifetime warnings for all `...View` types
  - Explicitly document:
    - views are ephemeral
    - copy now or lose data
    - transport status vs method/business result
    - directional request/response handshake limits
    - negotiated payload/batch limits

- Migration order recommendation:
  - first make the protocol/method layer independent of transports
  - then migrate baseline transports
  - then layer high-level client/server APIs on top
  - then add ordered-batch benchmarks
  - then revisit SHM redesign

- Expected non-goals for this phase:
  - no attempt to preserve the old fixed 64-byte wire format as the long-term protocol
  - no attempt to force the current single-slot SHM transports to carry the new protocol unchanged
  - no hidden allocation-based fallback masquerading as the default zero-copy API

## Phase 1 Implementation Snapshot (2026-03-12)
- Current status:
  - Phase 1 is now implemented for the protocol core and all current live handshake paths
  - the protocol core no longer uses the old symmetric hello / hello-ack sizing contract
  - the live POSIX UDS and Windows named-pipe transport handshakes now embed the approved directional hello / hello-ack payloads instead of the old ad-hoc symmetric negotiation fields
  - legacy fixed-frame `increment` encode/decode APIs are still kept in place for compatibility during migration
- Landed in Phase 1:
  - protocol-core support for the new outer message envelope in C, Rust, and Go
  - support for:
    - fixed 32-byte outer header
    - `offset + length` batch item refs
    - directional hello / hello-ack payloads
    - directional request/response payload and batch-item ceilings
    - negotiated single-payload default constant of `1024`
  - POSIX UDS live handshake rewritten to reuse the directional protocol payloads
  - Windows named-pipe live handshake rewritten to reuse the same directional protocol payloads in C, Rust, and Go
  - Go raw-hello negative-test driver updated to speak the same directional handshake
- Files changed in this phase so far:
  - C:
    - `src/libnetdata/netipc/include/netipc/netipc_schema.h`
    - `src/libnetdata/netipc/src/protocol/netipc_schema.c`
    - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - Rust:
    - `src/crates/netipc/src/protocol.rs`
    - `src/crates/netipc/src/lib.rs`
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
  - Go:
    - `src/go/pkg/netipc/protocol/frame.go`
    - `src/go/pkg/netipc/protocol/frame_test.go`
    - `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - `src/go/pkg/netipc/transport/posix/seqpacket_unix.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
    - `bench/drivers/go/main.go`
- Validation completed after the directional handshake rewrite:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `cmake --build build`
  - `/usr/bin/ctest --test-dir build --output-on-failure`
  - on `win11`:
    - `PATH=/c/Users/costa/.cargo/bin:$PATH cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `go test ./...` from `src/go`
    - `bash tests/smoke-win.sh` with result `32 passed, 0 failed`
- Important remaining gap after Phase 1:
  - baseline transports still expose the old fixed-frame / `increment`-specific public API
  - Phase 2/3 migration is still needed before the public library surface matches the new `...View` / callback-based design
- Current blocker status:
  - there is no longer an unresolved blocker for continuing autonomously into Phase 2
  - the first real service family is already approved as the fake cleaned `cgroups` snapshot service, so the next implementation work can proceed according to the phase plan

## Phase 2 Progress Snapshot (2026-03-12)
- Current status:
  - Phase 2 now has the first end-to-end baseline-transport slice implemented and validated
  - the protocol core has one authoritative derived-size calculation for:
    - aligned item payload bytes
    - maximum ordered-batch payload bytes
    - maximum total message bytes
  - baseline transports now also carry negotiated directional message ceilings and expose generic variable-message primitives on the baseline paths
- Landed in this Phase 2 slice:
  - C protocol helpers added in:
    - `src/libnetdata/netipc/include/netipc/netipc_schema.h`
    - `src/libnetdata/netipc/src/protocol/netipc_schema.c`
  - C baseline transport generic message APIs added in:
    - `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
    - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - Rust protocol helpers and tests added in:
    - `src/crates/netipc/src/protocol.rs`
  - Rust baseline transport generic message APIs added in:
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
  - Go protocol helpers and tests added in:
    - `src/go/pkg/netipc/protocol/frame.go`
    - `src/go/pkg/netipc/protocol/frame_test.go`
  - Go baseline transport generic message APIs added in:
    - `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
- Why this landed first:
  - baseline transports need one shared formula for directional receive/send buffer ceilings
  - adding it first avoided C/Rust/Go drift while the generic variable-message transport primitives were wired in
  - the baseline transports needed negotiated directional message ceilings stored in transport contexts before any real method migration could start
- Validation completed for this slice:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `cmake --build build`
  - `/usr/bin/ctest --test-dir build --output-on-failure`
- Remaining Phase 2 work after this slice:
  - generic message APIs exist, but no real variable-length service/method is using them yet
  - the legacy fixed-frame `increment` helpers are intentionally still raw fixed-frame compatibility paths on the baseline transports
  - the next practical Phase 2/3 step is to implement the first real method family on top of these generic message APIs
  - SHM still remains on the old fixed-frame compatibility path until its dedicated redesign phase
- Important limitation of this slice:
  - generic variable-message transport support is now present on the baseline transports, but the existing `increment` fixtures and interop tests still exercise the legacy fixed-frame path
  - this is intentional for compatibility during migration; the first real validation of the new generic message path will happen with the approved fake cleaned `cgroups` snapshot service

## Phase 3 Progress Snapshot (2026-03-12)
- Current status:
  - the first real variable-length method family is now implemented at the shared protocol/codec layer in C, Rust, and Go
  - this first real method family is the approved fake cleaned `cgroups` snapshot service, using the real semantic fields the current `ebpf` consumer depends on:
    - `hash`
    - `name`
    - `options`
    - `enabled`
    - `path`
    - snapshot-level `systemd_enabled`
  - the generic message path is now exercised by the file-based cross-language interop fixtures, not only by per-language unit tests
- Landed in this Phase 3 slice:
  - method id and wire layout constants for the fake cleaned `cgroups` snapshot service added in:
    - `src/libnetdata/netipc/include/netipc/netipc_schema.h`
    - `src/libnetdata/netipc/src/protocol/netipc_schema.c`
    - `src/crates/netipc/src/protocol.rs`
    - `src/go/pkg/netipc/protocol/frame.go`
  - zero-allocation decode/view support added for:
    - snapshot request views
    - snapshot response views
    - per-item `name` / `path` string views
  - response builders added for the ordered snapshot response in:
    - `src/libnetdata/netipc/src/protocol/netipc_schema.c`
    - `src/crates/netipc/src/protocol.rs`
    - `src/go/pkg/netipc/protocol/frame.go`
  - Rust root exports updated in:
    - `src/crates/netipc/src/lib.rs`
  - file-based codec fixtures upgraded to speak the new generic message path in:
    - `tests/fixtures/c/netipc_codec_tool.c`
    - `tests/fixtures/rust/src/bin/netipc_codec_rs.rs`
    - `tests/fixtures/go/main.go`
  - cross-language schema interop upgraded in:
    - `tests/run-interop.sh`
- Validation completed for this slice:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `cargo test --manifest-path tests/fixtures/rust/Cargo.toml --bin netipc-codec-rs`
  - `cmake --build build`
  - `/usr/bin/ctest --test-dir build --output-on-failure`
- What this slice proves:
  - the first real generic-message method family now round-trips in all three languages
  - the first real generic-message method family now interoperates across C, Rust, and Go through the file-based fixtures
  - the fake cleaned `cgroups` snapshot service contract is no longer just a TODO design; it now exists in executable codec form
- Remaining work after this slice:
  - baseline transport wrappers still do not expose high-level `cgroups` snapshot helpers
  - there is not yet a fake producer process or cache-backed client helper
  - snapshot refresh / lookup flow is still not exercised end-to-end over the baseline transports

## Phase 2/3 Baseline Transport Limit Slice (2026-03-12)
- Current status:
  - the baseline transport generic-message APIs now carry the negotiated directional request/response ceilings all the way from config to the live client/server contexts
  - this removed the last hardcoded single-item/default-size assumption from the baseline handshake/config layer before the first fake snapshot service uses multi-item responses
- Landed in this slice:
  - directional request/response payload and batch-item limits added to:
    - `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
    - `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
  - live baseline handshakes now negotiate against those configured directional ceilings instead of old hardcoded defaults in:
    - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
    - `src/go/pkg/netipc/transport/posix/seqpacket.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
- Validation completed for this slice:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `cmake --build build`
- Important constraint exposed by this slice:
  - the transport clients are still eager-connect objects
  - the approved `initialize()`/`refresh()` lifecycle for cache-backed services must therefore live in the higher-level helper layer, not in the baseline transport client types themselves
- Next implementation target after this slice:
  - add the first cache-backed helper on top of the generic baseline message APIs
  - add the first fake producer process that serves the cleaned `cgroups` snapshot service end-to-end over the baseline transports
  - SHM still remains on the legacy compatibility path until its dedicated redesign phase

## Phase 4 Progress Snapshot (2026-03-12)
- Current status:
  - the first transport-backed fake `cgroups` snapshot/cache slice now exists in C on the baseline POSIX and Windows baseline transport paths
  - this is the first end-to-end refresh/cache flow that uses:
    - the real generic message transport API
    - the cleaned fake `cgroups` snapshot method family
    - a higher-level cache-backed helper layered above the baseline transport client
- Landed in this slice:
  - high-level cache-backed C helper added in:
    - `src/libnetdata/netipc/include/netipc/netipc_cgroups_snapshot.h`
    - `src/libnetdata/netipc/src/service/netipc_cgroups_snapshot.c`
  - matching cache-backed Rust helper added in:
    - `src/crates/netipc/src/service/mod.rs`
    - `src/crates/netipc/src/service/cgroups_snapshot.rs`
  - matching cache-backed Go helper added in:
    - `src/go/pkg/netipc/service/cgroupssnapshot/client.go`
    - `src/go/pkg/netipc/service/cgroupssnapshot/client_unix.go`
    - `src/go/pkg/netipc/service/cgroupssnapshot/client_windows.go`
  - fake live producer/consumer fixture added in:
    - `tests/fixtures/c/netipc_cgroups_live.c`
  - baseline live smoke workflow added in:
    - `tests/run-live-cgroups-baseline.sh`
    - `tests/run-live-cgroups-win.sh`
  - CMake wiring added for:
    - fixture target `netipc-cgroups-live-c`
    - POSIX `ctest` workflow registration for `netipc-live-cgroups-baseline`
    - Windows `ctest` workflow registration for `netipc-live-cgroups-win`
- What this slice validates:
  - lazy high-level helper lifecycle on top of the eager baseline transport client
  - full-snapshot refresh using the generic message path
  - local cache rebuild and lookup by `hash + name`
  - fake producer behavior suitable for later rehearsal against the real `cgroups -> ebpf` replacement path
  - the same refresh/cache helper methodology now exists in all three library languages:
    - C
    - Rust
    - Go
- Important bug exposed and fixed during this slice:
  - the fake live server initially allocated its request buffer only for the actual 36-byte single request
  - the generic baseline transport API requires receive buffers to be sized for the full negotiated maximum message length
  - the fixture now allocates request/response buffers from the negotiated service limits instead of hard-coded message sizes
  - the Windows named-pipe transport still referenced removed fixed-frame helpers (`read_pipe_frame()` / `write_pipe_frame()`) after the generic-message migration
  - compatibility wrappers were restored on top of `read_pipe_message()` / `write_pipe_message()` in `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - the first Windows smoke attempt also exposed two workflow-only issues in `tests/run-live-cgroups-win.sh`:
    - native Windows output arrived with `\r\n`, so the captured client output needed `\r` normalization before assertions
    - the loop refresh path prints `REFRESHES` before `CGROUPS_CACHE`, so the smoke assertions now validate exact required lines by presence instead of assuming fixed line order
- Validation completed for this slice:
  - `cmake --build build --target netipc-cgroups-live-c`
  - `bash tests/run-live-cgroups-baseline.sh`
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `cargo check --manifest-path src/crates/netipc/Cargo.toml --target x86_64-pc-windows-gnu`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `/usr/bin/ctest --test-dir build --output-on-failure`
  - on `win11`:
    - `bash tests/run-live-cgroups-win.sh`
    - result: `8 passed, 0 failed`
    - matrix covered:
      - `c-native` server/client
      - `c-native` server + `c-msys` client
      - `c-msys` server + `c-native` client
      - `c-msys` server/client
      - each in both `refresh once` and `refresh loop` modes
- Remaining work after this slice:
  - the live fake producer/consumer fixture currently exists only in C
  - Rust/Go now have transport-backed helper coverage in library tests, but they do not yet have matching live snapshot fixtures/CLI tools
  - Rust helper has Windows-target compile coverage, but not a real Windows runtime smoke yet
  - Go helper has Windows-target compile coverage, but not a real Windows runtime smoke yet
  - the fake `cgroups` snapshot schema is still Linux-specific even though the refresh/cache methodology is now validated on Windows baseline too

## Phase 5 Progress Slice: live fake cgroups client matrix (2026-03-12)
- Current status:
  - the fake `cgroups` snapshot service now has real live client coverage in all three languages on Linux baseline transports
  - the live producer is still C-only, but the client-side refresh/cache helper path is now exercised end-to-end through:
    - C client helper
    - Go client helper
    - Rust client helper
- Landed in this slice:
  - Go fixture CLI extended with live cache-helper commands in:
    - `tests/fixtures/go/main.go`
  - Rust fixture CLI extended with live cache-helper commands in:
    - `tests/fixtures/rust/src/bin/netipc_codec_rs.rs`
  - baseline live smoke expanded from C-only to a mixed-language client matrix in:
    - `tests/run-live-cgroups-baseline.sh`
  - POSIX `ctest` workflow dependencies expanded so the live fake-service test always builds the Go and Rust fixture binaries too:
    - `CMakeLists.txt`
- What this slice validates:
  - the same fake C snapshot producer can now be consumed live by:
    - C client helper
    - Go client helper
    - Rust client helper
  - output/lookup semantics stay identical across the three client implementations for:
    - `client-refresh-once`
    - `client-refresh-loop`
  - the cache-backed helper layer is no longer validated only through unit/library tests in Go and Rust; it is now exercised through real baseline transport sessions too
- Validation completed for this slice:
  - `cmake --build build --target netipc-cgroups-live-c netipc-codec-go netipc-codec-rs`
  - `bash tests/run-live-cgroups-baseline.sh`
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - `go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
  - `GOOS=windows GOARCH=amd64 go build -o /tmp/netipc-codec-go.exe .` from `tests/fixtures/go`
  - `cargo check --manifest-path tests/fixtures/rust/Cargo.toml --bin netipc-codec-rs --target x86_64-pc-windows-gnu`
  - `/usr/bin/ctest --test-dir build --output-on-failure`
- Additional Windows runtime validation completed after this slice:
  - the dedicated `win11` validation clone was updated with the current task files by overwrite-only sync (no reset/clean)
  - `bash tests/run-live-cgroups-win.sh` now also builds and exercises:
    - native Go fake-service helper CLI
    - native Rust fake-service helper CLI
  - result on `win11`:
    - `16 passed, 0 failed`
    - covered:
      - existing C matrix:
        - `c-native` server/client
        - `c-native` server + `c-msys` client
        - `c-msys` server + `c-native` client
        - `c-msys` server/client
      - new helper-client matrix:
        - `c-native` server + `go-native` client
        - `c-native` server + `rust-native` client
        - `c-msys` server + `go-native` client
        - `c-msys` server + `rust-native` client
      - each in both `refresh once` and `refresh loop` modes
- Important fact exposed by this slice:
  - the new Rust live fake-service client path is Windows-target clean when checking the actual fixture binary used for fake-service work (`netipc-codec-rs`)
  - the full Rust fixtures manifest is still not Windows-target clean, but that is due to the older POSIX-only `netipc_live_rs` benchmark/live fixture binary, not the new `cgroups` helper path
- Important workflow fix exposed by the Windows helper run:
  - the first `win11` helper attempt failed because the Go helper output path in `tests/run-live-cgroups-win.sh` was relative to `tests/fixtures/go`, while later checks expected the binary under the repo-root build directory
  - the script now uses repo-root absolute helper binary paths for both:
    - `netipc-codec-go.exe`
    - `netipc-codec-rs.exe`
- Remaining work after this slice:
  - live fake-service producer coverage is still C-only; there is still no Rust or Go fake snapshot producer fixture
  - mixed-language live fake-service coverage currently means:
    - C producer -> C/Go/Rust clients
  - not yet Go producer or Rust producer -> other-language clients
  - the fake `cgroups` snapshot schema remains Linux-specific even though the refresh/cache methodology is now cross-platform

## Phase 5 Progress Slice: live fake cgroups full producer/client matrix (2026-03-12)
- Current status:
  - the fake `cgroups` snapshot service now has real live producer/client coverage in all three languages on Linux baseline transports
  - the same methodology now also has real runtime coverage on Windows baseline transports for all current producer/client implementations:
    - `c-native`
    - `c-msys`
    - `go-native`
    - `rust-native`
- Landed in this slice:
  - Go fixture CLI extended with live fake-service producer commands in:
    - `tests/fixtures/go/main.go`
    - `tests/fixtures/go/cgroups_server_common.go`
    - `tests/fixtures/go/cgroups_server_unix.go`
    - `tests/fixtures/go/cgroups_server_windows.go`
  - Rust fixture CLI extended with live fake-service producer commands in:
    - `tests/fixtures/rust/src/bin/netipc_codec_rs.rs`
  - baseline live Linux smoke expanded from `C producer -> C/Go/Rust clients` to a full `C/Go/Rust` producer/client matrix in:
    - `tests/run-live-cgroups-baseline.sh`
  - Windows live smoke expanded from the helper-client-only matrix to a full `c-native/c-msys/go-native/rust-native` producer/client matrix in:
    - `tests/run-live-cgroups-win.sh`
- What this slice validates:
  - the fake `cgroups` snapshot service is now exercised live in both directions across the current implementation set instead of only as:
    - `C producer -> other-language clients`
  - the fake producer behavior and the cache-backed client helper semantics stay identical across:
    - C
    - Go
    - Rust
  - the cross-platform snapshot/cache methodology now has real runtime proof on Windows too, not only compile-target sanity
- Validation completed for this slice:
  - Linux:
    - `cmake --build build --target netipc-cgroups-live-c netipc-codec-go netipc-codec-rs`
    - `bash tests/run-live-cgroups-baseline.sh`
    - result:
      - full `C/Go/Rust` producer/client matrix
      - each in both `refresh once` and `refresh loop` modes
  - additional local sanity:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `go test ./...` from `src/go`
    - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
    - `cargo check --manifest-path tests/fixtures/rust/Cargo.toml --bin netipc-codec-rs --target x86_64-pc-windows-gnu`
  - Windows on `win11` after overwrite-only sync of the current task files:
    - `bash tests/run-live-cgroups-win.sh`
    - result:
      - `32 passed, 0 failed`
      - full `4 x 4` producer/client matrix across:
        - `c-native`
        - `c-msys`
        - `go-native`
        - `rust-native`
      - each in both `refresh once` and `refresh loop` modes
- Important fact exposed by this slice:
  - the fake-service runtime matrix is now strong enough to close the current Phase 5 interop goal for the fake snapshot/cache service on baseline transports
  - the remaining Rust Windows-target manifest gap is still limited to the older POSIX-only `netipc_live_rs` fixture binary and does not affect the fake-service CLI used here
- Remaining work after this slice:
  - Phase 6 baseline performance coverage is now the next active phase:
    - snapshot refresh path
    - local cache lookup hot path
    - cross-language producer/client benchmark coverage on baseline transports
  - SHM redesign remains deferred to Phase 7 exactly as planned

## Implied Implementation Choice: Phase 6 initial benchmark scenarios (2026-03-12)
- Context:
  - the approved phase plan requires baseline performance coverage for:
    - snapshot refresh path
    - local cache lookup hot path
  - the user did not freeze exact rate-limited targets for this new fake-service benchmark family
- Implementation choice:
  - the first benchmark scripts will ship with two default scenarios for both refresh and local lookup paths:
    - `max` (`target_rps = 0`)
    - `1000/s`
  - the scripts will keep these scenario sets configurable via environment variables so they can be widened later without rewriting the helper binaries
- Rationale:
  - this follows the existing repo pattern of pairing `max` with at least one fixed-rate scenario
  - `1000/s` is conservative enough for a first snapshot/cache benchmark while still giving rate-limited latency/CPU signal

## Phase 6 Progress Slice: baseline performance coverage completed (2026-03-12)
- Facts:
  - baseline benchmark workflows now exist for the fake `cgroups` snapshot/cache service on both OS families:
    - Linux baseline script: `tests/run-live-cgroups-bench.sh`
    - Windows baseline script: `tests/run-live-cgroups-win-bench.sh`
  - the benchmark helpers now exist in all three language fixture sets:
    - C:
      - `server-bench`
      - `client-refresh-bench`
      - `client-lookup-bench`
    - Go:
      - `server-bench`
      - `client-refresh-bench`
      - `client-lookup-bench`
    - Rust:
      - `server-bench`
      - `client-refresh-bench`
      - `client-lookup-bench`
  - the build-system bug that previously left the Go fixture binary stale when new Go fixture source files were added is now fixed in `CMakeLists.txt` by tracking the full Go fixture source set as explicit dependencies.
  - the Windows fake-service smoke-script wait-path bug is fixed in `tests/run-live-cgroups-win.sh`.
  - Windows disconnect handling is now normalized for the fake snapshot/cache runtime paths in:
    - Go named-pipe transport:
      - `ERROR_BROKEN_PIPE`
      - `ERROR_NO_DATA`
      - `ERROR_PIPE_NOT_CONNECTED`
    - Rust named-pipe transport:
      - `ERROR_BROKEN_PIPE`
      - `ERROR_NO_DATA`
      - `ERROR_PIPE_NOT_CONNECTED`
- Validation completed for this slice:
  - Linux:
    - `bash tests/run-live-cgroups-bench.sh`
    - result:
      - full directed refresh matrix for `C/Go/Rust`
      - local lookup self/self benchmark for `C/Go/Rust`
    - repo-level workflow validation:
      - `cmake --build build --target netipc-bench-live-cgroups`
      - `cmake --build build --target netipc-bench`
      - result:
        - the new cgroups benchmark is now fully integrated into the benchmark workflow umbrella, not only runnable as a standalone script
  - additional local sanity:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - `go test ./...` from `src/go`
    - `GOOS=windows GOARCH=amd64 go test ./...` from `src/go`
    - `cargo check --manifest-path tests/fixtures/rust/Cargo.toml --bin netipc-codec-rs --target x86_64-pc-windows-gnu`
  - Windows on `win11`:
    - `bash tests/run-live-cgroups-win-bench.sh`
    - result:
      - full directed refresh matrix across:
        - `c-native`
        - `c-msys`
        - `go-native`
        - `rust-native`
      - local lookup self/self benchmark for the same four implementations
- Key measured facts:
  - Linux refresh max throughput reaches the low-hundreds-of-thousands requests/s class on the best producer/client pairs:
    - `c -> rust`: about `201k req/s`
    - `rust -> rust`: about `206k req/s`
    - `go -> go`: about `119k req/s`
  - Linux local lookup max throughput is in the multi-million lookups/s class:
    - `c -> c`: about `25.3M lookups/s`
    - `rust -> rust`: about `23.0M lookups/s`
    - `go -> go`: about `13.8M lookups/s`
  - Linux rate-limited `1000/s` scenarios hold the target cleanly.
  - Windows refresh max throughput is much lower for this first fake snapshot/cache service:
    - roughly `37-53 req/s` across the directed matrix on `win11`
  - Windows local lookup max throughput splits into two classes:
    - `go-native`: about `26.1M lookups/s`
    - `c-native`, `c-msys`, `rust-native`: about `69k-75k lookups/s`
  - Windows rate-limited `1000/s` local lookup scenarios hold the target cleanly.
- Working theory (explicit speculation):
  - the very low Windows refresh throughput is more likely a property of the current fake full-snapshot refresh/rebuild path on baseline transports than a proof that the underlying transport methodology is invalid.
  - this needs later stress-testing during the SHM redesign phase and when a less synthetic refresh path exists.
- Important fact exposed by this slice:
  - Phase 6 baseline performance coverage is now complete for the approved fake snapshot/cache service methodology on baseline transports.
  - the next active phase is Phase 7:
    - SHM redesign for the real protocol and snapshot publication/consumption model.

71. Phase 7 SHM redesign scope:
   - Status: approved on 2026-03-13 after Phase 6 baseline coverage completed.
   - Clarification added on 2026-03-13:
     - the goal is not to maintain two separate user-visible SHM products.
     - the desired end state is one SHM subsystem that, if realistic, can satisfy both:
       - generic request/response IPC
       - server-owned snapshot publication with client-side refresh/cache rebuild
     - the remaining question is whether a single SHM design can serve both well, or whether the first implementation phase should optimize for one flow first and extend later.
   - Context:
     - the current POSIX SHM path still exposes one fixed-size request slot and one fixed-size response slot in `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`.
     - the current Windows SHM path mirrors the same fixed-frame single-request/single-response shape in `src/crates/netipc/src/transport/windows.rs`.
     - both SHM paths still expose `receive_increment` / `send_increment` / `call_increment`-style APIs.
     - the approved fake `cgroups` snapshot/cache service is different:
       - payloads are variable-size
       - the server owns snapshot size and layout
       - the client refreshes a local cache from that server-owned payload
   - Evidence:
     - POSIX SHM region layout:
       - `request_frame[NETIPC_FRAME_SIZE]`
       - `response_frame[NETIPC_FRAME_SIZE]`
       - in `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
     - Windows SHM region layout:
       - request slot and response slot sized as `FRAME_SIZE`
       - in `src/crates/netipc/src/transport/windows.rs`
     - baseline fake service methodology is now validated on baseline transports and is the active next transport target.
   - Options:
     - Option A:
       - redesign SHM first as a snapshot-publication transport for cache-backed services
       - server owns one published snapshot region per service
       - clients refresh by consuming that published snapshot
       - Pros:
         - best fit for the real `cgroups -> ebpf` replacement target
         - directly addresses the approved fake service methodology
         - avoids forcing snapshot publication into a per-request session model first
       - Cons:
         - generic request/response SHM still comes later
       - Implications:
         - Phase 7 stays tightly aligned with the current fake `cgroups` service and the first real Netdata target
       - Risks:
         - later generic RPC SHM may still need a second SHM subphase
     - Option B:
       - redesign SHM first as a generic per-session variable-message transport
       - snapshot services keep using the normal request/response/session model on top of it
       - Pros:
         - more generic foundation
         - one SHM path for all service types
       - Cons:
         - weaker fit for server-owned snapshot publication
         - more complex for the first real SHM target
       - Implications:
         - snapshot/cache service continues to behave like a full refresh RPC over SHM
       - Risks:
         - may fail to deliver the main snapshot/publication benefit we actually want
     - Option C:
       - implement both SHM modes in Phase 7:
         - snapshot-publication SHM for cache-backed services
         - generic per-session variable-message SHM for normal RPC
       - Pros:
         - covers the whole design immediately
       - Cons:
         - by far the biggest scope jump
       - Implications:
         - Phase 7 becomes the hardest implementation phase by a wide margin
       - Risks:
         - highest chance of delay and rework
     - Option D:
       - redesign SHM once around a single generic publication/buffer model that can satisfy both:
         - generic request/response IPC
         - server-owned snapshot publication and client-side refresh
       - Pros:
         - one SHM subsystem
         - avoids architectural split
         - directly addresses the user's challenge that both patterns should ideally share one design
       - Cons:
         - requires a stronger first design than the old single-slot ping-pong model
         - likely needs a control-plane/data-plane split inside the same SHM subsystem
       - Implications:
         - Phase 7 becomes a unified SHM redesign instead of a mode-first redesign
       - Risks:
         - if the generic model is too abstract or too slow, it could underperform both use cases
   - Recommendation:
     - Option D
   - Reason:
     - shared memory itself does not force two products; the real requirement is to replace the current fixed-slot ping-pong layout with one generic shared-data model that can carry both RPC responses and published snapshots.
   - User decision:
     - Option D approved on 2026-03-13.
   - Decision:
     - Phase 7 will implement one unified SHM subsystem.
     - This SHM subsystem must be able to satisfy both:
       - generic request/response IPC
       - server-owned snapshot publication with client-side refresh/cache rebuild
     - The redesign must avoid introducing two separate user-visible SHM products.
   - Implication:
     - the next SHM work should focus on a shared control/data model, not on separate snapshot-mode and RPC-mode products.

72. Derived unified SHM control/data model:
   - Status: derived from approved Decision #71 on 2026-03-13.
   - Context:
     - the current SHM layouts are still fixed-frame single-slot ping-pong regions:
       - POSIX: `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
       - Windows: `src/crates/netipc/src/transport/windows.rs`
     - the approved real protocol is variable-length, directional, and supports:
       - normal request/response calls
       - server-owned snapshot publication with client-side refresh/cache rebuild
     - the fake `cgroups` helper already models the response side as server-owned and sized from negotiated response limits:
       - `src/libnetdata/netipc/src/service/netipc_cgroups_snapshot.c`
   - Evidence:
     - POSIX SHM region still hardcodes:
       - `request_frame[NETIPC_FRAME_SIZE]`
       - `response_frame[NETIPC_FRAME_SIZE]`
     - Windows SHM region still mirrors the same request-slot / response-slot model.
     - official SHM APIs are generic shared-memory primitives with user-defined synchronization:
       - POSIX `shm_overview(7)`
       - Windows file mapping documentation
   - Derived design:
     - SHM will be redesigned as one unified subsystem with:
       - one shared control header
       - one client-owned variable request publication area
       - one server-owned variable response publication area
     - each direction publishes at most one variable-length payload at a time in the first implementation slice
     - publication metadata per direction will include:
       - generation / sequence
       - published byte length
       - transport status
       - optional close / waiting hints
     - the payload bytes will contain the already-approved real protocol envelope:
       - outer header
       - optional batch item refs
       - self-contained method payloads
     - request/response and snapshot refresh therefore use the same SHM pattern:
       - client publishes request bytes in the request publication area
       - server consumes them
       - server publishes response or snapshot bytes in the response publication area
       - client consumes them
   - Implication:
     - the first SHM slice does not need a separate “snapshot mode”
     - snapshot/cache services will simply use the same publication model with server-owned response bytes
   - Risk:
     - this first slice still supports only one in-flight publication per direction
     - if later low-level pipelining is required on SHM, the control plane will need extension rather than another redesign

73. Phase 7 first implementation slice:
   - Status: derived on 2026-03-13 before coding started.
   - Scope:
     - implement the unified SHM publication model first in the C POSIX SHM path
     - keep the old `increment` wrappers as compatibility helpers on top of the new generic message flow during migration
     - use the same negotiated directional limits already approved in Decision #64
   - Reason:
     - the C fake `cgroups` helper and current SHM code live here first
     - this gives the fastest honest end-to-end validation path before porting the same model to Windows, Rust, and Go
   - Implication:
     - Phase 7 will be delivered in internal slices, but all slices must conform to the same unified SHM model
     - the first slice is not a design fork; it is only the implementation order
   - Execution record:
     - Implemented on 2026-03-13 in the C POSIX SHM path:
       - `src/libnetdata/netipc/include/netipc/netipc_shm_hybrid.h`
       - `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
       - `src/libnetdata/netipc/src/transport/posix/netipc_uds_seqpacket.c`
     - The old fixed-frame direct SHM slots were replaced with:
       - one shared control header
       - one variable-length client-owned request publication area
       - one variable-length server-owned response publication area
     - Compatibility kept:
       - old `increment` wrappers still exist on top of the new generic byte-message path
       - UDS negotiation now upgrades to the new SHM message path for the C POSIX slice
   - Root cause found and fixed during validation:
     - the first live SHM loop runs exposed an intermittent client-side failure during SHM endpoint creation
     - concrete evidence:
       - the client could open the `.ipcshm` file in the small window after `open(O_CREAT|O_EXCL)` and before the server had `ftruncate()`d and populated the region header
       - in that state `fstat()` succeeded with size `0`, and the client path leaked a stale socket `errno` (`EAGAIN`) instead of reporting a retryable protocol-not-ready state
     - fix applied:
       - treat undersized freshly created SHM files as `EPROTO` / protocol-not-ready
       - this keeps `create_shm_client_with_retry()` on the intended retry path until the server finishes SHM region setup
   - Validation:
     - focused reproducer:
       - `100` consecutive SHM client/server loop runs passed after the fix
     - repo scripts:
       - `tests/run-live-cgroups-shm.sh` passed repeatedly (`20` consecutive runs)
       - `tests/run-live-interop.sh` passed with the full direct `C <-> Rust` SHM matrix
       - `tests/run-live-uds-interop.sh` passed with the negotiated `profile=2` `C <-> Rust` SHM matrix
     - full repo validation:
       - Rust: `cargo test --manifest-path src/crates/netipc/Cargo.toml`
       - Go: `go test ./...`
       - Go cross-build: `GOOS=windows GOARCH=amd64 go test ./...`
       - CTest: `/usr/bin/ctest --test-dir build --output-on-failure` passed `7/7`
   - Execution record (Rust POSIX widening):
     - Implemented on 2026-03-13 in the Rust POSIX SHM path:
       - `src/crates/netipc/src/transport/posix.rs`
     - The Rust POSIX SHM transport now uses the same unified control/data model as the C POSIX slice:
       - shared control header
       - variable-length client-owned request publication area
       - variable-length server-owned response publication area
     - Compatibility kept:
       - old `increment` wrappers still exist on top of the new generic byte-message path
       - UDS negotiation now upgrades to the new SHM message path for `C <-> Rust` as well
     - Live validation restored:
       - direct SHM matrix: `tests/run-live-interop.sh` now covers `C->C`, `C->Rust`, `Rust->C`, and `Rust->Rust`
       - negotiated SHM matrix: `tests/run-live-uds-interop.sh` now covers `profile=2` for `C->C`, `C->Rust`, `Rust->C`, and `Rust->Rust`
   - Execution record (Windows widening):
     - Implemented on 2026-03-13 in the Windows SHM paths:
       - `src/libnetdata/netipc/src/transport/windows/netipc_shm_hybrid_win.c`
       - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
       - `src/crates/netipc/src/transport/windows.rs`
       - `src/go/pkg/netipc/transport/windows/pipe.go`
     - The C, Rust, and Go Windows transports now use the same unified SHM header/control model:
       - dynamic request/response publication capacities
       - directional negotiated message-size ceilings
       - generic byte-message SHM calls
       - legacy fixed-frame `increment` wrappers preserved on top for compatibility
     - Root cause found and fixed during live validation:
       - first `win11` smoke after the SHM port showed that:
         - `Rust <-> Rust`, `Go <-> Go`, and `Rust <-> Go` all passed on `profile=2`
         - every remaining mixed `C <-> Rust/Go` `profile=2` failure broke with either:
           - `message size does not match header`
           - `invalid SHM frame length`
           - server/client timeouts after a broken receive path
       - concrete cause:
         - Rust and Go had moved legacy frame compatibility through the new generic message validators, so old 64-byte `increment` frames were being parsed as variable-message envelopes
         - after that was fixed, a second mixed-language failure remained:
           - the new Windows SHM header offsets did not match across languages
           - C defined the header layout with:
             - `spin_tries` at offset `32`
             - `req_len` at offset `36`
             - `resp_len` at offset `40`
           - Rust and Go were incorrectly using:
             - `req_len` at `32`
             - `resp_len` at `36`
             - `spin_tries` at `40`
       - fixes applied:
         - Rust and Go now keep raw frame compatibility on SHM separate from variable-message validation
         - Rust and Go SHM constants were aligned to the C header layout
         - C now has `_Static_assert(offsetof(...))` checks for the critical Windows SHM header fields to prevent future drift
     - Validation:
       - local compile sanity:
         - Rust: `cargo check --manifest-path src/crates/netipc/Cargo.toml --target x86_64-pc-windows-gnu`
         - Go: `GOOS=windows GOARCH=amd64 go test ./...`
       - real Windows runtime on `win11`:
         - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
         - `go test ./...`
         - `bash tests/smoke-win.sh` passed with `32 passed, 0 failed`
       - the fake `cgroups` methodology is now validated on Windows across both:
         - baseline Named Pipe
         - unified SHM
         - `bash tests/run-live-cgroups-win.sh` passed with `64 passed, 0 failed`
       - the fake `cgroups` benchmark coverage is now also validated on real Windows across both:
         - baseline Named Pipe
         - unified SHM
         - `bash tests/run-live-cgroups-win-bench.sh` passed on `win11` after the Go teardown fixes below
     - Additional real issue found and fixed during the Windows fake `cgroups` SHM widening:
       - the first real `win11` SHM benchmark rerun exposed a lifecycle-only bug in the Go helper path:
         - `go-native` server + `c-native` client + `shm-hybrid`
         - the benchmark payloads and throughput were correct, but the Go server exited with `server-loop failed: client closed`
       - concrete causes:
         - the high-level Go `cgroupssnapshot.Client` helper did not expose `Close()`, so benchmark/smoke clients could exit without explicitly closing the underlying transport
         - the Go Windows SHM server-side receive path returned a plain `errors.New("client closed")`, which the benchmark fixture did not classify as a graceful disconnect
       - fixes applied:
         - `src/go/pkg/netipc/service/cgroupssnapshot/client.go`
           - added `Client.Close()`
           - `disconnectTransport()` now uses the same close path
         - `tests/fixtures/go/main.go`
           - all fake `cgroups` client commands now `defer client.Close()`
         - `tests/fixtures/go/cgroups_bench.go`
           - both refresh and lookup benchmark clients now `defer client.Close()`
         - `src/go/pkg/netipc/transport/windows/pipe.go`
           - the SHM server receive path now returns `io.EOF` on client-close instead of a plain string error
       - implication:
         - Windows fake `cgroups` runtime and benchmark teardown now use structured graceful-disconnect semantics across C, Rust, and Go instead of relying on ad-hoc string matching
   - Current status:
     - the unified SHM model is now validated for the legacy fixed-frame compatibility path in:
       - POSIX:
         - C
         - Rust
       - Windows:
         - C native
         - C msys
         - Rust native
         - Go native
     - the approved snapshot/cache methodology is also validated on Windows baseline transports with:
       - C native producer/client
       - C msys producer/client
       - Rust native producer/client
       - Go native producer/client
     - the approved snapshot/cache methodology is now also validated on Windows over the unified SHM path with the same four producer/client implementations
     - the remaining practical gap is narrower now:
       - the fake `cgroups` helper on Windows still defaults to Named Pipe unless profiles are overridden
       - public helper/profile selection policy still needs to be finalized later; the runtime methodology itself is now validated on both Windows baseline and SHM paths

74. Go POSIX SHM implementation strategy:
   - Status: pending user decision discovered on 2026-03-13 while widening the Linux fake `cgroups` SHM matrix from `C -> C` to `C/Rust/Go`.
   - Context:
     - the widened Linux fake `cgroups` SHM smoke immediately exposed that the remaining gap is no longer in the fake snapshot/cache helper layer.
     - the actual gap is the underlying Go POSIX transport:
       - the Linux SHM fake `cgroups` matrix currently passes for `C` and `Rust`
       - the first `Go` participant fails at connection/negotiation time with `operation not supported`
   - Evidence:
     - `tests/run-live-cgroups-shm.sh` was widened from `C -> C` only to a `C/Go/Rust` producer-client matrix
     - targeted repro:
       - `C server + Go client` with `NETIPC_SUPPORTED_PROFILES=2 NETIPC_PREFERRED_PROFILES=2`
       - client error: `client-refresh-once failed: operation not supported`
       - `Go server + C client` with the same profile override:
         - server error: `server-once failed: operation not supported`
         - client side then sees `Connection reset by peer`
     - current Go POSIX transport facts:
       - `src/go/pkg/netipc/transport/posix/seqpacket.go`
         - declares `ProfileSHMHybrid`
         - but `implementedProfiles = ProfileUDSSeqpacket`
         - so profile `2` is still not implemented on the Go POSIX path
     - current C and Rust POSIX SHM facts:
       - C uses unnamed shared-memory POSIX semaphores (`sem_t`) embedded in the shared region:
         - `src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`
           - `sem_t req_sem;`
           - `sem_t resp_sem;`
           - `sem_init`, `sem_post`, `sem_timedwait`, `sem_destroy`
       - Rust mirrors the same shared-region semaphore model through `libc::sem_t`:
         - `src/crates/netipc/src/transport/posix.rs`
   - Problem:
     - to make Go interoperate with the already-implemented unified POSIX SHM model, the Go Unix transport needs access to the same unnamed process-shared semaphore primitives (`sem_init`, `sem_post`, `sem_timedwait`, `sem_destroy`) used by C and Rust.
     - pure-Go `syscall` / `x/sys/unix` do not expose those POSIX semaphore functions directly.
     - Additional researched facts:
       - Go and `x/sys/unix` do expose raw Linux futex and eventfd syscalls at the syscall-number level:
       - local evidence:
         - `/usr/lib/go/src/syscall/zsysnum_linux_amd64.go` defines `SYS_FUTEX`
         - `/home/costa/go/pkg/mod/golang.org/x/sys@v0.41.0/unix/zsysnum_linux_amd64.go` defines `SYS_FUTEX`, `SYS_FUTEX_WAIT`, `SYS_FUTEX_WAKE`, and `SYS_EVENTFD2`
       - primary references:
         - `futex(2)` / `futex(7)` on `man7.org`
         - `eventfd(2)` on `man7.org`
     - other shared-memory IPC projects do not solve this by embedding POSIX unnamed semaphores into the shared memory region:
       - `/tmp/shmipc-go-purego/README.md`
         - pure-Go Go SHM IPC uses Unix/TCP synchronization plus shared memory queues
       - `/tmp/shmem-ipc-rs/src/sharedring.rs`
         - Rust shared-memory ring buffer uses `eventfd` for signaling, not `sem_t`
       - `/tmp/iceoryx-shm/doc/website/advanced/iceoryx-on-32-bit.md`
         - explicitly notes that production spin primitives should become `futex` on Linux and `WaitOnAddress` on Windows
       - futex is a Linux userspace API, not a portable Unix API:
         - primary reference: `futex(2)` / `futex(7)` on `man7.org`
       - `eventfd` is also Linux-specific:
         - primary reference: `eventfd(2)` on `man7.org`
       - local syscall metadata shows System V semaphore syscalls exist across Linux, FreeBSD, and Darwin targets in `x/sys/unix`, while futex does not provide that same portable Unix story
   - Decision needed:
     - we need one synchronization primitive for the Unix SHM path that:
       - is usable from C, Rust, and pure Go
       - does not require `cgo`
       - still fits the approved single unified SHM design instead of creating a Go-only fork
   - Options:
     - A. Redesign Linux POSIX SHM synchronization around shared-memory futex words
       - Pros:
         - pure Go can use `SYS_FUTEX` via raw syscalls
         - C and Rust can use the same futex words directly
         - stays inside the shared memory region, so it still matches the unified SHM design
         - maps conceptually well to Windows `WaitOnAddress`
       - Cons:
         - Linux-specific, not generic POSIX
         - requires reworking already-working C and Rust Linux SHM synchronization
       - Implications:
         - the Unix SHM implementation becomes Linux-first instead of generic-POSIX-first
         - `ProfileSHMFutex` can become the real Linux SHM profile and replace the current semaphore-backed path
       - Risks:
         - medium implementation risk
         - low long-term drift risk
     - B. Replace embedded `sem_t` with external System V semaphores
       - Pros:
         - pure Go can drive `semget` / `semop` / `semtimedop` via raw syscalls
         - C and Rust can also interoperate with them
       - Cons:
         - synchronization state moves outside the shared memory region
         - lifecycle/cleanup becomes more complex
         - weaker fit for the approved unified SHM design
       - Implications:
         - SHM would depend on extra kernel semaphore objects keyed outside the mapped region
       - Risks:
         - medium to high operational complexity
         - more cleanup corner cases
     - C. Keep the current semaphore-backed Linux SHM and leave Go POSIX SHM unsupported
       - Pros:
         - no redesign now
       - Cons:
         - Linux `C/Rust/Go` SHM methodology remains incomplete
         - contradicts the goal of complete cross-language coverage
       - Implications:
         - Go would stay baseline-only on Linux while Windows supports SHM
       - Risks:
         - high product inconsistency and permanent documentation complexity
     - Recommendation:
       - A. Redesign Linux POSIX SHM synchronization around futex words.
       - Reason:
         - it is the only option that stays pure-Go, preserves one shared-memory control model, and gives us a clean conceptual bridge to Windows `WaitOnAddress` instead of growing another semaphore subsystem.
   - Clarification:
     - `A` is a Linux-first solution.
     - it will not give us one shared Unix SHM implementation for macOS, FreeBSD, and Linux.
     - if the requirement is one pure-Go Unix SHM methodology across Linux + FreeBSD + macOS, we need a different decision than `A`.

75. Unix SHM portability target:
   - Status: resolved by user decision on 2026-03-13.
   - Context:
     - the approved SHM architecture is one unified SHM subsystem and one unified protocol/control model.
     - that does not require one identical kernel synchronization primitive on every Unix.
     - the real question is whether Unix SHM itself must be first-class on:
       - Linux
       - FreeBSD
       - macOS
   - Evidence:
     - `74.A` relies on `futex`, which is Linux-specific:
       - see `TODO-plugin-ipc.md` Decision `74`
       - primary reference: `futex(2)` / `futex(7)` on `man7.org`
     - `eventfd` is also Linux-specific:
       - primary reference: `eventfd(2)` on `man7.org`
     - local syscall metadata shows System V semaphore syscalls across Linux, FreeBSD, and Darwin targets in `x/sys/unix`, while futex does not give that same portable Unix story
   - Decision:
     - implement Unix SHM for Linux
     - FreeBSD and macOS fall back to UDS for now
   - Implication:
     - the unified SHM protocol/layout remains the design target
     - the first Unix SHM backend is Linux-only
     - FreeBSD/macOS stay baseline-only until a native pure-Go backend is justified later
   - Options:
     - A. Linux-first SHM
       - Meaning:
         - Linux gets unified SHM using futex
         - macOS and FreeBSD stay baseline-only for now
       - Pros:
         - fastest path to finish
         - lowest short-term implementation risk
       - Cons:
         - Unix SHM is not portable
       - Implications:
         - docs/tests must explicitly say SHM-on-Unix means Linux only
       - Risks:
         - medium design debt if macOS/FreeBSD matter later
     - B. Force one portable Unix synchronization primitive for SHM
       - Meaning:
         - choose a single primitive that can work across Linux + FreeBSD + macOS, likely external semaphores rather than futex
       - Pros:
         - one Unix synchronization story
       - Cons:
         - likely weaker fit than futex on Linux
         - higher complexity and cleanup burden
         - risks pulling the design away from the current shared-memory control model
       - Implications:
         - Linux gives up its best native primitive for portability
       - Risks:
         - medium to high performance/complexity risk
     - C. One unified SHM API/layout, but OS-specific synchronization backends
       - Meaning:
         - same public SHM behavior, same control/data layout, same protocol
         - Linux uses futex
         - FreeBSD/macOS use their best pure-Go-usable backend
       - Pros:
         - preserves one product design
         - preserves Linux performance
         - keeps portability possible without forcing a bad common denominator
       - Cons:
         - more implementation work
         - more OS-specific testing
       - Implications:
         - the sync backend becomes a platform detail, not a protocol detail
       - Risks:
         - medium implementation risk
   - User choice:
     - A. Linux-first SHM.
   - Derived implementation constraint:
     - keep the public Unix fast SHM profile as `profile 2` / `SHM_HYBRID`
     - replace only the Linux synchronization backend under that profile
     - use one shared control/data layout for all Unix builds:
       - replace embedded `sem_t` objects with shared `req_signal` / `resp_signal` words
       - Linux waits/wakes those words with futex
       - non-Linux Unix builds stop advertising SHM and stay on UDS
   - Reason:
     - the current docs, scripts, helper layer, and validation already treat `profile 2` as the fast Unix SHM path
     - changing the public profile now would create unnecessary spec drift

54. Negotiated payload and batch limits:
   - Status: historical, superseded by Decision #64 on 2026-03-12.
   - Source: early user decision on 2026-03-12 before the handshake ownership model was fully stress-tested against snapshot/cache services.
   - Decision:
     - `max_payload_bytes` refers to one single request payload or one single response payload
     - default `max_payload_bytes` should be `1024`
     - `max_batch_items` remains a negotiated limit
     - `max_batch_bytes` should be derived from `max_payload_bytes * max_batch_items`
   - Implication:
     - the `1024` default remains valid
     - the symmetric handshake reading here is no longer authoritative; Decision #64 replaced it with directional request/response limits and per-direction derived batch bytes

55. Go representation for zero-allocation string views:
   - Source: user decision on 2026-03-12 (`18.B`).
   - Decision:
     - Go should expose a dedicated wrapper type such as `StringView` / `CStringView` for decoded ephemeral borrowed string fields
     - Go should not expose normal `string` values by default for decoded zero-allocation views
   - Implication:
     - the Go API itself will signal borrowed/view semantics instead of looking like an ordinary durable string API
     - explicit copy helpers can exist for callers that need owned `string` values outside the current call/callback lifetime
- 15. Per-method payload layout discipline.
  - Resolved by Decisions #47, #49, #50, #53, and the derived protocol sketch:
    - each method payload is self-contained
    - each method uses a small fixed method-local header plus method-local scalar fields and `offset + length` members for variable data
- 16. String field representation inside method payloads.
  - Resolved by Decision #50 (`Option A`):
    - strings are represented by `offset + length`
    - the pointed bytes must also end with `\\0`
- 17. Decoded payload/view lifetime model.
  - Resolved by Decisions #48, #51, #52, and #56:
    - decoded request/response objects are non-owning ephemeral views
    - they are valid only during the current library call / callback
- 18. Go representation for zero-allocation string views.
  - Resolved by Decision #55 (`Option B`).
- 19. High-level API shape under the strict ephemeral-view lifetime rule.
  - Resolved by Decision #56 (`Option A`).

56. High-level zero-copy API shape under the strict ephemeral-view lifetime rule:
   - Source: user decision on 2026-03-12 (`19.A`).
   - Decision:
     - high-level zero-copy APIs must be callback-based
     - the library invokes the caller callback with the decoded response view while that view is still valid
     - high-level APIs must not return ephemeral decoded views directly to their callers
   - Implication:
     - callers must consume or copy response data inside the callback
     - explicit copy/materialize helpers may still exist as slower convenience paths
     - the strict ephemeral lifetime rule remains honest and enforceable in C, Rust, and Go

57. First concrete method schema for Phase 2:
   - Status: pending user decision discovered during Phase 1 implementation on 2026-03-12.
   - Context:
     - executable code still only implements the synthetic `increment` method
     - the `ip_to_asn` method in this TODO is still an illustrative design example, not a frozen production schema
   - Options:
     - Option A:
       - freeze the real `ip_to_asn` v1 request/response schema now and use it as the first concrete method implementation
     - Option B:
     - introduce a temporary synthetic string-bearing method only to build the generic view/builder machinery, then replace it later with the first real production method
     - Option C:
       - keep Phase 2 deferred for now and continue Phase 3 transport migration on top of the compatibility `increment` method until the first real production method schema is frozen

58. Cache-backed high-level service helpers:
   - Status: pending user decision raised on 2026-03-12 while evaluating whether `plugin-ipc` should replace the existing `cgroups.plugin -> ebpf.plugin` channel.
   - Context:
     - the existing `ebpf <-> cgroups` integration is a periodically refreshed shared snapshot that `ebpf.plugin` turns into a local cache
     - the cached state is then reused across many hot paths instead of making a request for every lookup
     - evidence:
       - producer populates the shared snapshot in [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c)
       - consumer refreshes local cached state in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c)
       - hot paths later consume that cached state, for example in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_process.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_process.c)
   - Options:
     - Option A:
       - keep the library generic and expose only transport/service primitives; each plugin owns any local cache it needs
     - Option B:
       - add a higher-level library layer for cache-backed services, so callers can use helpers such as `refresh_cgroups()` and `lookup_cgroup()`
     - Option C:
       - support both, with cache-backed helpers built strictly on top of the generic client/service layer
   - User decision:
     - Option C approved on 2026-03-12
   - Decision:
     - `plugin-ipc` keeps a generic typed client/service core
     - the library also provides optional cache-backed high-level helpers for snapshot-style services
     - cache-backed helpers must be implemented strictly on top of the generic client/service layer, not as a separate transport path
   - Implication:
     - lookup-style services and snapshot/cache-style services can coexist without forcing one model on all providers
     - `cgroups.plugin -> ebpf.plugin` becomes a valid target use case for the helper layer
     - layering must stay strict so cache helpers cannot drift into a second IPC implementation

59. No Netdata-repo integration before a fully tested fake producer exists:
   - Source: user decision on 2026-03-12.
   - Decision:
     - before touching the Netdata repository for real integration work, `plugin-ipc` must first gain a fake producer with dummy data and full tests
     - the fake producer must exercise the intended snapshot/cache-backed service flow end-to-end inside this repository
   - Implication:
     - the first implementation target is a self-contained fake service/provider plus client-side cache helper and tests
     - Netdata-agent integration stays blocked until the fake service validates the protocol, API, cache lifecycle, and test coverage

60. First fake producer/service target before Netdata integration:
   - Status: approved on 2026-03-12 after Decision #59.
   - Context:
     - this repository currently has only `increment`-based fixtures, helpers, and tests
     - evidence:
       - C fixture/codec tool: [`/home/costa/src/plugin-ipc.git/tests/fixtures/c/netipc_live_c.c`](/home/costa/src/plugin-ipc.git/tests/fixtures/c/netipc_live_c.c), [`/home/costa/src/plugin-ipc.git/tests/fixtures/c/netipc_codec_tool.c`](/home/costa/src/plugin-ipc.git/tests/fixtures/c/netipc_codec_tool.c)
       - Go fixture: [`/home/costa/src/plugin-ipc.git/tests/fixtures/go/main.go`](/home/costa/src/plugin-ipc.git/tests/fixtures/go/main.go)
       - Rust fixture: [`/home/costa/src/plugin-ipc.git/tests/fixtures/rust/src/bin/netipc_live_rs.rs`](/home/costa/src/plugin-ipc.git/tests/fixtures/rust/src/bin/netipc_live_rs.rs)
       - transport APIs are still `receive_increment` / `send_increment` / `call_increment` oriented in C, Rust, and Go
     - the newly approved cache-backed helper layer should be validated with a fake producer before any Netdata-repo integration
   - Options:
     - Option A:
       - make the first fake service a `cgroups`-style snapshot/cache service with dummy cgroup/container records and a cache-backed client helper
     - Option B:
       - make the first fake service an `ip_to_asn`-style request/response lookup service, and postpone cache-backed snapshot validation
     - Option C:
       - make the first fake service a tiny synthetic snapshot service unrelated to real Netdata domains, just to validate the cache/helper mechanics before the real schema is chosen
   - User decision:
     - Option A approved on 2026-03-12
   - Decision:
     - the first fake producer/service in this repository will be a `cgroups`-style snapshot/cache service with dummy cgroup/container records
     - it must include a cache-backed client helper and full end-to-end tests before any Netdata-repo integration work begins
   - Implication:
     - the next implementation target is a fake snapshot producer plus cache refresh/lookup helper, not `ip_to_asn`
     - the first real method family and test fixtures should validate snapshot refresh, local cache rebuild/use, and dummy cgroup/container lookups

61. Fake `cgroups` snapshot schema scope:
   - Status: approved on 2026-03-12.
   - Context:
     - the first fake service is now a `cgroups`-style snapshot/cache service
     - the current Netdata SHM layout contains transport/internal fields and historical details that should not be copied blindly into the new public service contract
   - Options:
     - Option A:
       - mirror the current Netdata SHM layout closely
     - Option B:
       - define a cleaned public snapshot contract for `plugin-ipc`
     - Option C:
       - keep a cleaned public contract plus optional compatibility/debug fields
   - User decision:
     - Option B approved on 2026-03-12
   - Decision:
     - the fake `cgroups` snapshot service should use a cleaned public `plugin-ipc` contract, not a direct copy of the current Netdata SHM structs
   - Implication:
     - the fake service validates the new library design rather than preserving historical SHM baggage
     - later Netdata integration may still require an adaptation layer from current producer data to the cleaned service contract

62. Snapshot-style services must not derail core transport/protocol implementation:
   - Source: user clarification on 2026-03-12.
   - Decision:
     - underlying protocol/transport implementation, correctness testing, and performance coverage remain the primary priority
     - the new cache-backed snapshot helper layer is a consumer of that foundation, not a replacement for it
   - Fact:
     - snapshot-style services invert data-shape ownership compared to the current request-sized flow:
       - for lookup/request-response traffic, the client shapes the request payload
       - for snapshot publication, the server determines item count, packed payload sizes, and total snapshot size
   - Implication:
     - the protocol and transport layers must support both client-shaped request payloads and server-shaped snapshot payloads cleanly
     - the fake snapshot service should be used to stress the generic protocol implementation, not to bypass it

63. Transport scope of the first fake snapshot/cache service:
   - Status: approved on 2026-03-12 via the strict phased execution plan.
   - Context:
     - the current reusable transport layer is still effectively `increment`-shaped in C, Rust, and Go
     - baseline transports (`UDS_SEQPACKET` / `Named Pipe`) already carry discrete messages naturally
     - current SHM implementations are still single-slot fixed-frame paths and do not yet model server-owned variable-size snapshot publication cleanly
     - evidence:
       - C API still exposes `receive_increment` / `send_increment` / `call_increment` in [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h)
       - Rust POSIX transport still exposes `receive_increment` / `send_increment` / `call_increment` in [`/home/costa/src/plugin-ipc.git/src/crates/netipc/src/transport/posix.rs`](/home/costa/src/plugin-ipc.git/src/crates/netipc/src/transport/posix.rs)
       - Go POSIX transport still exposes `ReceiveIncrement` / `SendIncrement` / `CallIncrement` in [`/home/costa/src/plugin-ipc.git/src/go/pkg/netipc/transport/posix/seqpacket.go`](/home/costa/src/plugin-ipc.git/src/go/pkg/netipc/transport/posix/seqpacket.go)
       - current SHM implementations remain single-slot request/response paths in:
         - [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c)
         - [`/home/costa/src/plugin-ipc.git/src/crates/netipc/src/transport/windows.rs`](/home/costa/src/plugin-ipc.git/src/crates/netipc/src/transport/windows.rs)
   - Options:
     - Option A:
       - implement and fully test the fake snapshot/cache service first on baseline transports only (`UDS_SEQPACKET` on POSIX, `Named Pipe` on Windows), then redesign SHM for snapshot publication in a later phase
     - Option B:
       - require the first fake snapshot/cache service to work on SHM too from the start
     - Option C:
       - implement the fake snapshot service first on baseline transports, but also add SHM-specific protocol tests/artifacts immediately even if the full snapshot service does not run on SHM yet
   - User decision:
     - Option A approved on 2026-03-12 as part of the approved strict phase plan (`Phase 2` baseline transport migration, `Phase 7` SHM redesign)
   - Decision:
     - the first fake snapshot/cache service will be implemented and fully tested first on baseline transports only
     - SHM redesign remains required, but it follows only after the fake snapshot/cache service is correct, tested, and performance-covered on the baseline transports
   - Implication:
     - the protocol and API can stabilize before the more difficult SHM publication redesign starts
     - SHM is still in scope for the full project, but it is no longer a blocker for starting the fake producer/service work

64. Handshake ownership model for request and response sizing:
   - Status: approved on 2026-03-12 before fake snapshot service implementation.
   - Context:
     - the current protocol core and TODO still model sizing as symmetric:
       - one `max_payload_bytes`
       - one derived `max_batch_bytes`
       - no separate request-owned vs response-owned limits
     - evidence:
       - C hello payload currently carries only `max_batch_items`, `max_batch_bytes`, and `max_payload_bytes` in [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_schema.h`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_schema.h)
       - C implementation encodes/decodes only that symmetric set in [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/protocol/netipc_schema.c`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/protocol/netipc_schema.c)
       - Rust protocol core mirrors the same fields in [`/home/costa/src/plugin-ipc.git/src/crates/netipc/src/protocol.rs`](/home/costa/src/plugin-ipc.git/src/crates/netipc/src/protocol.rs)
       - current TODO decision #54 also defines `max_batch_bytes = max_payload_bytes * max_batch_items`
     - snapshot/cache services introduce asymmetric ownership:
       - client shapes request payload sizes
       - server shapes response/snapshot payload sizes
   - Options:
     - Option A:
       - keep one symmetric `max_payload_bytes` contract and force both sides to live within it
     - Option B:
       - negotiate separate request and response payload limits, with client controlling request sizing and server controlling response sizing
     - Option C:
       - negotiate separate request and response limits plus separate batch limits for client-batched requests and server-published snapshots
   - User decision:
     - Option C approved on 2026-03-12
   - Decision:
     - the handshake must negotiate directional limits instead of one symmetric payload contract
     - ownership of payload shape stays in the compiled service/method contract
     - runtime handshake negotiates ceilings for each direction separately
     - batch bytes remain derived per direction, not independently negotiated
   - Directional contract:
     - request direction:
       - `max_request_payload_bytes`
       - `max_request_batch_items`
     - response direction:
       - `max_response_payload_bytes`
       - `max_response_batch_items`
   - Implication:
     - Decision #54 is superseded in protocol shape by this directional model
     - hello / hello-ack payloads in C, Rust, and Go must change before fake snapshot implementation begins
     - snapshot/cache services and request/response lookup services can now share one stable handshake model without forcing another redesign later

65. Overall execution model for implementing the project:
   - Status: approved on 2026-03-12 after agreeing that the project must be delivered in phases.
   - Context:
     - Phase 1 protocol-core work already landed, but its current hello/hello-ack fields are now outdated by Decision #64
     - fake producer work is required before any Netdata-repo integration (Decision #59)
     - snapshot/cache services are now in scope (Decision #58), but SHM still uses a fixed single-slot request/response layout today
     - evidence:
       - current phase plan starts at [`TODO-plugin-ipc.md`](/home/costa/src/plugin-ipc.git/TODO-plugin-ipc.md)
       - current hello/hello-ack structs are still symmetric in [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_schema.h`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/include/netipc/netipc_schema.h)
       - current SHM path is still fixed-frame/single-slot in [`/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c`](/home/costa/src/plugin-ipc.git/src/libnetdata/netipc/src/transport/posix/netipc_shm_hybrid.c)
   - Options:
     - Option A:
       - do the work in strict phases, with each phase frozen, tested, and documented before the next one starts
     - Option B:
       - implement protocol, fake service, cache helpers, and SHM redesign in parallel to reduce total wall-clock time
     - Option C:
       - do baseline transports and fake service first, but allow opportunistic partial SHM work in parallel when convenient
   - User decision:
     - Option A approved on 2026-03-12
   - Decision:
     - the project will be implemented in strict phases
     - each phase must be frozen, tested, and documented before the next phase begins
   - Implication:
     - implementation can proceed autonomously within each approved phase
     - phase boundaries become explicit review/control points and reduce protocol/API drift

66. Fake cleaned `cgroups` snapshot service contract:
   - Status: approved on 2026-03-12 with real Netdata reuse as the target.
   - Context:
     - the current Netdata producer shares:
       - header:
         - `cgroup_root_count`
         - `cgroup_max`
         - `systemd_enabled`
         - `body_length`
       - item:
         - `name`
         - `hash`
         - `options`
         - `enabled`
         - `path`
       - evidence:
         - [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h)
         - [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c)
     - the current `ebpf` consumer actually uses:
       - `hash` + `name` as identity
       - `options` to derive `systemd`
       - `path` to read `cgroup.procs`
       - evidence:
         - [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c)
     - user requirement:
       - the fake contract and fake test must be suitable to be used later with the real `cgroups` and `ebpf` plugins exactly as-is
       - implication:
         - this fake service is no longer just a representative rehearsal
         - it must preserve the real consumer semantics, while still excluding clearly transport/internal fields like `body_length`
   - Options:
     - Option A:
       - minimal cleaned snapshot
       - snapshot metadata:
         - `generation`
         - `item_count`
         - `systemd_enabled`
       - item fields:
         - `name`
         - `path`
         - `options`
         - `enabled`
       - Pros:
         - cleanest public contract
         - no derived/internal fields
       - Cons:
         - less direct rehearsal for the current `ebpf` consumer, which keys on `hash + name`
       - Implications:
         - later Netdata integration computes any needed identity locally
       - Risks:
         - may hide one real consumer need until integration
     - Option B:
       - cleaned snapshot with explicit stable identity
       - snapshot metadata:
         - `generation`
         - `item_count`
         - `systemd_enabled`
       - item fields:
         - `stable_id`
         - `name`
         - `path`
         - `options`
         - `enabled`
       - Pros:
         - still cleaned
         - closer to the current `ebpf` consumer behavior
         - easier future Netdata adaptation
       - Cons:
         - one more public field to define and maintain
       - Implications:
         - we must define what `stable_id` means in the fake service
       - Risks:
         - if the identity story changes later, this may need adjustment
     - Option C:
       - infrastructure-only toy subset
       - snapshot metadata:
         - `generation`
         - `item_count`
       - item fields:
         - `name`
         - `path`
         - `enabled`
       - Pros:
         - fastest to implement
       - Cons:
         - weaker rehearsal for the real replacement
       - Implications:
         - likely follow-up schema churn later
       - Risks:
         - validates the mechanics, but not enough of the real service contract
   - User decision:
     - the fake contract and fake test must be suitable to be used later with the real `cgroups` and `ebpf` plugins exactly as-is
   - Current accepted direction after user clarification:
     - the fake contract must be Netdata-ready for the real `cgroups` / `ebpf` replacement path
     - transport/internal fields remain excluded
     - semantic fields actually used by the consumer remain in scope
   - Decision:
     - the fake/public `cgroups` snapshot contract must be suitable for later use by the real `cgroups` and `ebpf` plugins without redesign
     - transport/internal fields such as `body_length` remain excluded
     - semantic fields actually used by the real consumer remain in scope

67. First fake snapshot refresh contract:
   - Status: approved on 2026-03-12.
   - Context:
     - the current `ebpf` side refreshes local state periodically from the producer snapshot
     - this fake service is meant to validate the cache-backed helper layer before Netdata integration
   - Options:
     - Option A:
       - always return the full snapshot
       - Request:
         - no payload or only a trivial control field
       - Response:
         - full snapshot every refresh
       - Pros:
         - simplest
         - best first implementation
         - easiest to test and benchmark
       - Cons:
         - no `unchanged` short-circuit yet
       - Implications:
         - good fit for the first fake service
       - Risks:
         - some extra traffic during refreshes
     - Option B:
       - generation-aware refresh
       - Request:
         - `known_generation`
       - Response:
         - either `unchanged` or a full snapshot with a new generation
       - Pros:
         - closer to a production-style cache refresh API
         - less unnecessary refresh traffic
       - Cons:
         - more state and protocol logic in the first service
       - Implications:
         - the cache helper gets more realistic behavior early
       - Risks:
         - more implementation complexity in the first real method family
     - Option C:
       - pagination / partial refresh
       - Pros:
         - most scalable
       - Cons:
         - too much for the first fake service
       - Implications:
         - not fit for the current phased plan
       - Risks:
         - slows the project for low immediate value
   - User decision:
     - Option A approved on 2026-03-12
   - Decision:
     - the first fake snapshot service will always return the full snapshot on refresh
     - generation-aware or differential refresh stays out of the first implementation
   - Implication:
     - the first cache-backed helper validates the core protocol, snapshot decode/build path, and local cache logic first
     - refresh optimization can come later without destabilizing the first real service family

70. Snapshot/cache helper methodology on Windows:
   - Status: approved on 2026-03-12.
   - Context:
     - `cgroups -> ebpf` itself is Linux-only, but the higher-level pattern under discussion is broader:
       - server-owned snapshot publication
       - client-side cache rebuild/lookup helpers
       - refresh-oriented local cache use instead of request-per-lookup
     - current evidence in this repo already points toward a cross-platform implementation model:
       - the approved project plan makes cache-backed helpers part of the library above the generic typed IPC core, not a Linux-only special case
       - the current C helper implementation is already written to select POSIX UDS on POSIX and named pipes on Windows/MSYS/Cygwin
       - baseline transports on both POSIX and Windows now expose generic variable-message APIs
     - evidence:
       - cache-backed helper direction approved in Decision #58
       - baseline-transport-first scope approved in Decision #63
       - directional request/response handshake approved in Decision #64
       - current cross-platform helper selection in:
         - `src/libnetdata/netipc/src/service/netipc_cgroups_snapshot.c`
         - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
         - `src/libnetdata/netipc/include/netipc/netipc_uds_seqpacket.h`
   - Question:
     - should the snapshot/cache helper methodology itself be treated as cross-platform library architecture, even though the first real service example (`cgroups`) is Linux-only?
   - Options:
     - Option A:
       - yes, make snapshot/cache helpers a cross-platform library pattern
       - the first concrete schema remains Linux-specific (`cgroups`), but the architecture and test methodology must also work on Windows with future Windows-native services
     - Option B:
       - no, keep snapshot/cache helpers effectively Linux-specific for now and use only generic request/response services on Windows
     - Option C:
       - keep the core generic on Windows, but defer any explicit commitment that snapshot/cache helpers are part of the Windows architecture until a real Windows service is implemented
   - User decision:
     - Option A approved on 2026-03-12
   - Decision:
     - snapshot/cache helpers are part of the cross-platform library architecture
     - the first real schema example (`cgroups`) remains Linux-specific
     - Windows must still support and test the same refresh/cache methodology with its own transports and future Windows-native services
   - Implication:
     - Windows baseline validation is required for the helper methodology itself, even though `cgroups` is not a Windows service
     - later Windows-native snapshot services should reuse the same helper architecture instead of inventing a separate model

68. Exact identity field for the fake `cgroups` contract:
   - Status: approved on 2026-03-12 before schema implementation.
   - Context:
     - the current producer exports `hash` and the current consumer uses `hash + name` as identity:
       - producer writes `ptr->hash = simple_hash(ptr->name)` in [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/cgroup-discovery.c)
       - consumer keys on `ect->hash == ptr->hash && !strcmp(ect->name, ptr->name)` in [`/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/ebpf.plugin/ebpf_cgroup.c)
     - user requirement is that the fake contract should later be usable with the real plugins exactly as-is
   - Options:
     - Option A:
       - expose the field as `hash`
       - Pros:
         - closest to the current producer/consumer reality
         - least adaptation later
         - best fit for the “exactly as-is” goal
       - Cons:
         - keeps a somewhat implementation-flavored field name in the public contract
       - Implications:
         - the fake service mirrors the real identity semantics directly
       - Risks:
         - if Netdata later wants a stronger identity story than the current hash, this field may need revision
     - Option B:
       - expose the field as `stable_id`
       - Pros:
         - cleaner public abstraction
       - Cons:
         - immediate mismatch with the current real consumer/producer naming and semantics
       - Implications:
         - later integration still needs adaptation or renaming logic
       - Risks:
         - contradicts the “exactly as-is” goal
   - User decision:
     - Option A approved on 2026-03-12
   - Decision:
     - the fake/public `cgroups` contract will expose the identity field as `hash`
     - the fake service preserves the current real identity semantics of `hash + name`
   - Implication:
     - the fake service is aligned with the current Netdata producer/consumer contract where it matters semantically
     - later integration should require less adaptation logic

69. Derived service-specific size limits for the fake `cgroups` snapshot service:
   - Status: derived from current Netdata facts on 2026-03-12; no user decision required.
   - Facts:
     - the current real producer schema uses:
       - `name[256]`
       - `path[FILENAME_MAX + 1]`
       - evidence:
         - [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.h)
     - the current real producer also defaults `cgroup_root_max` to `1000`:
       - evidence:
         - [`/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.c`](/home/costa/src/netdata/netdata/src/collectors/cgroups.plugin/sys_fs_cgroup.c)
   - Derived implications:
     - the generic negotiated payload default of `1024` is not enough for the real cleaned `cgroups` snapshot item payload
     - the fake `cgroups` snapshot helper must therefore use service-specific defaults instead of the generic protocol default
   - Decision:
     - the fake `cgroups` snapshot helper layer will use service-specific response sizing defaults derived from the real Netdata contract
     - request-side defaults remain small because the first refresh request payload is only the fixed 4-byte request payload
     - response-side default batch item count should match the current real `cgroup_root_max` default of `1000`

## Requirement Cleanup / Superseded Assumptions (2026-03-12)
- Fact: several older items below reflect the earlier benchmark/prototype phase and now contradict the current library design direction.
- Cleanup rule for this TODO:
  - keep historical decisions for traceability
  - but treat the following items as superseded for current architecture/planning

- Superseded by Decisions #41-#44:
  - Decision #5 (`batch only if benchmarks fail the target`)
  - Current direction is now batch-first v1 for the advanced-throughput path, with general pipelining deferred if ordered batch is sufficient.

- Superseded in implementation priority by Decisions #41-#44:
  - Decision #11 (`strict ping-pong baseline plus secondary pipelined mode`)
  - Current direction is:
    - keep ping-pong benchmarks
    - add ordered-batch benchmarks as the main advanced-throughput validation
    - treat general pipelining as a later phase unless measurements prove it is still required

- Superseded in protocol scope by Decisions #40-#44:
  - any assumption that the current fixed 64-byte `INCREMENT` frame is the target library protocol
  - Current direction is a fixed header plus variable payload envelope, because real requests/responses may carry strings and different payload sizes.

- Superseded in server-dispatch direction by Decisions #39 and #42:
  - any implicit session-sticky "one worker owns one client forever" managed-server assumption
  - Current direction is request-level batch parallelism, where a single client batch may be split across workers and then reassembled in order.
- User clarification state (2026-03-11):
  - Decision `1` selected as `A`: extend the existing Windows scripts to support both `c-native` and `c-msys`.
  - Decision `2` selected as `B`: include `c-native <-> c-msys` in the transition smoke matrix in addition to `c-msys <-> rust-native/go-native`.
  - Decision `3` selected as full cross-implementation coverage: benchmark all directed combinations, while keeping each individual run capped at 5 seconds.
  - Decision `7` selected as `B`: use `rust-native -> rust-native` to find the Windows SHM spin candidate knee, then validate the shortlist on representative transition pairs.

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
5. Run the strict ping-pong baseline and the current advanced-throughput benchmark mode.
   - Historical note: earlier plan text assumed general pipelined mode as the secondary path.
   - Current direction after Decisions #41-#44 is ordered-batch benchmarking first, with general pipelining deferred unless measurements prove it is still required.
6. Collect CPU metrics using both measurement approaches from Decision #12.
7. Produce decision report with a primary score for single-thread ping-pong and a secondary score for multi-client scaling.
8. Freeze transport + framing + custom binary schema for v1.
9. Implement minimal typed RPC surface in all 3 language libraries.
10. Implement auth handshake using shared SALT/session UUID.
11. Add integration test: C client -> Rust server, Rust client -> Go server, Go client -> C server.
12. Add stress tests to validate stability and tail latency.

## Historical Phase Plan (Decision #21)
1. Define and freeze a minimal v1 typed schema for one RPC method (`increment`) with fixed-size binary frame and explicit little-endian encode/decode.
2. Add a C API module for POSIX shared-memory hybrid transport with default spin window 20, targeting single-thread ping-pong first.
3. Add simple C server/client examples using the new C API and schema.
4. Add Rust and Go schema codecs and interoperability fixtures.
5. Add an automated interop test runner to validate:
   - C client framing <-> Rust server framing.
   - Rust client framing <-> Go server framing.
   - Go client framing <-> C server framing.
6. Document baseline and current limitations explicitly.

## Historical Phase Plan (Decision #26/#29)
1. Add a POSIX C `UDS_SEQPACKET` transport module as the v1 baseline profile.
2. Add fixed-binary handshake frames for capability negotiation and server-selected profile selection.
3. Keep handshake bitmask-compatible with optional future profiles.
4. Add C server/client demos for persistent session request-response.
5. Add an automated test for handshake correctness plus increment loop behavior.
6. Keep SHM transport and live interop tests intact to avoid regressions in parallel work.

## Historical Phase Plan (Decision #33)
1. Extend C UDS transport profile mask to implement `NETIPC_PROFILE_SHM_HYBRID` negotiation candidate.
2. Keep handshake over `UDS_SEQPACKET`, then switch request/response data-plane to shared-memory hybrid when profile `2` is selected.
3. Extend Rust native UDS live runner to support the same negotiated profile switch.
4. Keep Go native UDS live runner baseline-only (`profile 1`) so C/Rust<->Go automatically fall back to UDS.
5. Add/extend live interoperability tests to validate:
   - C<->Rust prefers `SHM_HYBRID` when both advertise it.
   - Any path involving Go negotiates `UDS_SEQPACKET`.
6. Add/extend benchmark scripts to compare negotiated `profile 1` vs `profile 2` in C/Rust paths.

## Historical Phase Plan (Decision #34)
1. Replace busy/yield rate pacing loops with adaptive sleep pacing in:
   - the deleted prototype benchmark source
   - `interop/rust/src/bin/netipc_live_uds_rs.rs`
   - `interop/go-live/main.go`
2. Keep full-speed (`target_rps=0`) behavior unchanged.
3. Rebuild and rerun benchmark comparisons:
   - `tests/run-live-uds-bench.sh`
   - `tests/run-negotiated-profile-bench.sh`
4. Update TODO and results tables with post-fix metrics.

## Historical Phase Plan (Decision #35)
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
- Done (historical benchmark scaffold): first typed v1 schema implemented (`increment`) with fixed 64-byte frame and explicit little-endian encode/decode.
  - Current architecture direction after Decisions #40-#44 is to replace this with a fixed-header plus variable-payload protocol for the real library API.
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
  - Benchmark modes:
    - strict single in-flight ping-pong baseline
    - ordered-batch advanced-throughput mode for v1
    - general pipelining remains a later phase unless batch measurements prove it is still required
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
      - the library default spin window is now `128` in both C and Rust:
        - `src/libnetdata/netipc/include/netipc/netipc_shm_hybrid.h`: `NETIPC_SHM_DEFAULT_SPIN_TRIES 128u`
        - `src/crates/netipc/src/transport/posix.rs`: `SHM_DEFAULT_SPIN_TRIES: u32 = 128`
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

87. Linux pure-Go SHM completion and negotiated SHM matrix widening
    - Facts:
      - Linux pure-Go POSIX SHM is now implemented in `src/go/pkg/netipc/transport/posix/shm_linux.go` on top of the unified SHM control/data layout.
      - non-Linux Unix now keeps `ProfileUDSSeqpacket` only via `src/go/pkg/netipc/transport/posix/shm_other.go`, so FreeBSD/macOS fall back to UDS as approved.
      - the widened fake `cgroups` SHM matrix passed for `C/Rust/Go` producers and clients via `tests/run-live-cgroups-shm.sh`.
      - the widened negotiated UDS SHM matrix initially failed only because `tests/run-live-uds-interop.sh` still launched the Go bench driver with the old C-only negotiation CLI.
    - Fix:
      - `bench/drivers/go/main.go` now honors `NETIPC_SUPPORTED_PROFILES`, `NETIPC_PREFERRED_PROFILES`, and `NETIPC_AUTH_TOKEN` through the same env-driven path already used by the Rust helper and the Go fake-service fixture.
      - `tests/run-live-uds-interop.sh` now drives Go negotiated SHM cases through that env-based override path instead of passing the C-only positional negotiation arguments.
    - Validation:
      - `bash tests/run-live-uds-interop.sh`
      - `bash tests/run-live-cgroups-shm.sh`
      - `/usr/bin/ctest --test-dir build --output-on-failure`
      - `cd src/go && go test ./...`
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - Result:
      - Linux baseline UDS interop is green for the full `C/Rust/Go` matrix.
      - Linux negotiated SHM `profile=2` interop is green for the full `C/Rust/Go` matrix.
      - Linux fake `cgroups` SHM methodology is green for the full `C/Rust/Go` producer/client matrix.

88. Complete POSIX benchmark picture before continuing snapshot/publish work
    - Source: user requirement on 2026-03-13 to see the complete POSIX benchmark picture before proceeding further.
    - Facts:
      - `benchmarks-posix.md` currently shows:
        - UDS matrix
        - legacy direct SHM matrix (`C/Rust` only)
        - negotiated profile matrix
      - it does not show the newer fake `cgroups` snapshot/cache benchmark, even though:
        - baseline `C/Go/Rust` snapshot/cache benchmark exists in `tests/run-live-cgroups-bench.sh`
        - SHM `C/Go/Rust` snapshot/cache benchmark exists in `tests/run-live-cgroups-shm-bench.sh`
      - therefore the current document cannot answer the user's Go-over-SHM question for the approved snapshot/cache methodology.
    - Decision:
      - widen `tests/generate-benchmarks-posix.sh` and `benchmarks-posix.md` so the generated document includes the fake `cgroups` snapshot/cache benchmark on:
        - baseline transports
        - SHM transports
      - keep the legacy direct `SHM_HYBRID` matrix too, but treat it as the old low-level ping-pong benchmark scope.
    - Goal:
      - make `benchmarks-posix.md` the complete Linux benchmark picture for both:
        - low-level transport throughput
        - approved snapshot/cache methodology (`C/Go/Rust`, baseline and SHM)
    - Progress slice (2026-03-13):
      - `tests/run-live-cgroups-bench.sh` was benchmark-hardened so server idle-exit is controlled by `NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS` instead of racing the harness watchdog at the old 10s timeout.
      - `tests/fixtures/c/netipc_cgroups_live.c`, `tests/fixtures/go/cgroups_server_unix.go`, `tests/fixtures/go/cgroups_server_windows.go`, and `tests/fixtures/rust/src/bin/netipc_codec_rs.rs` now honor that benchmark-only idle timeout for `server-loop`, while `server-once` keeps the longer functional timeout.
      - `tests/generate-benchmarks-posix.sh` was fixed to:
        - consume the real CSV exports produced by `tests/run-live-cgroups-bench.sh` and `tests/run-live-cgroups-shm-bench.sh` (`*.csv` sidecar files),
        - validate four-column cgroups rows correctly (`bench_type`, `scenario`, `client`, `server`),
        - regenerate the document from a complete staged run.
    - Validation:
      - `bash tests/run-live-cgroups-shm-bench.sh`
      - `env NETIPC_SKIP_CONFIGURE=1 NETIPC_SKIP_BUILD=1 bash tests/generate-benchmarks-posix.sh`
    - Result:
      - `benchmarks-posix.md` is now the complete Linux benchmark picture for:
        - UDS baseline throughput,
        - legacy direct low-level SHM throughput (`C/Rust` only),
        - negotiated profile throughput,
        - snapshot/cache baseline refresh and local lookup (`C/Go/Rust`),
        - snapshot/cache SHM refresh and local lookup (`C/Go/Rust`).
      - The high-speed POSIX path is still clearly high-speed:
        - negotiated `profile2-shm` max is about `2.96M req/s`,
        - legacy direct SHM max remains about `2.93M` to `3.10M req/s`,
        - UDS max remains about `155k` to `230k req/s`.
      - The approved snapshot/cache methodology also benefits strongly from SHM for C and Rust:
        - baseline refresh is about `198k` to `211k req/s`,
        - SHM refresh rises to about `2.06M` to `2.44M req/s`.
      - Linux Go SHM is now benchmarked and correctness-proven, but it is the current performance weak spot:
        - mixed Go SHM refresh is about `1.39M` to `1.48M req/s`,
        - `go->go` SHM refresh is only about `235k req/s`,
        - this is much slower than C/Rust SHM refresh and only modestly above the UDS/baseline class.
      - Local cache lookup is excellent for all languages and validates the cache-backed architecture:
        - baseline and SHM local lookup stay in the tens of millions of lookups per second,
        - the refresh path is the actual performance-critical IPC path.

89. Linux Go SHM refresh performance investigation before snapshot/publish continuation
    - Source: user decision on 2026-03-13 after reviewing the completed `benchmarks-posix.md`.
    - Goal:
      - explain with evidence why Linux Go SHM refresh throughput is materially lower than C/Rust SHM refresh, especially for `go->go`.
      - fix the performance issue if the cause is in the current Go SHM implementation.
      - only continue deeper into snapshot/publish implementation after this gap is understood.
    - Initial facts:
      - the completed POSIX benchmark picture now shows:
        - `c->c` fake snapshot SHM refresh max about `2.44M req/s`
        - `rust->rust` fake snapshot SHM refresh max about `2.06M req/s`
        - mixed Go SHM refresh about `1.39M` to `1.48M req/s`
        - `go->go` fake snapshot SHM refresh max only about `235k req/s`
      - local lookup after refresh remains fast for Go too, so the hot issue is refresh IPC, not cache lookup.
    - Investigation findings:
      - The Go snapshot client and server paths do allocate per refresh, but that alone does not explain the collapse:
        - mixed Go SHM rows were already far above the broken `go->go` row.
      - The transport-level asymmetry was stronger:
        - C POSIX SHM uses `pause` in its spin loop.
        - Rust POSIX SHM uses `std::hint::spin_loop()` in its spin loop.
        - Go POSIX SHM was spinning without any CPU pause hint in `shmWaitForSequence(...)`.
      - Focused reproducer before the fix:
        - `go->go` fake snapshot SHM refresh max about `236k req/s`.
        - disabling Go GC did not solve it and actually made it worse, which ruled out "GC alone" as the primary cause.
      - Working theory confirmed by the fix:
        - two Go peers were busy-spinning against each other much more aggressively than C/Rust peers because the Linux Go SHM spin loop lacked a pause instruction.
    - Implemented fix:
      - Added a pure-Go-compatible `spinPause()` helper on Linux:
        - amd64 uses a tiny `PAUSE` assembly stub.
        - other Linux architectures use a no-op fallback.
      - Wired `spinPause()` into the Go POSIX SHM spin loop so it now matches the intent of the C and Rust implementations.
    - Verified result:
      - focused `go->go` fake snapshot SHM refresh improved from about `236k req/s` to about `1.16M req/s`.
      - regenerated official `benchmarks-posix.md` now shows:
        - `c->c` fake snapshot SHM refresh max `2,452,843.760 req/s`
        - `rust->rust` fake snapshot SHM refresh max `2,011,559.962 req/s`
        - `go->c` fake snapshot SHM refresh max `1,482,550.491 req/s`
        - `go->rust` fake snapshot SHM refresh max `1,475,460.735 req/s`
        - `go->go` fake snapshot SHM refresh max `1,165,422.668 req/s`
      - local lookup remains fast for all languages, including Go:
        - SHM `go->go` local cache lookup max now documented around `13.42M lookups/s`
    - Conclusion:
      - the catastrophic Linux Go SHM collapse is fixed.
      - Go SHM refresh is still slower than C/Rust SHM refresh, but it is now firmly in the high-speed class and no longer near the UDS/baseline class.
      - the remaining gap is likely dominated by Go-side allocation/materialization overhead in the current fake snapshot refresh path, not by a broken SHM synchronization loop.
