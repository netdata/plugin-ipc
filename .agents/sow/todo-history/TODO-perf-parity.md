## TL;DR

- user is concerned that the latest published Windows benchmarks show large performance deltas between languages for the same scenarios.
- The immediate purpose is to determine whether the deltas follow specific client/server implementations, identify the likely bottleneck side, and produce a fact-based analysis before any fixes.
- Initial benchmark evidence already shows:
  - Windows baseline / named-pipe scenarios are dominated by server language, with the C server path much slower than the others.
  - Windows SHM scenarios are dominated mainly by the Go client path, with a smaller secondary penalty from the Go server path.

## Purpose

- Fit for purpose:
  - ensure Windows transport implementations have credible performance parity across languages for the same protocol and benchmark shape
  - identify implementation-level bottlenecks before changing benchmark floors or accepting current numbers as normal

## Analysis

### Current benchmark evidence

- Source:
  - `benchmarks-windows.csv`
- Max-rate (`target_rps=0`) throughput facts:
  - `np-ping-pong`
    - `c->c`: `22543`
    - `rust->rust`: `56878`
    - `go->go`: `70464`
  - `snapshot-baseline`
    - `c->c`: `22158`
    - `rust->rust`: `57843`
    - `go->go`: `69307`
  - `shm-ping-pong`
    - `c->c`: `2787028`
    - `rust->rust`: `2735477`
    - `go->go`: `1654196`
  - `snapshot-shm`
    - `c->c`: `1230487`
    - `rust->rust`: `1294096`
    - `go->go`: `741659`

### Role-isolation facts already confirmed

- Windows baseline / named pipes:
  - holding the server fixed at `c` keeps throughput low regardless of client
  - holding the client fixed at `c` allows high throughput when the server changes to `rust` or `go`
  - therefore the current evidence points to the Windows C baseline server path as the primary bottleneck
- Windows SHM:
  - holding the client fixed at `go` keeps throughput low regardless of server
  - holding the server fixed at `go` also reduces throughput, but less severely
  - therefore the current evidence points to the Go SHM client as the primary bottleneck and the Go SHM server as a secondary bottleneck

### What is not yet proven

- It is not yet proven whether the root cause is:
  - benchmark-driver behavior
  - transport implementation differences
  - runtime / scheduler interaction
  - worker-count or warmup mismatch
  - buffer / syscall pattern differences

### Linux control points

- Linux does not show the same catastrophic baseline server asymmetry.
- Source:
  - `benchmarks-posix.csv`
- Max-rate (`target_rps=0`) control facts:
  - `uds-ping-pong`
    - `c->c`: `188654`
    - `rust->rust`: `187330`
    - `go->go`: `179475`
  - `shm-ping-pong`
    - `c->c`: `3376439`
    - `rust->rust`: `3447455`
    - `go->go`: `2841426`
- Implication:
  - the Windows baseline gap is not a generic protocol property
  - the Go SHM client has a mild generic penalty even on Linux, but Windows amplifies it significantly

### Code-path facts already checked

- `worker_count` is a session-concurrency limit, not parallel request handling within one session:
  - public header documents it as `max concurrent sessions`
  - C Windows accept loop enforces it with `session_count >= worker_count`
  - Go Windows server uses a semaphore of size `workerCount` around accepted sessions
  - Rust Windows server documents and enforces it as `thread per session (up to worker_count concurrent sessions)`
- The max-rate ping-pong benchmark uses one connection/session and then loops request/response on that same session:
  - C client connects once, then enters one `while` loop on that session
  - Go client connects once, then enters one `for` loop on that session
  - Rust client connects once, then enters one `while` loop on that session
- Implication:
  - increasing `worker_count` above `1` should not improve one-session ping-pong throughput
  - it is not a credible primary explanation for the `np-ping-pong` C server collapse

- Windows benchmark-driver worker-count defaults are not symmetric:
  - C benchmark server uses `nipc_server_init(..., 1, ...)` for non-batch servers
  - C benchmark batch server uses `nipc_server_init(..., 4, ...)`
  - Go managed server defaults to `8` workers
  - Rust managed server defaults to `8` workers
- Evidence:
  - `bench/drivers/c/bench_windows.c`
  - `src/go/pkg/netipc/service/raw/client_windows.go`
  - `src/crates/netipc/src/service/raw.rs`
- Important constraint:
  - for the max-rate single-session ping-pong and snapshot cases, this is not yet enough to explain the baseline gap by itself

- Build configuration does not show an obvious fairness bug:
  - Windows benchmark harness builds C in Release mode via CMake
  - Rust uses `cargo build --release`
  - Go uses normal `go build`
- Implication:
  - there is no current evidence that C is accidentally benchmarked in debug mode

- The C managed Windows server does not use the same extra readiness wait as Go and Rust:
  - C named-pipe server path blocks directly in `nipc_np_receive()`
  - Go server path calls `WaitReadable()` before `Receive()`
  - Rust server path calls `wait_readable()` before `receive()`
- Implication:
  - the slow C baseline server is not explained by an extra wait-readability polling step in the managed server loop

- The C benchmark handlers themselves do not show an obvious catastrophic per-request allocation pattern:
  - ping-pong handler decodes and encodes directly into caller-provided buffers
  - snapshot handler uses a prebuilt template and a caller-provided response buffer
- Implication:
  - the current evidence points more strongly to shared Windows service / transport code than to the benchmark handler body itself

- One concrete C transport disadvantage is already confirmed for larger named-pipe responses:
  - the C Windows named-pipe `raw_send_msg()` path uses a small stack buffer and falls back to `malloc()` / `free()` for larger messages
  - the Go and Rust Windows named-pipe paths reuse persistent scratch buffers instead
- Evidence:
  - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - `src/go/pkg/netipc/transport/windows/pipe.go`
  - `src/crates/netipc/src/transport/windows.rs`
- Implication:
  - this is a credible explanation for why `snapshot-baseline` is especially bad for the C server
  - it does not explain the full `np-ping-pong` C server collapse, because the ping-pong messages fit in the small stack buffer path

### Suspects not yet proven

- Windows baseline / named pipes:
  - most likely suspect remains the C server-side Windows baseline path below the benchmark handler:
    - named-pipe transport receive/send
    - C managed-server request/response loop on Windows

- Windows SHM:
  - most likely suspect remains the Go SHM receive/client path, with the Go SHM server path as a smaller secondary suspect

- Important restraint on speculation:
  - the C Windows SHM implementation explicitly documents that Go's `atomic.LoadInt64` compiles to a plain `MOV` on x86-64
  - therefore “Go atomics are expensive” is not established as the root cause from source inspection alone

### Strongest current working theory for `np-ping-pong`

- C Windows server path blocks directly in `ReadFile()` through `nipc_np_receive()`
- Go and Rust Windows server paths first run a `PeekNamedPipe()` readability loop with `SwitchToThread()` yielding, and only call `Receive()` once data is already visible
- Evidence:
  - `src/libnetdata/netipc/src/service/netipc_service_win.c`
  - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - `src/go/pkg/netipc/service/raw/client_windows.go`
  - `src/go/pkg/netipc/transport/windows/pipe.go`
  - `src/crates/netipc/src/service/raw.rs`
  - `src/crates/netipc/src/transport/windows.rs`
- Interpretation:
  - for one-session strict ping-pong, a blocking kernel wake-up per request may be materially slower than a hot `PeekNamedPipe()` + `SwitchToThread()` polling loop
  - this would fit:
    - the Windows-only nature of the collapse
    - the one-session ping-pong benchmark shape
    - the low C server CPU utilization despite poor throughput
- Status:
  - this is a working theory, not yet proven

### Narrow experiment result: C server switched to readability wait before receive

- Experiment:
  - patched `src/libnetdata/netipc/src/service/netipc_service_win.c`
  - changed the C Windows named-pipe server session loop to call `nipc_np_wait_readable()` before `nipc_np_receive()`
  - ran only the Windows `np-ping-pong` slice with `target_rps=0` on `win11`
  - output CSV:
    - `/tmp/netipc-np-pingpong-experiment.csv`

- Before -> after for the rows that isolate the C server:
  - `c->c`: `22543` -> `80573`
  - `rust->c`: `22551` -> `81089`
  - `go->c`: `21803` -> `67590`

- Control rows with non-C servers stayed in the same general band:
  - `c->rust`: `58609` -> `58410`
  - `rust->rust`: `56878` -> `56993`
  - `go->rust`: `46891` -> `49140`
  - `c->go`: `75227` -> `71356`
  - `rust->go`: `71197` -> `74231`
  - `go->go`: `70464` -> `67039`

- Conclusion:
  - the old ~22k ceiling for the Windows C server was caused by the blocking `ReadFile()` wake-up path in the one-session named-pipe ping-pong workload
  - the readiness-wait loop removes the catastrophic C-server collapse
  - worker-count mismatch is not the primary cause for this benchmark shape

### Narrow experiment result: Rust Windows wait loop switched from `Instant` to `GetTickCount64`

- Experiment:
  - patched `src/crates/netipc/src/transport/windows.rs`
  - changed Rust Windows `wait_readable()` deadline checks from `Instant::elapsed()` to `GetTickCount64()`, matching the cheap deadline pattern already used by the C helper
  - ran only the Windows `np-ping-pong` slice with `target_rps=0` on `win11`
  - output CSV:
    - `/tmp/netipc-np-pingpong-rust-tickcount-experiment.csv`

- Before -> after for the rows that isolate the Rust server:
  - `c->rust`: `58609` -> `78162`
  - `rust->rust`: `56878` -> `77051`
  - `go->rust`: `46891` -> `69254`

- Control rows stayed in the same general band:
  - `c->c`: `76407`
  - `rust->c`: `75239`
  - `go->c`: `67281`
  - `c->go`: `69647`
  - `rust->go`: `65698`
  - `go->go`: `66102`

- Conclusion:
  - the remaining Windows baseline Rust-server gap was a wait-loop timer-cost issue
  - it is not explained by generic Rust codec or dispatch overhead
  - the Rust Windows baseline server reaches the same general parity band as the fixed C and Go servers once the hot wait loop stops paying `Instant::elapsed()` cost

### Narrow experiment result: Go Windows SHM `spinPause()` switched from `SwitchToThread()` to CPU `PAUSE`

- Experiment:
  - patched:
    - `src/go/pkg/netipc/transport/windows/shm_pause.go`
    - `src/go/pkg/netipc/transport/windows/shm_pause_amd64.go`
    - `src/go/pkg/netipc/transport/windows/shm_pause_amd64.s`
  - changed the amd64 Go Windows SHM spin primitive from `SwitchToThread()` to CPU `PAUSE`, matching the intent already used by the C and Rust SHM loops

- Raw SHM max-rate run:
  - scenario:
    - `shm-ping-pong`
  - output CSV:
    - `/tmp/netipc-shm-pingpong-go-pause-experiment.csv`
  - Before -> after for the rows that isolate the Go SHM client:
    - `go->c`: `1918790` -> `2195717`
    - `go->rust`: `1811462` -> `2227030`
    - `go->go`: `1654196` -> `2190922`
  - Smaller secondary improvement on Go-server rows too:
    - `c->go`: `2439835` -> `2649510`
    - `rust->go`: `2227165` -> `2578539`

- Typed SHM max-rate run:
  - scenario:
    - `snapshot-shm`
  - output CSV:
    - `/tmp/netipc-snapshot-shm-go-pause-experiment.csv`
  - Before -> after for the rows that isolate the Go SHM client:
    - `go->c`: `828974` -> `1050093`
    - `go->rust`: `830640` -> `1163628`
    - `go->go`: `741659` -> `1040210`
  - Smaller secondary improvement on Go-server rows too:
    - `c->go`: `981562` -> `1135858`
    - `rust->go`: `946348` -> `1146853`

- Conclusion:
  - the Go Windows SHM gap is largely caused by using `SwitchToThread()` inside the SHM spin loops instead of a cheap CPU pause hint
  - the primary effect is on the Go client role
  - there is also a smaller but real secondary improvement on the Go server role

### Narrow experiment result: C Windows named-pipe reusable send buffer for larger replies

- Experiment:
  - patched:
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - added a reusable per-session `send_buf` and switched the larger-message named-pipe send path to reuse it instead of doing per-message `malloc()` / `free()`
  - ran only the Windows `snapshot-baseline` slice with `target_rps=0` on `win11`
  - output CSV:
    - `/tmp/netipc-snapshot-baseline-c-sendbuf-experiment.csv`

- Current cumulative results:
  - `c->c`: `78253`
  - `rust->c`: `76023`
  - `go->c`: `64521`
  - `c->rust`: `77986`
  - `rust->rust`: `79436`
  - `go->rust`: `70202`
  - `c->go`: `71814`
  - `rust->go`: `70350`
  - `go->go`: `67048`

- Important restraint:
  - this experiment was run after the earlier C Windows server prewait fix was already in place
  - therefore these numbers prove that the catastrophic C snapshot/server state is gone in the current accumulated code
  - they do **not** isolate exactly how much of the `snapshot-baseline` improvement comes from:
    - the C server prewait fix
    - the reusable C send buffer

- Conclusion:
  - the reusable C send buffer is still justified by direct code comparison against Go and Rust
  - but its isolated benchmark contribution remains to be measured separately if exact attribution is required

### Isolation run: `snapshot-baseline` without the C reusable send buffer

- Experiment:
  - generated a reverse patch for only:
    - `src/libnetdata/netipc/include/netipc/netipc_named_pipe.h`
    - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
  - applied that reverse patch temporarily on `win11`
  - reran only Windows `snapshot-baseline` with:
    - the already-committed C server prewait fix still present
    - the Rust wait-loop fix still present
    - the local C reusable send-buffer patch temporarily removed
  - restored the reverse patch immediately after the run
  - output CSV:
    - `/tmp/netipc-snapshot-baseline-without-c-sendbuf.csv`

- Direct comparison: with reusable send buffer vs without reusable send buffer
  - `c->c`: `78253` vs `78966` (`-0.90%`)
  - `rust->c`: `76023` vs `76956` (`-1.21%`)
  - `go->c`: `64521` vs `66785` (`-3.39%`)
  - `c->rust`: `77986` vs `78745` (`-0.96%`)
  - `rust->rust`: `79436` vs `78281` (`+1.48%`)
  - `go->rust`: `70202` vs `69667` (`+0.77%`)
  - `c->go`: `71814` vs `69224` (`+3.74%`)
  - `rust->go`: `70350` vs `70812` (`-0.65%`)
  - `go->go`: `67048` vs `66384` (`+1.00%`)

- Conclusion:
  - the isolated effect of the reusable C send buffer is not convincingly positive in this benchmark slice
  - the current data looks like noise around parity, not a clear win
  - therefore the reusable C send-buffer patch is not currently justified by benchmark evidence alone
  - the major C snapshot/server recovery came from the already-committed server prewait fix, not from this extra allocation change

### External review status

- External reviewer rerun requested for the Windows C server baseline question.
- Reviewers launched:
  - `claude`
  - `glm`
  - `qwen`
  - `kimi`
  - `minimax`
- Usable output:
  - `kimi`
- Unusable / incomplete:
  - `claude` produced no output
  - `qwen` produced no usable output
  - `glm` read files but did not return a final review
  - `minimax` returned session-status text before a late substantive review and is not reliable enough to treat as independent confirmation

### External review findings worth keeping

- `kimi` independently confirmed one solid C-side named-pipe disadvantage:
  - the C Windows named-pipe send path falls back to per-message `malloc()` / `free()` for larger messages
  - Go and Rust reuse persistent scratch buffers instead
- This aligns with the local source review and is a credible explanation for:
  - `snapshot-baseline`

- Second rerun:
  - `qwen` provided one substantive review
  - it independently converged on the same two strongest points:
    - worker-count mismatch is real
    - C named-pipe send-path allocation behavior is real
  - it also repeated the claim that worker-count mismatch is the primary cause
- Current assessment:
  - the send-path allocation finding remains worth keeping
  - the worker-count finding remains real but not yet proven as the primary cause of the `np-ping-pong` ceiling

### External review findings that remain unproven

- `kimi` also ranked these as root causes:
  - worker-count mismatch
  - blocking receive pattern in the C managed server
- Current assessment:
  - worker-count mismatch is real, but not yet sufficient to explain the catastrophic max-rate single-session baseline gap
  - the blocking receive explanation is weaker, because Go and Rust add an extra readiness-wait step, so this does not naturally explain why C is slower
- Therefore these remain:
  - plausible contributing factors
  - not proven primary causes

## Decisions
- user decision:
  - drop the C named-pipe reusable send-buffer patch
  - rationale: isolated Windows `snapshot-baseline` run shows no clear benefit
- user decision:
  - publish the remaining proven fixes as two separate commits
  - one commit for the Rust Windows wait-loop fix
  - one commit for the Go Windows SHM pause fix
- user decision:
  - the same library fixes must also be vendored into the Netdata integration PR and committed/pushed there
- Next target: the C Windows named-pipe snapshot/server send path, which still allocates on larger replies via `malloc()`/`free()` in `raw_send_msg()` instead of reusing a persistent scratch buffer like Go and Rust.
- Result of the Go SHM `spinPause()` experiment on Windows `snapshot-shm`: confirmed on the typed path too. Replacing Go's amd64 SHM spin loop primitive from `SwitchToThread()` to CPU `PAUSE` raised `go->c 828974 -> 1050093`, `go->rust 830640 -> 1163628`, `go->go 741659 -> 1040210`, and also improved the smaller Go-server rows `c->go 981562 -> 1135858`, `rust->go 946348 -> 1146853`. The Go SHM spin primitive is therefore a major cross-cutting cause of both the raw and typed Windows SHM gaps.
- Result of the Go SHM `spinPause()` experiment on Windows `shm-ping-pong`: strong confirmation. Replacing Go's amd64 SHM spin loop primitive from `SwitchToThread()` to CPU `PAUSE` raised all Go SHM rows: `go->c 1918790 -> 2195717`, `go->rust 1811462 -> 2227030`, `go->go 1654196 -> 2190922`, and also improved the smaller Go-server rows `c->go 2439835 -> 2649510`, `rust->go 2227165 -> 2578539`. This identifies the Go SHM `SwitchToThread()` spin primitive as a major cause of the Windows Go SHM gap, especially on the client side.
- Next narrow experiment for the Windows Go SHM client: replace the amd64 Go SHM `spinPause()` implementation from `SwitchToThread()` to an assembly `PAUSE` instruction, matching the C and Rust SHM spin loops, and rerun only the Windows SHM max-rate slice.
- New working theory for the Windows Go SHM client: Go SHM spin loops call `SwitchToThread()` on every `spinPause()`, while C uses `YieldProcessor()` and Rust uses the CPU `pause` instruction. For very short SHM round trips this can force costly scheduler yields on the Go client hot path instead of cheap busy-spin hints.
- Next target after the Rust-server fix: investigate the Windows Go SHM client, which still dominates the remaining SHM gap when the client role is held constant.
- Result of the Rust `GetTickCount64()` wait-loop experiment: strong confirmation. Changing Rust Windows `wait_readable()` from `Instant::elapsed()` deadline checks to cheap `GetTickCount64()` checks raised the Rust-server rows from the old `~58k` band to parity with the fixed C/Go server band: `c->rust 78162`, `rust->rust 77051`, `go->rust 69254` versus published `c->rust 58609`, `rust->rust 56878`, `go->rust 46891`. This identifies the remaining Windows baseline Rust-server discrepancy as a wait-loop timer-cost problem, not a generic Rust codec or dispatch problem.
- New working theory: the remaining Windows Rust-server baseline gap may come from `wait_readable()` using `Instant::elapsed()` inside the 256-iteration hot spin loop, while the C helper uses cheap `GetTickCount64()` deadline checks. Linux parity and Windows server-CPU saturation point to a Windows-specific wait-loop CPU cost rather than generic codec overhead.\n- Result of the Rust small-message send-path experiment: rejected as root cause. Changing Rust `NpSession::send()` non-chunked path to encode directly into the reusable send buffer did not improve Windows `np-ping-pong`; Rust-server rows became slightly worse (`c->rust 54683`, `rust->rust 53296`, `go->rust 45765`), so the remaining baseline Rust-server gap is not explained by the extra header staging/copy in the small-message send path.
- Working theory for the next narrow experiment: the remaining Windows baseline Rust-server gap may come from the Rust small-message named-pipe send path doing extra per-message header staging/copying compared to Go; test by making the Rust non-chunked send path encode directly into the reusable send buffer and rerun only Windows `np-ping-pong` max-rate rows.

- User request:
  - create this TODO and perform the analysis now
- No code changes are approved yet.
- Additional user request:
  - create a prompt file under `/tmp`
  - rerun the external reviewers against that file
  - make it explicit that the simple 8-byte ping-pong slowdown is the main mystery
  - make it explicit that the snapshot allocation issue is real but is not the root cause for the 8-byte ping-pong collapse
- User decision:
  - run the narrow experiment that changes the C Windows named-pipe server loop to use the existing readability-wait helper before `nipc_np_receive()`
  - validate only the Windows `np-ping-pong` benchmark slice first
- User decision:
  - the C Windows server ping-pong issue is considered solved enough to commit and push the wait-path fix now
  - remaining discrepancies to investigate next are the non-C gaps
  - correction:
    - the known allocation issue is in the C snapshot/server larger-message named-pipe send path, not in the simple ping-pong path and not currently established as a client-side issue
- User decision:
  - proceed to the next discrepancy
  - current target:
    - Windows Rust baseline / named-pipe server performance gap relative to the Go server

## Plan
- Next target: explain the remaining Windows baseline server gap between Rust and Go after the C server prewait fix, using code-path comparison and benchmark-role isolation.

1. Re-validate the role-isolation pattern directly from `benchmarks-windows.csv`.
2. Inspect Windows benchmark-driver paths for C, Rust, and Go to find configuration differences:
   - worker count
   - warmup behavior
   - stop / shutdown model
   - batching and request loop shape
3. Inspect Windows transport hot paths for:
   - C baseline server
   - Go SHM client
   - Go SHM server
   - Rust baseline server for comparison
4. Compare the code paths against Linux behavior to see whether the divergence is Windows-specific implementation logic.
5. Produce a fact-only diagnosis, then identify the smallest set of suspects worth instrumenting or fixing next.

## Implied decisions

- The analysis should stay benchmark-focused and should not rerun non-benchmark test suites.
- Any future implementation work should target parity of real implementations, not benchmark threshold relaxation.

## Testing requirements

- For this analysis phase:
  - no new test execution is required unless a code-path ambiguity cannot be resolved from source inspection
- For any later fix phase:
  - rerun the affected Windows benchmark slices first
  - then rerun the full Windows benchmark suite only after the suspect path is corrected

## Documentation updates required

- If the analysis proves a real implementation bug, update the fit-for-purpose validation TODO with:
  - root cause
  - evidence
  - benchmark impact
  - validation after the fix
