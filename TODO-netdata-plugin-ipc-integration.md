## Purpose

Fit-for-purpose goal: integrate `plugin-ipc` into `~/src/netdata/netdata/` so Netdata can immediately replace the current Linux `cgroups.plugin` -> `ebpf.plugin` custom metadata transport with typed IPC that is reliable, maintainable, testable, and ready for guarded production rollout.

## TL;DR

- Analyze how `plugin-ipc` should be integrated into the Netdata repo and build.
- Before any Netdata integration, implement transparent SHM resizing in `plugin-ipc` itself.
- Validate that feature thoroughly first, including full C/Rust/Go interop matrices on Unix and Windows.
- Use it first to replace the current `cgroups.plugin` -> `ebpf.plugin` metadata channel on Linux.
- Make the library available to C, Rust, and Go code inside Netdata.
- Record integration design decisions before implementation.
- Current execution scope:
  - remove the multi-method service drift from docs, code, tests, and public APIs
  - align the implementation to one-service-kind-per-endpoint
  - implement the accepted SHM resize / renegotiation behavior
  - eliminate contradictory wording and examples across the repository
  - refresh the Linux and Windows benchmark matrices on the current tree
  - update benchmark artifacts and all benchmark-derived docs so everything is in sync
  - investigate the remaining benchmark spreads and identify whether they reflect real transport/runtime inefficiency, measurement distortion, or pair-specific implementation overhead
  - correct the benchmark build path so C benchmark results are generated from optimized C libraries, not from a local Debug CMake tree
  - Current implementation status:
  - docs/specs/TODOs now explicitly state service-oriented discovery and one request kind per endpoint
  - Go public cgroups APIs and Go raw service/tests were rewritten to the single-kind model
  - `cd src/go && go test -count=1 ./pkg/netipc/service/raw` now passes after aligning the raw client/server with learned SHM req/resp capacities and transparent overflow-driven reconnect/retry
  - `cd src/go && go test -count=1 ./pkg/netipc/service/cgroups` now passes
  - Rust public cgroups facade now uses the single-kind raw server constructor instead of the old multi-handler bundle
  - targeted Rust verification now passes:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib service::cgroups:: -- --test-threads=1`
  - Rust raw Unix tests no longer use the old mixed `pingpong_handlers()` helper
  - the Rust raw service subset now passes after binding increment-only and string-reverse-only endpoints explicitly and teaching the raw client/server the learned SHM req/resp resize path:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib service::raw::tests:: -- --test-threads=1`
  - Go raw L2 now tracks learned request/response capacities, treats `STATUS_LIMIT_EXCEEDED` as an overflow signal, reconnects, renegotiates larger capacities, and retries transparently for overflow-safe calls
  - Rust raw L2 now tracks learned request/response capacities, treats `STATUS_LIMIT_EXCEEDED` as an overflow signal, reconnects, renegotiates larger capacities, and retries transparently for overflow-safe calls
  - Go and Rust transport listeners now expose payload-limit setters so the server can advertise learned capacities to later clients before `accept()`:
    - Go POSIX: `src/go/pkg/netipc/transport/posix/uds.go`
    - Go Windows: `src/go/pkg/netipc/transport/windows/pipe.go`
    - Rust POSIX: `src/crates/netipc/src/transport/posix.rs`
    - Rust Windows: `src/crates/netipc/src/transport/windows.rs`
  - `src/crates/netipc/src/service/raw.rs` no longer exposes the generic `Handlers` bundle or the transitional `new_single_kind` / `with_workers_single_kind` constructors
  - `src/crates/netipc/src/service/raw.rs` now models managed servers as single-kind endpoints directly:
    - `ManagedServer::new(..., expected_method_code, handler)`
    - `ManagedServer::with_workers(..., expected_method_code, handler, worker_count)`
  - Rust POSIX and Windows benchmark drivers now use the single-kind raw service surface instead of the deleted multi-handler `Handlers` bundle:
    - `bench/drivers/rust/src/main.rs`
    - `bench/drivers/rust/src/bench_windows.rs`
  - `src/crates/netipc/src/service/raw_unix_tests.rs` and `src/crates/netipc/src/service/raw_windows_tests.rs` now use that single-kind raw service surface directly instead of feeding a generic handler bundle into the raw server
  - verified source-level residue scan for `src/crates/netipc/src/service/raw_windows_tests.rs` is now clean:
    - no remaining `Handlers`
    - no remaining `test_cgroups_handlers()`
    - no remaining `increment_handlers()`
  - verified source-level residue scan for `src/crates/netipc/src/service/raw.rs` and `src/crates/netipc/src/service/raw_unix_tests.rs` is now clean:
    - no remaining `Handlers`
    - no remaining `new_single_kind`
    - no remaining `with_workers_single_kind`
  - C public naming drift was reduced from plural handler bundles to singular service-handler naming
  - `tests/fixtures/c/test_win_service.c` is now snapshot-only; it no longer starts a typed snapshot service and then exercises increment / string-reverse / batch calls against it
  - source-level cleanup of the remaining Windows C fixtures is only partial so far:
    - the obvious typed snapshot `.on_increment` / `.on_string_reverse` bundle drift was removed from:
      - `tests/fixtures/c/test_win_service_extra.c`
      - `tests/fixtures/c/test_win_stress.c`
      - `tests/fixtures/c/test_win_service_guards.c`
      - `tests/fixtures/c/test_win_service_guards_extra.c`
    - but real `win11` compilation later proved these files still contain stale calls to removed C APIs and stale raw-server assumptions
  - verified source-level residue scan across the touched Windows C fixtures is therefore not enough on its own:
    - it proves only that the obvious typed-handler bundle names were removed
    - it does not prove runtime or even compile-time correctness on Windows
  - verified source-level residue scan for the touched Windows Go raw helpers/tests is now clean:
    - no remaining `Handlers{...}` bundle initializers
    - no remaining `winTestHandlers()` / `winFailingHandlers()` helpers
    - no remaining `server.handlers` references in the Windows raw tests
  - Windows Go package cross-compile proof now passes from this Linux host:
    - `cd src/go && GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/raw`
    - `cd src/go && GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/cgroups`
  - the Unix interop/service/cache matrix now passes end-to-end after the resize rewrite:
    - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_uds_interop|test_shm_interop|test_service_interop|test_service_shm_interop|test_cache_interop|test_cache_shm_interop)$'`
  - the broader Unix shm/service/cache slice across C, Rust, and Go now also passes:
    - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_shm|test_service|test_cache|test_shm_rust|test_service_rust|test_shm_go|test_service_go|test_cache_go)$'`
  - the previously exposed POSIX UDS mismatch is now resolved:
    - Rust `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1` now passes `299/299`
    - the stale transport tests were rewritten to match the accepted directional negotiation semantics:
      - requests are sender-driven
      - responses are server-driven
    - C `test_uds` now proves directional negotiation explicitly and keeps direct receive-limit coverage through a raw malformed-response path
    - the broader non-fuzz Unix CTest sweep now passes end-to-end:
      - `/usr/bin/ctest --test-dir build --output-on-failure -E '^(fuzz_protocol_30s|go_FuzzDecodeHeader|go_FuzzDecodeChunkHeader|go_FuzzDecodeHello|go_FuzzDecodeHelloAck|go_FuzzDecodeCgroupsRequest|go_FuzzDecodeCgroupsResponse|go_FuzzBatchDirDecode|go_FuzzBatchItemGet)$'`
      - result: `28/28` passed
  - the public docs now match the accepted directional handshake semantics:
    - `docs/level1-wire-envelope.md` explicitly says request limits are sender-driven and response limits are server-driven
    - `docs/getting-started.md` no longer documents the deleted Rust `CgroupsHandlers` / `CgroupsServer` surface
  - Windows transport test sources were aligned to the same directional contract:
    - Go `src/go/pkg/netipc/transport/windows/pipe_integration_test.go` no longer expects the old min-style negotiation
    - Rust `src/crates/netipc/src/transport/windows.rs` now contains a matching directional negotiation test
    - Go Windows transport tests still have cross-compile proof from this Linux host:
      - `cd src/go && GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/transport/windows`
  - local source checks are clean for the touched Windows C files:
    - `git diff --check -- tests/fixtures/c/test_win_stress.c tests/fixtures/c/test_win_service_guards.c tests/fixtures/c/test_win_service_guards_extra.c TODO-netdata-plugin-ipc-integration.md`
  - local source checks are also clean for the touched Go/Rust raw files:
    - `git diff --check -- src/crates/netipc/src/service/raw.rs src/crates/netipc/src/service/raw_unix_tests.rs src/go/pkg/netipc/service/raw/client.go src/go/pkg/netipc/service/raw/client_windows.go src/go/pkg/netipc/service/raw/shm_unix_test.go src/go/pkg/netipc/service/raw/helpers_windows_test.go src/go/pkg/netipc/service/raw/more_windows_test.go src/go/pkg/netipc/service/raw/shm_windows_test.go TODO-netdata-plugin-ipc-integration.md`
  - limitation:
    - this Linux host does not have `x86_64-w64-mingw32-gcc`
    - so local source cleanup alone is not enough for the edited Windows C fixtures
    - the same host limitation means the `raw_windows_tests.rs` source cleanup is not backed by a real Windows Rust compile/run proof from this environment either
    - the touched Windows Go packages now have cross-compile proof, but still do not have a real Windows runtime proof from this environment
  - current verified Windows runtime status from the real `win11` workflow:
    - the documented `ssh win11` + `MSYSTEM=MINGW64` toolchain path works and has been used for real validation
    - after syncing the local tree, `cmake --build build -j4` on `win11` exposed real stale C fixture/API mismatches that were not visible from Linux source scans alone
    - the first verified `win11` failure classes were:
      - stale removed client helpers:
        - `nipc_client_call_increment`
        - `nipc_client_call_increment_batch`
        - `nipc_client_call_string_reverse`
      - stale internal error enum usage:
        - `NIPC_ERR_INTERNAL_ERROR`
      - stale raw-server handler signature assumptions:
        - old `bool` raw handlers instead of `nipc_error_t (*)(..., const nipc_header_t *, ...)`
      - stale `nipc_server_init(...)` argument ordering under the internal test macro path
      - stale client struct field assumptions such as `client.request_buf_size`
    - those compile-time failures have now been corrected locally and revalidated on `win11`:
      - `test_win_service_extra.exe` now builds and passes on `win11`
    - the remaining active Windows C problem is now narrower and runtime-only:
    - after correcting the stale Windows C fixture/API mismatches and the baseline request-overflow signaling gap, `test_win_service_guards.exe` now passes on `win11`:
        - `=== Results: 141 passed, 0 failed ===`
      - the previous apparent timeout was not a persistent runtime hang:
        - later reruns completed normally once the stale one-item batch test drift was removed
      - the last real guard-binary contradiction was:
        - a one-item increment "batch" test still expecting reconnect/growth
      - that expectation was wrong under the accepted semantics:
        - one-item increment batches are normalized to the plain increment path
        - the guard was rewritten to use a real 2-item batch for baseline request-resize coverage
    - the rest of the edited Windows C runtime slice has now been validated on `win11` too:
      - `test_win_service.exe`:
        - `=== Results: 80 passed, 0 failed ===`
      - `test_win_service_extra.exe`:
        - `=== Results: 82 passed, 0 failed ===`
      - `test_win_service_guards_extra.exe`:
        - `=== Results: 93 passed, 0 failed ===`
      - `test_win_stress.exe`:
        - `=== Results: 1 passed, 0 failed ===`
      - a combined rerun of all edited Windows C binaries also passed cleanly on `win11`
    - the earlier `test_win_service.exe` timeout is not currently reproducible as a deterministic bug:
      - it timed out once in a combined slice and once in an early soak run
      - after the stale guard/test contradictions were removed, a focused rerun passed
      - a subsequent combined rerun passed
      - a targeted 3-run `win11` soak of `test_win_service.exe` also passed `3/3`
      - working theory:
        - that earlier timeout was a transient host/process stall, not a currently reproducible library correctness bug
    - a real L2 behavior gap was exposed and fixed during this `win11` investigation:
      - on baseline request overflow, the server session loop now emits a zero-payload `LIMIT_EXCEEDED` response before disconnecting, instead of silently breaking the session
      - this fix was needed for transparent request-side resize/reconnect to work on Windows baseline transport at all
    - current remaining Windows Rust runtime blocker:
      - focused `win11` run:
        - `timeout 120 cargo test --manifest-path src/crates/netipc/Cargo.toml test_cache_round_trip_windows -- --nocapture --test-threads=1`
      - current observed behavior:
        - build completes
        - test process prints:
          - `running 1 test`
          - `test service::cgroups::windows_tests::test_cache_round_trip_windows ...`
        - then stalls without completing
      - strongest current evidence:
        - Rust raw Windows tests already implement reliable Windows shutdown by:
          - storing the service name + wake client config
          - setting `running_flag = false`
          - issuing a dummy `NpSession::connect(...)` to wake the blocking `ConnectNamedPipe()`
        - cgroups Windows tests and Rust Windows interop binaries still use the weaker pattern:
          - only `running_flag = false`
          - no wake connection
        - the Windows accept loop in `src/crates/netipc/src/service/raw.rs` blocks in `listener.accept()`, which ultimately blocks in `ConnectNamedPipe()`, so `running_flag = false` alone is not sufficient to stop the server reliably on Windows
      - working theory:
        - the cache test body may already be completing
        - the stall is very likely in Windows server shutdown/join, not in snapshot/cache decoding itself
    - that Rust Windows blocker is now verified fixed on `win11`:
      - fix:
        - cgroups Windows tests and Rust Windows interop binaries now use the same reliable Windows stop pattern already used by the Rust raw Windows tests:
          - set `running_flag = false`
          - then issue a wake connection so the blocking `ConnectNamedPipe()` returns and the accept loop can observe shutdown
      - focused proof:
        - `timeout 120 cargo test --manifest-path src/crates/netipc/Cargo.toml test_cache_round_trip_windows -- --nocapture --test-threads=1`
        - result:
          - `test service::cgroups::windows_tests::test_cache_round_trip_windows ... ok`
      - full Rust Windows lib proof:
        - `timeout 900 cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1`
        - result:
          - `176 passed`
          - `0 failed`
          - `1 ignored`
      - factual conclusion:
        - the live bug was stale Windows shutdown/test-fixture behavior, not a current Rust cache decode/refresh correctness issue
    - broader real Windows interop/service/cache proof is now also green on `win11`:
      - command:
        - `timeout 1800 ctest --test-dir build --output-on-failure -R "^(test_named_pipe_interop|test_win_shm_interop|test_service_win_interop|test_service_win_shm_interop|test_cache_win_interop|test_cache_win_shm_interop)$"`
      - result:
        - `test_named_pipe_interop`: passed
        - `test_win_shm_interop`: passed
        - `test_service_win_interop`: passed
        - `test_service_win_shm_interop`: passed
        - `test_cache_win_interop`: passed
        - `test_cache_win_shm_interop`: passed
        - summary:
          - `100% tests passed, 0 tests failed out of 6`
  - targeted C rebuild and runtime verification now passes:
    - `cmake --build build --target test_service test_hardening test_ping_pong`
    - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_service|test_hardening|test_ping_pong)$'`
  - the latest naming / contract cleanup slice is now backed by both local Linux and real `win11` proof:
    - local Linux rerun:
      - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_hardening|test_ping_pong)$'`
      - result:
        - `100% tests passed, 0 failed`
    - after syncing this slice's edited files to `win11`, targeted rebuild passed:
      - `cmake --build build -j4 --target test_win_service test_win_service_extra test_win_service_guards test_win_service_guards_extra`
    - direct `win11` runtime proof for the edited guard binaries passed:
      - `./test_win_service_guards.exe`
        - result:
          - `=== Results: 141 passed, 0 failed ===`
      - `./test_win_service_guards_extra.exe`
        - result:
          - `=== Results: 93 passed, 0 failed ===`
    - direct `win11` runtime proof for the edited service binaries also passed via CTest:
      - `ctest --test-dir build --output-on-failure -R "^(test_win_service|test_win_service_extra)$"`
      - result:
        - `test_win_service`: passed
        - `test_win_service_extra`: passed
  - benchmark refresh on the current tree is now complete and synced:
    - factual root cause of the benchmark blocker:
      - the C and Rust batch benchmark clients still generated random batch sizes in the range `1..1000`
      - the actual batch protocol normalizes `item_count == 1` to the non-batch path
      - Go was already correct and generated `2..1000`, which is why the same C batch server still interoperated with the Go client
    - fixed in:
      - `bench/drivers/c/bench_posix.c`
      - `bench/drivers/c/bench_windows.c`
      - `bench/drivers/rust/src/main.rs`
      - `bench/drivers/rust/src/bench_windows.rs`
      - `bench/drivers/go/main.go`
      - `tests/run-posix-bench.sh`
      - `tests/run-windows-bench.sh`
    - specific fixes:
      - batch benchmark generators now use `2..1000` items for real batch scenarios
      - Windows benchmark failure reporting now defines `server_out` before calling `dump_server_output`
    - targeted proof after the fix:
      - the previously failing pairs now succeed locally and on `win11`:
        - `uds-batch-ping-pong c->c`
        - `uds-batch-ping-pong rust->c`
        - `shm-batch-ping-pong c->c`
        - `shm-batch-ping-pong rust->c`
        - `np-batch-ping-pong c->c`
        - `np-batch-ping-pong rust->c`
    - clean official reruns:
      - Linux:
        - `bash tests/run-posix-bench.sh benchmarks-posix.csv 5`
        - result:
          - `Total measurements: 201`
      - Windows:
        - `ssh win11 'cd /tmp/plugin-ipc-bench-fixed && ... && bash tests/run-windows-bench.sh benchmarks-windows.csv 5'`
        - result:
          - `Total measurements: 201`
    - clean generated artifacts:
      - `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md`
        - result:
          - `All performance floors met`
      - `ssh win11 'cd /tmp/plugin-ipc-bench-fixed && ... && bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md'`
        - result:
          - `All performance floors met`
  - the follow-up benchmark spread investigation has now established a real benchmark-build bug on POSIX:
    - the local benchmark runner used:
      - C from `build/bin/bench_posix_c`
      - Rust from `src/crates/netipc/target/release/bench_posix`
      - Go from `build/bin/bench_posix_go`
    - the local CMake tree used for the C benchmark was configured as:
      - `build/CMakeCache.txt`:
        - `CMAKE_BUILD_TYPE:STRING=Debug`
    - the benchmark target itself added `-O2`, but the C libraries it linked against were still unoptimized:
      - `build/CMakeFiles/bench_posix_c.dir/flags.make`:
        - `C_FLAGS = -g -std=gnu11 -O2`
      - `build/CMakeFiles/netipc_protocol.dir/flags.make`:
        - `C_FLAGS = -g -std=gnu11`
      - `build/CMakeFiles/netipc_service.dir/flags.make`:
        - `C_FLAGS = -g -std=gnu11`
    - a dedicated optimized benchmark tree proved this materially changes the published POSIX rows:
      - release build setup:
        - `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`
        - `cmake --build build-release --target bench_posix_c bench_posix_go -j8`
      - direct targeted reruns:
        - published `shm-batch-ping-pong c->c`:
          - `25,947,290`
        - optimized C libs `shm-batch-ping-pong c(rel)->c(rel)`:
          - `63,699,472`
        - published `uds-pipeline-batch-d16 c->c`:
          - `49,512,090`
        - optimized C libs `uds-pipeline-batch-d16 c(rel)->c(rel)`:
          - `103,212,623`
      - mixed-language targeted reruns also moved sharply upward when the C side used optimized libraries:
        - intended `shm-batch-ping-pong c(rel)->rust`:
          - `57,122,454`
        - intended `shm-batch-ping-pong rust->c(rel)`:
          - `52,041,263`
        - intended `uds-pipeline-batch-d16 c(rel)->rust`:
          - `91,093,895`
        - intended `uds-pipeline-batch-d16 rust->c(rel)`:
          - `101,978,294`
    - implemented fix:
      - `tests/run-posix-bench.sh` now configures and uses a dedicated optimized benchmark tree:
        - default: `build-bench-posix`
        - build type: `Release`
      - `tests/run-windows-bench.sh` now configures and uses a dedicated optimized benchmark tree:
        - default: `build-bench-windows`
        - build type: `Release`
        - explicit MinGW toolchain export on `win11`
    - factual conclusion:
      - the old checked-in POSIX benchmark report was distorted by linking the C benchmark binary against Debug-built C libraries
      - the current checked-in POSIX and Windows benchmark artifacts now come from the corrected dedicated benchmark build paths
  - the Windows benchmark tree is not affected by the same local Debug-build distortion:
    - `ssh win11 '... grep CMAKE_BUILD_TYPE build/CMakeCache.txt'`
      - `CMAKE_BUILD_TYPE:STRING=RelWithDebInfo`
    - the previously suspicious Windows SHM batch outlier did not survive the corrected rerun:
      - old checked-in row:
        - `shm-batch-ping-pong c->rust = 9,282,667`
      - corrected clean rerun row:
        - `shm-batch-ping-pong c->rust = 55,868,058`
    - final artifact sanity checks:
      - `benchmarks-posix.csv`
        - rows: `201`
        - duplicate keys: `0`
        - zero-throughput rows: `0`
      - `benchmarks-windows.csv`
        - rows: `201`
        - duplicate keys: `0`
        - zero-throughput rows: `0`
    - checked-in benchmark docs are now synced to the refreshed artifacts:
      - `benchmarks-posix.csv`
      - `benchmarks-posix.md`
      - `benchmarks-windows.csv`
      - `benchmarks-windows.md`
      - `README.md`
    - corrected max-throughput ranges from the current checked-in artifacts:
      - POSIX:
        - `uds-ping-pong`: `182,963` to `231,160`
        - `shm-ping-pong`: `2,460,317` to `3,450,961`
        - `uds-batch-ping-pong`: `27,182,404` to `40,240,940`
        - `shm-batch-ping-pong`: `31,250,784` to `64,148,960`
        - `uds-pipeline-d16`: `568,373` to `735,829`
        - `uds-pipeline-batch-d16`: `51,960,946` to `102,954,841`
        - `snapshot-baseline`: `158,948` to `205,624`
        - `snapshot-shm`: `1,006,053` to `1,738,616`
        - `lookup`: `114,556,227` to `203,279,430`
      - Windows:
        - `np-ping-pong`: `18,241` to `21,039`
        - `shm-ping-pong`: `2,099,392` to `2,715,487`
        - `np-batch-ping-pong`: `7,013,700` to `8,550,220`
        - `shm-batch-ping-pong`: `36,494,096` to `58,768,397`
        - `np-pipeline-d16`: `245,420` to `270,488`
        - `np-pipeline-batch-d16`: `28,977,365` to `41,270,903`
        - `snapshot-baseline`: `16,090` to `20,967`
        - `snapshot-shm`: `857,823` to `1,262,493`
        - `lookup`: `107,472,315` to `164,305,717`
  - current remaining raw Rust drift is now narrower and well-scoped:
    - the raw managed server already enforces one `expected_method_code`
    - the raw client surface still exposes a generic constructor and mixed call surface under the stale internal name `CgroupsClient`
    - the next cleanup slice is to bind the raw Rust client constructors to one service kind and migrate the raw Rust tests to those constructors, matching the already-correct Go raw design
  - raw Rust client drift is now removed from the active service surface:
    - `src/crates/netipc/src/service/raw.rs` now exposes `RawClient` instead of the stale internal multi-kind name `CgroupsClient`
    - the raw client is now created only through service-kind-specific constructors:
      - `RawClient::new_snapshot(...)`
      - `RawClient::new_increment(...)`
      - `RawClient::new_string_reverse(...)`
    - request kind remains only as envelope validation on the raw client
    - the raw Rust Unix/Windows tests now create snapshot, increment, and string-reverse clients explicitly instead of reusing one generic constructor across service kinds
  - local Linux Rust proof for that slice is now green:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib service::raw::tests:: -- --test-threads=1`
      - result:
        - `75 passed`
        - `0 failed`
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1`
      - result:
        - `299 passed`
        - `0 failed`
  - real `win11` Rust proof for that slice is now green too:
    - `timeout 900 cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1`
      - result:
        - `176 passed`
        - `0 failed`
        - `1 ignored`
  - the broader `win11` interop/service/cache matrix initially exposed two more stale constructor residues outside the Rust raw tests:
    - Rust benchmark drivers still imported the deleted raw `CgroupsClient` instead of using the public snapshot facade
      - fixed in:
        - `bench/drivers/rust/src/main.rs`
        - `bench/drivers/rust/src/bench_windows.rs`
    - Go public cgroups wrappers still called the deleted generic raw constructor:
      - `raw.NewClient(...)`
      - fixed in:
        - `src/go/pkg/netipc/service/cgroups/client.go`
        - `src/go/pkg/netipc/service/cgroups/client_windows.go`
    - Go benchmark drivers still hand-rolled the stale raw dispatch signature instead of using the single-kind increment adapter
      - fixed in:
        - `bench/drivers/go/main.go`
  - the next verified contradiction slice was documentation-heavy and is now resolved:
    - low-level SHM / handshake docs now describe the accepted directional negotiation model and the current session-scoped SHM lifecycle:
      - request limits are sender-driven
      - response limits are server-driven
      - SHM capacities are fixed per session
      - larger learned capacities require a reconnect and a new session, not in-place SHM resize
    - `docs/level1-wire-envelope.md` no longer says handshake rule 6 takes the minimum of client and server values
    - `docs/level1-windows-np.md` now documents per-session Windows SHM object names with `session_id`, aligned with both code and `docs/level1-windows-shm.md`
    - public L2 comments/docs no longer claim a blanket "retry ONCE":
      - ordinary failures still retry once
      - overflow-driven resize recovery may reconnect more than once while capacities grow
    - Unix test/script cleanup helpers no longer remove the stale pre-session path `{service}.ipcshm`; they now use per-session cleanup that matches `{service}-{session_id}.ipcshm`
    - validation for this slice is green:
      - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib service::raw::tests:: -- --test-threads=1`
        - result:
          - `75 passed`
          - `0 failed`
      - `cd src/go && go test -count=1 ./pkg/netipc/service/raw`
        - result:
          - `ok`
      - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_service_interop|test_cache_interop|test_shm_interop)$'`
        - result:
          - `100% tests passed`
          - `0 failed`
  - the next verified residue slice is narrower and fixture-focused:
    - several Unix C/Go fixture cleanup helpers still unlink the dead pre-session path `{service}.ipcshm` instead of using per-session cleanup
    - current proven hits:
      - `tests/fixtures/c/test_service.c`
      - `tests/fixtures/c/test_cache.c`
      - `tests/fixtures/c/test_hardening.c`
      - `tests/fixtures/c/test_chaos.c`
      - `tests/fixtures/c/test_multi_server.c`
      - `tests/fixtures/c/test_stress.c`
      - `src/go/pkg/netipc/service/cgroups/cgroups_unix_test.go`
  - that Unix fixture-cleanup residue slice is now resolved:
    - the touched Unix C fixtures now use `nipc_shm_cleanup_stale(TEST_RUN_DIR, service)` instead of unlinking the dead `{service}.ipcshm` path
    - the touched Go public cgroups Unix tests now use `posix.ShmCleanupStale(testRunDirUnix, service)` instead of removing the dead `{service}.ipcshm` path
    - validation for this slice is green:
      - `cd src/go && go test -count=1 ./pkg/netipc/service/cgroups`
        - result:
          - `ok`
      - `cmake --build build --target test_service test_cache test_hardening test_multi_server test_chaos test_stress`
        - result:
          - rebuild passed
      - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_service|test_cache|test_hardening|test_multi_server|test_chaos|test_stress)$'`
        - result:
          - `100% tests passed`
          - `0 failed`
  - one more live Unix fixture contradiction remains after that cleanup pass:
    - `tests/fixtures/c/test_chaos.c:test_shm_chaos()` still opens the dead pre-session SHM path `{run_dir}/{service}.ipcshm`
    - this is not just stale cleanup text; it likely means the SHM-chaos path is not actually targeting the live per-session SHM file today
  - that live SHM-chaos contradiction is now resolved:
    - `tests/fixtures/c/test_chaos.c:test_shm_chaos()` now captures the live `session_id` from the ready client session and opens `{run_dir}/{service}-{session_id}.ipcshm`
    - the test no longer treats "SHM file not found" as an acceptable skip on this path
    - validation:
      - `cmake --build build --target test_chaos`
        - result:
          - rebuild passed
      - `/usr/bin/ctest --test-dir build --output-on-failure -R '^test_chaos$'`
        - result:
          - `100% tests passed`
          - `0 failed`
  - current residue scan excluding this TODO file is now clean for the main drift markers:
    - no remaining old `{service}.ipcshm` path literals
    - no remaining deleted `CgroupsHandlers` / `CgroupsServer` API references
    - no remaining deleted `raw.NewClient(...)` / `service::raw::CgroupsClient` references
    - no remaining deleted `new_single_kind` / `with_workers_single_kind` references
  - broader Unix validation after these cleanup passes is also green:
    - `/usr/bin/ctest --test-dir build --output-on-failure -R '^(test_uds_interop|test_shm_interop|test_service_interop|test_service_shm_interop|test_cache_interop|test_cache_shm_interop|test_shm|test_service|test_cache|test_shm_rust|test_service_rust|test_shm_go|test_service_go|test_cache_go|test_hardening|test_ping_pong|test_multi_server|test_chaos|test_stress)$'`
      - result:
        - `100% tests passed`
        - `0 failed`
        - `19/19` passed
        - `bench/drivers/go/main_windows.go`
  - local Go proof for the wrapper/benchmark cleanup is now green:
    - `cd src/go && go test -count=1 ./pkg/netipc/service/cgroups`
      - result:
        - `ok`
    - `cd bench/drivers/go && go test -run '^$' ./...`
      - result:
        - compile-only pass
  - real `win11` build + matrix proof after those residue fixes is now green:
    - `cmake --build build -j4`
      - result:
        - build succeeds again after the Rust/Go constructor cleanup
    - `timeout 1800 ctest --test-dir build --output-on-failure -R "^(test_named_pipe_interop|test_win_shm_interop|test_service_win_interop|test_service_win_shm_interop|test_cache_win_interop|test_cache_win_shm_interop)$"`
      - result:
        - `test_named_pipe_interop`: passed
        - `test_win_shm_interop`: passed
        - `test_service_win_interop`: passed
        - `test_service_win_shm_interop`: passed
        - `test_cache_win_interop`: passed
        - `test_cache_win_shm_interop`: passed
        - summary:
          - `100% tests passed, 0 tests failed out of 6`
  - verified residue scan for the stale constructor names used in this slice is now clean:
    - no remaining `raw.NewClient`
    - no remaining `service::raw::CgroupsClient`
    - no remaining `RawClient::new(`
  - a smaller cross-platform residue cleanup is now also complete:
    - the test-only Rust helper `dispatch_single()` in `src/crates/netipc/src/service/raw.rs` is now explicitly marked as dead-code-tolerant under test builds, so Windows lib-test builds no longer emit the stale unused-function warning
    - the remaining public docs/spec wording in this slice was normalized away from the older "method-specific" phrasing where it described the public L2 service surface or service contracts:
      - `docs/level1-transport.md`
      - `docs/codec.md`
      - `docs/level2-typed-api.md`
      - `docs/code-organization.md`
      - `docs/codec-cgroups-snapshot.md`
  - local Linux validation after that wording/test-helper cleanup is still green:
    - `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1`
      - result:
        - `299 passed`
        - `0 failed`
  - real `win11` validation after that cleanup is also still green:
    - `timeout 900 cargo test --manifest-path src/crates/netipc/Cargo.toml --lib -- --test-threads=1`
      - result:
        - `176 passed`
        - `0 failed`
        - `1 ignored`
      - factual note:
        - the previous Windows-only `dispatch_single` unused-function warning is no longer present in this run
      - the Windows guard output still shows the accepted request-resize behavior:
        - transparent recovery
        - exactly one reconnect
        - negotiated request-size growth
  - new verified internal raw-client alignment:
    - fact:
      - the raw managed servers in Go and Rust were already bound to one `expected_method_code`
      - the remaining client-side drift was that one long-lived raw client context still exposed multiple service-kind calls
    - implementation slice now completed in Go:
      - raw Go clients are now created per service kind:
        - `NewSnapshotClient(...)`
        - `NewIncrementClient(...)`
        - `NewStringReverseClient(...)`
      - each client now stores one expected request code and rejects wrong-kind calls as validation failures instead of pretending one client can legitimately serve multiple service kinds
      - the cache helpers now bind explicitly to `cgroups-snapshot`
    - exact local Unix proof:
      - `cd src/go && go test -count=1 ./pkg/netipc/service/raw`
      - result:
        - `ok`
    - exact real Windows proof on `win11`:
      - `cd ~/src/plugin-ipc.git/src/go && go test -count=1 ./pkg/netipc/service/raw`
      - first rerun exposed one Windows-only missed constructor site:
        - `pkg/netipc/service/raw/shm_windows_test.go:334`
        - stale `NewClient(...)`
      - after correcting that last Windows-only leftover and resyncing:
        - result:
          - `ok`
    - factual conclusion:
      - the Go raw helper layer is now materially aligned with the accepted single-service-kind design on both Unix and Windows
      - remaining work is to carry the same invariant through the remaining Rust raw helper surface
  - a full Rust `cargo test --lib` run is still blocked by one unrelated transport failure outside this rewrite slice:
    - `transport::posix::tests::test_receive_batch_count_exceeds_limit`
  - remaining heavy work is now concentrated in:
    - proving the accepted resize behavior with the full interop/service/cache matrices on Unix and Windows, not just the targeted raw suites
    - getting real Windows compile/run proof for the edited Rust/Go/C Windows test surfaces
    - reconciling the current C path with the final single-kind + learned-size design language everywhere, then validating all 3 languages together

## Analysis

### Verified facts about Netdata today

- `cgroups.plugin` is not an external executable. It runs inside the Netdata daemon:
  - `cgroups_main()` is started from `src/daemon/static_threads_linux.c`.
- `ebpf.plugin` is a separate external executable:
  - built by `add_executable(ebpf.plugin ...)` in `CMakeLists.txt`.
- Current `cgroups.plugin` -> `ebpf.plugin` integration is a custom SHM + semaphore contract:
  - producer: `src/collectors/cgroups.plugin/cgroup-discovery.c`
  - shared structs: `src/collectors/cgroups.plugin/sys_fs_cgroup.h`
  - consumer: `src/collectors/ebpf.plugin/ebpf_cgroup.c`
- The shared payload currently transports cgroup metadata, not PID membership:
  - fields: `name`, `hash`, `options`, `enabled`, `path`
  - `ebpf.plugin` still reads each `cgroup.procs` file itself.
- Netdata already has a stable per-run invocation identifier:
  - `src/libnetdata/log/nd_log-init.c`
  - Netdata reads `NETDATA_INVOCATION_ID`, else `INVOCATION_ID`, else generates a UUID and exports `NETDATA_INVOCATION_ID`.
- External plugins are documented to receive `NETDATA_INVOCATION_ID`:
  - `src/plugins.d/README.md`
- Netdata already exposes plugin environment variables centrally:
  - `src/daemon/environment.c`
- Netdata already has the right build roots for all 3 languages:
  - C via top-level `CMakeLists.txt`
  - Rust workspace in `src/crates/Cargo.toml`
  - Go module in `src/go/go.mod`

### Verified facts about plugin-ipc today

- `plugin-ipc` already has the exact L3 cgroups snapshot API for this use case:
  - `docs/level3-snapshot-api.md`
- The typed snapshot schema closely matches Netdata’s current SHM payload:
  - `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
- The C API already supports:
  - managed server lifecycle
  - typed cgroups client/cache
  - POSIX transport with negotiated SHM fast path
- Authentication in `plugin-ipc` is a `uint64_t auth_token`:
  - `src/libnetdata/netipc/include/netipc/netipc_service.h`
  - `src/libnetdata/netipc/include/netipc/netipc_uds.h`
  - Rust/Go implementations use the same concept.

### Important integration implications

- Phase 1 can replace the metadata transport only.
- Phase 1 will not remove `ebpf.plugin` reads of `cgroup.procs`.
- The default `plugin-ipc` response size is too small for real Netdata snapshots on large hosts, so Linux integration must use an explicit large response limit.
- The best build/distribution model is in-tree vendoring inside Netdata, not an external system dependency.
- Current Netdata payload sizing evidence already proves this:
  - `cgroup_root_max` default is `1000` in `src/collectors/cgroups.plugin/sys_fs_cgroup.c`
  - current per-item SHM body carries `name[256]` and `path[FILENAME_MAX + 1]` in `src/collectors/cgroups.plugin/sys_fs_cgroup.h`
  - `FILENAME_MAX` on this Linux build environment is `4096`
  - this means the current per-item shape is already about `4.3 KiB` before protocol framing/alignment

### Verified design-drift findings

- The original written phase plan did **not** describe a multi-method server.
  - Evidence:
    - `TODO-plugin-ipc.history.md`
    - historical phase plan still says:
      - `Define and freeze a minimal v1 typed schema for one RPC method ('increment')`
- The first generated L2 spec also did **not** need a multi-method server model.
  - Evidence:
    - initial `docs/level2-typed-api.md` from commit `1722f95`
    - handler contract was framed as one typed request view + one response builder per handler callback
    - no raw transport-level switch over multiple method codes in that initial text
- The history TODO already contained the correct service-oriented discovery model.
  - Evidence:
    - `TODO-plugin-ipc.history.md`
    - explicit historical decisions already said:
      - discovery is service-oriented, not plugin-oriented
      - service names are the stable public contract
      - one endpoint per service
      - one persistent client context per service
      - startup order can remain random
      - caller owns reconnect cadence via `refresh(ctx)`
  - Implication:
    - the later multi-method server model was not a missing discussion
    - it was drift away from an already-decided service model
- The first explicit spec drift appears in commit `53b5e5a` on `2026-03-16`.
  - Evidence:
    - `docs/level2-typed-api.md` in commit `53b5e5a`
    - handler contract changed to:
      - raw-byte transport handler
      - `switch(method_code)`
      - `INCREMENT`
      - `STRING_REVERSE`
      - `CGROUPS`
    - this is the first clear documentation model where one server endpoint dispatches multiple request kinds
- The first strong implementation-level generalization appears the same day in commit `69bb794`.
  - Evidence:
    - commit message explicitly says:
      - `Add dispatch_increment(), dispatch_string_reverse(), dispatch_cgroups_snapshot()`
    - `docs/getting-started.md` in that commit adds typed helper examples for more than one method family
    - this widened the implementation and examples toward a generic multi-method dispatch surface
- The drift was then reinforced in public examples in commit `6014b0e` on `2026-03-17`.
  - Evidence:
    - `docs/getting-started.md`
    - C example registers:
      - `.on_increment`
      - `.on_cgroups`
    - Rust example registers:
      - `on_increment`
      - `on_cgroups`
    - Go example registers:
      - `OnIncrement`
      - `OnSnapshot`
    - text says:
      - `You register typed callbacks for the supported methods`
- The drift became operationally entrenched in interop in commit `099945b` on `2026-03-16`.
  - Evidence:
    - commit message explicitly says:
      - `Cross-language interop now tests all method types`
    - interop fixtures for C, Rust, and Go on POSIX and Windows all dispatch:
      - `INCREMENT`
      - `CGROUPS_SNAPSHOT`
      - `STRING_REVERSE`
- The drift later propagated into current coverage/TODO planning and the repository README.
  - Evidence:
    - `TODO-pending-from-rewrite.md` planned:
      - `snapshot / increment / string-reverse / batch over SHM`
    - `README.md` now says:
      - `servers register typed handlers`

### Current factual conclusion from the drift investigation

- There is currently **no evidence** in the TODO history that the original direction from the user was:
  - one server should serve multiple request kinds
- The strongest historical evidence points the other way:
  - the original phase plan explicitly named one RPC method only
- Working theory:
  - the drift started when the typed API was generalized from:
    - one typed request kind per server
    - to
    - one generic server dispatching multiple method codes
  - then examples, interop fixtures, tests, coverage plans, and README text copied that model until it felt normal

## Decisions

### Made

0. Windows runtime validation host
   - User decision: use `win11` over SSH for real Windows proof instead of stopping at source cleanup or cross-compilation from Linux.
   - Constraint:
     - prefer the already-documented `win11` workflow from this repository's TODOs/docs
     - do not guess the Windows execution flow when the repo already documents it
   - Implication:
     - touched Windows Rust/Go/C transport/service/interop/cache surfaces should now be proven on a real Windows runtime, not just by static review or Linux-hosted cross-compilation
     - the next implementation slice should follow the existing `win11` operational guidance already captured in the repo

1. Authentication source
   - User decision: use `NETDATA_INVOCATION_ID` for authentication.
   - Meaning:
     - the auth value changes on every Netdata run
     - only plugins launched under the same Netdata instance can authenticate
   - Evidence:
     - `src/libnetdata/log/nd_log-init.c` creates/exports `NETDATA_INVOCATION_ID`
     - `src/plugins.d/README.md` documents it for external plugins
   - Implication:
     - this is stronger than a machine-stable token for local plugin-to-plugin IPC
     - restarts invalidate old clients automatically

2. Source layout in Netdata
   - User decision: native Netdata layout.
   - Layout:
     - C in `src/libnetdata/netipc/`
     - Rust in `src/crates/netipc/`
     - Go in `src/go/pkg/netipc/`
   - Implication:
     - the library becomes a first-class internal Netdata component in all 3 languages
     - future sync from `plugin-ipc` upstream will be manual/curated, not subtree-based

3. Invocation ID to auth-token mapping
   - User decision: derive the `plugin-ipc` `uint64_t auth_token` from `NETDATA_INVOCATION_ID` using a deterministic hash.
   - Constraint:
     - the mapping must be identical in C, Rust, and Go
   - Implication:
     - only processes launched under the same Netdata run can authenticate
     - Netdata restart rotates auth automatically

4. Rollout mode
   - User decision: big-bang switch.
   - Implication:
     - there will be no legacy custom-SHM fallback path for this metadata channel
   - Risk:
     - any bug in the new path blocks `ebpf.plugin` cgroup metadata integration immediately

5. Linux response size policy
   - User concern/decision direction:
     - do not accept a large fixed memory cost such as `16 MiB` just for this IPC path
     - prefer dynamic behavior that adapts to actual payload size
     - allocation should happen only when needed
   - Implication:
     - the current `plugin-ipc` response budgeting model needs review before integration
     - response sizing / negotiation may need design changes, not just configuration

6. Snapshot overflow handling direction
   - User decision direction:
     - reconnect is acceptable for snapshot overflow handling
     - growth policy should be power-of-two
     - SHM L2 should transparently handle overflow-driven resizing, hidden from both L2 clients and L2 servers
   - User design intent:
     - the server should not need to know the final safe snapshot size before the first request
     - the first real overflow during response preparation should trigger the resize path
     - once the server has learned a larger size from a real snapshot, later clients should negotiate into that larger size automatically
   - Implication:
     - current fixed per-session SHM sizing and current HELLO/HELLO_ACK limit semantics are not sufficient as-is for this Netdata use case
     - the growth mechanism likely needs new L2 protocol behavior, not only implementation tweaks

7. Pre-integration gating
   - User decision:
     - implement this transparent SHM resize behavior in `plugin-ipc` first
     - do not start Netdata integration before it is done
     - require thorough validation first, including full interop matrices across C/Rust/Go on Unix and Windows
   - Verified evidence that the repo already has the right validation scaffolding:
     - POSIX interop tests in `CMakeLists.txt`:
       - `test_uds_interop`
       - `test_shm_interop`
       - `test_service_interop`
       - `test_service_shm_interop`
       - `test_cache_interop`
       - `test_cache_shm_interop`
     - Windows interop tests in `CMakeLists.txt`:
       - `test_named_pipe_interop`
       - `test_win_shm_interop`
       - `test_service_win_interop`
       - `test_cache_win_interop`
     - Existing transport-specific integration tests already exist:
       - POSIX SHM: `tests/fixtures/c/test_shm.c`, Rust `src/crates/netipc/src/transport/shm_tests.rs`
       - Windows SHM: `tests/fixtures/c/test_win_shm.c`, Rust `src/crates/netipc/src/transport/win_shm.rs`, Go `src/go/pkg/netipc/transport/windows/shm_test.go`
   - Implication:
     - the resize feature must be proven at:
       - L1 transport level
       - L2 service/client level
       - cross-language interop level
       - both POSIX and Windows implementations

8. Design priorities for the resize rewrite
   - User decision:
     - optimize for long-term correctness, reliability, robustness, and performance
     - backward compatibility is not required
     - do not optimize for minimizing work now
     - prefer the right design even if that means a substantial rewrite
   - Implication:
     - decisions should favor clean semantics and maintainability over preserving current handshake/transport structure
     - a third rewrite is acceptable if it produces a better architecture

9. User design constraints from follow-up discussion
   - IPC servers should service a single request kind.
   - Sessions should be assumed long-lived:
     - connect once
     - serve many requests
     - disconnect on shutdown or exceptional recovery

10. Benchmark refresh slice disposition
   - User decision:
     - commit and push the refreshed benchmark slice now
     - then investigate the remaining benchmark spreads separately
   - Implication:
     - commit only the benchmark-fix, benchmark-artifact, and benchmark-doc sync files from this slice
     - do not mix this commit with unrelated cleanup or integration work

11. Current commit scope
   - User decision:
     - commit and push the full remaining work from this task now
   - Implication:
     - stage the remaining drift-removal, SHM-resize, service-kind alignment, test, and doc changes that belong to this task
     - avoid unrelated local or user-owned changes outside this task
   - Steady-state fast path matters far more than the rare resize path.
   - Learned transport sizes are important:
     - adapt automatically
     - stabilize quickly
     - then remain fixed for the lifetime of the process
     - reset on restart
   - Separate request and response sizing should exist.
   - Variable sizing pressure is expected mainly on responses, not requests.
   - Artificial hard caps are not acceptable as a design crutch.
   - Disconnect-based recovery is acceptable if it is reliable and the system stabilizes.

10. Accepted architecture decisions for the SHM resize rewrite
   - User accepted:
     - L2 service model: single-method-per-server
     - Resize signaling path: explicit `LIMIT_EXCEEDED` signal, then disconnect/reconnect
     - Auto-resize scope: separate learned request and response sizing, both supported
     - Initial size policy: per-server-kind compile-time defaults
     - Learned-size lifetime: in-memory only for the current process lifetime, reset on restart
   - Implication:
     - the current generic multi-method service abstraction is now known design drift
     - the rewrite should simplify transport/service code around one request kind per server

11. Service discovery and availability model
   - User clarified the intended service model explicitly:
     - clients connect to a service kind, not to a specific plugin implementation
     - each service endpoint serves one request kind only
     - example service kinds include:
       - `cgroups-snapshot`
       - `ip-to-asn`
       - `pid-traffic`
     - the serving plugin is intentionally abstracted away from clients
   - User clarified the intended runtime model explicitly:
     - plugins are asynchronous peers
     - startup order is not guaranteed
     - enrichments from other plugins/services are optional
     - a client plugin may start before the service it needs exists
     - a service may disappear and reappear during runtime
     - clients must reconnect periodically and tolerate service absence
   - Implication:
     - repository docs/specs/TODOs must describe:
       - service-name-based discovery
       - service-type ownership independent from plugin identity
       - optional dependency semantics
       - reconnect / retry behavior for not-yet-available services

12. Execution mandate for this phase
   - User decision:
     - proceed autonomously to remove the drift from implementation and docs
     - align code, tests, and examples to the single-service-kind model
     - implement the accepted SHM size renegotiation / resize behavior
     - remove contradictory wording and stale examples that preserve the wrong model
   - Implication:
     - this is now a repository-wide consistency and implementation task
     - active docs, public APIs, interop fixtures, and validation must converge on the same model before Netdata integration

13. Request-kind field semantics
   - User clarification:
     - request type / method code may remain in wire structures and headers
     - its role is validation, not public multi-method dispatch
     - a service endpoint expects exactly one request kind
     - any other request kind must be rejected
   - Implication:
     - we can keep method codes in the protocol
     - service implementations must bind one endpoint to one expected request kind
     - public APIs/tests/docs must not imply that one service endpoint accepts multiple unrelated request kinds

14. Payload-vs-service boundary
   - User clarification:
     - if a service needs arrays of things, batching belongs to that service payload/codec
     - batching is not a reason for one L2 endpoint to expose multiple public request kinds
   - Implication:
     - the public L2 service layer should not keep generic multi-method or generic batch dispatch as part of its contract
     - `INCREMENT`, `STRING_REVERSE`, and batch ping-pong traffic can remain at protocol / transport / benchmark level
     - the public cgroups snapshot service should be snapshot-only

### Pending

1. Service naming and endpoint placement
   - Context:
     - POSIX transport needs a service name and run-dir placement.
     - Netdata already has `os_run_dir(true)`.
   - Open question:
     - exact service name/versioning strategy for the cgroups snapshot endpoint

2. Exact Linux response-size budget
   - Context:
     - user rejected a large fixed per-connection budget as bad for footprint
     - dynamic/adaptive options must be evaluated against the current `plugin-ipc` design
   - Current hard payload evidence:
     - `1000` cgroups at roughly `4.3 KiB` each already implies multi-megabyte worst-case snapshots
   - Open question:
     - what protocol / implementation change best preserves low idle footprint while still supporting large snapshots

4. Dynamic response sizing model
   - Context:
     - current `plugin-ipc` session handshake negotiates `agreed_max_response_payload_bytes` once
     - current implementations then size buffers against that session-wide maximum
   - Verified evidence:
     - handshake uses `min(client, server)` in `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
     - C client allocates request/response/send buffers eagerly in `src/libnetdata/netipc/src/service/netipc_service.c`
     - C server allocates per-session response buffer sized to the full negotiated maximum in `src/libnetdata/netipc/src/service/netipc_service.c`
     - Linux SHM region size is fixed from negotiated request/response capacities in `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
     - UDS chunked receive is already dynamically grown with `realloc` in `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
     - Rust and Go clients are already more dynamic and grow buffers lazily in:
       - `src/crates/netipc/src/service/cgroups.rs`
       - `src/go/pkg/netipc/service/cgroups/client.go`
     - Netdata `ebpf.plugin` refreshes cgroup metadata every 30 seconds:
       - `src/collectors/ebpf.plugin/ebpf_process.h`
       - `src/collectors/ebpf.plugin/ebpf_cgroup.c`
   - Decision needed:
     - choose whether to keep the current protocol and improve allocation policy only, or evolve the protocol to support truly dynamic large snapshots
   - Options:
     - A. Keep protocol, make implementation adaptive, and use baseline-only transport for the cgroups snapshot service in phase 1
     - B. Add paginated snapshot requests/responses
     - C. Add out-of-band exact-sized bulk snapshot transfer for large responses
     - D. Keep the current fixed session-wide max model and just configure a large cap
     - E. Keep SHM for data, but negotiate/create SHM capacity per request instead of per session
     - F. Split transport into a tiny control channel plus ephemeral payload channel/object
     - G. Add a small size-probe step before fetching the full snapshot
     - H. Add true server-streamed snapshot responses (multi-message response sequence)
     - I. Allow snapshot responses to return "resize to X bytes and retry", so the client grows once on demand and reuses that larger buffer from then on
      - J. Make SHM L2 transparently reconnect and double capacities on overflow, so resizing is hidden from both clients and servers and the server retains the learned larger size for future sessions
   - Current preferred direction under discussion:
      - J, but it still needs stress-testing against the current HELLO/HELLO_ACK semantics, SHM lifecycle, and L2 retry behavior

5. Transparent SHM resize semantics
   - Context:
     - user direction is to make SHM L2 resizing automatic and transparent to both clients and servers
     - reconnect is acceptable and growth should be power-of-two on overflow
   - Verified evidence:
     - current server sends `NIPC_STATUS_INTERNAL_ERROR` on handler/batch failure in `src/libnetdata/netipc/src/service/netipc_service.c`
     - current C/Go/Rust clients treat any non-`OK` response transport status as bad layout / failure:
       - `src/libnetdata/netipc/src/service/netipc_service.c`
       - `src/go/pkg/netipc/service/cgroups/client.go`
       - `src/crates/netipc/src/service/cgroups.rs`
     - `NIPC_STATUS_LIMIT_EXCEEDED` already exists in `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
   - Corrected layering rule from user discussion:
     - transport/L2 may handle overflow signaling, reconnect, and shared-memory remap mechanics
     - replay detection for mutating RPCs belongs to the request payload and the server business logic, not to transport-level semantic dedupe
   - Clarified implication:
     - transport should not try to "understand" whether a mutation was already applied
     - if a mutating method cares about replay safety, it must carry a request identity / idempotency token in its own payload and the server method must enforce it
   - For the Netdata cgroups snapshot use case:
     - this is not a blocker, because snapshot is read-only
   - Open question:
     - whether transparent reconnect-and-retry should be generic transport behavior for all methods, or exposed as a capability that higher layers opt into when their payload semantics make replay safe

6. Negotiation semantics for learned SHM size
   - Context:
     - user correctly rejected the current `min(client, server)` rule for learned snapshot sizing
     - current handshake stores only one scalar per direction, so it cannot distinguish:
       - client hard cap
       - client initial size
       - server learned target size
   - Verified evidence:
     - current HELLO/HELLO_ACK uses fixed `agreed_max_*` fields in:
       - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
       - `src/crates/netipc/src/transport/posix.rs`
       - `src/crates/netipc/src/transport/windows.rs`
   - Open question:
     - should the protocol split "current operational size" from "hard ceiling", so the server can advertise a learned larger target without losing the client’s ability to refuse absurd allocations

7. Request-side vs response-side SHM growth asymmetry
   - Verified evidence:
     - POSIX SHM send rejects oversize messages locally before the peer can react:
       - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
     - existing tests already cover this class of failure:
       - `tests/fixtures/c/test_shm.c`
       - `tests/fixtures/c/test_service.c` (`test_shm_batch_send_overflow_on_negotiated_limit`)
       - `tests/fixtures/c/test_win_shm.c`
       - `tests/fixtures/c/test_win_service_guards.c`
   - Implication:
     - response-capacity growth can be learned by the server while building a response
     - request-capacity growth cannot be learned the same way, because an oversize request fails client-side before the server sees it
   - Open question:
     - should the first implementation cover:
       - response-side transparent resize only
       - or symmetric request+response resize with separate client-learned request sizing semantics

3. Netdata lifecycle ownership details
   - Context:
     - `cgroups.plugin` runs in-daemon
     - `ebpf.plugin` is external
   - Open question:
     - exact daemon init/shutdown points for starting/stopping the `plugin-ipc` cgroups server and for initializing the `ebpf.plugin` client cache

## Plan

1. Audit the current implementation surfaces that still encode multi-method service behavior.
2. Define the replacement public model in code terms:
   - one service module per service kind
   - one endpoint per request kind
   - service-specific typed clients/servers/cache helpers
3. Redesign SHM resize semantics in implementation terms:
   - explicit `LIMIT_EXCEEDED`
   - disconnect/reconnect recovery
   - separate learned request/response sizes
   - process-lifetime learned sizing
4. Rewrite the C, Rust, and Go Level 2 service layers to match the corrected model.
5. Rewrite interop/service fixtures and validation scripts to test one service kind per server.
6. Rewrite public docs/examples/specs to remove contradictory multi-method wording.
7. Run targeted tests first, then the full relevant Unix/Windows matrices required to trust the rewrite.
8. Summarize any residual risk or remaining ambiguity before starting Netdata integration work.
9. Rerun the current Linux and Windows benchmark matrices on the aligned tree.
10. Regenerate benchmark artifacts and update all benchmark-derived docs/README summaries.

## Implied decisions

- Preserve Level 1 transport interoperability work where still valid.
- Preserve codec/message-family work where it remains useful under a service-oriented split.
- Prefer removal/rename of drifted APIs over keeping compatibility shims, because backward compatibility is not required.
- Keep request-kind and outer-envelope metadata available to single-kind handlers only for:
  - validating that the endpoint received the expected request kind
  - reading transport batch metadata when a single service kind supports batched payloads
- Do not use that metadata to reintroduce generic multi-method dispatch at the public Level 2 surface.
- If a generic Level 2 helper remains for tests/benchmarks, keep it internal and single-kind:
  - one expected request kind per endpoint
  - no public multi-method callback surface
  - no docs/examples presenting it as a production service model

## Testing requirements

- C, Rust, and Go unit tests for the rewritten service APIs
- POSIX interop matrix for corrected service identities and SHM resize behavior
- Windows interop matrix for corrected service identities and SHM resize behavior
- Explicit tests for:
  - late provider startup
  - reconnect after provider restart
  - service absence as a tolerated state
  - SHM resize on response overflow
  - learned-size reuse after reconnect
  - request-side and response-side learned sizing behavior

## Documentation updates required

- Keep README, docs specs, and active TODOs aligned with:
  - service-oriented discovery
  - one request kind per endpoint
  - optional asynchronous enrichments
  - reconnect-driven recovery
  - SHM resize / renegotiation behavior

1. Finalize remaining design details above.
2. Vendor `plugin-ipc` into Netdata in the chosen native layout.
3. Add a Linux `cgroups` typed server inside Netdata daemon lifecycle.
4. Replace `ebpf.plugin` shared-memory metadata reader with `plugin-ipc` cgroups cache client.
5. Keep existing PID membership logic in `ebpf.plugin` unchanged in phase 1.
6. Remove the old custom SHM metadata path as part of the big-bang switch.
7. Add tests for:
   - normal metadata refresh
   - stale/restarted Netdata invalidating old clients
   - large snapshots
   - `ebpf.plugin` recovery on server restart

## Implied decisions

- Phase 1 is Linux-only.
- Phase 1 targets `cgroups.plugin` -> `ebpf.plugin` metadata only.
- Current `collectors-ipc/ebpf-ipc.*` apps/pid SHM remains untouched.
- `NETDATA_INVOCATION_ID` must be available to the `ebpf.plugin` launcher path and any future external clients.
- A deterministic invocation-id hashing helper will be needed in C, Rust, and Go.

## Testing requirements

- Unit tests for invocation-id to auth-token derivation in C, Rust, and Go.
- Integration test proving only same-run plugins can connect.
- Integration test proving restart rotates auth and old clients fail cleanly.
- Snapshot scale test with high cgroup counts and long names/paths.
- `ebpf.plugin` regression test for existing cgroup discovery semantics.

## Documentation updates required

- Netdata integration design note for the new cgroups metadata transport.
- Developer docs for the new in-tree `netipc` layout and per-language use.
- `ebpf.plugin` and `cgroups.plugin` internal docs describing the new IPC path.
- Rollout/kill-switch documentation if dual-path rollout is selected.
