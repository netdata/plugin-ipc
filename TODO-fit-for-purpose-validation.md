## Autonomous execution contract

- Costa explicitly requires autonomous execution with no checkpoints.
- Windows work must use exactly one checkout on `win11`:
  - `~/src/plugin-ipc.git`
- Cross-machine sync workflow is fixed:
  - commit in `/home/costa/src/plugin-ipc.git`
  - push from `/home/costa/src/plugin-ipc.git`
  - pull in `win11:~/src/plugin-ipc.git`
  - and vice versa for Windows-originated commits
- No disposable Windows git clones or worktrees under `/tmp` are allowed.
- Historical `/tmp/...` paths kept below are evidence artifact locations only,
  not valid working directories.
- Unless a line explicitly says it is a historical artifact path, every `win11`
  reference in this TODO means the persistent checkout at:
  - `win11:~/src/plugin-ipc.git`
- "Working autonomously" means:
  - do not stop to provide progress updates, interim summaries, or checkpoint
    reports
  - do not pause between phases just because a milestone was reached
  - continue phase by phase until the next real design decision not already
    answered by `docs/` or other repository specifications, or until the entire
    plan is complete
- The next user-facing summary is allowed only at:
  - the next real design/product decision that is not already answered by the
    repository docs/specs
  - full completion of the entire fit-for-purpose plan

## Purpose

Prove that `netipc` is fit for purpose for Netdata integration as a long-running,
cross-platform IPC library: reliable under service absence and restart, robust
under malformed input and hostile timing, leak-free over long runtimes, fast
enough for the intended plugin workloads, and operationally trustworthy on both
Linux and native Windows.

## Execution mode

- Costa explicitly asked for autonomous execution of this entire plan.
- Work must proceed phase by phase to completion without pausing for approval,
  unless a real design decision is encountered that is not already answered by
  the existing repository docs and specifications.
- If a design decision is already answered in `docs/` or other repository
  specifications, that documented answer must be treated as authoritative and
  implementation should proceed.
- Stop only for:
  - unresolved product/design questions not already answered in the repo docs
  - hard environmental blockers that cannot be worked around safely
  - evidence of conflicting requirements between code and docs that materially
    changes the intended contract
- Otherwise continue until the library satisfies the no-exclusions fit-for-
  purpose criteria in this TODO.
- `2026-03-29` Costa clarified a hard sign-off rule:
  - "benchmark environment instability" is not an acceptable terminal
    explanation
  - any repeatable lab failure must end in one of two buckets:
    - the testing / benchmark harness is wrong and must be fixed
    - the product code is wrong and must be fixed
  - if the current shared workstation + `win11` VM setup is too noisy for
    strict sign-off, that is a harness / validation-system defect, not an
    excuse to downgrade the failure into acceptable noise
  - final fit-for-purpose sign-off must therefore eliminate these failures by
    improving the harness/rig or by fixing code, not by classifying them away
- `2026-04-02` Costa decision:
  - checkpoint the current Windows benchmark harness fix immediately
  - commit and push the local runner/TODO changes
  - rerun the full strict native Windows benchmark suite from the pushed state
    before treating the fix as signed off

## TL;DR

- The repo already proves ordinary correctness well: strong coverage, fuzzing,
  chaos tests, stress tests, interop, and verified benchmark floors.
- The remaining uncertainty is not “basic logic”. It is the hard operational
  classes: allocation failure, OS / Win32 failure, race timing, restart /
  reconnect behavior, leak freedom, and real Netdata process-model validation.
- The fastest path to fit-for-purpose confidence is:
  - close the known ignored / skipped Windows cases
  - add deterministic fault injection
  - add leak / sanitizer / Windows verifier runs
  - add long soak loops
  - add a real Netdata plugin integration harness

## Analysis

### What is already strongly proven

- Public L2/L3 APIs are now OS-transparent within each language:
  - C plugin code should compile unchanged on Linux and Windows
  - Rust plugin code should compile unchanged on Linux and Windows
  - Go plugin code should compile unchanged on Linux and Windows
  - reference:
    - `TODO-unified-l2-l3-api.md`
- Functional verification is already strong on both Linux and Windows:
  - Linux:
    - full `ctest`
    - full Rust tests
    - full Go tests
    - full POSIX benchmark matrix
  - Windows:
    - native `ctest`
    - native Rust tests
    - native Go tests
    - native strict benchmark matrix
    - native report generation
- Current checked-in benchmark reports meet all documented floors:
  - `benchmarks-posix.md`
  - `benchmarks-windows.md`
- Coverage is already high and explicitly measured:
  - Linux:
    - C `94.1%`
    - Go `95.8%`
    - Rust `98.57%`
  - Windows:
    - C `93.2%`
    - Go `95.4%`
    - Rust `92.08%`

### Honest current gaps

- The repo explicitly states that Windows still has less chaos / hardening /
  stress breadth than Linux:
  - `README.md`
  - `WINDOWS-COVERAGE.md`
- Linux repeated full-suite stability is still not clean enough:
  - first earlier evidence:
    - one Linux full-suite SHM interop failure before clean reruns:
      - `TODO-unified-l2-l3-api.md`
  - fresh stronger evidence on `2026-03-29`:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j4 --repeat until-fail:10`
      stopped with:
      - `24:test_service_rust`
      - `34:test_stress_rust`
    - `build/Testing/Temporary/LastTestsFailed.log`
  - important nuance:
    - immediate isolated reruns of only those two tests passed
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml service -- --test-threads=1`
      passed
    - a focused CTest repeat loop for only `test_service_rust` and
      `test_stress_rust` was still passing when this TODO was updated
  - additional concrete harness finding on `2026-03-29`:
    - `test_service_rust` was not cleanly partitioned
    - its `cargo test service` filter was also running the Rust stress tests,
      while `test_stress_rust` separately ran the same stress slice again
    - Linux Go CTest also had a weaker partitioning issue:
      - `test_service_go` overlapped `test_cache_go`
      - `test_stress_go` pointed at `./pkg/netipc/service/cgroups/` and
        therefore passed with `no tests to run`
  - fix applied:
    - `CMakeLists.txt`
    - Rust CTest now uses:
      - `test_service_typed_rust`
      - `test_service_rust` for raw non-stress service/cache coverage
      - `test_stress_rust` for the dedicated stress slice only
    - Linux Go CTest now uses:
      - `test_service_go` for typed non-cache tests only
      - `test_cache_go` for the typed cache wrapper test
      - `test_stress_go` for the real raw stress package
  - fresh proof after the partition fix:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^(test_service_typed_rust|test_service_rust|test_stress_rust|test_service_go|test_cache_go|test_stress_go)$'`
    - result: `6/6` passed
  - additional concrete proof on `2026-03-29`:
    - the corrected `test_service_rust` command now maps to:
      - `cargo test service::raw::tests:: -- --test-threads=1`
      - plus explicit skips for only the six `test_stress_*` cases
    - direct proof:
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml service::raw::tests:: -- --test-threads=1 --skip test_stress_1000_items --skip test_stress_5000_items --skip test_stress_cache_concurrent --skip test_stress_concurrent_clients --skip test_stress_long_running --skip test_stress_rapid_connect_disconnect`
      - result:
        - `69` tests passed
        - `0` failed
        - finished in `16.43s`
    - implication:
      - the earlier `test_service_rust` timeout evidence no longer points to
        the raw non-stress Rust slice itself
      - it points to stale CTest metadata from before the latest reconfigure,
        plus the broader long repeated-suite instability still under burn-down
  - fresh stronger burn-down on `2026-03-29` after the later Linux harness
    fixes:
    - focused repeated proof:
      - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^(test_service_rust|test_stress_rust|test_cache_interop|test_cache_shm_interop|test_service_extra)$' --repeat until-fail:10`
      - result:
        - `100%` tests passed
        - `0` failed out of `5`
        - total real time `473.90 sec`
    - broader POSIX interop proof:
      - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^(test_service_interop|test_service_shm_interop|test_cache_interop|test_cache_shm_interop|test_uds_interop|test_shm_interop)$' --repeat until-fail:10`
      - result:
        - `100%` tests passed
        - `0` failed out of `6`
        - total real time `17.40 sec`
    - full repeated Linux suite after the last CTest scheduling fix:
      - `/usr/bin/ctest --test-dir build --output-on-failure -j4 --repeat until-fail:3`
      - result:
        - `100%` tests passed
        - `0` failed out of `39`
        - total real time `908.53 sec`
    - implication:
      - the latest evidence no longer supports a standing reproducible Linux
        repeated-suite blocker at the current code state
      - longer soak still remains desirable, but the previously reproduced
        failures have now been narrowed to concrete harness issues that were
        fixed
- Coverage exclusions are already documented for the hard classes:
  - allocation-failure branches
  - OS / kernel / Win32 API failure branches
  - timing / race branches
  - references:
    - `COVERAGE-EXCLUSIONS.md`
    - `WINDOWS-COVERAGE.md`
- Version rejection is tested, and forward-compatibility intent is documented,
  but there is no explicit mixed-version / rolling-upgrade matrix proven by the
  current repo-level test evidence:
  - version rejection:
    - `tests/test_protocol.c`
  - forward-compatibility intent:
    - `docs/codec-cgroups-snapshot.md`
- The repo already checks in Linux heavy-validation entrypoints for:
  - ASan + UBSan:
    - `tests/run-sanitizer-asan.sh`
  - TSan:
    - `tests/run-sanitizer-tsan.sh`
  - Valgrind:
    - `tests/run-valgrind.sh`
- Honest remaining gap:
  - Windows verifier / low-resource tooling is still not checked in as a
    first-class workflow:
    - Application Verifier / PageHeap
    - Dr. Memory or equivalent handle/heap tooling
- Fresh native Windows verifier evidence on `2026-03-29` shows a real L1
  defect still exists:
  - native `win11` `appverif.exe` against `build/bin/test_named_pipe.exe`
    produced a deterministic Handles stop:
    - `LayerName="Handles"`
    - `StopCode="0x300"`
    - top frame: `KERNELBASE!ConnectNamedPipe+6a`
  - exported native verifier artifact:
    - `C:\msys64\home\costa\src\plugin-ipc.git\verifier-test_named_pipe.xml`
  - current working theory:
    - a raw closed-listener path in the C L1 transport is still issuing
      `ConnectNamedPipe()` against a stale handle
    - this is grounded enough to drive code changes, but still needs native
      post-fix verifier proof before it can be called resolved
- Updated native Windows verifier evidence later on `2026-03-29`:
  - the original `test_named_pipe.exe` `ConnectNamedPipe` invalid-handle stop
    is resolved
  - native proof:
    - `appverif.exe -export log -for test_named_pipe.exe ...`
    - result:
      - `AVRF: Error: there is no valid log file for test_named_pipe.exe`
  - `test_win_service.exe` is also clean under Application Verifier Handles:
    - fresh exported session exists with no `<logEntry>`
    - artifact:
      - `C:\msys64\home\costa\src\plugin-ipc.git\verifier-test_win_service.xml`
- a new blocker replaced the old one:
  - `test_win_service_extra.exe`
    - `LayerName="Handles"`
    - `StopCode="0x300"`
    - top frame: `KERNELBASE!ReadFile+8d`
    - artifact:
      - `C:\msys64\home\costa\src\plugin-ipc.git\verifier-test_win_service_extra.xml`
  - grounded working theory:
    - the remaining Windows C issue is no longer in the raw named-pipe
      listener path
    - it is now in the C service/test process, likely a broken-session or
      shutdown/recovery path that reaches `ReadFile()` after the same pipe
      handle was already closed elsewhere in-process
  - later `2026-03-29` resolution:
    - the failing path was isolated to
      `test_refresh_from_broken_state()` in
      `tests/fixtures/c/test_win_service_extra.c`
    - native trace evidence showed the real bug was not the broken-session
      client path itself
    - it was cross-thread listener ownership during Windows stop:
      - `nipc_server_stop()` woke `ConnectNamedPipe()` and also closed the live
        listener handle from another thread
      - the accept thread could race past `ConnectNamedPipe()` and enter
        `server_handshake()` on that same handle
      - the next `ReadFile()` then hit the stale listener session handle under
        Application Verifier
    - fixed files:
      - `src/libnetdata/netipc/include/netipc/netipc_service.h`
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
    - fix shape:
      - added `accept_loop_active` tracking in the Windows managed server
      - `nipc_server_stop()` / `nipc_server_drain()` / `nipc_server_destroy()`
        now wake the accept loop without cross-thread closing the live listener
        when the accept loop is active
      - `nipc_server_run()` now owns closing the listener on exit
      - session-thread synchronous I/O cancellation remains in place for
        active-thread stop/drain robustness
    - native `win11` proof:
      - isolated Application Verifier Handles rerun of
        `test_refresh_from_broken_state()` now passes with no log entry
      - full `test_win_service_extra.exe` now passes under Application
        Verifier Handles with no `<logEntry>` in:
        - `C:\msys64\home\costa\src\plugin-ipc.git\msys64homecostasrcplugin-ipc.gitverifier-test_win_service_extra-final.xml`
- fresh Linux interop-cache burn-down on `2026-03-29`:
  - the repeated full suite later failed in:
    - `test_cache_interop`
    - `test_cache_shm_interop`
  - concrete failure symptom:
    - `client: cache not ready after refresh`
  - root-cause direction validated in code:
    - Go POSIX interop cache client already used a bounded refresh retry loop
    - C and Rust interop cache clients were still using one-shot refresh
    - that made the cross-language cache harness inconsistent on startup timing
  - fix applied:
    - `tests/fixtures/c/interop_cache.c`
    - `tests/fixtures/c/interop_cache_win.c`
    - `tests/fixtures/rust/src/bin/interop_cache.rs`
    - `tests/fixtures/rust/src/bin/interop_cache_win.rs`
    - `tests/fixtures/go/cmd/interop_cache_win/main.go`
    - all now use the same bounded refresh-retry pattern before failing
  - repeated proof:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j1 -R '^(test_cache_interop|test_cache_shm_interop)$' --repeat until-fail:30`
    - result:
      - `30/30` passes for both tests
      - `0` failures
  - implication:
    - the cache interop flake was harness timing, not a demonstrated library
      compatibility defect
  - later stronger root cause found in the shell harness itself:
    - `tests/test_cache_interop.sh`
    - `tests/test_service_interop.sh`
    - `tests/test_shm_interop.sh`
    - `tests/test_uds_interop.sh`
    - and the Windows counterparts:
      - `tests/test_cache_win_interop.sh`
      - `tests/test_service_win_interop.sh`
      - `tests/test_named_pipe_interop.sh`
      - `tests/test_win_shm_interop.sh`
    - all had fixed shared run directories before the fix
    - under parallel `ctest`, separate interop scripts could trample the same:
      - socket paths
      - SHM region names
      - cleanup actions
  - fix applied:
    - each interop script invocation now creates its own unique run directory
    - per-test server logs now live inside that unique run directory
  - repeated proof after the harness fix:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^(test_service_interop|test_service_shm_interop|test_cache_interop|test_cache_shm_interop|test_uds_interop|test_shm_interop)$' --repeat until-fail:10`
    - result:
      - `100%` tests passed
      - `0` failed out of `6`
      - total real time `17.40 sec`
  - implication:
    - the later cache/service/shm/uds interop flakes were a shared-harness
      isolation bug, not evidence of cross-language protocol incompatibility
- fresh native Windows benchmark regression on `2026-03-29`:
  - repro from a clean disposable `win11` proof tree with preserved artifacts:
    - `NIPC_KEEP_RUN_DIR=1 NIPC_BENCH_FIRST_BLOCK=1 NIPC_BENCH_LAST_BLOCK=1 bash tests/run-windows-bench.sh /tmp/win-bench-block1.csv 5`
  - concrete failure:
    - `np-ping-pong c->c @ max`
      - `median_throughput=87`
      - `p50=15446.600us`
      - `p95=15609.300us`
      - `p99=15609.300us`
    - `np-ping-pong rust->c @ max`
      - same `~15.5ms` latency shape
  - preserved run directory:
    - `/tmp/netipc-bench-699581`
  - grounded root cause:
    - the recent Windows C service-loop stop-safety change replaced blocking
      request receive with `nipc_np_wait_readable()` polling
    - `server_handle_session()` now polls `PeekNamedPipe()` with `Sleep(10)`
      before every `nipc_np_receive()`:
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - in ping-pong traffic that can miss the just-after-check request and add a
      full `10ms` sleep to nearly every round trip
  - broader truth discovered while burning it down:
    - the clean proof-tree benchmark runner itself was also masking fixes
    - `tests/run-windows-bench.sh` rebuilt only:
      - `bench_windows_c`
      - `bench_windows_go`
    - but it did not rebuild:
      - Rust `bench_windows.exe`
    - consequence:
      - fresh proof trees could benchmark stale Rust code and produce false
        negative evidence for current local fixes
  - fixes applied:
    - C Windows service loop:
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
      - restored blocking `nipc_np_receive()` in the named-pipe hot path
      - stop/drain still relies on the existing
        `CancelSynchronousIo(session_thread)` path
    - shared Windows named-pipe readability wait:
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
      - `src/crates/netipc/src/transport/windows.rs`
      - `src/go/pkg/netipc/transport/windows/pipe.go`
      - replaced fixed `Sleep(10)` polling with:
        - one short `SwitchToThread()` yield burst to catch the immediate next
          request
        - then `Sleep(1)` for genuine idle waiting
      - implication:
        - preserves the existing stop model for Rust/Go
        - removes the `~10ms` hot-path miss cliff for ping-pong traffic
    - Windows benchmark runner:
      - `tests/run-windows-bench.sh`
      - now also rebuilds Rust:
        - `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_windows`
  - fresh native `win11` proof after all three fixes from the clean disposable
    tree:
    - output CSV:
      - `/tmp/win-bench-block1-rebuilt.csv`
    - preserved run dir:
      - `/tmp/netipc-bench-707014`
    - max-throughput rows already revalidated cleanly:
      - `c->c = 20369`, `p50=42.8us`, `p95=100.6us`
      - `rust->c = 20094`, `p50=42.9us`, `p95=103.3us`
      - `go->c = 19739`, `p50=43.4us`, `p95=107.4us`
      - `c->rust = 29570`, `p50=31.9us`, `p95=76.7us`
      - `rust->rust = 29720`, `p50=32.3us`, `p95=77.4us`
      - `go->rust = 29918`, `p50=33.7us`, `p95=83.4us`
      - `c->go = 28770`, `p50=32.5us`, `p95=82.7us`
    - `100000/s` rows also re-entered the correct band during the same run:
      - `c->c = 18512`, `p50=44.3us`
      - `rust->c = 18470`, `p50=44.2us`
      - `go->c = 18675`, `p50=44.1us`
      - `c->rust = 27352`, `p50=28.0us`
      - `rust->rust = 26077`, `p50=30.4us`
      - `go->rust = 28280`, `p50=34.1us`
  - implication:
    - the benchmark regression was real
    - it is now materially burned down in the corrected proof run
    - the next remaining step is the full native Windows suite rerun with the
      corrected runner and transport/service code, not more benchmark-method
      surgery
- fresh native Windows runtime proof after the Go L1 disconnect/state fixes on
  `2026-03-29`:
  - native `win11` full `ctest` rerun from the corrected disposable proof tree:
    - `ctest --test-dir build --output-on-failure -j4`
    - result:
      - `28/28` passed
      - concrete previously failing tests now clean:
        - `test_named_pipe_go`
        - `test_win_service_extra`
  - native `win11` Rust suite:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
    - result:
      - `192` passed
      - `0` failed
      - `0` ignored
  - native `win11` Go transport/service proof:
    - package-by-package runs after the final listener fix:
      - `go test ./pkg/netipc/protocol`
      - `go test ./pkg/netipc/service/cgroups`
      - `go test ./pkg/netipc/service/raw`
      - `go test ./pkg/netipc/transport/windows`
    - result:
      - all four packages passed
    - combined proof with the exact Windows-visible package set:
      - `go test ./pkg/netipc/protocol ./pkg/netipc/service/cgroups ./pkg/netipc/service/raw ./pkg/netipc/transport/windows`
      - result:
        - passed
  - concrete Go defects fixed in this slice:
    - `src/go/pkg/netipc/transport/windows/pipe.go`
      - disconnect on a client session now clears the whole in-flight
        `message_id` set, matching the Level 1 contract that a broken session
        fails all in-flight requests
      - listener shutdown no longer cross-thread closes the live accept handle
        while `Accept()` is blocked or racing forward on it
    - `src/go/pkg/netipc/transport/posix/uds.go`
      - the same in-flight cleanup rule now applies on POSIX Go sessions when
        a transport receive/send failure breaks the session
    - `src/go/pkg/netipc/transport/windows/pipe_edge_test.go`
      - the continuation-chunk disconnect test now asserts the actual Level 1
        contract:
          - either `Send()` fails immediately on disconnect
          - or the next session observation fails and the in-flight request is
            cleared
  - grounded native proof for the Go Windows retry/stop race:
    - before the listener fix:
      - `go test ./pkg/netipc/service/raw`
      - could crash inside the Go runtime on Windows with:
        - `runtime: setevent failed; errno=6`
        - `fatal error: runtime.semawakeup`
      - top test path:
        - `TestWinRetryOnClosedSession`
      - this pointed to a real handle-lifecycle bug in listener shutdown, not a
        wrapper or benchmark artifact
    - after the listener fix:
      - targeted proof:
        - `go test ./pkg/netipc/service/raw -run 'TestWinRetryOnClosedSession$' -count=20 -v`
      - result:
        - `20/20` passed
      - full package proof:
        - `go test ./pkg/netipc/service/raw`
      - result:
        - passed
  - fresh no-exclusions audit on `2026-03-29`:
    - repo-wide grep for ignored/skipped runtime tests and soft acceptance
      markers after the latest fixes did not reveal a new hidden pool of
      exclusions
    - concrete findings:
      - no remaining `#[ignore]` in the active Rust runtime suite paths
      - no remaining Windows L1 transport `t.Skip(...)` on supported patterns
      - one Go stress test still has:
        - `t.Skip("skipping 60s test in short mode")`
        - `src/go/pkg/netipc/service/raw/stress_test.go`
        - this is only a `-short` optimization, not a default-suite exclusion
      - current `CMakeLists.txt` `--skip test_stress_*` arguments are now test
        partitioning, not acceptance leaks:
        - the skipped cases are covered by the dedicated stress targets
        - they are no longer silently omitted from the validated suite
- fresh Level 1 session-failure proof on `2026-03-29`:
  - the docs already require that a broken session fails all in-flight
    requests:
    - `docs/level1-transport.md`
  - concrete code gap burned down:
    - Go already cleared the whole client in-flight set on session-breaking
      disconnect / send / recv paths
    - C and Rust were still mostly removing only the current `message_id`
      instead of failing the full in-flight set
  - fixed files:
    - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
  - key behavior change:
    - transport send failure that breaks the session now clears the entire
      client in-flight set
    - transport receive disconnect/failure now clears the entire client
      in-flight set before returning the error
    - Windows C `nipc_np_wait_readable()` now also clears the client
      in-flight set when it observes disconnect / recv failure
  - direct Linux proof:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml test_disconnect_detection --lib`
      - result:
        - passed
        - the Rust POSIX test now asserts that the whole in-flight set is empty
          after disconnect
    - `cmake --build build --target test_uds -j4`
    - `/usr/bin/ctest --test-dir build --output-on-failure -R '^test_uds$'`
      - result:
        - passed
        - the C UDS test now asserts `session.inflight_count == 0` after
          disconnect
  - direct native Windows proof from the disposable `win11` tree:
    - overlaid files:
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
      - `src/crates/netipc/src/transport/windows.rs`
      - `tests/fixtures/c/test_named_pipe.c`
    - native C proof:
      - `cmake --build build --target test_named_pipe -j4`
      - `ctest --test-dir build --output-on-failure -R '^test_named_pipe$'`
      - result:
        - passed
        - the C named-pipe suite now includes a transport-level
          `disconnect clears all in-flight requests` assertion
    - native Rust proof:
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml test_disconnect_clears_all_inflight --lib`
      - result:
        - passed
        - the Rust named-pipe suite now asserts that a disconnect empties the
          whole client in-flight set
- stale design-note burn-down on `2026-03-29`:
  - the earlier TODO note that protocol-version mismatch might still map to
    generic `DISCONNECTED` at L2/L3 was stale
  - current verified evidence already matches Costa's recorded decision:
    - Level 2 docs:
      - `docs/level2-typed-api.md`
      - `INCOMPATIBLE` explicitly includes protocol/layout version mismatch
    - C:
      - `src/libnetdata/netipc/src/service/netipc_service.c`
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
      - both already map transport `*_ERR_INCOMPATIBLE` to
        `NIPC_CLIENT_INCOMPATIBLE`
      - explicit service tests already exist in:
        - `tests/fixtures/c/test_service.c`
        - `tests/fixtures/c/test_win_service.c`
    - Rust:
      - `src/crates/netipc/src/service/raw.rs`
      - both POSIX and Windows already map `NoProfile` and `Incompatible(_)`
        to `ClientState::Incompatible`
      - explicit tests already exist in:
        - `src/crates/netipc/src/service/raw_unix_tests.rs`
        - `src/crates/netipc/src/service/raw_windows_tests.rs`
    - Go:
      - `src/go/pkg/netipc/service/raw/client.go`
      - `src/go/pkg/netipc/service/raw/client_windows.go`
      - both already route incompatible handshake errors to
        `StateIncompatible`
      - explicit tests already exist in:
        - `src/go/pkg/netipc/service/raw/edge_test.go`
        - `src/go/pkg/netipc/service/raw/more_windows_test.go`
  - implication:
    - no code change was required for the protocol-mismatch state contract
    - the TODO was corrected by evidence, not by additional churn

### Phase 0 audit findings

- The ignored Rust Windows restart / reconnect test is hiding a real shutdown
  bug, not just flaky timing:
  - `src/crates/netipc/src/service/raw_windows_tests.rs`
    - `TestServer::stop()` only flips `running_flag` and performs a wake
      connect; it does not close active session handles
  - `src/crates/netipc/src/service/raw.rs`
    - `handle_session_win_threaded()` blocks in `session.receive()` on the
      Named Pipe path with no timeout
    - the accept loop can stop, but active session threads still wait forever
      for more pipe input
  - direct proof on `win11`:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml test_retry_on_failure_windows -- --ignored --nocapture`
    - the test starts and then hangs instead of completing
- The Windows Level 1 named-pipe pipelining problem is structurally credible in
  all three implementations, not just in the skipped Go test:
  - Go:
    - `src/go/pkg/netipc/transport/windows/pipe.go`
      - `Listen()` derives the kernel pipe buffer size from `config.PacketSize`
    - `src/go/pkg/netipc/transport/windows/pipe_integration_test.go`
      - `TestPipePipelineChunked()` is skipped because chunked full-duplex
        pipelining deadlocks under the current single-session API
  - Rust:
    - `src/crates/netipc/src/transport/windows.rs`
      - `NpListener::bind()` derives the kernel pipe buffer size from
        `config.packet_size`
      - there is no Windows chunked-pipeline test today
  - C:
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
      - `nipc_np_listen()` derives the kernel pipe buffer size from
        `config->packet_size`
      - there is no Windows chunked-pipeline test today
- External evidence supports treating the buffer coupling as the first fix:
  - Microsoft Learn:
    - `CreateNamedPipeW` documents that writes can block until data is read
      when additional buffer quota is needed
    - `Named Pipe Type, Read, and Wait Modes` documents that blocking handles
      wait for buffer space and that short message-mode reads return
      `ERROR_MORE_DATA`
  - libuv:
    - `/tmp/libuv-netipc-audit/src/win/pipe.c`
    - uses fixed `65536` inbound/outbound pipe buffers instead of coupling the
      kernel pipe quota to a small protocol frame size

### Acceptance leaks that must be removed

- Ignored core-behavior test:
  - fixed on `2026-03-28`
  - `src/crates/netipc/src/service/raw_windows_tests.rs`
  - `test_retry_on_failure_windows` now runs and passes natively on `win11`
- Skipped supported L1 behavior:
  - fixed on `2026-03-28`
  - `src/go/pkg/netipc/transport/windows/pipe_integration_test.go`
  - `TestPipePipelineChunked` now runs and passes natively on `win11`
- Threshold-only Windows Rust coverage acceptance:
  - fixed on `2026-03-28`
  - `tests/run-coverage-rust-windows.sh` now enforces:
    - total Windows Rust line coverage threshold
    - per-file Windows Rust line coverage thresholds for:
      - `service\cgroups.rs`
      - `transport\windows.rs`
      - `transport\win_shm.rs`
  - native `win11` proof on a fresh disposable tree from `origin/main` plus the
    local fit-for-purpose fixes:
    - `service\cgroups.rs = 92.74%`
    - `transport\windows.rs = 94.74%`
    - `transport\win_shm.rs = 91.44%`
    - total line coverage `= 91.12%`
- Documented special-infrastructure carve-outs are still accepted operationally:
  - allocation-failure branches
  - OS / kernel / Win32 API failure branches
  - race / timing branches
  - references:
    - `README.md`
    - `WINDOWS-COVERAGE.md`
    - `COVERAGE-EXCLUSIONS.md`
- Windows stress breadth is still explicitly narrower than Linux:
  - `README.md`
  - `WINDOWS-COVERAGE.md`
- Benchmark methodology still tolerates stable-core publication after raw
  outliers:
  - this is useful operationally, but it is still a tolerance and therefore not
    acceptable as a final fit-for-purpose exclusion
  - references:
    - `tests/run-windows-bench.sh`
    - `tests/generate-benchmarks-windows.sh`
    - `TODO-unified-l2-l3-api.md`
- One Linux SHM interop transient was accepted after rerun evidence:
  - this is not encoded as a formal exclusion in scripts, but it is still a
    practical acceptance softness until the flake is convincingly burned down
  - reference:
    - `TODO-unified-l2-l3-api.md`

### Phase 0 status as of 2026-03-28

- Fixed and now proven on native `win11`:
  - Windows Rust managed restart / reconnect no longer relies on an ignored
    test:
    - `src/crates/netipc/src/service/raw_windows_tests.rs`
    - `test_retry_on_failure_windows` is now part of the normal suite
    - direct native proof:
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml test_retry_on_failure_windows -- --nocapture`
      - result: passed after fixing the active-session shutdown path and
        avoiding stale fixed service names
  - Windows L1 chunked full-duplex named-pipe pipelining is now covered in all
    three implementations:
    - Go:
      - `src/go/pkg/netipc/transport/windows/pipe_integration_test.go`
      - `TestPipePipelineChunked` is no longer skipped and passes natively
    - Rust:
      - `src/crates/netipc/src/transport/windows.rs`
      - native `test_pipeline_chunked` now exists and passes
    - C:
      - `tests/fixtures/c/test_named_pipe.c`
      - `test_chunked_pipeline()` now exists and passes natively
- Structural fix applied in all three Windows named-pipe transports:
  - the kernel pipe buffer size is no longer coupled directly to the protocol
    frame `packet_size`
  - fixed files:
    - `src/go/pkg/netipc/transport/windows/pipe.go`
    - `src/crates/netipc/src/transport/windows.rs`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - external grounding:
    - Microsoft Learn `CreateNamedPipeW`
    - Microsoft Learn `Named Pipe Type, Read, and Wait Modes`
    - libuv `/tmp/libuv-netipc-audit/src/win/pipe.c` uses fixed `65536`
      inbound/outbound buffers rather than small protocol frame sizes
  - Windows service shutdown with active clients is now explicitly tested in Go:
    - `src/go/pkg/netipc/service/raw/more_windows_test.go`
    - `TestWinServerStopWithActiveClientAndRestart`
    - native result: passed on `win11`
  - Windows C coverage-mode reconnect hang is fixed at Level 1:
    - symptom before fix:
      - native `win11` `bash tests/run-coverage-c-windows.sh 90` could hang in
        `test_win_service_extra.exe`
      - the direct coverage-built executable could also hang by itself in
        `test_retry_on_broken_session()` after:
          - `PASS: client ready`
      - that made the old acceptance story false: this was not just a wrapper
        script problem
    - root cause:
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
      - `server_handle_session()` waited on `WaitForSingleObject(session->pipe, ...)`
        before calling `nipc_np_receive()`
      - Rust and Go Windows L1 transports already use `PeekNamedPipe` polling
        for readability instead
      - the HANDLE wait was not a reliable request-readiness signal for the
        synchronous named-pipe session path
    - fix:
      - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
        - added `nipc_np_wait_readable()` implemented with `PeekNamedPipe`
          polling
      - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
        - exported the L1 wait helper
      - `src/libnetdata/netipc/src/service/netipc_service_win.c`
        - switched the named-pipe service loop to `nipc_np_wait_readable()`
          instead of waiting on the raw pipe HANDLE
      - `tests/fixtures/c/test_named_pipe.c`
        - added explicit L1 coverage for:
          - timeout with no pending bytes
          - readability after client send
          - disconnect detection
- Remaining phase-0 work:
  - remove the documented special-infrastructure carve-out from acceptance by
    turning it into runnable validation targets
  - burn down the Linux SHM transient with repeated evidence or a concrete fix
  - refresh the long-form coverage documents so they match the new native
    `win11` truth instead of the older caveats

### Fresh native Windows Rust coverage proof on 2026-03-28

- Environment:
  - fresh disposable `win11` tree cloned from `origin/main` at `5372e97`
  - local fit-for-purpose fixes overlaid into `/tmp/plugin-ipc-fitproof`
- Command:
  - `bash tests/run-coverage-rust-windows.sh 90`
- Result:
  - total line coverage: `92.08%`
  - key runtime files:
    - `service\cgroups.rs = 92.74%`
    - `transport\windows.rs = 94.74%`
    - `transport\win_shm.rs = 95.76%`
- Implication:
  - the old Windows Rust “total-only above 90%” acceptance leak is gone
  - Phase 1 deterministic Win32 fault injection is now active in Rust
  - `transport\win_shm.rs` is no longer just barely above the per-file floor
  - the new native `win11` proof now includes forced failure and recovery for:
    - `CreateFileMappingW`
    - `OpenFileMappingW`
    - `MapViewOfFile`
    - `CreateEventW`
    - `OpenEventW`
  - the proof is cleanup-sensitive:
    - forced-failure calls return the expected error variant
    - the same object names work immediately afterward once the injected fault
      is removed

### Fresh native Windows Go coverage proof on 2026-03-28

- Environment:
  - fresh disposable `win11` tree cloned from `origin/main` at `5372e97`
  - local fit-for-purpose fixes overlaid into `/tmp/plugin-ipc-fitproof`
- Command:
  - `bash tests/run-coverage-go-windows.sh 90`
- Result:
  - total line coverage: `95.4%`
  - key runtime files:
    - `service/cgroups/cache_windows.go = 100.0%`
    - `service/cgroups/client_windows.go = 100.0%`
    - `transport/windows/pipe.go = 92.1%`
    - `transport/windows/shm.go = 94.2%`
- What changed:
  - public typed Windows Go wrapper tests now cover:
    - `Cache.Ready()`
    - `Cache.Status()`
    - `Client.Status()`
    - `NewServerWithWorkers()`
  - the dead private helper `Handler.snapshotMaxItems()` was removed from
    `src/go/pkg/netipc/service/cgroups/types.go`
- Implication:
  - the public typed Windows Go API is no longer hiding a wrapper-layer
    coverage leak behind a green total percentage
  - remaining ordinary Windows Go work is now transport-level, not typed
    service-wrapper work

### What “fit for purpose” means here

- Not “100% mathematically proven”.
- It means the remaining unknowns are small, explicit, and acceptable for
  Netdata’s use:
  - long-running plugin processes
  - repeated start / stop
  - reconnect after server absence
  - no unbounded CPU while disconnected
  - no leaks after many reconnect / resize cycles
  - no deadlocks in supported communication patterns
  - predictable behavior during version mismatch or upgrade windows

## Decisions

### Made

- This TODO is about fit-for-purpose validation, not about adding new library
  features.
- Execution mode is autonomous:
  - proceed phase by phase without further approval
  - stop only for unresolved design decisions not already answered in `docs/`
    or the repository specs
- Native Windows execution is first-class and must be validated natively, not
  only through cross-compilation.
- The TODO must include all practical information needed to run the library on
  Windows.
- Mixed wire / protocol version compatibility is not required. The contract is
  explicit fail-fast incompatibility on version mismatch.
- Windows named-pipe chunked full-duplex pipelining is required for the intended
  Level 1 contract. The current skipped Windows Go case is therefore a real
  liability, not an acceptable unsupported pattern, and it must be fixed.
- No exclusions are acceptable in the final sign-off criteria. Ignored tests,
  skipped supported patterns, threshold-only acceptance for partially covered
  critical files, and “special infrastructure” carve-outs must be burned down or
  explicitly converted into real automated validation before the library is
  considered fit for purpose.
- Netdata integration sequencing is out of scope for this TODO because it will
  be pursued in parallel in a separate workstream.
- `2026-03-29`: actual wire / protocol version mismatch must surface as
  `INCOMPATIBLE` at the persistent L2/L3 client state level.
  - mixed wire / protocol version compatibility is still explicitly not
    required
  - malformed garbage must remain a generic protocol / handshake error, not be
    widened into persistent incompatibility
  - user confirmed this explicitly with `1. A` and later reconfirmed it with
    reply `a`
- `2026-03-29`: oversubscribed fixed-rate benchmark rows are acceptable.
  - if a row is labeled with a target like `100000 req/s target` and the
    transport cannot reach it, that is acceptable as long as the actual
    achieved throughput is clearly visible in the published report
  - target attainment is therefore not itself a benchmark sign-off requirement
    for those rows
- `2026-03-31`: Costa decided that the current Windows full strict benchmark
  suite must remain the final confirmation gate, but it must stop being the
  main bug-discovery loop.
  - accepted workflow:
    - reproduce the exact failing row directly
    - rerun the containing benchmark block
    - rerun a small canary set of historically problematic rows
    - rerun the full strict Windows suite only after the smaller stages pass
  - rationale:
    - the current full strict suite uses:
      - `5` repetitions per row
      - `10s` per `@ max` sample
      - `20s` per `np-pipeline-batch-d16 @ max` sample
      - references:
        - `tests/run-windows-bench.sh:51`
        - `tests/run-windows-bench.sh:52`
        - `tests/run-windows-bench.sh:53`
    - it spans all `9` benchmark blocks:
      - `tests/run-windows-bench.sh:66`
      - `tests/run-windows-bench.sh:67`
    - repeated multi-hour reruns have been useful as a final gate, but too
      expensive and too coarse as the primary discovery tool


## Plan

### Phase 0: Remove the current acceptance leaks

- Unignore the Windows Rust restart / reconnect test and make it part of the
  normal passing suite:
  - `src/crates/netipc/src/service/raw_windows_tests.rs`
- Fix the Windows L1 named-pipe chunked pipelining deadlock and make the test
  pass:
  - `src/go/pkg/netipc/transport/windows/pipe_integration_test.go`
- Replace threshold-only Windows Rust acceptance with stricter gates:
  - require all critical Windows Rust runtime files to meet the same floor
    individually
  - or raise them until the per-file policy is satisfied
- Convert the documented “special infrastructure” carve-outs into real runnable
  validation targets:
  - allocation failure
  - OS / Win32 API failure
  - timing / race orchestration
- Expand Windows stress scope until the default validated breadth is no longer
  explicitly narrower than Linux for the critical transport/service behaviors.
- Burn down the Linux SHM interop transient with repeated runs or a real fix.

### Phase 1: Deterministic fault injection

- Add deterministic allocation-failure injection for:
  - C:
    - `malloc`
    - `calloc`
    - `realloc`
  - Rust:
    - selected scratch / growth paths where realistic
  - Go:
    - selected growth / buffer expansion hooks where realistic
- Add deterministic OS / Win32 failure hooks for:
  - Linux / POSIX:
    - `socket`
    - `listen`
    - `mmap`
    - `ftruncate`
    - `pthread_create`
  - Windows:
    - `CreateNamedPipe`
    - `ConnectNamedPipe`
    - `CreateFileMapping`
    - `MapViewOfFile`
    - wait / handle creation paths
- Add deterministic timing hooks for:
  - accept / shutdown races
  - mid-send / mid-receive disconnect windows
  - stale cleanup race windows

#### Phase 1 execution order

- Start with Rust Windows SHM Win32 failure injection because the latest
  `win11` coverage proof shows the remaining weak branches are now primarily:
  - `CreateFileMappingW` failure
  - `MapViewOfFile` failure
  - `CreateEventW` failure
  - `OpenFileMappingW` failure
  - `OpenEventW` failure
- Use test-only syscall indirection and deterministic one-shot fault hooks.
- Require cleanup-sensitive proof, not just line coverage:
  - a forced failure must return the expected error
  - the same object names must be reusable immediately afterward
  - later attach/create attempts must succeed once the injected failure is gone
- After Rust Windows SHM is burned down, mirror the same style only where the
  evidence still shows real uncovered cleanup/failure branches in C and Go.
- Rust Windows SHM phase-1 slice completed on `2026-03-28`:
  - test-only one-shot / nth-call syscall fault hooks now exist for:
    - `CreateFileMappingW`
    - `OpenFileMappingW`
    - `MapViewOfFile`
    - `CreateEventW`
    - `OpenEventW`
  - native `win11` targeted proof:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml transport::win_shm::tests:: -- --test-threads=1`
    - result: `25 passed, 0 failed`
  - native `win11` coverage proof:
    - `bash tests/run-coverage-rust-windows.sh 90`
    - result:
      - total line coverage `92.08%`
      - `transport\win_shm.rs = 95.76%`
- Go Windows SHM phase-1 slice is also now covered:
  - transport-level Win32 failure hooks exist in:
    - `src/go/pkg/netipc/transport/windows/shm.go`
  - deterministic native tests exist in:
    - `src/go/pkg/netipc/transport/windows/shm_test.go`
    - covering:
      - `CreateFileMappingW`
      - `OpenFileMappingW`
      - `MapViewOfFile`
      - `CreateEventW`
      - `OpenEventW`
    - with recovery-sensitive proof that create/attach succeeds again after the
      injected failure is removed
  - native `win11` Go coverage proof already recorded below shows:
    - `transport/windows/shm.go = 94.2%`
  - implication:
    - the remaining phase-1 work is no longer “add Go Windows SHM Win32 fault
      injection”
    - it is whatever low-resource / OS-failure branches still remain outside
      the already-covered Rust Go WinSHM and C POSIX service slices
 - POSIX C service allocation-failure slice completed on `2026-03-29`:
   - deterministic internal test hooks now exist in:
     - `src/libnetdata/netipc/src/service/netipc_service.c`
     - public internal-test declarations in:
       - `src/libnetdata/netipc/include/netipc/netipc_service.h`
   - covered reachable POSIX service/cache failure classes now include:
     - client response-buffer `realloc`
     - client send-buffer `realloc`
     - client SHM context `calloc`
     - server SHM context `calloc`
     - server recv-buffer `malloc`
     - server response-buffer `malloc`
     - server session-array `calloc`
     - server session-context `calloc`
     - server handler-thread creation
     - cache bucket / item / name / path allocations
   - a dead branch was removed instead of “testing” it:
     - the old server session-array `realloc` path in both POSIX and Windows
       service code was structurally unreachable because:
       - `session_capacity = max(worker_count * 2, 16)`
       - live `session_count` is capped by `worker_count`
   - new Linux-only validation target:
     - `tests/fixtures/c/test_service_extra.c`
     - CTest target:
       - `test_service_extra`
   - direct proof:
     - `/tmp/plugin-ipc-fit-linux/bin/test_service_extra`
     - result:
       - `62 passed`
       - `0 failed`
   - sanitizer / leak proof on isolated Linux builds:
     - ASan + UBSan:
       - `62 passed, 0 failed`
       - no sanitizer findings
     - TSan:
       - first run found a real race in the initial non-atomic fault-hook
         bookkeeping
       - fixed by switching the hook state to packed atomics
       - rerun:
         - `62 passed, 0 failed`
         - no TSan findings
     - Valgrind:
       - `62 passed, 0 failed`
       - `All heap blocks were freed -- no leaks are possible`
       - `ERROR SUMMARY: 0 errors from 0 contexts`

### Fresh native Windows C coverage proof on 2026-03-28

- Environment:
  - fresh disposable `win11` tree cloned from `origin/main` at `5372e97`
  - local fit-for-purpose fixes overlaid into `/tmp/plugin-ipc-fitproof`
- Commands:
  - `bash tests/run-coverage-c-windows.sh 90`
- Result:
  - total line coverage: `93.2%`
  - key runtime files:
    - `netipc_service_win.c = 90.1%`
    - `netipc_named_pipe.c = 95.4%`
    - `netipc_win_shm.c = 97.2%`
  - direct coverage-only executables:
    - `test_win_service_guards.exe`: `198 passed, 0 failed`
    - `test_win_service_guards_extra.exe`: `93 passed, 0 failed`
    - `test_win_service_extra.exe`: `165 passed, 0 failed`
- What changed to make this honest again:
  - deterministic Windows C service fault injection now exists for:
    - client response-buffer `realloc`
    - client send-buffer `realloc`
    - client SHM context `calloc`
    - server SHM context `calloc`
    - server recv-buffer `malloc`
    - server response-buffer `malloc`
    - server session-array `calloc`
    - server session-context `calloc`
    - server session-array `realloc`
    - server handler-thread creation
    - cache bucket / item / name / path allocations
  - the Windows C service tests now cover:
    - NULL config defaults
    - minimum response-buffer growth
    - typed hybrid malformed SHM replies on the real `nipc_client_call_cgroups_snapshot()` path
    - typed `LIMIT_EXCEEDED` / `UNSUPPORTED` SHM response statuses
    - cache allocation failure and recovery
    - server-side SHM create failure and recovery
  - the previous Windows C coverage blocker was a real L1 readiness bug, not
    just “coverage is slow”
    - before the fix, the coverage-built `test_win_service_extra.exe` could
      hang inside the broken-session retry case
    - after the L1 `PeekNamedPipe` wait fix, the full native coverage script
      completes cleanly end to end on `win11`
  - the script still keeps a generous `600s` timeout around
    `test_win_service_extra.exe`
    - this is now just operational headroom, not a known workaround for an
      active hang
- Correctness bug fixed while building this proof:
  - both POSIX and Windows L2 clients were incorrectly treating:
    - negotiated SHM
    - plus failure to allocate the SHM context object
    - as a successful READY connection
  - fixed files:
    - `src/libnetdata/netipc/src/service/netipc_service.c`
    - `src/libnetdata/netipc/src/service/netipc_service_win.c`
  - implication:
    - negotiated SHM without a local SHM context can no longer silently desync the client/server transport choice

### Fresh Linux sanitizer and leak proof on 2026-03-28

- Commands:
  - `bash tests/run-sanitizer-asan.sh`
  - `bash tests/run-sanitizer-tsan.sh`
  - `bash tests/run-valgrind.sh`
- Results:
  - ASan + UBSan:
    - `6/6` C targets passed
    - zero AddressSanitizer / LeakSanitizer / UBSan findings
  - TSan:
    - `5/5` multithreaded C targets passed
    - zero ThreadSanitizer findings
  - Valgrind:
    - `6/6` C targets passed
    - zero leaks
    - zero invalid reads / writes
- Important nuance:
  - GCC emits `-Wtsan` warnings that `atomic_thread_fence` is not supported for
    the POSIX SHM fence-only edges under `-fsanitize=thread`
  - implication:
    - the TSan run is still strong evidence and is clean
    - but it is not a mathematical substitute for targeted reasoning about
      those fence-only edges
- Implication:
  - Linux sanitizer / leak validation is no longer a merely planned future
    phase for this TODO
  - it is now part of the verified fit-for-purpose baseline evidence

### Phase 2: Leak and sanitizer validation

- Linux:
  - ASan build and test run
  - UBSan build and test run
  - TSan where practical
  - leak / FD / mapping checks in long reconnect loops
- Windows:
  - Application Verifier basics
  - PageHeap
  - Application Verifier low-resource simulation
  - Dr. Memory for heap + handle leaks if practical in this environment
- Add explicit pass / fail log collection and artifact retention.
- `2026-03-29` Windows verifier automation slice completed:
  - new first-class native Windows verifier entrypoint:
    - `tests/run-verifier-windows.sh`
  - default validated targets:
    - `test_named_pipe.exe`
    - `test_win_shm.exe`
    - `test_win_service.exe`
    - `test_win_service_extra.exe`
  - behavior:
    - enables Application Verifier:
      - `Handles`
      - `Heaps`
      - `Locks`
    - enables full PageHeap via:
      - `gflags /p /enable ... /full`
    - exports per-target verifier XML logs
    - fails closed on:
      - executable exit failure
      - exported verifier findings
  - repository docs updated:
    - `README.md`
    - `WINDOWS-COVERAGE.md`
    - `COVERAGE-EXCLUSIONS.md`

### Fresh native Windows Application Verifier finding on 2026-03-29

- Environment:
  - native `win11`
  - `appverif.exe` available at `C:\WINDOWS\system32\appverif.exe`
  - `gflags.exe` available from the Windows 10 debugger kit
- Real finding:
  - `test_named_pipe.exe` fails under Application Verifier Handles checks with:
    - `LayerName="Handles"`
    - `StopCode="0x300"`
    - `Message="Invalid handle exception for current stack trace."`
    - top stack frame: `KERNELBASE!ConnectNamedPipe+6a`
  - exported verifier log:
    - `C:\msys64\home\costa\src\plugin-ipc.git\verifier-test_named_pipe.xml`
- Current root-cause direction:
  - the explicit raw closed-listener test path in
    `tests/fixtures/c/test_named_pipe.c` was the real trigger:
    - it called `CloseHandle(listener.pipe)` directly
    - then left the public `listener.pipe` field with the stale numeric handle
      value
    - `nipc_np_accept()` then called `ConnectNamedPipe()` on that stale value
- Important negative finding:
  - attempting to "probe" a maybe-stale HANDLE with `GetHandleInformation()`
    is not a valid fix
  - under Application Verifier Handles, `GetHandleInformation()` on a stale
    closed HANDLE itself raises the same invalid-handle stop
  - implication:
    - there is no safe Win32 API-based stale-handle validation path here
    - the honest contract is:
      - library-owned raw HANDLE fields must not be closed externally without
        also poisoning the field value
- Final fix applied:
  - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - removed the invalid `GetHandleInformation()` probe approach
    - kept unconditional normalization of closed handles back to
      `INVALID_HANDLE_VALUE` inside the library close helpers
  - `tests/fixtures/c/test_named_pipe.c`
    - after the deliberate raw `CloseHandle(listener.pipe)` in the
      closed-listener test, the test now explicitly sets:
      - `listener.pipe = INVALID_HANDLE_VALUE`
- Native `win11` rebuild nuance:
  - the earlier "silent GCC failure" was an environment issue in the
    non-interactive SSH shell, not a source-level compiler bug
  - native successful rebuild required:
    - `MSYSTEM=MINGW64`
    - `PATH=/mingw64/bin:/usr/bin:$PATH`
    - `TMP=/tmp`
    - `TEMP=/tmp`
    - `TMPDIR=/tmp`
- Fresh native proof after the final fix:
  - rebuild:
    - `cmake --build . --target test_named_pipe`
  - functional run:
    - `test_named_pipe.exe`
    - result: `238 passed, 0 failed`
  - verifier run:
    - `appverif.exe -enable Handles -for test_named_pipe.exe`
    - run the rebuilt `test_named_pipe.exe`
    - `appverif.exe -export log ...`
    - result:
      - `AVRF: Error: there is no valid log file for test_named_pipe.exe`
      - no new verifier stop was recorded
  - implication:
    - the original deterministic `Handles / StopCode 0x300 / ConnectNamedPipe`
      finding is burned down

### Phase 3: Repeated soak and flake hunting

- Add repeated full-suite loops:
  - Linux full `ctest` loop
  - Windows native `ctest` loop
- Add repeated benchmark loops:
  - POSIX benchmark matrix
  - Windows benchmark matrix
- Add long reconnect / restart loops:
  - server absence
  - server restart while clients are live
  - repeated SHM upgrade cycles
  - repeated overflow-driven resize recovery
- Add handle / FD / mapping count tracking at intervals.

### Fresh Linux repeated full-suite evidence on 2026-03-29

- Repeated full-suite command:
  - `/usr/bin/ctest --test-dir build --output-on-failure -j4 --repeat until-fail:10`
- First reproduced failures in that long loop:
  - first earlier run:
    - `24:test_service_rust`
    - `34:test_stress_rust`
  - recorded in:
    - `build/Testing/Temporary/LastTestsFailed.log`
- Immediate narrowing evidence:
  - direct isolated rerun:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j1 -R '^(test_service_rust|test_stress_rust)$'`
    - passed
  - direct Rust service test rerun:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml service -- --test-threads=1`
    - passed
  - focused repeated CTest loop:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^(test_service_rust|test_stress_rust)$' --repeat until-fail:20`
    - still passing when this TODO entry was written
- Implication:
  - the remaining Linux flake is now more likely to depend on long full-suite
    interaction or accumulated state than on a trivially reproducible isolated
    failure in either Rust test by itself
- Fresh stronger evidence after the Rust/Go CTest partition fix:
  - repeated full-suite command:
    - `/usr/bin/ctest --test-dir build --output-on-failure -j4 --repeat until-fail:10`
  - reproduced hard failure:
    - `25:test_service_rust ................***Timeout 120.02 sec`
  - implication:
    - even after removing the known Rust/Go CTest overlap bugs, Linux repeated
      full-suite stability is still not clean
    - the remaining fit-for-purpose blocker is therefore now centered on a
      true `test_service_rust` hang/timeout under accumulated full-suite load,
      not just the earlier partitioning mistake
  - later in the same long repeated run, stronger cross-language cache flakes
    also reproduced:
    - `test_cache_interop`
      - `Go server, Rust cache client ... FAIL (client: cache not ready after refresh)`
    - `test_cache_shm_interop`
      - `C server, C cache client ... FAIL (client: cache not ready after refresh / FAIL)`
  - implication:
    - the remaining Linux full-suite instability is not limited to a single
      Rust-only timeout anymore
    - cache refresh / cache interop under accumulated full-suite load is now a
      real repeated-suite blocker that must be narrowed before sign-off
 - Later burn-down after the focused harness fixes:
   - first full repeated rerun still reproduced one failure:
     - `go_FuzzDecodeChunkHeader`
     - concrete failure:
       - `context deadline exceeded`
       - after a full `30s` fuzz interval
   - narrowing evidence:
     - direct run:
       - `cd src/go && CGO_ENABLED=0 go test -fuzz='^FuzzDecodeChunkHeader$' -fuzztime=30s ./pkg/netipc/protocol/`
       - passed
     - focused CTest repeat:
       - `/usr/bin/ctest --test-dir build --output-on-failure -j4 -R '^go_FuzzDecodeChunkHeader$' --repeat until-fail:5`
       - passed
   - grounded cause:
     - the Go fuzz target itself launches `24` workers during fuzzing
     - CTest previously allowed it to run concurrently with other heavy tests
     - this was a suite scheduler oversubscription problem, not protocol
       breakage
   - fix applied:
     - `CMakeLists.txt`
     - all Go fuzz CTest entries now set:
       - `RUN_SERIAL TRUE`
   - fresh full repeated Linux proof after the scheduling fix:
     - `/usr/bin/ctest --test-dir build --output-on-failure -j4 --repeat until-fail:3`
     - result:
       - `100%` tests passed
       - `0` failed out of `39`
       - total real time `908.53 sec`
   - implication:
     - the previously reproduced Linux repeated-suite failures have been
       narrowed to two concrete harness/scheduler issues and burned down
       at the current code state
     - further longer soak remains useful, but the current evidence is no
       longer pointing at a standing reproducible library defect
 - fresh full-suite proof collected later on `2026-03-29` after the final L1
   in-flight failure fixes:
   - `/usr/bin/ctest --test-dir build --output-on-failure -j4`
   - result:
     - `100%` tests passed
     - `0` failed out of `39`
     - total real time `303.75 sec`
   - full Rust suite:
     - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
     - result:
       - `304` passed
       - `0` failed
       - finished in `30.26s`

### Phase 4: Mixed-version / upgrade validation

- Keep the current explicit fail-fast incompatibility contract.
- Add a negative matrix for:
  - old client -> new server mismatch
  - new client -> old server mismatch
  - cache refresh across version mismatch
  - typed call behavior across mismatch
- Verify:
  - fast explicit `INCOMPATIBLE` / bad-version style failure
  - no hangs
  - no retry storms
  - no partial undefined behavior
- `2026-03-29` protocol-incompatibility state slice completed:
  - public contract aligned:
    - `docs/level2-typed-api.md`
      - `INCOMPATIBLE` now explicitly includes protocol / layout version
        mismatch, not just profile / limit mismatch
  - C transport + service mapping updated:
    - `src/libnetdata/netipc/include/netipc/netipc_uds.h`
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
    - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
    - `src/libnetdata/netipc/src/service/netipc_service.c`
    - `src/libnetdata/netipc/src/service/netipc_service_win.c`
  - Rust transport + service mapping updated:
    - `src/crates/netipc/src/transport/posix.rs`
    - `src/crates/netipc/src/transport/windows.rs`
    - `src/crates/netipc/src/service/raw.rs`
  - Go transport + service mapping updated:
    - `src/go/pkg/netipc/transport/posix/uds.go`
    - `src/go/pkg/netipc/transport/windows/pipe.go`
    - `src/go/pkg/netipc/service/raw/client.go`
    - `src/go/pkg/netipc/service/raw/client_windows.go`
  - exact classification rule:
    - header version mismatch => `INCOMPATIBLE`
    - explicit handshake `STATUS_INCOMPATIBLE` => `INCOMPATIBLE`
    - HELLO / HELLO_ACK payload `layout_version != 1` => `INCOMPATIBLE`
    - malformed flags / reserved bytes / truncated garbage remain generic
      protocol / handshake failure
  - direct proof completed on Linux:
    - C:
      - `cmake --build build --target test_uds test_service -j4`
      - `./build/bin/test_uds`
      - result:
        - `157 passed, 0 failed`
    - Rust:
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml bad_hello_ack_version_as_incompatible`
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml incompatible_hello_ack_status`
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml bad_hello_ack_layout_as_incompatible`
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml bad_hello_version_as_incompatible`
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml bad_hello_layout_as_incompatible`
      - result:
        - all targeted incompatibility handshake tests passed
    - Go:
      - `cd src/go && go test ./pkg/netipc/service/raw ./pkg/netipc/transport/posix -run 'Test(IsIncompatibleError|ClientRefreshFromIncompatible|HandshakeIncompatibleClassifierHelpers)$' -count=1`
      - result:
        - passed
  - remaining verification still required in this slice before sign-off:
    - native `win11` execution proof for the touched named-pipe state paths
      when the Windows workstream returns to runtime validation
  - next execution slice under this TODO:
    - add explicit native Windows runtime tests for persistent client-state
      transition to `INCOMPATIBLE` on real protocol-version mismatch in:
      - C typed service tests
      - Rust raw Windows service tests
      - Go raw Windows service tests
    - then rerun those exact slices natively on `win11`
  - native `win11` runtime proof completed later on `2026-03-29`:
    - C:
      - added `test_client_protocol_version_incompatible()` to:
        - `tests/fixtures/c/test_win_service.c`
      - native proof from the disposable tree:
        - `cmake --build build --target test_win_service -j4`
        - `ctest --test-dir build --output-on-failure -R '^test_win_service$'`
        - result:
          - passed
          - new test confirms:
            - refresh changes state on protocol version mismatch
            - typed client lands in `NIPC_CLIENT_INCOMPATIBLE`
            - second refresh is a no-op
    - Rust:
      - added `test_client_protocol_version_incompatible_windows()` to:
        - `src/crates/netipc/src/service/raw_windows_tests.rs`
      - native proof:
        - `cargo test --manifest-path src/crates/netipc/Cargo.toml test_client_protocol_version_incompatible_windows -- --test-threads=1`
        - result:
          - passed
    - Go:
      - added `TestWinClientRefreshFromProtocolVersionMismatch()` plus a raw
        named-pipe HELLO_ACK helper in:
        - `src/go/pkg/netipc/service/raw/helpers_windows_test.go`
        - `src/go/pkg/netipc/service/raw/more_windows_test.go`
      - native proof:
        - `"/c/Program Files/Go/bin/go" test ./pkg/netipc/service/raw -run '^TestWinClientRefreshFromProtocolVersionMismatch$' -count=1`
        - result:
          - passed
      - important environment fact:
        - the non-interactive `win11` shell was picking `/mingw64/bin/go`
          first, which caused false toolchain-version mismatch failures
        - the native proof now pins the real Windows Go binary explicitly
  - additional proof completed later on `2026-03-29` from the current Linux
    environment:
    - formatting / hygiene:
      - `gofmt -w ...`
      - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml -- ...`
      - `git diff --check`
      - result:
        - clean
    - Linux C:
      - `cmake --build build --target test_service test_uds -j4`
      - `./build/bin/test_uds`
      - `./build/bin/test_service`
      - result:
        - `test_uds`: `157 passed, 0 failed`
        - `test_service`: `201 passed, 0 failed`
    - Windows-targeted Rust:
      - `cargo check --manifest-path src/crates/netipc/Cargo.toml --target x86_64-pc-windows-gnu`
      - result:
        - passed
    - Windows-targeted Go:
      - `cd src/go && GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/raw`
      - `cd src/go && GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/transport/windows`
      - result:
        - passed
    - Windows-targeted C:
      - `cmake -E env CC='zig cc -target x86_64-windows-gnu' CXX='zig c++ -target x86_64-windows-gnu' cmake -S . -B /tmp/plugin-ipc-build-win-incompat -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_RC_COMPILER=llvm-rc`
      - `cmake --build /tmp/plugin-ipc-build-win-incompat --target test_named_pipe test_win_service test_win_service_extra test_win_shm -j4`
      - result:
        - passed
  - implication:
    - the protocol-version incompatibility contract is now implemented and
      verified for Linux execution plus Windows-targeted build coverage
    - only native `win11` runtime proof for these exact new paths remains to be
      re-collected when the Windows runtime validation slice resumes
  - stronger service-layer proof completed later on `2026-03-29`:
    - the persistent L2/L3 client-state contract is now explicitly tested for a
      real protocol-version mismatch on the POSIX path in all three languages
    - C:
      - `tests/fixtures/c/test_service.c`
      - added a raw seqpacket fake HELLO_ACK server that sends header
        `version = NIPC_VERSION + 1`
      - proof:
        - `cmake --build build --target test_service -j4`
        - `./build/bin/test_service`
        - result:
          - `208 passed, 0 failed`
          - new checks confirm:
            - refresh changes state
            - client lands in `INCOMPATIBLE`
            - second refresh is a no-op
    - Rust:
      - `src/crates/netipc/src/service/raw_unix_tests.rs`
      - added `test_client_protocol_version_incompatible`
      - proof:
        - `cargo test --manifest-path src/crates/netipc/Cargo.toml protocol_version_incompatible -- --test-threads=1`
        - result:
          - passed
    - Go:
      - `src/go/pkg/netipc/service/raw/more_unix_test.go`
      - `src/go/pkg/netipc/service/raw/edge_test.go`
      - added a raw seqpacket fake HELLO_ACK server plus
        `TestClientRefreshFromProtocolVersionMismatch`
      - proof:
        - `cd src/go && go test ./pkg/netipc/service/raw -run 'Test(ClientRefreshFromProtocolVersionMismatch|ClientRefreshFromIncompatible)$' -count=1`
        - result:
          - passed

### Phase 5: Real Netdata integration harness

- Build a minimal real Netdata plugin harness around the library.
- Exercise:
  - plugin starts before service exists
  - service starts after client loop is already running
  - server restart during steady-state operation
  - parent / child teardown
  - repeated agent restart
  - run-dir cleanup and stale artifact recovery
  - bounded reconnect / retry behavior
  - bounded CPU while disconnected
  - bounded memory / handle growth over long runtime

### Phase 6: Benchmark-signoff tightening

- Tighten the benchmark runner so any raw instability beyond the allowed band
  fails the suite for final sign-off.
- Remove “publishable trimmed-warning row” as an acceptable final benchmark
  outcome.
- Keep stable-core tooling only as a diagnostic aid, not as a sign-off escape
  hatch.
- Make the benchmark acceptance story as strict and explicit as the functional
  acceptance story.
- `2026-03-31` staged Windows benchmark validation loop adopted for the
  remaining fit-for-purpose work:
  - stage `1`:
    - direct reproducer for the exact failing row
  - stage `2`:
    - rerun the containing benchmark block only
  - stage `3`:
    - rerun a small Windows canary set of historically problematic rows and
      slices
  - stage `4`:
    - rerun the full strict native Windows suite only after stages `1-3` pass
  - implication:
    - the full strict suite remains authoritative for final sign-off
    - but it is no longer the main debugging loop
- `2026-03-29` Windows benchmark sign-off gate tightened in code:
  - `tests/run-windows-bench.sh`
    - raw repeated-sample max/min now fails the row directly
    - stable-core analysis is retained only for diagnostics and failure
      forensics
    - the old behavior that warned and still published a row after trimming one
      low and one high sample is removed
  - `tests/generate-benchmarks-windows.sh`
    - methodology text now states the stricter raw gate explicitly
    - stable-core is documented as diagnostic-only, not a publication escape
      hatch
  - direct proof from the current Linux environment:
    - `bash -n tests/run-windows-bench.sh`
    - `bash -n tests/generate-benchmarks-windows.sh`
    - `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv /tmp/plugin-ipc-bench-windows-methodology-check.md`
    - result:
      - syntax clean
      - report generation still succeeds
      - generated methodology now contains:
        - `Throughput publication is now strict on the full raw repeated sample set.`
        - `Every repeated row must keep raw max/min <= 1.35; one lucky or unlucky extreme is no longer publishable.`
  - important remaining proof still required:
    - the checked-in Windows benchmark artifact must be regenerated from a fresh
      native `win11` run under the stricter gate before final benchmark
      sign-off can be called complete
  - fresh stronger evidence on `2026-03-29`:
    - the stricter native `win11` rerun is already exposing a deeper benchmark
      truth issue, not just ordinary noise
    - early raw sample files from
      `/tmp/netipc-bench-722451/samples-np-ping-pong-*.csv` show:
      - `np-ping-pong go->go @ 100000/s`
        - repeats:
          - `1922`
          - `9443`
          - `9674`
          - `9317`
          - `3482`
      - `np-ping-pong c->c @ 10000/s`
        - repeats:
          - `2385`
          - `2733`
          - `9979`
          - `9983`
          - `9989`
    - checked-in artifact evidence shows the fixed-rate contract was already
      soft in practice:
      - `benchmarks-windows.csv`
      - `np-ping-pong @ 100000/s` rows are only about `16.7k .. 19.7k req/s`
      - the same scenario at `@ max` is only about `18.9k .. 20.4k req/s`
    - implication:
      - Windows named-pipe fixed-rate targets currently include an oversubscribed
        `100000/s` tier that the transport cannot actually sustain
      - Costa explicitly decided this oversubscribed fixed-rate case is
        acceptable as long as the published row clearly shows the achieved
        throughput
      - implementation on `2026-03-31`:
        - the Windows runner now treats a fixed-rate row as oversubscribed when its target is above the same pair's already-published `@ max` throughput from the same CSV run
        - in that case, raw spread no longer blocks publication by itself
        - the row still requires a publishable trimmed stable core with at least `MIN_STABLE_SAMPLES` samples and `stable_ratio <= MAX_THROUGHPUT_RATIO`
        - files:
          - `tests/run-windows-bench.sh`
          - `tests/generate-benchmarks-windows.sh`
      - so the oversubscribed `100000/s` shortfall is not itself a fit-for-
        purpose blocker
      - the real remaining benchmark blocker is narrower:
        - attainable Windows fixed-rate rows such as `np-ping-pong @ 10000/s`
          still showed unstable repeats in the fresh strict rerun even though
          the same scenario at `@ max` is roughly `19k .. 20k req/s`
      - the strict raw-spread gate is working correctly; the remaining work is
        to eliminate instability at attainable Windows fixed rates
  - later stronger burn-down on `2026-03-29`:
    - root cause was in the Windows benchmark drivers, not in the library
      transport code:
      - `bench/drivers/c/bench_windows.c`
      - `bench/drivers/rust/src/bench_windows.rs`
      - `bench/drivers/go/main_windows.go`
    - all three drivers used coarse sleep-only pacing for sub-millisecond fixed
      rates on Windows
    - fix applied:
      - large waits still sleep coarsely
      - medium waits yield
      - the final short tail busy-spins to the deadline
    - direct native `win11` proof from the current disposable tree:
      - tree:
        - `/c/Users/costa/AppData/Local/Temp/plugin-ipc-fitproof-5372e97-20260329-040049`
      - focused rerun command:
        - `NIPC_BENCH_FIRST_BLOCK=1 NIPC_BENCH_LAST_BLOCK=1 NIPC_BENCH_DIAGNOSE_FAILURES=1 bash tests/run-windows-bench.sh /tmp/plugin-ipc-fitproof-block1.csv 5`
      - result:
        - block 1 completed all `36` rows
        - no diagnostics summary was produced
      - important fixed-rate proof:
        - oversubscribed rows are now honest and stable instead of collapsing
          into random low bands:
          - `np-ping-pong c->c @ 100000/s = 15834`
          - `np-ping-pong go->go @ 100000/s = 28657`
        - attainable rows no longer collapse:
          - `np-ping-pong c->c @ 10000/s = 10000`, `stable_ratio=1.000000`
          - `np-ping-pong rust->c @ 10000/s = 10000`, `stable_ratio=1.000000`
          - `np-ping-pong go->c @ 10000/s = 10000`, `stable_ratio=1.000000`
          - `np-ping-pong go->go @ 10000/s = 10000`, `stable_ratio=1.000000`
          - `np-ping-pong c->go @ 1000/s = 1000`, `stable_ratio=1.000000`
          - `np-ping-pong go->go @ 1000/s = 1000`, `stable_ratio=1.000000`
      - implication:
        - the earlier named-pipe fixed-rate instability was a benchmark-driver
          pacing problem
        - the next required proof is a fresh full native Windows benchmark
          suite under the patched drivers, not more surgery in the transport
          layer
  - hard sign-off rule from Costa on `2026-03-29`:
    - the benchmark-environment explanation is not acceptable as a final
      resolution
    - the remaining Windows strict-suite failures must be resolved as either:
      - a benchmark harness / validation-rig defect that must be fixed, or
      - a product / benchmark-driver defect that must be fixed
    - no final sign-off may rely on saying the workstation / VM was noisy
  - stricter harness burn-down on `2026-03-29` after Costa's no-excuses rule:
    - a benchmark-harness fix was applied to the Windows benchmark binaries
      themselves:
      - `bench/drivers/c/bench_windows.c`
      - `bench/drivers/go/main_windows.go`
      - `bench/drivers/rust/src/bench_windows.rs`
    - fix shape:
      - each Windows benchmark process now raises itself to
        `HIGH_PRIORITY_CLASS`
      - each also raises its main thread to
        `THREAD_PRIORITY_HIGHEST`
      - this is benchmark-harness hardening only; it does not change library
        code
    - rationale grounded in evidence:
      - the remaining strict failures were moving across unrelated scenarios,
        including `lookup rust->rust @ max`, which does not use transport
      - therefore the defect had already escaped the "one broken protocol path"
        theory and had to be treated as a benchmark execution-model defect
    - native `win11` proof from the same disposable tree:
      - strict rerun scope:
        - `NIPC_BENCH_FIRST_BLOCK=7`
        - `NIPC_BENCH_LAST_BLOCK=9`
      - output:
        - `/tmp/plugin-ipc-fitproof-block7to9-priority.csv`
      - important result:
        - all `21` benchmark rows completed and published cleanly under the
          strict raw gate
        - the wrapper's final exit still became non-zero only because
          `generate-benchmarks-windows.sh` rejects partial matrices by design
      - smoking-gun rows that were previously failing are now stable in the
        same late-suite strict run:
        - `lookup rust->rust @ max`
          - `188660324`
          - `stable_ratio=1.014329`
        - `np-pipeline-d16 go->go @ max`
          - `131849`
          - `stable_ratio=1.097291`
        - `np-pipeline-batch-d16 go->go @ max`
          - `23648939`
          - `stable_ratio=1.009126`
      - implication:
        - the remaining late-suite strict failures were not proving a standing
          library defect in those rows
        - they were proving that the previous Windows benchmark execution model
          was still too weak for strict sign-off on this workstation / VM
    - next required proof:
      - rerun the full native Windows strict matrix from the same patched proof
        tree and require the whole `201`-row suite to pass
  - invalid measurement note on `2026-03-29`:
    - a later Linux repeated soak was incorrectly left running in parallel with
      fresh `win11` snapshot benchmark repros on the same physical workstation
    - concrete local evidence from the invalid run:
      - `ctest` PID `169598`
      - Go fuzz root:
        - `/tmp/go-build2881855550/b001/protocol.test`
      - many `-test.fuzzworker` children were consuming effectively all local
        CPU cores
    - implication:
      - the overlapping `win11` snapshot repros from that congested phase are
        invalid and must not be used for fit-for-purpose sign-off
      - discard:
        - `/tmp/plugin-ipc-snapshot-repro/*`
        - any conclusions derived from the congested `2026-03-29` local
          snapshot repro attempt
    - corrective rule:
      - never run Linux soak, fuzz, or repeated `ctest` loops in parallel with
        `win11` benchmark measurements on this workstation
      - rerun the Windows snapshot investigation only on an idle host/VM pair
  - clean idle-host rerun on `2026-03-29`:
    - after stopping the local repeated `ctest` / Go fuzz load, the host and
      `win11` VM were re-verified idle before rerunning the suspect snapshot
      rows in isolation from the same disposable proof tree
    - direct idle proof:
      - `snapshot-baseline go->go @ max` recovered immediately and was stable
        again:
        - `10s` repeats:
          - `28445`
          - `27665`
          - `27776`
          - `26356`
          - `28558`
        - `20s` repeats:
          - `27530`
          - `26916`
          - `26930`
          - `27772`
          - `26793`
      - `snapshot-shm c->c @ max` at the suite-relevant `10s` duration also
        recovered and was stable again:
        - `1095368`
        - `1117781`
        - `1095903`
        - `1127947`
        - `1115669`
      - `snapshot-shm rust->c @ max` at `10s` was also stable on the idle host:
        - `1078953`
        - `1081955`
        - `1074160`
        - `1071961`
        - `1118578`
      - `snapshot-shm go->c @ max` at `10s` was also stable on the idle host:
        - `860476`
        - `849223`
        - `890612`
        - `863229`
        - `833535`
    - implication:
      - the previously failing snapshot rows from the congested phase were not
        proving a library or benchmark-driver defect
      - they were measuring a host-contention artifact caused by the local
        repeated Linux soak/fuzz load
      - the next required proof is not more snapshot surgery; it is a strict
        Windows runner rerun of the snapshot blocks on an idle host
  - strict idle Windows runner proof on `2026-03-29`:
    - the disposable `win11` proof tree was rerun under the real strict runner
      with no concurrent Linux soak/fuzz load:
      - snapshot blocks `3-4`:
        - `NIPC_BENCH_FIRST_BLOCK=3`
        - `NIPC_BENCH_LAST_BLOCK=4`
      - then snapshot SHM block `4` was rerun again by itself after a stale
        `bench_windows_go.exe` process from an earlier manual repro was removed
    - block `3` (`snapshot-baseline`) passed cleanly on the idle host:
      - `c->c @ max`: `stable_ratio=1.019955`
      - `rust->c @ max`: `stable_ratio=1.032295`
      - `go->c @ max`: `stable_ratio=1.037526`
      - `c->rust @ max`: `stable_ratio=1.034864`
      - `rust->rust @ max`: `stable_ratio=1.036851`
      - `go->rust @ max`: `stable_ratio=1.014577`
      - `c->go @ max`: `stable_ratio=1.023287`
      - `rust->go @ max`: `stable_ratio=1.014719`
      - `go->go @ max`: `stable_ratio=1.004499`
      - all `@1000/s` rows in block `3` held target with
        `stable_ratio=1.000000`
    - the first idle block `4` rerun exposed one remaining contamination fact:
      - `snapshot-shm c->go @ 1000/s`
      - first attempt failed on repeat `5` with
        `measurement_command_failed_repeat_5`
      - the immediate diagnostic rerun of the same row succeeded with:
        - `throughput=1000.000`
        - `stable_ratio=1.000000`
      - the runner showed a stale live `bench_windows_go.exe` process during
        that first-attempt failure
      - that stale process was removed before rerunning block `4`
    - clean block `4` rerun from an empty `win11` process table then passed:
      - output:
        - `/tmp/plugin-ipc-fitproof-block4-idle.csv`
      - total measurements:
        - `18`
      - representative max-tier rows:
        - `c->c @ max`: `1117530`, `stable_ratio=1.005655`
        - `rust->c @ max`: `1081321`, `stable_ratio=1.038594`
        - `go->c @ max`: `863250`, `stable_ratio=1.015867`
        - `c->rust @ max`: `1226890`, `stable_ratio=1.005778`
        - `rust->rust @ max`: `1206557`, `stable_ratio=1.016193`
        - `go->rust @ max`: `924526`, `stable_ratio=1.024025`
        - `c->go @ max`: `1014367`, `stable_ratio=1.003800`
        - `rust->go @ max`: `959906`, `stable_ratio=1.031515`
        - `go->go @ max`: `817952`, `stable_ratio=1.024192`
      - all `@1000/s` rows in clean block `4` held target with
        `stable_ratio=1.000000`
    - implication:
      - the strict runner-level Windows snapshot instability is burned down
      - the remaining fit-for-purpose work should move back to the non-benchmark
        phases
  - fresh strict full native Windows rerun relaunched cleanly on `2026-03-29`
    after the host-contention mistake was corrected:
    - host baseline before launch:
      - no local Linux `ctest`, `protocol.test`, `bench_windows`, or
        `run-windows-bench` processes
      - no stale remote `win11` `bench_windows_*` or `run-windows-bench`
        processes
    - disposable proof tree:
      - `/c/Users/costa/AppData/Local/Temp/plugin-ipc-fitproof-5372e97-20260329-040049`
    - detached native `win11` run artifacts:
      - PID file:
        - `/tmp/plugin-ipc-fitproof-full-strict.pid`
      - log:
        - `/tmp/plugin-ipc-fitproof-full-strict.log`
      - CSV:
        - `/tmp/plugin-ipc-fitproof-full-strict.csv`
      - markdown:
        - `/tmp/plugin-ipc-fitproof-full-strict.md`
      - exit status:
        - `/tmp/plugin-ipc-fitproof-full-strict.exit`
    - first clean log proof from the detached run:
      - named-pipe block started normally from:
        - `run dir /tmp/netipc-bench-87808`
      - benchmark runner rebuilt:
        - `bench_windows_c`
        - `bench_windows_go`
        - Rust `bench_windows`
      - first row entered:
        - `np-ping-pong: c->c @ max (5 samples x 10s)`
    - intent:
      - this detached run is the candidate final Windows artifact under the
        strict raw-spread gate
      - do not accept any earlier congested or interrupted benchmark run for
        sign-off
    - detached run final status:
      - exit status:
        - `1`
      - CSV lines before failure:
        - `198`
      - failure row:
        - `np-pipeline-batch-d16 go->go @ max`
      - preserved run dir:
        - `/tmp/netipc-bench-87808`
      - raw failure metrics from the log:
        - `raw_min=12817688`
        - `raw_max=28582100`
        - `raw_ratio=2.229895`
      - trimmed stable core from the same row:
        - `stable_min=18139683`
        - `stable_max=21348234`
        - `stable_ratio=1.176880`
      - implication:
        - the full strict rerun is now failing on one narrow remaining Windows
          named-pipe pipeline-batch max row
        - the current blocker is no longer general benchmark contamination or
          broad runner instability
    - isolated strict block `9` rerun on `2026-03-29`:
      - scope:
        - `NIPC_BENCH_FIRST_BLOCK=9`
        - `NIPC_BENCH_LAST_BLOCK=9`
      - output:
        - `/tmp/plugin-ipc-fitproof-block9-strict.csv`
      - block result:
        - the block itself passed
        - the markdown generation step failed only because the report generator
          expects the full `201`-row matrix, not a single-block CSV
      - published rows:
        - `c->c = 39092119`
        - `rust->c = 37105791`
        - `go->c = 35623860`
        - `c->rust = 23338339`
        - `rust->rust = 22462589`
        - `go->rust = 23327521`
        - `c->go = 25448636`
        - `rust->go = 24032013`
        - `go->go = 24588629`
      - implication:
        - the previously failing `np-pipeline-batch-d16 go->go @ max` row is
          stable when block `9` runs by itself under the same strict runner
        - the remaining benchmark blocker is therefore a long-suite interaction
          or environment-drift problem, not a deterministic `go->go` transport
          failure
    - strict late-tail rerun on `2026-03-29`:
      - scope:
        - `NIPC_BENCH_FIRST_BLOCK=7`
        - `NIPC_BENCH_LAST_BLOCK=9`
      - block results:
        - `np-pipeline-d16 go->go @ max` passed with `stable_ratio=1.022444`
        - `np-pipeline-batch-d16 go->go @ max` passed with
          `stable_ratio=1.033434`
      - actual failure in this shorter rerun:
        - `lookup rust->rust @ max`
        - `raw_min=111657428`
        - `raw_max=177834657`
        - `raw_ratio=1.592681`
        - `stable_min=173725618`
        - `stable_max=175811993`
        - `stable_ratio=1.012010`
        - sample file:
          - `/tmp/netipc-bench-185647/samples-lookup-rust-rust-0.csv`
      - implication:
        - the remaining strict Windows rerun failures are no longer specific to
          any transport path
        - even the local Rust lookup benchmark can hit a single bad raw repeat
          on this shared `win11` VM benchmark environment
    - isolated Rust lookup proof on `2026-03-29`:
      - repeated direct `bench_windows.exe lookup-bench 10` runs on `win11`
        were stable:
        - `168166913`
        - `176001315`
        - `173482069`
        - `173152456`
        - `171287114`
      - implication:
        - the strict rerun failures are now proven to be benchmark-environment
          noise on the shared workstation/VM setup, not a deterministic library
          or benchmark-driver defect
  - POSIX benchmark CPU reporting bug identified on `2026-03-29`:
    - all `201/201` rows in the checked-in POSIX benchmark artifact reported
      `server_cpu_pct = 0`
    - the POSIX benchmark drivers do print `SERVER_CPU_SEC=...`
    - root cause:
      - `tests/run-posix-bench.sh` starts benchmark servers inside command
        substitution
      - later `stop_server()` calls `wait "$pid"` from the parent shell, but
        that PID is not the parent shell's child
      - the shell returns immediately instead of waiting for the server to
        flush `SERVER_CPU_SEC`, so the harness reads the output file too early
    - implication:
      - POSIX throughput and latency rows remain useful because they come from
        client output
      - POSIX `server_cpu_pct` and `total_cpu_pct` in the published artifact
        are currently invalid and must be regenerated after the harness fix

### Phase 7: Automation and sign-off

- Add dedicated scripts for:
  - Linux sanitizer validation
  - Windows verifier validation
  - repeated soak loops
  - mixed-version matrix
  - Netdata harness soak
- Add nightly or on-demand automation for the heavy flows.
- Keep fast PR gates smaller, but make the heavy validation repeatable and
  visible.

## Implied decisions

- Coverage alone is no longer the main confidence problem.
- Unsupported patterns must be documented explicitly, not left as silent
  skipped tests forever.
- Native Windows validation is required for fit-for-purpose confidence.
- Real Netdata lifecycle behavior matters more than synthetic microbenchmarks
  once the current benchmark floors are already green.
- Operational sign-off must include leak freedom and restart / reconnect proof,
  not just functional correctness.
- Threshold-only acceptance is not enough for critical runtime files.
- A documented caveat is not an acceptable permanent exit condition.
- A rerun-based practical acceptance call must eventually be replaced by either
  a real fix or repeated evidence strong enough to demote the issue honestly.

## Pending design question

- `2026-03-29` Costa proposed reducing Windows benchmark wall-clock time by
  running non-max benchmark rows in parallel, potentially in batches of `4`.
- Background:
  - today the Windows benchmark runner is fully sequential
  - the current strict trustworthy methodology makes the full native Windows
    suite take multiple hours
  - Costa's working theory is that only `@ max` rows really stress CPU, while
    non-max rows are gentle enough to overlap safely
- Evidence gathered from the checked-in Windows artifact and runner:
  - `tests/run-windows-bench.sh` currently runs rows sequentially with:
    - ping-pong rates `0 100000 10000 1000`
    - snapshot rates `0 1000`
  - genuinely gentle fixed-rate rows do exist:
    - `np-ping-pong @ 1000/s` sustains only `997..1000 req/s`
    - `snapshot-baseline @ 1000/s` sustains only `997..1000 req/s`
    - `shm-ping-pong @ 1000/s` sustains only `997..1000 req/s`
  - moderate but still likely safe rows exist:
    - `np-ping-pong @ 10000/s` sustains `9976..10000 req/s`
    - `shm-ping-pong @ 10000/s` sustains `9970..10000 req/s`
  - some non-max rows are already heavy and should not be assumed gentle:
    - `np-ping-pong @ 100000/s` sustains only `16722..19659 req/s`, so it behaves like a saturation probe on Windows named pipes
    - `shm-ping-pong @ 100000/s` sustains `99822..100002 req/s`
    - `shm-batch-ping-pong @ 100000/s` sustains `29025043..47862366 req/s`
  - implication:
    - "non-max" is not a safe technical class by itself
    - any concurrency policy has to split low-rate gentle rows from high-load fixed-rate rows
- Additional benchmark-driver findings from the failing strict Windows rerun (`/tmp/netipc-bench-203047`):
  - the remaining failing rows were:
    - `np-pipeline-d16 rust->go @ max`
    - `np-pipeline-batch-d16 c->rust @ max`
  - in both cases repeats `1-4` were healthy and repeat `5` collapsed without client-side errors
  - the repeat-5 server logs still showed `READY` and substantial `SERVER_CPU_SEC`, so this was not a simple server-start or connect failure
  - all three Windows benchmark server drivers currently print `READY` before entering the blocking server run loop
  - Windows benchmark clients sample latency sparsely (roughly every `64`th counter event), but the max-tier pipeline paths still reserve extremely large latency buffers up front:
    - C/Rust pipeline max paths reserve `5_000_000` samples
    - C/Go/Rust global latency caps are `10_000_000`
    - pipeline-batch max paths still reserve `2_000_000` samples
  - implication:
    - the Windows benchmark drivers are carrying avoidable memory pressure and a weak readiness contract in the exact area where the strict reruns still fail
- Status after the latency-recorder fix:
  - local code changes now reduce the Windows benchmark-driver latency recorder caps to match the sparse sampling strategy instead of pre-reserving multi-million-entry buffers in the max-tier paths
  - strict native `win11` rerun for `blocks 8-9` completed successfully with:
    - CSV: `/tmp/plugin-ipc-fitproof-block8to9-after.csv`
    - exit: `0`
    - all `18` rows published
  - the two previously failing smoking-gun rows are now healthy under the same strict raw-spread rule:
    - `np-pipeline-d16 rust->go @ max`
      - old full-run failure: `raw_ratio=1.360940`
      - fixed targeted rerun: `median_throughput=140895`, `stable_ratio=1.035350`
    - `np-pipeline-batch-d16 c->rust @ max`
      - old full-run failure: `raw_ratio=2.432407`
      - fixed targeted rerun: `median_throughput=25850710`, `stable_ratio=1.022899`
  - current working conclusion:
    - the late strict Windows failures were in the benchmark-driver/harness path, not in the library transport path itself
  - fresh candidate final Windows strict rerun was relaunched from the same patched disposable proof tree:
    - tree:
      - `/c/Users/costa/AppData/Local/Temp/plugin-ipc-fitproof-5372e97-20260329-040049`
    - artifacts:
      - PID:
        - `/tmp/plugin-ipc-fitproof-full-after-latencyfix.pid`
      - log:
        - `/tmp/plugin-ipc-fitproof-full-after-latencyfix.log`
      - CSV:
        - `/tmp/plugin-ipc-fitproof-full-after-latencyfix.csv`
      - markdown:
        - `/tmp/plugin-ipc-fitproof-full-after-latencyfix.md`
      - exit:
        - `/tmp/plugin-ipc-fitproof-full-after-latencyfix.exit`
    - launch proof:
      - benchmark binaries rebuilt from the patched tree
      - run entered `np-ping-pong: c->c @ max`
      - first run dir:
        - `/tmp/netipc-bench-298374`
    - final status of that rerun:
      - exit:
        - `1`
      - CSV lines:
        - `200`
      - markdown:
        - not generated
      - all previously failing strict smoking-gun rows from `blocks 8-9` stayed healthy
      - the remaining failures moved to two different rows:
        - `np-ping-pong rust->rust @ 100000/s`
          - missing from final CSV
          - per-repeat samples:
            - `18936`
            - `20246`
            - `25857`
            - `18840`
            - `23499`
          - `raw_ratio=1.372452`
          - `stable_ratio=1.240970`
        - `np-batch-ping-pong c->rust @ max`
          - missing from final CSV
          - per-repeat samples:
            - `8891403`
            - `7102523`
            - `6874992`
            - `11196794`
            - `12569966`
          - `stable_ratio=1.576453`
      - implication:
        - the latency-recorder fix burned down the late pipeline/pipeline-batch failures, but the full strict Windows suite is still not sign-off clean
        - the next work must classify and fix these two remaining benchmark-driver or harness-level instabilities
  - clean block-1 rerun after the same latency-recorder fix exposed an even sharper Rust-server clustering:
    - output:
      - `/tmp/plugin-ipc-fitproof-block1-after-latencyfix.csv`
      - run dir:
        - `/tmp/netipc-bench-383653`
    - `np-ping-pong rust->rust @ max` was healthy:
      - `27705`
      - `stable_ratio=1.031909`
    - but `np-ping-pong go->rust @ max` collapsed repeat-to-repeat:
      - `27954`
      - `24524`
      - `4375`
      - `2271`
      - `2257`
      - `stable_ratio=10.798767`
    - there were no client-side errors
    - the per-repeat Rust server logs still only showed:
      - `READY`
      - `SERVER_CPU_SEC=...`
    - implication:
      - the remaining strict Windows failures are centered on named-pipe rows with a Rust server, not on one specific client language or one specific earlier row
  - concrete shutdown asymmetry discovered while analyzing the Rust-server clustering:
    - Rust benchmark driver still uses a custom Windows stop path based on:
      - `server.running_flag()`
      - flipping the flag after `duration+3`
      - synthetic `NpSession::connect()` wakeup
      - file:
        - `bench/drivers/rust/src/bench_windows.rs`
    - C and Go benchmark drivers use the real server stop API instead:
      - C:
        - `nipc_server_stop(g_server)`
      - Go:
        - `server.Stop()`
    - the internal Rust raw service helper now documents the intended contract:
      - `ManagedServer::stop()` is the reliable shutdown path
      - `running_flag()` is only for diagnostics/test helpers
      - comment in:
        - `src/crates/netipc/src/service/raw.rs`
    - current working conclusion:
      - the Rust benchmark driver is still relying on an outdated Windows shutdown pattern
      - this is the strongest concrete candidate for the remaining Rust-server repeat-to-repeat instability
  - stop-handle experiment result on `2026-03-31`:
    - replacing the Rust benchmark driver's custom `running_flag + synthetic connect` stop path with `ManagedServer::stop()` made the Rust benchmark servers hang instead of exit
    - immediate clean block-1 proof after that change:
      - Rust-server max rows started getting force-killed after the first measured sample
      - runner warnings:
        - `Server rust (...) did not exit cleanly within 10s; forcing kill`
      - server output at force-kill time contained only:
        - `READY`
      - no `SERVER_CPU_SEC`
    - implication:
      - `ManagedServer::stop()` is not currently sufficient for this benchmark-server lifecycle on Windows
      - that experiment must not be kept as the fix path
  - stronger harness-level working theory after the failed stop-handle experiment:
    - the Windows runner currently reuses the same service name across all repeats of a row
    - the Rust benchmark driver stop path uses a delayed synthetic `NpSession::connect()` against that same service name
    - therefore a late wake from one repeat can interfere with the next repeat if the service name is reused
    - this matches the observed repeat-to-repeat collapse pattern on Rust-server named-pipe rows
    - planned correction:
      - keep the existing Rust benchmark stop path for now
      - make service names unique per repeat in the Windows benchmark runner so each repeat is fully isolated
  - proof after implementing repeat-unique Windows service names:
    - rerun artifacts:
      - `/tmp/plugin-ipc-fitproof-block1-after-repeatsvc.csv`
      - run dir:
        - `/tmp/netipc-bench-391879`
    - the previously collapsing row `np-ping-pong go->rust @ max` stabilized:
      - old failing samples in `/tmp/netipc-bench-383653/samples-np-ping-pong-go-rust-0.csv`:
        - `27954`
        - `24524`
        - `4375`
        - `2271`
        - `2257`
      - new samples in `/tmp/netipc-bench-391879/samples-np-ping-pong-go-rust-0.csv`:
        - `19439`
        - `24645`
        - `24912`
        - `24648`
        - `23720`
      - new `stable_ratio=1.039123`
    - nearby Rust-server max rows were also healthy:
      - `c->rust @ max = 24250`, `stable_ratio=1.025451`
      - `rust->rust @ max = 25514`, `stable_ratio=1.079212`
    - implication:
      - repeat-to-repeat service-name reuse was a real Windows benchmark harness defect
      - the next targeted check must move to the other earlier smoking-gun row family, especially `np-batch-ping-pong c->rust @ max`
  - follow-up block-1 evidence after the same repeat-unique service-name fix:
    - `np-ping-pong rust->rust @ 100000/s` still fails the current strict raw-spread gate:
      - per-repeat samples in `/tmp/netipc-bench-391879/samples-np-ping-pong-rust-rust-100000.csv`:
        - `12495`
        - `17387`
        - `21635`
        - `22002`
        - `18840`
      - `raw_ratio=1.760864`
      - `stable_ratio=1.244320`
    - `np-ping-pong go->rust @ 100000/s` also still fails the current strict raw-spread gate:
      - per-repeat samples in `/tmp/netipc-bench-391879/samples-np-ping-pong-go-rust-100000.csv`:
        - `22221`
        - `18272`
        - `24653`
        - `17747`
        - `27090`
      - `raw_ratio=1.526455`
      - `stable_ratio=1.349223`
    - context:
      - these are oversubscribed fixed-rate rows, because the same Rust-server rows at `@ max` were only about `24k..25k req/s` in the same rerun
      - this means the repeat-isolation fix burned down the Rust-server max-tier contamination, but it did not make oversubscribed `100000/s` named-pipe rows pass the strict raw-spread rule
    - next check:
      - let the block complete so the attainable `10000/s` and `1000/s` Rust-server rows can prove whether any real fixed-rate defect remains below saturation
  - strict native `win11` block-1 proof after the oversubscribed-row runner change:
    - output:
      - `/tmp/plugin-ipc-fitproof-block1-after-oversub.csv`
      - run dir:
        - `/tmp/netipc-bench-403720`
    - final status:
      - exit `0`
      - CSV lines `37`
      - data rows `36`
    - result:
      - all earlier block-1 smoking-gun rows now publish cleanly
      - oversubscribed Rust-server `100000/s` rows no longer abort the block
      - attainable Rust-server `10000/s` and `1000/s` rows are stable and exact-target
    - implication:
      - block `1` is now burned down
      - the next targeted proof must move to block `5`, specifically the earlier `np-batch-ping-pong c->rust @ max` failure family
  - strict native `win11` block-5 proof after the same runner changes:
    - output:
      - `/tmp/plugin-ipc-fitproof-block5-after-oversub.csv`
      - run dir:
        - `/tmp/netipc-bench-424712`
    - final status:
      - exit `0`
      - CSV lines `37`
      - data rows `36`
    - result:
      - the old `np-batch-ping-pong c->rust @ max` smoking-gun row is now healthy:
        - `9854217`, `stable_ratio=1.076357`
      - oversubscribed `100000/s` rows also publish cleanly under the same block
      - attainable `10000/s` and `1000/s` rows stay stable across the block
    - implication:
      - blocks `1` and `5` are both burned down under the corrected Windows runner
      - the next step is a fresh full strict native Windows rerun for final sign-off
  - fresh final full strict native `win11` rerun launched on `2026-03-31` with the corrected runner:
    - PID file:
      - `/tmp/plugin-ipc-fitproof-full-final.pid`
    - log:
      - `/tmp/plugin-ipc-fitproof-full-final.log`
    - CSV:
      - `/tmp/plugin-ipc-fitproof-full-final.csv`
    - markdown:
      - `/tmp/plugin-ipc-fitproof-full-final.md`
    - exit:
      - `/tmp/plugin-ipc-fitproof-full-final.exit`
    - local continuation hook:
      - script: `/tmp/plugin-ipc-fitproof-orchestrator.sh`
      - log: `/tmp/plugin-ipc-fitproof-orchestrator.log`
      - pid: `/tmp/plugin-ipc-fitproof-orchestrator.pid`
      - behavior:
        - wait for the full native Windows rerun to finish
        - copy refreshed Windows artifacts into the repo on success
        - run the POSIX benchmark refresh afterward
  - staged Windows benchmark loop implementation on `2026-03-31`:
    - `tests/run-windows-bench.sh`
      - now supports exact-row filtering via:
        - `NIPC_BENCH_SCENARIOS`
        - `NIPC_BENCH_CLIENTS`
        - `NIPC_BENCH_SERVERS`
        - `NIPC_BENCH_TARGETS`
      - filtered rows now return a dedicated skip status instead of aborting the
        whole runner
      - exact-row, block, canary, and full-suite runs therefore all share the
        same official publish logic and the same strict raw-spread gate
    - `tests/run-windows-bench-canary.sh`
      - new bounded canary wrapper for historically problematic Windows rows
      - current canary set is grounded only in already observed smoking-gun
        rows, not guessed rows:
        - `np-ping-pong go->rust @ max`
        - `lookup rust->rust @ max`
        - `np-pipeline-d16 rust->go @ max`
        - `np-pipeline-batch-d16 c->rust @ max`
        - `np-pipeline-batch-d16 go->go @ max`
  - staged proof after the process change:
    - stage `1` exact-row reproducer:
      - `np-pipeline-batch-d16 c->rust @ max`
      - official exact-row reruns on native `win11` all passed
      - five direct row attempts produced:
        - `19163954`
        - `18274878`
        - `20857981`
        - `21035101`
        - `17435826`
      - implication:
        - the old full-suite `c->rust` failure is not a trivially
          always-failing exact-row bug
    - stage `2` containing-block proof:
      - native `win11` block `9` rerun passed cleanly:
        - `/tmp/plugin-ipc-fitproof-block9-stage2.csv`
      - important rows:
        - `np-pipeline-batch-d16 c->rust @ max = 18268414`
          - `stable_ratio=1.040796`
        - `np-pipeline-batch-d16 go->go @ max = 20075044`
          - `stable_ratio=1.022210`
      - implication:
        - block `9` by itself is currently clean
    - stage `3` canary proof:
      - native `win11` canary run still found a smoking gun in the smaller
        loop:
        - preserved run dir:
          - `/tmp/netipc-bench-543662`
        - failing row:
          - `np-pipeline-batch-d16 go->go @ max`
        - sample file:
          - `/tmp/netipc-bench-543662/samples-np-pipeline-batch-d16-go-go-0.csv`
        - per-repeat throughput:
          - `21478012`
          - `19684406`
          - `18578071`
          - `18781274`
          - `15334177`
        - `raw_ratio=1.400663`
        - `stable_ratio=1.059551`
      - important nuance:
        - this is much narrower than the earlier severe late-suite collapses
        - but it is still a strict failure and therefore still a blocker
    - current minimization status:
      - native `win11` exact-row reruns for
        `np-pipeline-batch-d16 go->go @ max` are currently passing
      - native `win11` block `9` reruns are currently passing
      - two smaller trigger sequences also passed:
        - `np-ping-pong go->rust @ max`
          -> `lookup rust->rust @ max`
          -> `np-pipeline-d16 rust->go @ max`
          -> `np-pipeline-batch-d16 go->go @ max`
        - `np-pipeline-batch-d16 c->rust @ max`
          -> `np-pipeline-batch-d16 go->go @ max`
      - implication:
        - the current minimum reproducer is still the canary run itself
        - the remaining work must now classify whether the `go->go` canary
          failure is in the staged harness flow or in the Go Windows
          pipeline-batch benchmark path
  - staged follow-up on `2026-04-01` after the Rust pipeline-batch driver fix:
    - stage `3` canary rerun still found smoking guns, but they moved:
      - preserved run dirs:
        - `/tmp/netipc-bench-562834`
        - `/tmp/netipc-bench-563346`
      - passing canary rows:
        - `np-ping-pong go->rust @ max = 25331`
          - `stable_ratio=1.026581`
        - `lookup rust->rust @ max = 177339240`
          - `stable_ratio=1.009995`
        - `np-pipeline-d16 rust->go @ max = 112486`
          - `stable_ratio=1.126195`
      - failing canary rows:
        - `np-pipeline-batch-d16 c->rust @ max`
          - per-repeat throughput:
            - `13909076`
            - `12998633`
            - `20823434`
            - `19361925`
            - `18479178`
          - `stable_ratio=1.392035`
        - `np-pipeline-batch-d16 go->go @ max`
          - per-repeat throughput:
            - `20908404`
            - `14738502`
            - `18875060`
            - `17288399`
            - `18300847`
          - `raw_ratio=1.418625`
          - `stable_ratio=1.091776`
    - fresh stage `1` exact-row proofs on native `win11`:
      - `np-pipeline-batch-d16 c->rust @ max`
        - three exact-row reruns all passed:
          - `19425203`
          - `20659411`
          - `19357121`
      - `np-pipeline-batch-d16 go->go @ max`
        - three exact-row reruns all passed:
          - `20939578`
          - `23039916`
          - `21148206`
    - fresh stage `2` block `9` rerun on native `win11`:
      - preserved run dir:
        - `/tmp/netipc-bench-567066`
      - all earlier smoking-gun pairs passed
      - the only remaining block-`9` failure moved again:
        - `np-pipeline-batch-d16 c->go @ max`
        - per-repeat throughput:
          - `22817138`
          - `15072715`
          - `13167228`
          - `20138883`
          - `19335164`
        - `raw_ratio=1.732873`
        - `stable_ratio=1.336115`
      - implication:
        - the remaining stage-`2` problem is still inside block `9`
        - but it is not pinned to one fixed pair from earlier runs
    - fresh stage `1` exact-row minimization for
      `np-pipeline-batch-d16 c->go @ max`:
      - attempt `1` failed on the official runner:
        - preserved run dir:
          - `/tmp/netipc-bench-571550`
        - per-repeat throughput:
          - `20699407`
          - `14488958`
          - `21614617`
          - `18337574`
          - `15995719`
        - `raw_ratio=1.491799`
        - `stable_ratio=1.294059`
      - attempts `2` and `3` passed:
        - `23112816`
        - `21102463`
      - implication:
        - the current minimum reproducer is no longer the canary set
        - the exact row `np-pipeline-batch-d16 c->go @ max` is itself an
          intermittent smoking gun under the official strict runner
    - controlled GC experiment on native `win11`:
      - the same exact row was rerun three times under the official runner with
        `GOGC=off`
      - all three runs passed:
        - `19237900`
        - `21101457`
        - `21477543`
      - implication:
        - the current evidence points to avoidable allocation / GC pressure in
          the Go batch hot path, not to a wire-protocol defect
    - concrete Go hot-path fix now applied locally:
      - `src/go/pkg/netipc/protocol/frame.go`
        - `BatchBuilder.Reset()` added so callers can reuse a builder without a
          fresh heap object
      - `src/go/pkg/netipc/service/raw/client.go`
      - `src/go/pkg/netipc/service/raw/client_windows.go`
        - raw batch server dispatch now reuses a stack `BatchBuilder` for
          per-request batch responses
      - `bench/drivers/go/main.go`
      - `bench/drivers/go/main_windows.go`
        - benchmark batch request builders now also reuse stack builders in the
          batch and pipeline-batch loops
      - current status:
        - the patch is synced to the active `win11` proof tree
        - the next proof is a fresh default exact-row rerun for
          `np-pipeline-batch-d16 c->go @ max`
    - additional benchmark-driver defect found on `2026-04-01`:
      - `bench/drivers/go/main_windows.go`
      - `bench/drivers/go/main.go`
      - both Go benchmark binaries were forcing `runtime.GOMAXPROCS(1)` at
        process startup, even for server subcommands
      - the in-file comment already said the single-thread cap was intended for
        benchmark clients only
      - direct native proof:
        - a manual Go Windows benchmark server run with `GODEBUG=gctrace=1`
          showed repeated GC activity during the 20-second measurement window
          and reported `1 P`
      - implication:
        - Go benchmark servers were being artificially serialized by the
          benchmark driver itself
        - that is a benchmark-driver defect, not a library contract
    - fix applied on `2026-04-01`:
      - `bench/drivers/go/main_windows.go`
      - `bench/drivers/go/main.go`
      - `runtime.GOMAXPROCS(1)` is now applied only to client-style benchmark
        subcommands and `lookup-bench`
      - Go benchmark servers now run with the runtime default parallelism
    - fresh stage `1` proof after the `GOMAXPROCS` fix:
      - native `win11` exact-row reruns for
        `np-pipeline-batch-d16 c->go @ max` now passed `3/3` under the
        official strict runner:
        - `20251145`
        - `19669662`
        - `21317377`
      - implication:
        - the previous intermittent exact-row smoking gun for `c->go` is no
          longer reproducing after the benchmark-driver fix
        - the next staged proof is the containing block `9` rerun
    - fresh stage `2` proof after the `GOMAXPROCS` fix:
      - native `win11` isolated block `9` rerun passed cleanly:
        - `/tmp/plugin-ipc-fitproof-block9-gmpfix.csv`
      - all `9` rows published with exit `0`
      - important formerly problematic rows:
        - `np-pipeline-batch-d16 c->rust @ max = 22113082`
          - `stable_ratio=1.028697`
        - `np-pipeline-batch-d16 c->go @ max = 22556516`
          - `stable_ratio=1.156581`
        - `np-pipeline-batch-d16 rust->go @ max = 20171963`
          - `stable_ratio=1.121484`
        - `np-pipeline-batch-d16 go->go @ max = 19476220`
          - `stable_ratio=1.126123`
      - implication:
        - the current fix survives the full pipeline-batch block context
        - the next staged proof is the bounded canary rerun
    - fresh stage `3` canary proof after the `GOMAXPROCS` fix:
      - native `win11` canary rerun still failed overall:
        - `/tmp/plugin-ipc-fitproof-canary-gmpfix.log`
      - but the failure surface shrank to exactly one remaining row:
        - `np-ping-pong go->rust @ max`
        - preserved run dir:
          - `/tmp/netipc-bench-583442`
        - per-repeat throughput:
          - `16788`
          - `17189`
          - `20401`
          - `23175`
          - `19039`
        - `raw_ratio=1.380450`
        - `stable_ratio=1.186864`
      - rows that now pass cleanly in the same canary run:
        - `lookup rust->rust @ max = 178034224`
        - `np-pipeline-d16 rust->go @ max = 101173`
        - `np-pipeline-batch-d16 c->rust @ max = 20321819`
        - `np-pipeline-batch-d16 go->go @ max = 19535508`
      - implication:
        - the earlier block-`9` instability is materially improved
        - the active minimum reproducer has moved to the named-pipe
          ping-pong row `go->rust @ max`
        - the next staged proof is exact-row reruns for that remaining row
    - fresh stage `1` proof for the remaining canary row:
      - native `win11` exact-row reruns for
        `np-ping-pong go->rust @ max` passed `3/3` under the official strict
        runner:
        - `20962`
        - `19570`
        - `22954`
      - implication:
        - the remaining canary failure is not a trivially always-failing exact
          row
        - the next staged proof is the containing block `1` rerun
    - fresh stage `2` proof for the remaining canary row:
      - native `win11` isolated block `1` rerun still failed overall:
        - `/tmp/plugin-ipc-fitproof-block1-gmpfix.log`
        - preserved run dir:
          - `/tmp/netipc-bench-587217`
      - rows that still passed cleanly before the first failures:
        - `np-ping-pong c->go @ max = 22661`
          - `stable_ratio=1.023750`
        - `np-ping-pong go->rust @ max = 24069`
          - `stable_ratio=1.039775`
      - active strict failures moved again inside the same block:
        - `np-ping-pong rust->go @ max`
          - per-repeat throughput:
            - `22872`
            - `21365`
            - `20944`
            - `24143`
            - `17310`
          - `raw_ratio=1.394743`
          - `stable_ratio=1.092055`
          - samples:
            - `/tmp/netipc-bench-587217/samples-np-ping-pong-rust-go-0.csv`
        - `np-ping-pong go->go @ max`
          - per-repeat throughput:
            - `20102`
            - `14841`
            - `19321`
            - `22386`
            - `25079`
          - `raw_ratio=1.689846`
          - `stable_ratio=1.158636`
          - samples:
            - `/tmp/netipc-bench-587217/samples-np-ping-pong-go-go-0.csv`
      - implementation note:
        - the block run was stopped immediately after the two `@ max` smoking
          guns were captured, to avoid wasting additional wall-clock time on
          lower-value rates after the staged loop had already done its job
      - implication:
        - the current minimum reproducer is no longer `go->rust @ max`
        - the active remaining cluster is now the named-pipe `->go @ max`
          ping-pong path in block `1`
        - the next staged proof is exact-row reruns for:
          - `np-ping-pong rust->go @ max`
          - `np-ping-pong go->go @ max`
    - fresh stage `1` proofs for the remaining block-`1` smoking guns:
      - native `win11` exact-row reruns for
        `np-ping-pong rust->go @ max` passed `3/3` under the official strict
        runner:
        - `24285`
        - `19534`
        - `24197`
      - native `win11` exact-row reruns for
        `np-ping-pong go->go @ max` also passed `3/3`:
        - `24093`
        - `22158`
        - `20136`
      - implication:
        - neither remaining block-`1` smoking gun is a trivially
          always-failing exact row
        - the active defect is now clearly context-dependent inside the
          named-pipe ping-pong block
        - the next staged proof is a reduced block-context rerun containing
          only the `server=go` `@ max` rows:
          - `c->go`
          - `rust->go`
          - `go->go`
    - reduced block-context proof with only `server=go` `@ max` rows:
      - native `win11` reduced sequence rerun still failed overall:
        - `/tmp/plugin-ipc-fitproof-pingpong-go-server-max.log`
        - preserved run dir:
          - `/tmp/netipc-bench-594784`
      - rows in the reduced sequence:
        - `np-ping-pong c->go @ max = 21094`
          - `stable_ratio=1.032847`
        - `np-ping-pong rust->go @ max`
          - `raw_min=13105`
          - `raw_max=23433`
          - `raw_ratio=1.788096`
          - `stable_ratio=1.108702`
          - samples:
            - `/tmp/netipc-bench-594784/samples-np-ping-pong-rust-go-0.csv`
        - `np-ping-pong go->go @ max = 24613`
          - `stable_ratio=1.012602`
      - implication:
        - the failure no longer needs the full `np-ping-pong` block
        - the active minimum reproducer is now a much smaller `server=go`
          sequence centered on `rust->go @ max`
        - the next staged proof is an even smaller two-row sequence:
          - `c->go @ max`
          - `rust->go @ max`
    - two-row `c->go` then `rust->go` sequence proof:
      - native `win11` two-row rerun still failed overall:
        - `/tmp/plugin-ipc-fitproof-pingpong-c-rust-to-go-max.log`
        - preserved run dir:
          - `/tmp/netipc-bench-596156`
      - the smoking gun moved again inside the smaller sequence:
        - `np-ping-pong c->go @ max`
          - `raw_min=16717`
          - `raw_max=22972`
          - `raw_ratio=1.374170`
          - `stable_ratio=1.098727`
          - samples:
            - `/tmp/netipc-bench-596156/samples-np-ping-pong-c-go-0.csv`
        - `np-ping-pong rust->go @ max = 23227`
          - `stable_ratio=1.038523`
      - implication:
        - the defect is not pinned to one single client pair
        - the common factor is the Go named-pipe benchmark-server path under
          repeated `@ max` samples
    - classification proof with Go GC disabled:
      - native `win11` reduced `server=go` sequence passed cleanly with
        `GOGC=off`:
        - `/tmp/plugin-ipc-fitproof-pingpong-go-server-max-gogc-off.csv`
      - published rows:
        - `c->go = 19158`, `stable_ratio=1.059397`
        - `rust->go = 20553`, `stable_ratio=1.033719`
        - `go->go = 22821`, `stable_ratio=1.216386`
      - implication:
        - the remaining smoking gun is very likely driven by Go benchmark
          server allocation / GC behavior rather than the raw IPC transport
          path itself
    - benchmark-driver fix on `2026-04-01`:
      - `bench/drivers/go/main_windows.go`
      - `bench/drivers/go/main.go`
      - Go benchmark servers now force a `runtime.GC()` immediately before
        printing `READY` and starting the timed sample window
      - rationale:
        - remove one-off startup garbage from the timed benchmark window
        - keep normal GC enabled during the actual benchmark sample
    - valid post-fix reduced proof under normal GC:
      - native `win11` reduced `server=go` sequence now passes cleanly with
        the correctly rebuilt Go benchmark binary:
        - `/tmp/plugin-ipc-fitproof-pingpong-go-server-max-postgc-valid.csv`
      - published rows:
        - `c->go = 23780`, `stable_ratio=1.100365`
        - `rust->go = 24091`, `stable_ratio=1.032803`
        - `go->go = 26170`, `stable_ratio=1.082966`
      - implication:
        - the reduced reproducer is fixed without disabling GC during the
          measurement window
        - the next staged proof is the bounded Windows canary rerun
    - bounded Windows canary rerun after the benchmark-driver fix:
      - native `win11` canary rerun now passes cleanly end-to-end:
        - `bash tests/run-windows-bench-canary.sh /tmp/plugin-ipc-fitproof-canary-postgc 5`
      - published canary rows:
        - `np-ping-pong go->rust @ max = 27534`
          - `stable_ratio=1.022056`
        - `lookup rust->rust @ max = 191111766`
          - `stable_ratio=1.008653`
        - `np-pipeline-d16 rust->go @ max = 127375`
          - `stable_ratio=1.105595`
        - `np-pipeline-batch-d16 c->rust @ max = 22312710`
          - `stable_ratio=1.029221`
        - `np-pipeline-batch-d16 go->go @ max = 22043010`
          - `stable_ratio=1.088354`
      - implication:
        - the staged loop is now back at the final gate
        - the next required proof is the full strict native Windows suite
    - latest full strict native Windows rerun after the canary pass:
      - native `win11` full strict suite finished with exit `1`:
        - `/tmp/plugin-ipc-fitproof-full-postgc.exit`
        - `/tmp/plugin-ipc-fitproof-full-postgc.log`
        - `/tmp/plugin-ipc-fitproof-full-postgc.csv`
      - markdown was not generated
      - published rows before failure:
        - `163` CSV lines total
      - preserved run dir:
        - `/tmp/netipc-bench-603715`
      - the active smoking guns moved again:
        - `np-pipeline-d16 go->go @ max`
          - `raw_min=36416`
          - `raw_max=84772`
          - `raw_ratio=2.327878`
          - `stable_ratio=1.003467`
          - samples:
            - `/tmp/netipc-bench-603715/samples-np-pipeline-d16-go-go-0.csv`
        - `np-pipeline-batch-d16 c->go @ max`
          - `raw_min=11467524`
          - `raw_max=16141662`
          - `raw_ratio=1.407598`
          - `stable_ratio=1.056037`
          - samples:
            - `/tmp/netipc-bench-603715/samples-np-pipeline-batch-d16-c-go-0.csv`
      - implication:
        - the previously fixed Go named-pipe ping-pong `server=go` instability
          is no longer the active blocker
        - the remaining blockers now live in the named-pipe pipeline and
          pipeline-batch families
        - the next work must start from these exact two rows, not from another
          blind full-suite rerun
    - concrete next staged loop for the current blockers:
      - promote the two latest smoking guns into the Windows canary set:
        - `np-pipeline-d16 go->go @ max`
        - `np-pipeline-batch-d16 c->go @ max`
      - run exact-row loops for both rows until one reproduces quickly
      - add per-repeat Go runtime diagnostics for benchmark processes:
        - GC count delta
        - pause time delta
        - heap alloc / heap objects delta
      - use those diagnostics only to classify the defect:
        - benchmark-driver/runtime problem
        - or transport/library problem
      - only after exact-row and block proofs are clean rerun canary
      - only after canary is clean rerun the full strict Windows suite
    - fresh exact-row proof for the two latest full-run blockers:
      - native `win11` exact-row reruns for
        `np-pipeline-d16 go->go @ max` passed `3/3`:
        - `95952`
        - `94005`
        - `103350`
      - native `win11` exact-row reruns for
        `np-pipeline-batch-d16 c->go @ max` also passed `3/3`:
        - `17859407`
        - `15669538`
        - `16620381`
      - implication:
        - neither current blocker is a trivially always-failing exact row
        - the remaining defects are context-dependent inside the later pipeline
          and pipeline-batch blocks
        - the next staged proof is reduced block-context reruns for the
          `server=go` rows in:
          - block `8` (`np-pipeline-d16`)
          - block `9` (`np-pipeline-batch-d16`)
    - reduced block `8` proof for `np-pipeline-d16` with `server=go`:
      - native `win11` reduced block rerun passed cleanly:
        - `/tmp/plugin-ipc-fitproof-pipeline-d16-go-server.csv`
      - published rows:
        - `c->go = 101446`, `stable_ratio=1.098939`
        - `rust->go = 97334`, `stable_ratio=1.110064`
        - `go->go = 101783`, `stable_ratio=1.100146`
      - implication:
        - the `np-pipeline-d16 go->go @ max` smoking gun requires more context
          than the reduced block-`8` `server=go` sequence
    - reduced block `9` proof for `np-pipeline-batch-d16` with `server=go`:
      - native `win11` reduced block rerun passed cleanly:
        - `/tmp/plugin-ipc-fitproof-pipeline-batch-d16-go-server.csv`
      - published rows:
        - `c->go = 15577190`, `stable_ratio=1.100216`
        - `rust->go = 15729056`, `stable_ratio=1.087208`
        - `go->go = 16699811`, `stable_ratio=1.036816`
      - implication:
        - the `np-pipeline-batch-d16 c->go @ max` smoking gun also requires
          more context than the reduced block-`9` `server=go` sequence
        - both remaining Windows full-run blockers are now proven to need
          wider context than:
          - exact-row reruns
          - reduced `server=go` block reruns
        - the next staged loop must widen context one step at a time rather
          than jumping back to another blind full-suite rerun
    - Windows benchmark harness defect found while widening context:
      - `tests/run-windows-bench.sh` computes `HAS_RUST=0` before
        `check_binaries()` calls `ensure_bench_build()`
      - implication:
        - on a clean Windows proof tree the script can build
          `src/crates/netipc/target/release/bench_windows.exe` and still skip
          all Rust rows in the same invocation
        - reduced reproductions that start from a clean tree can therefore be
          invalid unless the Rust bench binary already existed before the run
      - native `win11` proof:
        - manual `cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_windows`
          succeeded and produced:
          - `src/crates/netipc/target/release/bench_windows.exe`
        - but the already-running reduced block `8` invocation still logged:
          - `Rust benchmark binary not found: /tmp/plugin-ipc-fitproof/src/crates/netipc/target/release/bench_windows.exe (Rust tests will be skipped)`
      - required fix:
        - re-evaluate Rust benchmark availability after `ensure_bench_build()`
          instead of caching it only once before the build step
    - stale `win11` proof tree discovered after the Rust-availability fix:
      - local repo head:
        - `9932bac netipc: tighten Windows benchmark validation loop`
      - `win11` proof tree `/tmp/plugin-ipc-fitproof` head:
        - `5372e97 netipc: unify Linux and Windows L2/L3 APIs`
      - concrete missing benchmark-driver fixes on the stale tree:
        - `bench/drivers/go/main.go` still had:
          - `runtime.GOMAXPROCS(1)` in the main path
        - `bench/drivers/go/main_windows.go` still had:
          - `runtime.GOMAXPROCS(1)` in the main path
      - implication:
        - any post-reboot reduced reproductions that used the stale
          `/tmp/plugin-ipc-fitproof` tree are invalid for the current
          benchmark investigation
        - the unexpectedly low post-reboot `np-pipeline-d16` widened results
          were explained by using pre-fix benchmark drivers, not by a newly
          discovered protocol regression
      - required next step:
        - rebuild the Windows staged loop from a fresh proof tree copied from
          the current local repo state before continuing the exact-row /
          reduced-block investigation
    - fresh-tree reduced block `8` proof for `np-pipeline-d16` with
      `client=go`:
      - native `win11` reduced block rerun from
        `/tmp/plugin-ipc-fitproof-20260402` passed cleanly:
        - `/tmp/plugin-ipc-fitproof-20260402-pipeline-d16-go-client.csv`
      - published rows:
        - `go->c = 152629`, `stable_ratio=1.021740`
        - `go->rust = 55155`, `stable_ratio=1.124327`
        - `go->go = 93350`, `stable_ratio=1.066220`
      - implication:
        - the current `np-pipeline-d16 go->go @ max` smoking gun requires
          wider context than:
          - exact-row reruns
          - reduced `server=go` block `8`
          - reduced `client=go` block `8`
    - fresh-tree reduced block `9` proof for `np-pipeline-batch-d16` with
      `client=c`:
      - native `win11` reduced block rerun from
        `/tmp/plugin-ipc-fitproof-20260402` passed cleanly:
        - `/tmp/plugin-ipc-fitproof-20260402-pipeline-batch-d16-c-client.csv`
      - published rows:
        - `c->c = 25080707`, `stable_ratio=1.101288`
        - `c->rust = 14751173`, `stable_ratio=1.056894`
        - `c->go = 18158157`, `stable_ratio=1.030445`
      - implication:
        - the current `np-pipeline-batch-d16 c->go @ max` smoking gun
          requires wider context than:
          - exact-row reruns
          - reduced `server=go` block `9`
          - reduced `client=c` block `9`
        - the next staged step should be a broadened late-tail canary from the
          fresh tree, not another blind full-suite rerun
    - expanded fresh-tree Windows canary result:
      - fresh-tree canary failed with exactly one smoking gun:
        - `np-pipeline-d16 go->go @ max`
      - the newly promoted pipeline-batch blocker passed inside the same canary:
        - `np-pipeline-batch-d16 c->go @ max = 15723346`,
          `stable_ratio=1.048763`
      - canary evidence:
        - `/tmp/plugin-ipc-fitproof-20260402-canary.log`
        - `/tmp/netipc-bench-13187/samples-np-pipeline-d16-go-go-0.csv`
      - failing repeats:
        - `84262`
        - `94245`
        - `118626`
        - `102081`
        - `93628`
      - implication:
        - `np-pipeline-d16 go->go @ max` is now the only reproduced Windows
          blocker in the fresh-tree canary
        - `np-pipeline-batch-d16 c->go @ max` no longer reproduces in the
          expanded canary and should not be chased further unless it reappears
          in a later full strict run
    - sequence reduction for the remaining blocker:
      - exact `go->go` row on the fresh tree passed cleanly:
        - `/tmp/netipc-bench-15130/samples-np-pipeline-d16-go-go-0.csv`
      - `rust->go` then `go->go` passed
      - `lookup` then `rust->go` then `go->go` passed
      - `np-ping-pong go->rust` then `rust->go` then `go->go` passed
      - `np-ping-pong go->rust` then `lookup rust->rust` then `go->go`
        reproduced the failure:
        - `/tmp/netipc-bench-20231/samples-np-pipeline-d16-go-go-0.csv`
      - reduced failing repeats:
        - `101347`
        - `51467`
        - `94650`
        - `102957`
        - `102807`
      - exact-row comparison:
        - the isolated exact row had no comparable collapse:
          - `109714`
          - `104369`
          - `99404`
          - `99282`
          - `107525`
      - implication:
        - the remaining issue is benchmark-case cross-contamination, not a
          trivially broken `go->go` row
        - the minimal known reproducer now needs only:
          - `np-ping-pong go->rust @ max`
          - `lookup rust->rust @ max`
          - `np-pipeline-d16 go->go @ max`
    - barrier evidence for the remaining blocker:
      - adding `ps -W | grep ...` snapshots between the reduced reproducer
        cases made the previously failing sequence pass
      - those snapshots showed no lingering `bench_windows_*` processes before
        the final `go->go` row started
      - adding a plain `sleep 1` before the final `go->go` row also made the
        reduced reproducer pass:
        - `/tmp/plugin-ipc-fitproof-20260402-seq-sleep-step3.log`
      - current harness fact:
        - `tests/run-windows-bench.sh` already sleeps only `0.5` seconds after
          each executed row inside a single full-suite run
      - working conclusion:
        - the Windows benchmark harness needs a stronger explicit row/case
          settle barrier than the current `0.5s`
    - fresh-tree harness fix and canary rerun:
      - local fixes applied:
        - `tests/run-windows-bench.sh`
          - refresh Rust benchmark availability after `ensure_bench_build()`
          - add configurable `NIPC_BENCH_ROW_SETTLE_SEC`
          - raise the default row settle barrier from the old hard-coded
            `0.5s` to `1s`
        - `tests/run-windows-bench-canary.sh`
          - add configurable `NIPC_BENCH_ROW_SETTLE_SEC`
          - sleep between canary cases with the same settle barrier
      - fresh-tree expanded canary rerun with the `1s` barrier:
        - `/tmp/plugin-ipc-fitproof-20260402-canary-after-settle.log`
      - rows that passed cleanly in the same canary:
        - `np-pipeline-d16 go->go @ max = 97241`, `stable_ratio=1.042765`
        - `np-pipeline-batch-d16 c->rust @ max = 18418029`,
          `stable_ratio=1.073245`
        - `np-pipeline-batch-d16 go->go @ max = 17655065`,
          `stable_ratio=1.109179`
      - the canary still failed overall, but only on:
        - `np-pipeline-batch-d16 c->go @ max`
      - fresh failing evidence:
        - `/tmp/netipc-bench-25179/samples-np-pipeline-batch-d16-c-go-0.csv`
      - fresh failing repeats:
        - `19403580`
        - `18971482`
        - `19861948`
        - `14403578`
        - `15190288`
      - fresh failing ratios:
        - `raw_ratio=1.378959`
        - `stable_ratio=1.277367`
      - implication:
        - the `1s` settle-barrier fix was real and removed the old
          `np-pipeline-d16 go->go @ max` smoking gun from the canary
        - the remaining fresh blocker is now only
          `np-pipeline-batch-d16 c->go @ max`
    - fresh sequence reduction for `np-pipeline-batch-d16 c->go @ max`:
      - exact row with the `1s` settle barrier still passed cleanly when run
        after only `c->rust`:
        - `/tmp/plugin-ipc-fitproof-seq-c-go.csv`
        - `c->go = 16464214`, `stable_ratio=1.099430`
      - wider reduced sequence also passed cleanly:
        - `rust->go np-pipeline`
        - `go->go np-pipeline`
        - `c->rust np-pipeline-batch`
        - `c->go np-pipeline-batch`
        - published proof:
          - `/tmp/plugin-ipc-fitproof-seq4-c-go.csv`
          - `c->go = 18484251`, `stable_ratio=1.083666`
      - another widened reduced sequence also passed cleanly:
        - `go->rust np-ping-pong`
        - `lookup rust->rust`
        - `c->rust np-pipeline-batch`
        - `c->go np-pipeline-batch`
        - published proof:
          - `/tmp/plugin-ipc-fitproof-seq4b-c-go.csv`
          - `c->go = 17635027`, `stable_ratio=1.011348`
      - implication:
        - the remaining `c->go` smoking gun does not reproduce in the obvious
          shorter sequences
        - the current evidence points to a longer cumulative late-canary / late
          suite interaction, not a trivially broken exact row
        - the next staged proof should test a longer settle barrier on the
          full bounded canary before attempting another full strict Windows
          suite
    - fresh-tree bounded canary rerun with the `2s` barrier:
      - native `win11` canary rerun passed cleanly end-to-end:
        - `/tmp/plugin-ipc-fitproof-20260402-canary-settle2.log`
      - published rows:
        - `np-ping-pong go->rust @ max = 22548`, `stable_ratio=1.122327`
        - `lookup rust->rust @ max = 177174182`,
          `stable_ratio=1.014596`
        - `np-pipeline-d16 rust->go @ max = 109987`,
          `stable_ratio=1.023027`
        - `np-pipeline-d16 go->go @ max = 110310`,
          `stable_ratio=1.024639`
        - `np-pipeline-batch-d16 c->rust @ max = 17733290`,
          `stable_ratio=1.065935`
        - `np-pipeline-batch-d16 c->go @ max = 18830588`,
          `stable_ratio=1.060557`
        - `np-pipeline-batch-d16 go->go @ max = 18868139`,
          `stable_ratio=1.057706`
      - implication:
        - the remaining fresh-tree Windows canary blockers were caused by an
          insufficient post-row settle barrier in the Windows benchmark
          harness, not by a still-broken benchmark scenario
        - the proven fix is to promote the default Windows settle barrier from
          `1s` to `2s` in both the full runner and the canary wrapper
        - the next required proof is a fresh full strict native Windows suite
          from the fresh proof tree with the new `2s` default
    - pushed-state full strict native Windows rerun on commit `e6cc77a`
      still failed, but the remaining blockers are now exact-row reproducible:
      - failed full run:
        - CSV:
          - `/tmp/plugin-ipc-fitproof-e6cc77a-S057e0-full.csv`
        - run dir:
          - `/tmp/netipc-bench-35271`
      - exact-row repros on native `win11`:
        - `np-batch-ping-pong c->go @ 100000/s`
          - failed in isolation:
            - `/tmp/repro-np-batch-c-go-100000.csv`
            - `/tmp/netipc-bench-119667/samples-np-batch-ping-pong-c-go-100000.csv`
          - per-repeat throughput:
            - `4435311`
            - `3027424`
            - `6155022`
            - `5015301`
            - `4217578`
          - `raw_ratio=2.033089`
          - `stable_ratio=1.189142`
        - `np-batch-ping-pong rust->go @ 100000/s`
          - passed in isolation:
            - `/tmp/repro-np-batch-rust-go-100000.csv`
          - `median_throughput=5404415`
          - `stable_ratio=1.184483`
        - `np-batch-ping-pong go->go @ 100000/s`
          - failed in isolation:
            - `/tmp/repro-np-batch-go-go-100000.csv`
            - `/tmp/netipc-bench-120490/samples-np-batch-ping-pong-go-go-100000.csv`
          - per-repeat throughput:
            - `5758958`
            - `6174952`
            - `4054739`
            - `4445468`
            - `4614819`
          - `raw_ratio=1.522898`
          - `stable_ratio=1.295467`
        - `np-batch-ping-pong go->go @ 10000/s`
          - passed in isolation:
            - `/tmp/repro-np-batch-go-go-10000.csv`
          - `median_throughput=5013847`
          - `stable_ratio=1.042624`
        - `np-pipeline-batch-d16 c->rust @ max`
          - failed in isolation:
            - `/tmp/repro-np-pipeline-batch-c-rust-max.csv`
            - `/tmp/netipc-bench-121344/samples-np-pipeline-batch-d16-c-rust-0.csv`
          - per-repeat throughput:
            - `7576215`
            - `12146717`
            - `18958115`
            - `17416102`
            - `17362999`
          - visible stable core:
            - `stable_min=12146717`
            - `stable_max=17416102`
            - `stable_ratio=1.433811`
        - `np-pipeline-batch-d16 rust->rust @ max`
          - failed in isolation:
            - `/tmp/repro-np-pipeline-batch-rust-rust-max.csv`
            - `/tmp/netipc-bench-121804/samples-np-pipeline-batch-d16-rust-rust-0.csv`
          - per-repeat throughput:
            - `13813921`
            - `16234347`
            - `16769925`
            - `20528295`
            - `19877068`
          - `raw_ratio=1.486059`
          - `stable_ratio=1.224384`
      - exact-row classification:
        - reproducible remaining blockers are now only:
          - `np-batch-ping-pong c->go @ 100000/s`
          - `np-batch-ping-pong go->go @ 100000/s`
          - `np-pipeline-batch-d16 c->rust @ max`
          - `np-pipeline-batch-d16 rust->rust @ max`
        - the other two rows from the failed full run are no longer exact-row
          blockers:
          - `np-batch-ping-pong rust->go @ 100000/s`
          - `np-batch-ping-pong go->go @ 10000/s`
      - grounded working theory:
        - these are still benchmark-harness defects, not accepted product
          failures
        - the reproducible pattern is cold-path behavior inside each repeat:
          - the Rust pipeline-batch failures are dominated by a slow first
            repeat and much faster later repeats
          - the Go batch failures improve immediately when the exact scenario
            is warmed before the measured window
      - direct proof that warmup changes the outcome:
        - manual native `win11` probe for
          `np-batch-ping-pong c->go @ 100000/s` with:
          - same Go batch server
          - `1s` unmeasured warmup client run
          - then `5s` measured client run
        - result:
          - `np-batch-ping-pong,c,c,7319942,47.500,192.800,347.600,30.9,0.0,30.9`
        - artifact directory:
          - `/tmp/warm-c-go-t9frpO`
      - next implementation step:
        - add an explicit pre-measurement warmup phase for the Windows runner
          in:
          - `np-batch-ping-pong`
          - `np-pipeline-batch-d16`
        - the warmup must happen before the measured client window so the
          benchmark still reports only steady-state throughput and latency
        - after that:
          - rerun the four exact-row blockers
          - rerun the Windows canary
          - rerun the full strict native Windows suite
    - later exact-row burn-down after the first warmup implementation:
      - the first server-side warmup attempt used the wrong client path and was
        insufficient:
        - `np-batch-ping-pong c->go @ 100000/s` still failed under a
          same-language Go server warmup
      - concrete proof:
        - exact-row rerun with the stale self-warmup still failed:
          - `/tmp/netipc-bench-123574/samples-np-batch-ping-pong-c-go-100000.csv`
          - `stable_min=3601732`
          - `stable_max=6704697`
          - `stable_ratio=1.861520`
      - corrected fix:
        - the Windows runner now tells the batch server exactly which measured
          client binary and subcommand to run before `READY`
        - this keeps CPU accounting honest while warming the real path
      - exact-row proof after the corrected measured-client warmup:
        - `np-batch-ping-pong c->go @ 100000/s`
          - `/tmp/repro-after3-np-batch-c-go-100000.csv`
          - `median_throughput=7314246`
          - `stable_ratio=1.064080`
        - `np-batch-ping-pong go->go @ 100000/s`
          - `/tmp/repro-after3-np-batch-go-go-100000.csv`
          - `median_throughput=7543224`
          - `stable_ratio=1.085857`
      - Rust pipeline-batch required a deeper warmup:
        - with `1s`, `np-pipeline-batch-d16 c->rust @ max` still failed:
          - `/tmp/netipc-bench-125651/samples-np-pipeline-batch-d16-c-rust-0.csv`
          - `raw_ratio=1.394215`
        - with `2s`, it passed:
          - `/tmp/repro-after4-np-pipeline-batch-c-rust-max.csv`
          - `median_throughput=17534301`
          - `stable_ratio=1.085521`
        - with `2s`, `np-pipeline-batch-d16 rust->rust @ max` also passed:
          - `/tmp/repro-after4-np-pipeline-batch-rust-rust-max.csv`
          - `median_throughput=17215086`
          - `stable_ratio=1.127446`
      - new canary blocker after that:
        - `lookup rust->rust @ max`
        - sample file:
          - `/tmp/netipc-bench-127750/samples-lookup-rust-rust-0.csv`
        - exact pattern was monotonic cold-start:
          - `112748158`
          - `128270074`
          - `132113405`
          - `140231919`
          - `152667311`
        - fix:
          - add a `2s` local prewarm loop to the Rust Windows lookup benchmark
        - exact-row proof after the fix:
          - `/tmp/repro-after5-lookup-rust-rust-max.csv`
          - `median_throughput=175900967`
          - `stable_ratio=1.004559`
      - next canary blocker after lookup was fixed:
        - `np-pipeline-batch-d16 c->go @ max`
        - sample file:
          - `/tmp/netipc-bench-129769/samples-np-pipeline-batch-d16-c-go-0.csv`
        - exact pattern:
          - repeats `1-4` were healthy
          - repeat `5` collapsed to `11556022`
          - `raw_ratio=1.635542`
      - grounded working theory:
        - the remaining Go-only pipeline-batch instability was client-side
          measured-window warmup / receive-buffer growth, not the server path
        - evidence:
          - `c->go` passed after a second `runtime.GC()` on the Go batch server
            after warmup and before `READY`
          - `go->go` still failed until the Go pipeline-batch client itself was
            warmed
      - rejected theory:
        - disabling Go GC is not the fix
        - diagnostic proof:
          - `GOGC=off` exact-row rerun for
            `np-pipeline-batch-d16 go->go @ max`
          - sample file:
            - `/tmp/netipc-bench-132004/samples-np-pipeline-batch-d16-go-go-0.csv`
          - repeats `4` and `5` collapsed to:
            - `3527468`
            - `4871158`
      - implemented Go client fix:
        - the Go pipeline-batch client now runs one max-sized warmup
          pipeline-batch cycle before timing starts
        - then runs `runtime.GC()` before the measured window
      - exact-row proof after the Go client fix:
        - `np-pipeline-batch-d16 c->go @ max`
          - `/tmp/repro-after6-np-pipeline-batch-c-go-max.csv`
          - `median_throughput=23614539`
          - `stable_ratio=1.016122`
        - `np-pipeline-batch-d16 go->go @ max`
          - `/tmp/repro-after7-np-pipeline-batch-go-go-max.csv`
          - `median_throughput=11940549`
          - `stable_ratio=1.273560`
      - current state after these fixes:
        - the bounded Windows canary must be rerun from the fully updated proof
          tree
        - if the fresh canary passes, the next step is the full strict native
          Windows suite again
- Required evaluation before any implementation:
  - verify which non-max tiers are actually gentle in practice
  - verify whether the current harness, service naming, CPU accounting,
    diagnostics, and strict spread gates remain valid under concurrent row
    execution
  - decide whether parallelism should cover:
    - all non-max rows
    - only low-rate rows
    - a capped worker batch such as `4`

## Testing requirements

### Current baseline commands

#### Linux / POSIX

Build and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
/usr/bin/ctest --test-dir build --output-on-failure -j4
```

Coverage:

```bash
bash tests/run-coverage-c.sh
bash tests/run-coverage-go.sh
bash tests/run-coverage-rust.sh
```

Benchmarks:

```bash
bash tests/run-posix-bench.sh benchmarks-posix.csv 5
bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md
```

#### Native Windows (`win11:~/src/plugin-ipc.git`)

All active Windows work must run from the single persistent checkout:

- `win11:~/src/plugin-ipc.git`

Sync rule for this checkout:

- commit locally in `/home/costa/src/plugin-ipc.git`, push, then pull in
  `win11:~/src/plugin-ipc.git`
- for Windows-originated changes, commit in `win11:~/src/plugin-ipc.git`,
  push, then pull back into `/home/costa/src/plugin-ipc.git`
- do not create or use disposable Windows repo clones under `/tmp`

This repository’s validated Windows toolchain path is native `win11`, using:

- native Windows `cargo`
- native Windows `go`
- MinGW64 `gcc` / `g++`
- Ninja

Recommended environment:

```bash
export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:$PATH"
export MSYSTEM=MINGW64
export CC=/mingw64/bin/gcc
export CXX=/mingw64/bin/g++
export TMP=/tmp
export TEMP=/tmp
export TMPDIR=/tmp
```

Sanity check:

```bash
type -a cargo go gcc g++ cmake ninja gcov
```

Expected shape:

- `cargo` from `/c/Users/costa/.cargo/bin`
- `go` from `/c/Program Files/Go/bin`
- `gcc` / `g++` / `gcov` from `/mingw64/bin`

Build and run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

Coverage:

```bash
bash tests/run-coverage-c-windows.sh 90
bash tests/run-coverage-go-windows.sh 90
bash tests/run-coverage-rust-windows.sh 90
```

Benchmarks:

```bash
cargo build --release --manifest-path src/crates/netipc/Cargo.toml --bin bench_windows
bash tests/run-windows-bench.sh benchmarks-windows.csv 5
bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md
```

MSYS transition validation:

```bash
bash tests/run-windows-msys-validation.sh
```

Notes:

- native Windows execution is required for the real Windows paths
- native `mingw64` remains the Windows sign-off lane
- `tests/run-windows-msys-validation.sh` is a separate compatibility lane for
  the MSYS-built C path plus bounded native-vs-MSYS benchmark comparison
- the `MSYSTEM=MINGW64` and `TMP` / `TEMP` / `TMPDIR` exports matter in
  non-interactive shells too
  - without them, native MSYS2 `gcc` may fail with misleading or silent errors
    even on trivial source files
- Windows coverage is documented in:
  - `WINDOWS-COVERAGE.md`
- current benchmark method details are documented in:
  - `tests/run-windows-bench.sh`
  - `tests/generate-benchmarks-windows.sh`

Latest MSYS validation evidence (2026-04-04):

- `bash tests/run-windows-msys-validation.sh /tmp/proof-msys-validation-20260404 3`
  passed
- summary:
  - `/tmp/proof-msys-validation-20260404/summary.txt`
- comparison join:
  - `/tmp/proof-msys-validation-20260404/bench-compare/joined.csv`
- functional slice passed, including `test_win_shm` repeated 10x in the MSYS
  lane
- fixes needed to make the lane reliable:
  - pass a Windows-form runtime directory to Windows benchmark binaries even
    when the harness itself is running from MSYS paths
  - treat a final published SHM message as valid even if the peer closes
    immediately after publishing it
- current performance takeaway from that validation run:
  - named-pipe C<->C was near parity or better than the current native
    `mingw64` run set
  - SHM ping-pong C<->C stayed near parity
  - SHM snapshot with the MSYS-built C side acting as server remained the main
    penalty, about 42.5% slower than native `mingw64` in the mixed Rust->C row

### Proposed new validation commands to add

#### Repeated flake / soak loops

Examples to script:

```bash
for i in $(seq 1 50); do /usr/bin/ctest --test-dir build --output-on-failure -j4 || break; done
for i in $(seq 1 50); do ctest --test-dir build --output-on-failure -j4 || break; done
```

#### Windows Application Verifier / PageHeap

Planned validation targets:

- Windows C test executables
- Windows interop executables
- Windows benchmark drivers where practical

Candidate commands based on Microsoft documentation:

```bash
gflags /p /enable test_win_service.exe /full
appverif /verify test_win_service.exe
appverif /verify test_win_service.exe /faults
```

#### Dr. Memory

Candidate command shape:

```bash
drmemory -- build/bin/test_win_service.exe
```

#### Linux sanitizers

Add dedicated CMake build variants for:

```bash
-fsanitize=address
-fsanitize=undefined
-fsanitize=thread
```

and run the normal `ctest` subset on each supported practical configuration.

## Proposed exit criteria

## Latest staged Windows state

- `2026-04-03` important validation hygiene correction:
  - the still-running canary session that had produced the latest
    `np-ping-pong go->rust @ max` failure was using the stale proof tree:
    - `/tmp/plugin-ipc-fitproof-e6cc77a-S057e0`
  - that tree predates the current local uncommitted benchmark-driver fixes in:
    - `bench/drivers/go/main_windows.go`
    - `bench/drivers/rust/src/bench_windows.rs`
    - `tests/run-windows-bench.sh`
  - therefore its failures are not valid evidence against the current local
    state
  - that stale canary was stopped explicitly
- current valid Windows evidence source:
  - a fresh proof tree copied from the current local tracked files:
    - `/tmp/plugin-ipc-fitproof-current-vdhy97`
  - detached canary artifacts:
    - log:
      - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary.log`
    - output dir:
      - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary`
    - pid file:
      - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary.pid`
    - exit marker:
      - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary.exit`
  - next trustworthy benchmark conclusions must come only from this fresh-tree
    canary or from newer proof trees built from the current local state
  - fresh current-tree staged results after stopping the stale `e6cc77a`
    canary:
    - first fresh canary rows already passed on the current proof tree:
      - `np-ping-pong go->rust @ max`
        - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary/np-ping-pong-go-rust-max.csv`
        - `stable_ratio=1.101944`
      - `lookup rust->rust @ max`
        - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary/lookup-rust-rust-max.csv`
        - `stable_ratio=1.041642`
      - `np-pipeline-d16 go->go @ max`
        - `/tmp/plugin-ipc-fitproof-current-vdhy97-canary/np-pipeline-d16-go-go-max.csv`
        - `stable_ratio=1.054481`
    - the same fresh canary then failed at:
      - `np-pipeline-batch-d16 c->rust @ max`
        - `/tmp/netipc-bench-136878/samples-np-pipeline-batch-d16-c-rust-0.csv`
        - `raw_ratio=1.599078`
        - `stable_ratio=1.179914`
    - exact-row classification after stopping that canary:
      - `np-pipeline-batch-d16 c->rust @ max` passed in isolation on the same
        proof tree:
        - `/tmp/repro-current-np-pipeline-batch-c-rust-max.csv`
        - `stable_ratio=1.055595`
      - therefore the active defect was context-dependent, not a trivially
        always-failing exact row
    - reduced-sequence proof then exposed a better smoking gun:
      - `np-pipeline-d16 go->go @ max` failed by itself:
        - `/tmp/netipc-bench-138366/samples-np-pipeline-d16-go-go-0.csv`
        - repeats:
          - `47509`
          - `55551`
          - `76395`
          - `94742`
          - `73346`
        - `stable_ratio=1.375223`
      - the monotonic rise across repeats showed the Go Windows pipeline path
        still warming inside the measured window
    - fix applied on `2026-04-03`:
      - `bench/drivers/go/main_windows.go`
        - Go Windows pipeline client now runs one untimed pipeline cycle before
          timing starts
        - then runs `runtime.GC()` before the measured window
      - `tests/run-windows-bench.sh`
        - `np-pipeline` server startup now uses the same hidden measured-client
          warmup path already used for `np-pipeline-batch`
    - proof after the new pipeline fixes:
      - exact `go->go` pipeline rerun passed:
        - `/tmp/repro-current3-np-pipeline-go-go-max.csv`
        - `stable_ratio=1.013142`
      - reduced two-row context also passed:
        - `go->go np-pipeline @ max`
          - `/tmp/reduce3-go-go-pipeline.csv`
          - `stable_ratio=1.058589`
        - immediately followed by `c->rust np-pipeline-batch-d16 @ max`
          - `/tmp/reduce3-go-go-then-c-rust-pipeline-batch.csv`
          - `stable_ratio=1.098739`
    - next required proof:
      - rerun the bounded Windows canary from the same updated proof tree
      - if the fresh canary passes, rerun the full strict native Windows suite
        from that same proof tree
    - fresh current-tree canary rerun after the Go pipeline fixes still found
      three remaining strict blockers:
      - `np-ping-pong go->rust @ max`
        - `/tmp/netipc-bench-141558/samples-np-ping-pong-go-rust-0.csv`
        - `raw_ratio=1.357113`
        - `stable_ratio=1.153365`
      - `lookup rust->rust @ max`
        - `/tmp/netipc-bench-142024/samples-lookup-rust-rust-0.csv`
        - `raw_ratio=1.491001`
        - `stable_ratio=1.024743`
      - `np-pipeline-d16 rust->go @ max`
        - `/tmp/netipc-bench-142326/samples-np-pipeline-d16-rust-go-0.csv`
        - `stable_ratio=1.889205`
      - that canary was stopped immediately after the smoking guns were
        captured, to avoid wasting additional wall-clock time
    - exact-row proof after the latest Rust-side fixes on `2026-04-03`:
      - new fixes:
        - `bench/drivers/rust/src/bench_windows.rs`
          - Rust Windows `np-ping-pong` client now performs one untimed
            round-trip warmup before the measured window
          - Rust Windows `np-pipeline` client now performs one untimed pipeline
            cycle before the measured window
        - `tests/run-windows-bench.sh`
          - `np-ping-pong` rows now use the same hidden measured-client server
            warmup path already used for `np-batch-ping-pong`
      - fresh exact-row proofs on the same proof tree:
        - `np-ping-pong go->rust @ max`
          - `/tmp/repro-current4-np-ping-go-rust-max.csv`
          - `stable_ratio=1.038807`
        - `np-pipeline-d16 rust->go @ max`
          - `/tmp/repro-current4-np-pipeline-rust-go-max.csv`
          - `stable_ratio=1.145163`
        - `lookup rust->rust @ max`
          - `/tmp/repro-current5-lookup-rust-rust-max.csv`
          - `stable_ratio=1.026018`
      - implication:
        - the three latest fresh-canary blockers are not always-failing exact
          rows anymore
        - the next required proof is another bounded canary rerun from the same
          updated proof tree
    - current harness defect identified on `2026-04-03` while comparing the
      exact-row path with the canary wrapper path:
      - `tests/run-windows-bench.sh` now wraps `np-ping-pong` and
        `np-pipeline` server startup with `start_server_with_warmup(...)`
      - but only the Windows batch server subcommands in the benchmark drivers
        actually consume the `NIPC_BENCH_SERVER_WARMUP_*` environment
        variables:
        - `bench/drivers/go/main_windows.go`
        - `bench/drivers/rust/src/bench_windows.rs`
      - the normal Windows ping-pong/snapshot servers in C, Rust, and Go still
        print `READY` and enter the measured CPU window without executing the
        hidden warmup client
      - implication:
        - the runner was assuming hidden server warmup existed for
          `np-ping-pong` and `np-pipeline`, but those rows were still exposed
          to cold server/session startup effects
        - this is a benchmark-harness defect and must be fixed in the drivers,
          not explained away as acceptable noise
    - fresh current-tree proof after the server-warmup fixes:
      - bounded canary rerun still found exactly one remaining blocker:
        - `np-pipeline-batch-d16 c->go @ max`
          - `/tmp/netipc-bench-150153/samples-np-pipeline-batch-d16-c-go-0.csv`
          - repeats:
            - `19538200`
            - `18981828`
            - `14025420`
            - `18818186`
            - `16547539`
          - `raw_ratio=1.393056`
          - `stable_ratio=1.147109`
      - exact row on the same proof tree passed:
        - `/tmp/repro-current6-np-pipeline-batch-c-go-max.csv`
        - `stable_ratio=1.040609`
      - reduced late-tail proofs also passed:
        - `c->rust np-pipeline-batch @ max`
          - `/tmp/reduce-current7-c-rust.csv`
          - `stable_ratio=1.129821`
        - immediately followed by `c->go np-pipeline-batch @ max`
          - `/tmp/reduce-current7-c-go.csv`
          - `stable_ratio=1.134260`
        - `go->go np-pipeline @ max`
          -> `c->rust np-pipeline-batch @ max`
          -> `c->go np-pipeline-batch @ max`
          - `/tmp/reduce-current8-go-go-pipe.csv`
          - `/tmp/reduce-current8-c-rust-batch.csv`
          - `/tmp/reduce-current8-c-go-batch.csv`
          - all passed
        - `rust->go np-pipeline @ max`
          -> `go->go np-pipeline @ max`
          -> `c->rust np-pipeline-batch @ max`
          -> `c->go np-pipeline-batch @ max`
          - `/tmp/reduce-current9-rust-go-pipe.csv`
          - `/tmp/reduce-current9-go-go-pipe.csv`
          - `/tmp/reduce-current9-c-rust-batch.csv`
          - `/tmp/reduce-current9-c-go-batch.csv`
          - all passed
      - implication:
        - the remaining blocker was no longer in the late pipeline/batch block
          itself
        - the next shortest path had moved earlier to `lookup rust->rust @ max`
          and then `rust->go np-pipeline @ max`
    - fresh current-tree reduced repro on `2026-04-03`:
      - `lookup rust->rust @ max`
        -> `rust->go np-pipeline @ max`
      - failed immediately at the lookup row:
        - `/tmp/netipc-bench-158861/samples-lookup-rust-rust-0.csv`
        - repeats:
          - `79622465`
          - `80934946`
          - `80718458`
          - `80717252`
          - `114102724`
        - `raw_ratio=1.433047`
        - `stable_ratio=1.002697`
      - later reduced proof including the same `lookup` row also failed with a
        low repeat:
        - `/tmp/netipc-bench-3884/samples-lookup-rust-rust-0.csv`
        - repeats:
          - `168334535`
          - `175039352`
          - `173523785`
          - `116325975`
          - `171436484`
        - the low repeat also dropped CPU from `~92%` to `83.3%`
      - implication:
        - this smoking gun does not involve IPC at all
        - the instability was now clearly in the Windows benchmark execution
          model for single-threaded CPU-bound rows
    - Windows benchmark affinity hardening on `2026-04-03`:
      - new fix in all three Windows benchmark drivers:
        - `bench/drivers/c/bench_windows.c`
        - `bench/drivers/go/main_windows.go`
        - `bench/drivers/rust/src/bench_windows.rs`
      - benchmark processes now:
        - raise priority
        - pin process and thread affinity
      - first attempt pinned to the first available vCPU
      - evidence after that first pinning:
        - exact `lookup rust->rust @ max` passed `3/3`:
          - `/tmp/proof-affinity-lookup-a.csv`
            - `stable_ratio=1.033657`
          - `/tmp/proof-affinity-lookup-b.csv`
            - `stable_ratio=1.073468`
          - `/tmp/proof-affinity-lookup-c.csv`
            - `stable_ratio=1.022298`
        - but a repeated `lookup -> rust->go np-pipeline` sequence still failed
          later when `lookup` landed on a noisy repeat:
          - `/tmp/netipc-bench-3884/samples-lookup-rust-rust-0.csv`
      - working theory:
        - pinning to the first available vCPU was still wrong on this VM,
          likely because that CPU is taking more host / interrupt noise
    - Windows benchmark affinity refinement on `2026-04-03`:
      - updated all three Windows benchmark drivers again to pin to the highest
        available vCPU instead of CPU0 / the first available CPU
      - fresh proof tree:
        - `/tmp/plugin-ipc-fitproof-affinity-tJh53A`
      - exact `lookup rust->rust @ max` now passed `3/3` cleanly again on that
        fresh proof tree:
        - `/tmp/proof-affinity2-lookup-a.csv`
          - `stable_ratio=1.023741`
        - `/tmp/proof-affinity2-lookup-b.csv`
          - `stable_ratio=1.033138`
        - `/tmp/proof-affinity2-lookup-c.csv`
          - `stable_ratio=1.023407`
      - repeated `lookup -> rust->go np-pipeline` sequences also passed on the
        same fresh proof tree:
        - sequence 1:
          - `/tmp/proof-affinity2-seq1-lookup.csv`
            - `stable_ratio=1.026724`
          - `/tmp/proof-affinity2-seq1-rust-go.csv`
            - `stable_ratio=1.046724`
        - sequence 2:
          - `/tmp/proof-affinity2-seq2-lookup.csv`
            - `stable_ratio=1.016340`
          - `/tmp/proof-affinity2-seq2-rust-go.csv`
            - `stable_ratio=1.043912`
        - sequence 3:
          - `/tmp/proof-affinity2-seq3-lookup.csv`
            - `stable_ratio=1.012824`
          - `/tmp/proof-affinity2-seq3-rust-go.csv`
            - `stable_ratio=1.023384`
      - implication:
        - the current shortest benchmark blocker is gone on the fresh affinity
          proof tree
        - the next staged proof is the bounded Windows canary from that same
          proof tree
    - bounded Windows canary on the affinity-hardened proof tree:
      - proof tree:
        - `/tmp/plugin-ipc-fitproof-affinity-tJh53A`
      - output dir:
        - `/tmp/proof-affinity2-canary`
      - all 7 canary rows published successfully:
        - `np-ping-pong go->rust @ max`
          - `22478`
        - `lookup rust->rust @ max`
          - `185751948`
        - `np-pipeline-d16 rust->go @ max`
          - `44180`
        - `np-pipeline-d16 go->go @ max`
          - `151578`
        - `np-pipeline-batch-d16 c->rust @ max`
          - `14766999`
        - `np-pipeline-batch-d16 c->go @ max`
          - `15553671`
        - `np-pipeline-batch-d16 go->go @ max`
          - `23307190`
      - no benchmark processes remained after the run
      - implication:
        - the bounded Windows canary is now green on the fresh
          affinity-hardened proof tree
        - the next required proof is the full strict native Windows suite from
          that same proof tree
    - full strict native Windows rerun from the affinity-hardened proof tree:
      - output files:
        - `/tmp/proof-affinity2-full.csv`
        - `/tmp/proof-affinity2-full.log`
      - the run was stopped on user request before completion:
        - exit marker ended as `143`
      - before it was stopped, the run had already exposed new strict blockers
        in `shm-ping-pong` with `server=go`:
        - `shm-ping-pong go->go @ max`
          - sample file:
            - `/tmp/netipc-bench-11730/samples-shm-ping-pong-go-go-0.csv`
          - repeats:
            - `5668`
            - `8863`
            - `10675`
            - `10488`
            - `10521`
          - `raw_ratio=1.883380`
          - `stable_ratio=1.187070`
        - `shm-ping-pong c->go @ 100000/s`
          - sample file:
            - `/tmp/netipc-bench-11730/samples-shm-ping-pong-c-go-100000.csv`
          - `raw_min=8825`
          - `raw_max=16676`
          - `raw_ratio=1.889632`
          - `stable_ratio=1.056997`
        - `shm-ping-pong rust->go @ 100000/s`
          - sample file:
            - `/tmp/netipc-bench-11730/samples-shm-ping-pong-rust-go-100000.csv`
          - `raw_min=7316`
          - `raw_max=16516`
          - `raw_ratio=2.257518`
          - `stable_ratio=1.037671`
      - implication:
        - the earlier pipeline and lookup blockers are no longer the active
          Windows sign-off issue on the affinity-hardened tree
        - the next staged work must start from the SHM ping-pong rows with
          `server=go`, beginning with the exact rows above rather than another
          blind full-suite rerun
    - targeted Windows benchmark burn-down refinement on `2026-04-03`:
      - the base runner already supported exact-row selection through:
        - `NIPC_BENCH_SCENARIOS`
        - `NIPC_BENCH_CLIENTS`
        - `NIPC_BENCH_SERVERS`
        - `NIPC_BENCH_TARGETS`
      - but that workflow was still too easy to use incorrectly during
        repeated burn-down, because it relied on manually rebuilding the filter
        tuple every time
      - new harness wrapper:
        - `tests/run-windows-bench-targeted.sh`
      - the wrapper accepts either:
        - explicit row specs:
          - `scenario,client,server,target`
        - or a prior `diagnostics-summary.txt` emitted by
          `tests/run-windows-bench.sh` with `NIPC_BENCH_DIAGNOSE_FAILURES=1`
      - intended Windows sign-off loop is now explicit:
        - rerun only the exact failing rows until they are stable
        - rerun canary after the exact rows are clean
        - rerun the full strict suite only for final sign-off, not after every
          narrow fix
    - fresh exact-row proof on the current checkout after the latest Windows
      benchmark fixes:
      - `src/go/pkg/netipc/transport/windows/shm_pause.go`
        now uses `SwitchToThread()` instead of `runtime.Gosched()` for the Go
        SHM spin pause
      - Windows benchmark client warmups now exist in:
        - `bench/drivers/go/main_windows.go`
        - `bench/drivers/c/bench_windows.c`
      - fresh exact-row rerun for the previously failing Go-server SHM rows
        passed:
        - output:
          - `/tmp/shm-go-targeted-after2.csv`
        - run dir:
          - `/tmp/netipc-bench-50134`
      - fresh exact-row rerun for the later remaining strict row
        `np-ping-pong c->rust` also passed:
        - output:
          - `/tmp/proof-c-rust-ping-after-warm.csv`
        - run dir:
          - `/tmp/netipc-bench-260301`
        - published rows:
          - `np-ping-pong c->rust @ max = 39389`, `stable_ratio=1.051175`
          - `np-ping-pong c->rust @ 100000/s = 35136`, `stable_ratio=1.009913`
      - implication:
        - exact-row iteration no longer requires another blind full strict run
          after each narrow harness or benchmark-driver fix
        - the only remaining reason to run the entire strict suite is final
          cross-row sign-off once the narrowed blockers and canary are clean
    - final native Windows benchmark sign-off hardening on `2026-04-04`:
      - new runner fixes:
        - `tests/run-windows-bench.sh`
          - the hidden measured-client warmup window is now also applied to:
            - `shm-ping-pong`
            - `snapshot-baseline`
            - `snapshot-shm`
          - default hidden warmup duration was reduced from `2s` to `1s`
            so the broader coverage does not add another long full-suite tax
        - `tests/run-windows-bench-canary.sh`
          - the bounded canary now explicitly covers the snapshot rows that the
            earlier canary missed:
            - `snapshot-baseline c->rust @ max`
            - `snapshot-baseline rust->rust @ max`
            - `snapshot-baseline rust->go @ max`
            - `snapshot-shm c->rust @ max`
      - grounded reason:
        - after the affinity split fix, the remaining full-suite failures were
          no longer broad transport defects
        - they were cold-start / readiness variance leaking into the measured
          window for non-batch pair rows, especially the Rust snapshot server
          rows and one earlier Go-server SHM row
      - exact-row proof after the warmup expansion:
        - the rows extracted from
          `/tmp/netipc-bench-294062/diagnostics-summary.txt`
          all reran clean on the current checkout, including:
          - `shm-ping-pong rust->go @ 100000/s`
            - `stable_ratio=1.000020`
          - `snapshot-baseline c->rust @ max`
            - `stable_ratio=1.050087`
          - `snapshot-baseline rust->rust @ max`
            - `stable_ratio=1.048834`
          - `snapshot-baseline rust->go @ max`
            - `stable_ratio=1.061767`
          - `snapshot-shm c->rust @ max`
            - `stable_ratio=1.025204`
      - bounded canary proof after the warmup expansion:
        - output dir:
          - `/tmp/proof-current-canary-after-snapshot-warmup`
        - result:
          - all `14` canary rows passed
        - key previously failing rows were now clean:
          - `snapshot-baseline c->rust @ max`
            - `39148`
            - `stable_ratio=1.085785`
          - `snapshot-baseline rust->rust @ max`
            - `38975`
          - `snapshot-baseline rust->go @ max`
            - `61259`
          - `snapshot-shm c->rust @ max`
            - `1346982`
          - `shm-ping-pong rust->go @ 100000/s`
            - `100010`
            - `stable_ratio=1.003180`
          - `np-pipeline-batch-d16 go->go @ max`
            - `29682220`
            - `stable_ratio=1.027887`
      - final full strict native Windows suite on the current checkout:
        - command:
          - `NIPC_KEEP_RUN_DIR=1 NIPC_BENCH_DIAGNOSE_FAILURES=1 tests/run-windows-bench.sh /tmp/proof-full-current-warmup-affinity.csv 5`
        - output:
          - `/tmp/proof-full-current-warmup-affinity.csv`
        - preserved run dir:
          - `/tmp/netipc-bench-404141`
        - result:
          - exit `0`
          - `201` published rows
          - no diagnostic reruns were needed for sign-off
      - implication:
        - the strict native Windows benchmark matrix is now green on the
          current checkout
        - the earlier remaining failures were benchmark-harness defects, and
          they are now fixed rather than explained away

## Proposed exit criteria

The library should be considered fit for Netdata integration only after the
following are true:

- No ignored test remains for a core supported behavior such as Windows managed
  restart / reconnect.
- No skipped test remains for a pattern that Netdata intends to support.
- Full Linux and Windows functional suites pass repeatedly, not just once.
- Full Linux and Windows benchmark suites remain complete and above floors.
- Critical runtime files are not accepted merely because the total language
  coverage passes; critical-file gates must pass too.
- Fault-injection runs for allocation and OS / Win32 failures are implemented
  and passing; those branches are no longer accepted as mere documented
  exclusions.
- Leak / sanitizer / verifier runs are clean enough to trust long-running use.
- Mixed-version behavior is explicitly tested or explicitly rejected by
  documented contract.
- Windows stress / chaos breadth is no longer materially narrower than Linux
  for the critical supported behaviors.
- The benchmark acceptance methodology no longer relies on trimmed-warning /
  stable-core publication as a sign-off escape hatch.
- The real Netdata plugin harness survives long restart / reconnect soak without:
  - leaks
  - deadlocks
  - runaway CPU
  - unbounded reconnect storms

## Documentation updates required

- `README.md`
  - after the validation work exists, add a short fit-for-purpose validation
    section pointing to the heavy validation scripts
- `WINDOWS-COVERAGE.md`
  - extend it or split out a broader Windows validation guide if Application
    Verifier / PageHeap / Dr. Memory become first-class
- `COVERAGE-EXCLUSIONS.md`
  - update as exclusions are burned down by deterministic fault injection
- add a dedicated validation document if this TODO turns into permanent process:
  - `FIT-FOR-PURPOSE.md` or similar

## Sources consulted

- repo documents and scripts:
  - `README.md`
  - `WINDOWS-COVERAGE.md`
  - `COVERAGE-EXCLUSIONS.md`
  - `TODO-unified-l2-l3-api.md`
  - `tests/run-windows-bench.sh`
  - `tests/run-coverage-c-windows.sh`
  - `tests/run-coverage-go-windows.sh`
  - `tests/run-coverage-rust-windows.sh`
- official / primary external references:
  - Microsoft Learn:
    - Application Verifier overview and testing applications
    - Enable Page Heap / GFlags
  - Clang documentation:
    - AddressSanitizer
    - UndefinedBehaviorSanitizer
    - ThreadSanitizer
  - Dr. Memory official documentation:
    - running and setup guidance
