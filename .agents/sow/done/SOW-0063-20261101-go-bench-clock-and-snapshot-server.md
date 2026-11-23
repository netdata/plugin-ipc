# SOW-0063 - Go Benchmark Clock Overhead, SHM Clock And Snapshot Server

## Status

Status: completed

Sub-state: delivered 2026-11-03.

## Requirements

### Purpose

Language implementations should not show dramatic performance differences. Benchmark rows must measure NetIPC, not the harness, and every remaining gap must be explained or fixed.

### User Request

2026-11-01: "this is a strong indication that one of the implementation is slow. Check the pairs that produced the low value. What is common to them? ... generally, the languages should not have so dramatic differences." Decisions: "1a 2a 3a 4a"; "ok, proceed" (test lock-file cleanup in this SOW).

### Assistant Understanding

Facts (SOW-0059 FreeBSD benchmark run, FreeBSD 14.3 VM, `kern.timecounter.hardware: kvmclock`):

- SHM ping-pong, max rate: Go clients 526k-643k req/s against every server; C and Rust clients 1.45M-1.81M. Go servers are fast (C client -> Go server 1.65M). On Linux (`benchmarks-posix.csv`) Go clients are about 0.7x of C (2.15M-2.27M vs 2.43M-3.33M).
- Go's FreeBSD runtime reads the clock in user space only for the TSC and HPET timecounters (`runtime/vdso_freebsd_x86.go:81-88`); with `kvmclock` every `nanotime`/`walltime` falls back to a syscall. Measured on the VM: Go `time.Now()` 310 ns, C `clock_gettime(CLOCK_MONOTONIC)` 41 ns.
- The Go benchmark client reads the clock 5 times per request (`bench/drivers/go/main.go`: loop `time.Since(wallStart)`, `t0 := time.Now()`, `t1 := time.Now()`; each `time.Now` reads wall and monotonic). The C client reads 3 times (`now_ns`).
- CPU profile of the Go SHM ping-pong client against a C server (pprof, 6 s): `time.Now` 36.6% + `time.Since` 10.3% cumulative (all in `fallback_nanotime`/`fallback_walltime`); wake syscall 14.8%; wait 11.2%; spin 12.8%; library `monotonicNow` 0.9%.
- The library's Go SHM `monotonicNow` (`src/go/pkg/netipc/transport/posix/shm_posix.go:670-672`) uses a raw `SYS_CLOCK_GETTIME` syscall, which bypasses the Linux vDSO; it runs in the wait phase when a timeout is set.
- Snapshot SHM, max rate: Go servers are slower than C and Rust servers on both FreeBSD (C client -> Go server 412k vs C -> C 727k) and Linux (936k vs 1.26M). Not profiled yet.
- Test run directories accumulate endpoint `.lock` files: the library never deletes them (by design, `docs/level1-posix-uds.md`, "Endpoint lock"), and tests that use per-PID unique service names leave one per run (about 25k leftover files found under `/tmp/nipc_*` on the workstation, 2026-11-01).

Inferences:

- Without the harness clock cost the Go SHM client would land near C and Rust on the FreeBSD VM (speculation until re-measured).

Unknowns:

- The cause of the Go snapshot-server gap (to be profiled).

### Acceptance Criteria

- The Go benchmark harness reads the clock no more often than the C harness, monotonic-only, per request.
- The library's Go SHM wait uses Go's monotonic clock (vDSO on Linux) instead of the raw syscall, with the same timeout semantics.
- The Go snapshot-server gap is profiled and its cause fixed or explained with evidence.
- Test fixtures stop accumulating endpoint lock files.
- Re-measured rows (FreeBSD VM and Linux) show the effect; remaining language gaps are explained.

## Analysis

Harness clock (decision 1a):

- The C client reads `CLOCK_MONOTONIC` 3 times per request (`bench/drivers/c/bench_posix.c:63-68` `now_ns`; loop check, `t0`, `t1`, e.g. `:577`, `:613`, `:709`). The Go client calls `time.Since(wallStart)` for the loop check plus `time.Now()` twice (`bench/drivers/go/main.go:1000`, `:1015`, `:1065`, and the same pattern in every client loop); each `time.Now` reads the wall and the monotonic clock.
- `time.Since(t)` for a `t` carrying a monotonic reading reads only the monotonic clock (`runtimeNano`, Go 1.27 `src/time/time.go:1227-1233`). A process-start base plus `time.Since(base)` gives a monotonic-only nanosecond clock without `linkname`.
- Plan: one `nowNs()` helper (monotonic-only) for every Go client loop, the latency recorder and the rate limiter; the loop deadline check reuses the request's end timestamp, so a request costs 2 clock reads (C: 3).

Library clock (decision 2a):

- `monotonicNow` (`src/go/pkg/netipc/transport/posix/shm_posix.go:668-672`) issues a raw `SYS_CLOCK_GETTIME`, used at `:504` and `:527` (futex-wait phase with a timeout). Both uses only need elapsed nanoseconds to compute a relative futex/umtx timeout (Linux `FUTEX_WAIT` and FreeBSD `_umtx_op` with a relative `_umtx_time`, `shm_umtx_freebsd.go:64`). Plan: `time.Since(base)` (vDSO on Linux), same deadline arithmetic.

Go snapshot server gap (decision 4a), Linux workstation, C `snapshot-shm-client` against a Go or C `snapshot-shm-server`, low priority:

- Throughput with a Go server varied 190k-605k req/s between runs; with a C server 880k-1.0M.
- CPU profile of the Go server (pprof, 7 s): futex wake syscall 34%, `CgroupsBuilder.Add` 19% (16 items per response), `ShmReceive` 14%, `memmove` 10%, GC 7%.
- Allocation profile: about 2.25 heap allocations per request. `protocol.NewCgroupsBuilder` returns a heap builder (`protocol/cgroups_snapshot.go:364`); the session loop's `respHdr` escapes because its address goes to the send function (`service/raw/server.go:245`); `DecodeCgroupsRequest`'s result escapes into the handler (`service/raw/cgroups_snapshot.go:60`). That is 41 GC cycles in 5 s, each costing about 25 ms of CPU across the 112 Ps (gctrace).
- Context switches over 5 s: with the Go server the C client sleeps 91k-135k times (it waits in futex when the server stalls, which makes the server's next wake a real one); with a C server 52k.
- Experiment (scratch copy, not committed): `respHdr` hoisted out of the loop and the snapshot request plus builder reused from a pool. Go server 810k req/s in both runs, client sleeps 2.8k-4.2k. The remaining gap to C (about 0.85x) matches the Go SHM ping-pong server gap on Linux (`benchmarks-posix.csv`: C client -> Go server 2.63M vs C 3.14M), so it is the shared transport path (Go syscall entry/exit, bounds-checked atomics), not snapshot-specific.
- Same class elsewhere (escape analysis, `go build -gcflags=-m`): Go lookup servers allocate their request view and builder per request (`protocol/cgroups_lookup.go:188`, `:517`, `protocol/apps_lookup.go:206`, `:581`) and, for cgroups lookups, a suffix-size slice of item count + 1 entries (`protocol/cgroups_lookup.go:790`); Go typed clients allocate the request header per call (`service/raw/client.go:312`), and snapshot/string-reverse/lookup clients return a heap view by API design (`service/raw/cgroups_snapshot.go:41`, `string_reverse.go:32`). C and Rust build on the stack.

Lock files (user approved fixing here, 2026-11-01):

- About 3,000 `.lock` files built up under `/tmp/nipc_*` within a day; names carry a PID and a counter (Go `gotest_<pid>_<n>`, `go_rob_*`; Rust `rs_*_<n>_<pid>`, `cache_<pid>_*`; C `svc_*_<n>_<pid>` in `nipc_service_extra`, `nipc_service_limits`). Tests remove the socket but never the lock, which the library keeps by design (`docs/level1-posix-uds.md`, "Endpoint lock").

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Go benchmark clients spend a large share of each request reading the wall and monotonic clocks (5 reads per request; syscalls on the FreeBSD VM's `kvmclock`), so their rows measure the harness.
- The Go SHM wait reads the clock through a raw syscall instead of the vDSO.
- Go typed servers allocate per request; the resulting GC cycles stall the server, the client falls asleep in futex, and every response then pays a real wake (Analysis).
- Tests leave one endpoint lock file per run under `/tmp/nipc_*`.

Evidence reviewed: Analysis (profiles, escape analysis, context-switch counts, the scratch experiment), `bench/drivers/go/main.go`, `bench/drivers/c/bench_posix.c`, `src/go/pkg/netipc/transport/posix/shm_posix.go`, `src/go/pkg/netipc/service/raw/*.go`, `src/go/pkg/netipc/protocol/{cgroups_snapshot,cgroups_lookup,apps_lookup,lookup_common}.go`, the test helpers that create services in C (`tests/fixtures/c/`), Rust (`src/crates/netipc/src/**/tests*.rs`) and Go (`*_test.go`).

Affected contracts and surfaces:

- Go benchmark driver (POSIX) timing: rows stay comparable; the CSV format is unchanged.
- Go library internals: SHM wait clock, typed server dispatch and session loop, typed client request header, protocol builders/decoders gain internal reset/in-place forms. No exported API is removed or changed; any new exported helper is additive and documented. Correction (2026-11-07, SOW-0070): decision 6C later changed exported Go types on purpose (`protocol.StringReverseView.Str`, `protocol.DispatchStringReverse`, `raw.StringReverseHandler` take borrowed `[]byte`); see Implications And Decisions.
- Wire format, C and Rust code, Windows Go driver (already uses a QPC `nowNS`): unchanged, except test helpers in C and Rust that create endpoints (lock cleanup).

Existing patterns to reuse: client scratch buffers (`ensureClientScratch`), the session loop's per-session buffers, `testing.AllocsPerRun`, existing socket cleanup helpers in each language's tests.

Risk and blast radius:

- Pooled scratch must never be shared by two in-flight requests: each dispatch takes its own entry and returns it after the response is copied out; handlers must not keep the request or builder after returning (already the documented contract: the builder writes into the caller's response buffer).
- Builder reset must clear every field the old allocation zeroed (error state, counts, payload-exceeded settings).
- Removing a lock file in test cleanup only after the listener closes; test names are unique per test, so no other process holds them.

Sensitive data handling plan:

- Paths, line numbers, function names, counts and benchmark numbers only; hosts by role; no workstation paths in durable artifacts.

Implementation plan:

1. Go benchmark driver: monotonic-only `nowNs()`; every POSIX client loop, the rate limiter and the latency recorder use it; the deadline check reuses the request end time.
2. `monotonicNow` replaced by a monotonic elapsed-nanoseconds helper based on `time.Since`.
3. Go server: session loop response header per session; typed dispatchers take request and builder scratch from a `sync.Pool`; protocol gains internal-use reset/decode-into forms so the pooled values are reused; cgroups lookup's suffix slice comes from the scratch. Typed clients keep the request header in the client.
4. Allocation tests: one `testing.AllocsPerRun` test per typed server method (dispatch level) and per typed client call path that can be zero-alloc, asserting 0.
5. Lock files: each language's test endpoint cleanup removes `{service}.lock` after the listener is closed.
6. Re-measure (purpose of this SOW): the affected rows on Linux and the FreeBSD VM: Go client and server SHM/UDS ping-pong and snapshot rows, compared with SOW-0059's FreeBSD run and `benchmarks-posix.csv`.

Validation plan:

- Allocation tests fail before the change (recorded) and pass after.
- Linux: `go test ./...`, `go vet` (linux, freebsd, darwin, windows), gofmt; CTest and `cargo test` for the lock cleanup changes; interop suites (service, cache, SHM); 9 Netdata Linux targets (Go 386/arm included).
- FreeBSD VM: full suite (C, Rust, Go including `GOARCH=386`, interop). macOS host: full suite (the Go library changes touch shared POSIX code).
- Windows: `GOOS=windows go vet`, and the Windows Go suite on the Windows test host, since the typed service layer is shared.
- Benchmarks: the rows in plan step 6.

Artifact impact plan: `docs/` only if a documented behavior changes (the handler contract on retaining the request/builder is checked and stated if missing); benchmark report regenerated if rows are re-measured into `benchmarks-posix.csv`; integrator skill if the handler contract text changes.

Performance assessment: this SOW changes established-session per-request code (Go server dispatch and session loop, Go client call, Go SHM wait clock); its purpose is performance, so it runs the benchmark rows it needs (plan step 6).

Open decisions: none (1a, 2a, 3a, 4a, 5A, 6C, 7A, 8A recorded).

## Implications And Decisions

- Harness clock (2026-11-01, user: "1a"): fix the Go benchmark harness to read the clock like C (monotonic-only, deadline checked less often), then re-measure FreeBSD and Linux.
- Library clock (2026-11-01, user: "2a"): switch `monotonicNow` to Go's monotonic clock in this SOW.
- Snapshot server (2026-11-01, user: "4a"): profile the Go snapshot-server gap inside this SOW.
- Lock-file accumulation in tests (2026-11-01, user: "ok, proceed" after the proposal to fix it here): fixed in this SOW.
- Ordering: after SOW-0062.
- Go per-request allocations (2026-11-03, user: "a"): option A. Remove steady-state per-request heap allocations from every Go typed server (snapshot, cgroups lookup, apps lookup, increment, string-reverse) and the server session loop, and the request-header allocation from Go typed clients, with no public API change; add Go tests that fail when a request allocates. Rejected: B, snapshot server and session loop only (leaves the same defect in the lookup servers); C, explain only. Client calls that return a decoded view by pointer keep that one allocation (an API change, to be raised separately if wanted).
- String-reverse server copy (2026-11-03, user: "6a", revised the same day to "6C" after the assistant found the spec conflict): option C. `docs/codec.md:50-55` requires that the default decode path does not allocate and that string fields are exposed as pointer + length into the payload; Go's `StringReverseDecode` copied into a `string` (`protocol/string_reverse.go:80`), while C (`const char *` + length) and Rust (`str_data: &[u8]`) borrow. Go `StringReverseView.Str` becomes a borrowed `[]byte`, and the Go STRING_REVERSE handlers (`raw.StringReverseHandler`, `protocol.DispatchStringReverse`) take the request as a borrowed `[]byte`; the response stays a `string`. A public Go API break limited to the test method (`docs/level1-wire-envelope.md:77`). Rejected: A, keep the copy and add a spec exception (Go out of parity with C and Rust); B, an `unsafe.String` alias of the receive buffer (a retained string would change under the handler, breaking Go's string immutability).
- Go client clock reads (2026-11-03, user: "7a"): option A. Go typed clients read the clock about 8 times per call, half of them wall-clock reads (`service/raw/client.go:48`, `:62`, `client_unix.go:239`, `:247`, `:253`), against 4 monotonic reads in C (`netipc_service_common.c:202`, `:209`, `netipc_service_posix_client_call.c:96`, `:102`); on the FreeBSD VM's `kvmclock` each is a syscall and they take about 40% of the Go snapshot client's CPU. Fix in this SOW: per-call deadlines in monotonic-only nanoseconds and one clock read per receive-loop iteration, in the POSIX and Windows Go clients and transports, with no API change; re-measure the snapshot rows. Rejected: B, explain and track separately.
- Go client returned views (2026-11-03, user: "8A"): option A. `CallSnapshot` and `CallStringReverse` moved the decoded view to the heap on every call (`result = &view`, `service/raw/cgroups_snapshot.go:47`, `string_reverse.go:35`). The documented lifetime is already "valid until the next typed call on this client" (`cgroups_snapshot.go:21`, `string_reverse.go:12`), so the view is kept in the client and its address returned: no API change. This corrects the note under the allocation decision above, which called it an API change. Rejected: B, leave the allocation. The Go lookup clients' per-call buffers for multi-part responses are a separate redesign, tracked in a pending SOW.

## Plan

The gate's implementation plan, in order.

## Execution Log

### 2026-11-01

Created from the SOW-0059 FreeBSD benchmark analysis.

### 2026-11-03

- Harness clock: `bench/drivers/go/main.go` reads a monotonic-only `nowNs()` (`time.Since` of a process-start base); every client loop reuses the request's end timestamp for the deadline check (`loopNow`), so a request costs 2 clock reads; the rate limiter works in the same nanoseconds. `main_windows.go` already used a QPC clock.
- Library clock: `monotonicNow` (raw `SYS_CLOCK_GETTIME`) replaced by `monotonicNs()` over `time.Since` (`transport/posix/shm_posix.go`); same deadline arithmetic.
- Allocation tests written first (`service/raw/alloc_test.go`, dispatch level, 1 request vs 2000 requests on one session): before the fix the Go server allocated 1 (increment), 2 (snapshot), 4 (cgroups lookup) and 3 (apps lookup) times per request. `alloc_unix_test.go` (`TestCallNoAllocsBaseline`, a real Go server and client over UDS, `testing.AllocsPerRun`): 9 allocations per increment call.
- Server fixes: response header per session (`service/raw/server.go`); snapshot request and builder from a `sync.Pool` (`service/raw/cgroups_snapshot.go`, `protocol.CgroupsBuilder.Reset`); lookup request views, builders and the cgroups payload-exceeded suffix slice from pools (`protocol/dispatch_scratch.go`, decode-into and `reset` forms in `protocol/cgroups_lookup.go`, `protocol/apps_lookup.go`, `reusePayloadExceededSuffixBytes` in `protocol/lookup_common.go`).
- Client and transport fixes: the request header lives in the client (`client_unix.go`, `client_windows.go`, `client.go`, `increment.go`); the snapshot request buffer uses the client scratch. The shared framing receiver took a method value per receive; it now takes a disconnect predicate and an in-flight reset built once per session (`transport/internal/framing/receive.go`, POSIX `uds_receive.go`/`uds_session.go`, Windows `pipe_receive.go`/`pipe_session.go`). `rawRecv` uses `read(2)` instead of `syscall.Recvmsg`, which allocated the source address (`transport/posix/uds.go`).
- All allocation tests pass after the fixes (UDS and SHM; Windows variants in `alloc_windows_test.go`). A constant +1 allocation seen in the session test is the runtime starting an OS thread (not per request); the test tolerates fewer than 1 per 100 requests.
- Harness allocations: escape analysis of the Go driver showed the request header escaping inside every measured client loop (`main.go`, 4 loops; `main_windows.go`, 4 loops); it is now declared once per run. The remaining escapes are once per run or in warmup.
- Lock files: each language's test helpers record the endpoints they create and remove `{dir}/{service}.lock` at process exit (Go `TestMain` in `transport/posix`, `service/raw`, `service/cgroups_snapshot`; Rust `test_locks.rs` via `atexit` with a PID guard; C `tests/fixtures/c/test_lock_cleanup.h` via `atexit` with a PID guard). After full Go, Rust and C runs no new lock files remained.
- String-reverse server: `StringReverseDecode` copies the request into a Go `string` (`protocol/string_reverse.go:80`) because the handler type takes a `string`; removing it changes the handler contract (decision 6).
- Re-measured after the harness and server/client fixes, then profiled the Go snapshot client on the FreeBSD VM: about 8 clock reads per call, half of them wall-clock reads, took about 40% of its CPU (decision 7).
- Client clock (decision 7A): new package `internal/monotime` (`Now`/`After`, int64 nanoseconds of `time.Since` a process-start base; zero means "no deadline"). Per-call deadlines in `service/raw/client.go` (`beginOp`, `endOp`, `opRemainingMs`, `doRawCallWithTimeout`), the SHM receive loops in `client_unix.go`/`client_windows.go` (one read per wait slice, as in C), lookup deadlines (`service/raw/lookup_common.go`), the POSIX and Windows transport receive waits (`transport/posix/uds_receive.go`, `transport/windows/pipe_receive.go`, `pipe_session.go`) and the SHM wait clock (`transport/posix/shm_posix.go`, `monotonicNs`) use it. Go now reads the clock 4 times per call, like C. The connect-time SHM attach loop and the SHM generation stamp (`shm_posix.go`, wall clock by design) are unchanged.
- Windows allocation tests failed on the Windows host: `TestWinCallNoAllocsBaseline` 1035 allocations per call and `TestWinCallNoAllocsShm` 3, from `LazyProc.Call` (variadic, `//go:uintptrescapes`) in the Named Pipe poll loop (`PeekNamedPipe`) and the SHM signal/wait (`SetEvent`, `WaitForSingleObject`). Those three now call `syscall.SyscallN(proc.Addr(), ...)`, which keeps arguments on the stack (`transport/windows/pipe.go`, `shm.go`); the tests pass.
- The allocation tests left `go_alloc_*.lock` files on the FreeBSD VM; `assertCallsDoNotAllocate` now tracks its endpoints for the exit-time cleanup.
- Decision 6 revisited (6C): `protocol.StringReverseView.Str` is a borrowed `[]byte` (`protocol/string_reverse.go`), `protocol.DispatchStringReverse` and `raw.StringReverseHandler` take the request as a borrowed `[]byte`; tests updated (`protocol/codec_edge_test.go`, `service/raw/ping_pong_test.go`, `ping_pong_windows_test.go`, `helpers_windows_test.go`, `shm_windows_test.go`, `lookup_common_test.go`). No Go fixture, benchmark driver or doc used the old shape.
- Decision 8A: `CallSnapshot` and `CallStringReverse` store the decoded view in the client (`snapshotView`, `stringReverseView` in `client_unix.go`/`client_windows.go`) and return its address; escape analysis no longer reports `moved to heap: view`. The client allocation tests now cover increment, snapshot and string-reverse calls (`service/raw/alloc_calls_test.go`, shared cases with non-allocating handlers; UDS, POSIX SHM, Named Pipe, Windows SHM). Red check: with the old copy-to-heap return restored in `CallSnapshot`, `TestCallNoAllocsBaseline/snapshot` failed with 1 allocation per call; restored, it passes.
- Lookup clients in all three languages copy every response item and stitch into a new buffer per call; tracked in `SOW-0065-20261103-lookup-client-reusable-stitch-storage.md` (pending).
- Independent review (a separate assistant reviewer over the Go diff, read-only, with build, vet and race runs) found no correctness bug in the library change. Findings and handling:
  1. The server allocation tests failed under `-race` (`sync.Pool` drops items on purpose there): fixed, the allocation tests skip under the race detector (`service/raw/race_enabled_test.go`, `race_disabled_test.go`).
  2. `testing.AllocsPerRun` floors the per-call average, so an allocation on only some calls passed: fixed, the client tests count total allocations over 2000 calls (`callAllocs`, `alloc_calls_test.go`) and fail at 20 or more. Red check: an allocation on 1 call in 4 in `CallSnapshot` now fails with "500 allocations in 2000 calls" (it reported 0 before); restored.
  3. The `service/cgroups_snapshot` wrapper did not repeat the view lifetime: added (`client_common.go`).
  4. The benchmark loop's deadline check reuses the previous request's end time and can overshoot by one rate-limiter interval: rejected, the reported rate divides by a freshly read elapsed time, so an overshoot of at most one interval does not bias it.
  5. The pooled cgroups-lookup scratch keeps its largest suffix table until GC clears the pool: rejected, bounded by the configured maximum item count.
  6. A stale `CLOCK_MONOTONIC` mention and the now-unused `shmClockMonotonic` constant in `transport/posix/shm_futex_linux.go`: fixed (the FreeBSD constant stays, it selects the `_umtx_op` timeout clock).

## Validation

Acceptance criteria evidence:

- Harness clock: the Go POSIX client loops read a monotonic-only `nowNs()` twice per request (C: 3), `bench/drivers/go/main.go`; the request header no longer escapes in the measured loops (escape analysis).
- Library clock: the SHM wait and every per-call deadline use `internal/monotime` (`time.Since` of a process-start base; vDSO on Linux); Go typed clients read the clock 4 times per call, like C.
- Snapshot server gap: profiled (Analysis); caused by per-request allocations and the resulting GC stalls; fixed (server allocation tests pass at 0 per request). The remaining Linux gap, C client to Go server about 0.85x of C, matches the SHM ping-pong server gap and is the shared transport path.
- Lock files: each language's test helpers remove the endpoint lock files they create; after full Go, Rust and C runs on Linux, FreeBSD and macOS no new lock files remained.
- Re-measured rows (5 s per row, max rate, all 9 pairs; client to server; noise about 20% on these hosts):

| Row | Linux before | Linux after | FreeBSD VM before | FreeBSD VM after |
|---|---|---|---|---|
| shm-ping-pong go to c | 904k | 1.50M | 562k | 1.06M |
| shm-ping-pong go to go | 636k | 1.10M-1.54M | 573k | 944k |
| shm-ping-pong c to go | 852k | 1.41M | 1.55M | 1.49M |
| shm-ping-pong c to c | 1.48M | 1.71M | 1.81M | 1.66M |
| snapshot-shm go to c | 715k | 898k | 232k | 418k |
| snapshot-shm go to go | 416k | 533k-639k | 235k | 361k |
| snapshot-shm c to go | 213k | 827k | 485k | 524k |
| snapshot-shm c to c | 844k | 977k | 743k | 699k |
| snapshot-baseline go to go | 30k | 52k | 69k | 73k |

  (Ranges: three extra repeats of the Go-to-Go rows, unchanged code.)

- Remaining gap, explained: on the FreeBSD VM Go clients reach about 0.55-0.65x of C. The Go client now reads the clock as often as C, but each read is a syscall: Go's FreeBSD runtime has a user-space clock only for the TSC and HPET timecounters (`runtime/vdso_freebsd_x86.go:81-88`) and the VM uses `kvmclock`; profile of the Go snapshot client against a C server after decision 7A: `runtime.fallback_nanotime` 32.6% of CPU. This is a Go runtime limitation on that timecounter; the library cannot remove it without reading the clock less often than C. On Linux Go clients are 0.8-0.9x of C.

Tests or equivalent validation (final tree):

- Linux: build, CTest 32/32, `interop_codec`, UDS, SHM, service, service SHM, cache, cache SHM and lookup-scale interop; `cargo test` 399 on stable and 1.91.0; clippy (Linux and Windows target); `cargo fmt --check` on both toolchains; `go test ./...` amd64 and `GOARCH=386`; `go vet` linux, linux/386, freebsd, darwin, windows; gofmt clean. `GOOS=windows go vet` reports 9 `possible misuse of unsafe.Pointer` warnings in `transport/windows/shm.go` and `shm_test.go`; the same 9 are reported at HEAD (`48cc98b`), not introduced here.
- Netdata Linux targets (Go and interop parts; C and Rust unchanged since the full run earlier in this SOW): x86_64-musl, i386-gnu, i386-musl, armv7-gnu, armv7-gnu-time64, armv7-musl, armv6-musl, aarch64-gnu, aarch64-musl, all rc=0.
- FreeBSD VM: CTest 31/31, all 8 interop suites, `cargo test` 397, `go test ./...` and `GOARCH=386 go test ./...`.
- macOS host: CTest 24/24, `cargo test`, `go test ./...`, UDS, service, cache, lookup-scale and codec interop.
- Windows host: Rust 273, Go suite (including the allocation tests on Named Pipe and Windows SHM), CTest 20/20.
- After the review fixes (test files, one comment and one constant): `go test ./...` on Linux, FreeBSD (amd64 and 386), macOS and Windows, `go test -race` of the allocation tests on Linux, vet on all targets.

Real-use evidence:

- The benchmark rows above run the real C, Rust and Go benchmark binaries across all 9 language pairs over UDS and SHM on Linux and the FreeBSD VM.

Reviewer findings:

- Independent assistant review of the Go diff: six findings, three fixed, one fixed as a doc gap, two rejected with reasons (Execution Log, 2026-11-03).

Same-failure scan:

- Escape analysis (`go build -gcflags=-m`) of `service/raw`: no remaining per-call `moved to heap`; the remaining escapes are per session (`server.go` `msgBuf`, `respHdr`) or in the lookup clients (SOW-0065).
- `LazyProc.Call` on Windows per-request paths: none left in `transport/windows` receive, send or SHM signalling (connect-time calls keep it).
- Wall-clock deadline reads (`time.Now`) in per-call Go paths: none left; the connect-time SHM attach loop and the SHM generation stamp keep `time.Now` by design.
- Client calls returning a view by pointer: snapshot and string-reverse fixed; lookups tracked in SOW-0065. The same per-item lookup copies exist in Rust and C and are covered by SOW-0065.
- The view-lifetime doc in Go wrappers: `service/cgroups_snapshot` fixed; the lookup wrappers carry no doc comments and their behavior did not change here.

Sensitive data gate:

- Durable artifacts carry file paths relative to the repository, line numbers, function names, counts and benchmark numbers only; hosts are named by role (Linux workstation, FreeBSD VM, macOS host, Windows host); no credentials, personal data or workstation paths.

Artifact maintenance gate:

- AGENTS.md: no update; workflow, commands and guardrails did not change.
- Runtime project skills: no update; `project-netdata-vendoring` workflow is unaffected (vendoring of these changes goes through SOW-0050).
- Specs: no update needed. `docs/codec.md:50-58` (non-allocating decode, strings as pointer + length) and `docs/level2-typed-api.md:440-462` (borrowed views valid until the next typed call) already describe the new Go behavior; the change brings Go into line with them.
- End-user/operator docs: no update; the only public API change is the STRING_REVERSE test method's Go types, which no doc, example or fixture shows (`grep` of `docs/`, `tests/`, `bench/`).
- End-user/operator skills: `docs/netipc-integrator-skill.md` unaffected; it names string-reverse as a non-production method (`:112`) and documents no Go signature for it.
- SOW lifecycle: `Status: completed`, moved to `.agents/sow/done/` in the same commit as the work; follow-up SOW-0065 created in `pending/`.

Specs update:

- None; see the artifact gate (docs already match).

Project skills update:

- None; see the artifact gate.

End-user/operator docs update:

- None; see the artifact gate.

End-user/operator skills update:

- None; see the artifact gate.

Lessons:

- See Lessons Extracted.

Follow-up mapping:

- Lookup client per-item allocations (C, Rust, Go): tracked, SOW-0065.
- FreeBSD Go client clock cost on `kvmclock`: rejected as a library change (Go runtime limitation, evidence above).
- Benchmark loop overshoot and pooled suffix table size (review findings 4 and 5): rejected with reasons in the Execution Log.
- Consolidated benchmark run and report regeneration: part of the agreed consolidated benchmark step before SOW-0050 vendoring; these rows are SOW evidence, not a `benchmarks-posix.csv` update.

## Outcome

Go benchmark rows now measure NetIPC instead of the harness clock. Go typed servers and the increment, snapshot and string-reverse clients do not allocate per request on an established session (UDS, POSIX SHM, Named Pipe, Windows SHM), Go per-call deadlines read only the monotonic clock as often as C, Go STRING_REVERSE decoding borrows the payload as the codec spec requires, and test runs no longer leave endpoint lock files behind. Go clients rose from about 0.4-0.6x to 0.8-0.9x of C on Linux and from about 0.3x to 0.55-0.65x on the FreeBSD VM, where the rest is the Go runtime's clock syscall on `kvmclock`.

## Lessons Extracted

- Check the authoritative spec before presenting a design option: decision 6 was first offered without `docs/codec.md:50-55`, and option A would have kept a spec violation.
- `testing.AllocsPerRun` floors the per-call average; allocation tests that must catch occasional allocations need total counts.
- `sync.Pool` drops items under the race detector; pool-based allocation tests must skip there.
- On Windows, `LazyProc.Call` allocates per call (variadic, `//go:uintptrescapes`); per-request syscalls use `syscall.SyscallN(proc.Addr(), ...)`.
- On FreeBSD guests the Go runtime clock is a syscall unless the timecounter is TSC or HPET; benchmark Go numbers there include it.

## Followup

- SOW-0065 (pending): lookup clients stitch into reusable client storage (C, Rust, Go).

## Regression Log

None yet.
