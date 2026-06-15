# SOW-0022 - Benchmark Language Deltas

## Status

Status: completed

Sub-state: completed on 2026-06-15. Root causes were isolated with targeted Windows diagnostics and POSIX profiling. The user accepted the recommended decisions `1A`, `2A`, `3A`, and `4A`, so implementation continues in SOW-0023.

## Requirements

### Purpose

Keep NetIPC performance evidence fit for Netdata-scale use. Large differences between C, Rust, and Go for the same benchmark scenario are strong signals that either one implementation is suboptimal or the benchmark is not measuring equivalent work.

The purpose of this SOW is to identify those cases with evidence before making design or implementation decisions.

### User Request

The user asked to create a new SOW for the benchmark anomalies and investigate them one by one. No fixes should be implemented yet. The output should support a decision on whether each anomaly should be fixed or treated as a benchmark-code artifact.

### Assistant Understanding

Facts:

- The latest benchmark CSVs show substantial same-scenario throughput deltas between languages.
- The largest anomaly is Windows `snapshot-shm @ max`, where Rust as the client is roughly 10x slower than C and Go across all server languages.
- Several POSIX and Windows batch/pipeline scenarios show Go server-side throughput materially below C and Rust.
- POSIX lookup method benchmarks show Go consistently behind C and Rust, especially for cgroups lookup.
- SOW-0021 is paused so this SOW can be the active investigation slot.
- No code implementation is approved in this SOW at creation time.

Inferences:

- Deltas that follow the client language across all server languages usually point to client-side implementation or benchmark-driver behavior.
- Deltas that follow the server language across all client languages usually point to server-side implementation or benchmark-driver behavior.
- Lookup-method benchmarks are local codec/dispatch benchmarks, so transport cannot explain their deltas.
- Batch/pipeline benchmarks may include transport, batching, runtime scheduling, allocation, and benchmark-driver effects; they need role-based matrix analysis before conclusion.

Resolved root causes:

- The Rust Windows `snapshot-shm` client delta is caused by the typed Rust Windows SHM receive-control wrapper, not by WinSHM itself, snapshot decode, the benchmark server, or the benchmark harness.
- The Go batch/pipeline server delta is caused by shared raw server batch dispatch and generic protocol batch helpers on the production server path.
- The POSIX Go lookup-method delta is caused by codec encode/dispatch/decode overhead: repeated directory, string, label, and checked-builder work in the Go lookup codec.

Remaining unknowns:

- Exact implementation shape for the fixes. This requires user decision before coding.
- Final post-fix performance deltas. These require implementation and rerunning the benchmark suite.

### Acceptance Criteria

- A ranked anomaly list is recorded from the latest POSIX and Windows CSVs.
- Each anomaly is investigated one by one with file/line evidence from benchmark drivers and relevant library paths.
- Each anomaly is classified as one of:
  - likely implementation issue worth fixing;
  - likely benchmark artifact worth fixing or excluding;
  - expected language/runtime overhead with no immediate action;
  - unresolved, with the specific missing evidence listed.
- No product code, benchmark code, or test code is changed before user decision.
- Recommended next decisions are numbered with options, pros, cons, implications, risks, and a recommendation.

## Analysis

Sources checked:

- `/tmp/plugin-ipc-posix-bench-latest.csv`
- `/tmp/plugin-ipc-windows-bench-latest.csv`
- `docs/level2-typed-api.md`
- `docs/level3-snapshot-api.md`
- `docs/code-organization.md`
- `tests/generate-benchmarks-posix.sh`
- `tests/generate-benchmarks-windows.sh`
- `bench/drivers/c/bench_posix.c`
- `bench/drivers/c/bench_windows.c`
- `bench/drivers/rust/src/main.rs`
- `bench/drivers/rust/src/bench_windows.rs`
- `bench/drivers/go/main.go`
- `bench/drivers/go/main_windows.go`

Current state:

- Latest pushed commit before this SOW: `fc24009`.
- This SOW intentionally changes only SOW artifacts. A tracked benchmark executable, `bench/drivers/go/go`, was already modified in the working tree during local diagnostics and is not part of this SOW.
- POSIX benchmark CSV has 345 measurement rows.
- Windows benchmark CSV has 201 measurement rows.
- Fixed-rate rows often hit their target by design and are less diagnostic than `target_rps=0` max-throughput rows unless a language fails to meet the cap.
- Recomputed max-throughput same-language anomalies:
  - Windows `snapshot-shm`: C `1.13M/s`, Rust `105.1k/s`, Go `816.3k/s`, `10.71x` delta.
  - POSIX `cgroups-lookup-mixed-16`: C `938.2k/s`, Rust `746.4k/s`, Go `454.7k/s`, `2.06x` delta.
  - POSIX `shm-batch-ping-pong`: C `50.15M item/s`, Rust `37.70M item/s`, Go `24.49M item/s`, `2.05x` delta.
  - Windows `snapshot-baseline`: C `133.7k/s`, Rust `69.2k/s`, Go `92.8k/s`, `1.93x` delta.
  - POSIX `uds-pipeline-batch-d16`: C `68.89M item/s`, Rust `62.75M item/s`, Go `36.68M item/s`, `1.88x` delta.
- Recomputed role-effect anomalies:
  - Windows `snapshot-shm` client average: C `1.10M/s`, Rust `104.5k/s`, Go `929.9k/s`; client-language delta `10.49x`.
  - POSIX `uds-pipeline-batch-d16` server average: C `70.56M item/s`, Rust `62.35M item/s`, Go `37.59M item/s`; server-language delta `1.88x`.
  - Windows `np-pipeline-batch-d16` server average: C `36.59M item/s`, Rust `34.43M item/s`, Go `21.54M item/s`; server-language delta `1.70x`.
  - POSIX `shm-batch-ping-pong` server average: C `42.76M item/s`, Rust `38.66M item/s`, Go `25.76M item/s`; server-language delta `1.66x`.

Risks:

- Treating benchmark artifacts as implementation problems can waste time and produce risky changes.
- Treating implementation bottlenecks as benchmark artifacts can hide production performance problems.
- Comparing languages without checking whether benchmark drivers perform equivalent work can produce false conclusions.
- Overreacting to Go/Rust runtime overhead can lead to complicated code that hurts maintainability without meaningful production benefit.

## Pre-Implementation Gate

Status: completed for investigation; implementation moves to SOW-0023 by user decision.

Problem / root-cause model:

- The benchmark matrix reports multiple cross-language throughput gaps for the same named scenario.
- Role-based matrix behavior can narrow likely ownership: client-following deltas point at client code or client benchmark harness; server-following deltas point at server code or server benchmark harness.
- The current question is not "how to fix" yet; it is "which deltas represent real suboptimal implementations versus benchmark artifacts."

Evidence reviewed:

- POSIX and Windows CSV summaries generated from the latest full benchmark runs.
- Existing docs define that Level 2/3 behavior should be equivalent across C, Rust, and Go.
- Benchmark generator scripts define scenario names, row requirements, and floors.
- Benchmark drivers define what each language measures for the same scenario.

Affected contracts and surfaces:

- Benchmark credibility and benchmark report interpretation.
- C/Rust/Go performance parity expectations.
- Potential future changes to benchmark drivers, transport/service implementations, or benchmark floor policy.
- No public API, wire format, or runtime contract is changed by this investigation.

Existing patterns to reuse:

- Use the full 3x3 client/server matrix when a scenario has transport interaction.
- Prefer max-throughput rows for anomaly detection.
- Treat benchmark code and production code separately.
- Preserve C/Rust/Go behavioral parity requirements from the docs.

Risk and blast radius:

- Investigation-only SOW has low runtime risk.
- Any future fix could touch Windows SHM snapshot client paths, Go server transport/service paths, or lookup codec/dispatch paths and would require separate validation before implementation.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write secrets, credentials, bearer tokens, private endpoints, customer data, personal data, or proprietary incident details into durable artifacts.
- Benchmark CSVs contain synthetic local measurements only; record summarized numeric evidence, not workstation-specific paths beyond temporary CSV references.

Implementation plan:

1. No implementation is approved in this SOW phase.
2. Investigate anomalies one by one and record evidence.
3. Present numbered user decisions before any code change.

Validation plan:

- Recompute ranked same-language and full-matrix deltas from the latest CSVs.
- Inspect benchmark-driver code for equivalence across C, Rust, and Go.
- Inspect the relevant production code path only enough to identify likely root-cause class.
- Run targeted diagnostic benchmarks or profilers only if needed for classification and without changing code.
- Record unresolved evidence gaps explicitly.

Artifact impact plan:

- AGENTS.md: no workflow change expected.
- Runtime project skills: no reusable workflow change expected unless this investigation discovers a durable benchmark-analysis rule.
- Specs: no protocol/API behavior change expected during analysis.
- End-user/operator docs: no public docs change expected during analysis.
- End-user/operator skills: no exported skill change expected during analysis.
- SOW lifecycle: SOW-0021 paused; SOW-0022 active analysis-only SOW.

Open-source reference evidence:

- External OSS reference evidence is not relevant at SOW creation because the question is about local benchmark deltas and local benchmark-driver equivalence.

Open decisions:

- None for this analysis SOW. The user accepted decisions `1A`, `2A`, `3A`, and `4A` on 2026-06-15. Implementation is tracked by SOW-0023.

## Implications And Decisions

User decisions recorded on 2026-06-15:

- Decision 1: A. Fix the Rust Windows SHM typed-client fast path while preserving timeout and abort semantics.
- Decision 2: A. Fix Go raw server batch hot paths with internal validated-batch and response-builder helpers.
- Decision 3: A. Fix Go lookup codec hot paths without changing public APIs.
- Decision 4: A. Keep this SOW as analysis-only and create a separate implementation SOW.

### Investigation 1 - Windows `snapshot-shm` Rust Client

Evidence:

- The anomaly follows the Rust client across every server language:
  - `rust->c`: `103.6k/s`
  - `rust->rust`: `105.1k/s`
  - `rust->go`: `104.6k/s`
  - C and Go clients to the same servers are around `816k/s` to `1.24M/s`.
- The Rust, C, and Go benchmark clients all use the typed Level 2 snapshot client, not a special benchmark-only snapshot fast path:
  - Rust: `bench/drivers/rust/src/bench_windows.rs:1026` creates `CgroupsClient`, and `bench/drivers/rust/src/bench_windows.rs:1060` calls `client.call_snapshot()`.
  - C: `bench/drivers/c/bench_windows.c:1353` initializes `nipc_client_ctx_t`, and `bench/drivers/c/bench_windows.c:1389` calls `nipc_client_call_cgroups_snapshot()`.
  - Go: `bench/drivers/go/main_windows.go:1741` creates `cgroups.NewClient`, and `bench/drivers/go/main_windows.go:1769` calls `client.CallSnapshot()`.
- Rust Windows SHM itself is not globally slow. The same benchmark CSV shows Rust Windows `shm-batch-ping-pong @ max` at `47.49M item/s` for `rust->rust`, so the raw WinSHM data plane can move data quickly in Rust.
- The slow path is specific to Windows typed snapshot over SHM. POSIX `snapshot-shm @ max` has Rust at `1.57M/s`, faster than C `1.39M/s` and Go `984k/s`.
- The Rust Windows typed raw call sends and receives through the active SHM transport in `src/crates/netipc/src/service/raw/client_call.rs:220` and `src/crates/netipc/src/service/raw/client_call.rs:303`.
- Rust Windows SHM receive loops in `src/crates/netipc/src/transport/win_shm.rs:739`.
- C Windows SHM has a performance warning about atomic reads in spin loops in `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:138`, while Rust uses `AtomicI32` and `AtomicI64` helpers in `src/crates/netipc/src/transport/win_shm.rs:1097` and `src/crates/netipc/src/transport/win_shm.rs:1115`.
- Rust snapshot facade maps `max_response_batch_items` from `max_request_batch_items` in `src/crates/netipc/src/service/cgroups_snapshot.rs:63` and `src/crates/netipc/src/service/cgroups_snapshot.rs:104`. This is probably not the root cause of this benchmark because snapshot uses one non-batch response, but it is a nearby consistency issue.

Classification:

- Real implementation issue worth fixing.
- Not a benchmark artifact.
- Root cause: Rust typed Windows SHM calls enter `receive_win_shm_with_control()` on every response, which performs per-call deadline and abort-poll bookkeeping before calling the already-fast WinSHM receive path.

Risk if ignored:

- Rust Windows snapshot consumers may be an order of magnitude slower than C/Go on the same transport.
- The benchmark report would hide a real per-call CPU waste pattern behind "language overhead", which is not supported by the POSIX Rust snapshot result.

Root-cause evidence:

- Reproduced on the same Rust server with targeted Windows rows:
  - C client: `1,243,659/s`.
  - Go client: `1,010,680/s`.
  - Rust typed client: `104,942/s`.
- Built a temporary diagnostic binary under `/tmp` from the authoritative local source copy. No repository source was changed.
- Direct Rust WinSHM snapshot phases against the same server:
  - raw send/receive only: `1,212,055/s`.
  - raw plus response header decode: `1,519,513/s`.
  - raw plus snapshot response decode: `1,522,825/s`.
  - normal typed `call_snapshot()`: `94,144/s`.
- Temporary-only bypass of the typed Windows receive-control wrapper raised normal typed `call_snapshot()` to `1,141,436/s`.
- Relevant source:
  - `bench/drivers/rust/src/bench_windows.rs:1060` calls `client.call_snapshot()`.
  - `src/crates/netipc/src/service/raw/client_call.rs:306` routes typed Windows SHM responses through `receive_win_shm_with_control()`.
  - `src/crates/netipc/src/service/raw/client_call.rs:414` through `src/crates/netipc/src/service/raw/client_call.rs:437` compute the deadline and wait slice using `Instant::now()` and abort checks.
  - `src/crates/netipc/src/service/raw/client_call.rs:470` through `src/crates/netipc/src/service/raw/client_call.rs:479` loop through that wrapper before calling `shm.receive()`.
  - `src/crates/netipc/src/transport/win_shm.rs:783` through `src/crates/netipc/src/transport/win_shm.rs:803` show the fast receive spin path that normally observes the response before kernel wait/deadline work is needed.

Diagnostic caveat:

- The Windows host checkout had source/target drift during diagnosis. A clean temp clone from that host's Git HEAD did not compile the Rust benchmark, while the existing benchmark runner reused prior target output. The root-cause measurements above were therefore rebuilt from the authoritative local source copy into a `/tmp` Windows diagnostic directory before testing.

### Investigation 2 - Go Server Batch/Pipeline Throughput

Evidence:

- The anomaly follows the Go server across client languages:
  - POSIX `uds-pipeline-batch-d16 @ max` server averages: C `70.56M item/s`, Rust `62.35M item/s`, Go `37.59M item/s`.
  - Windows `np-pipeline-batch-d16 @ max` server averages: C `36.59M item/s`, Rust `34.43M item/s`, Go `21.54M item/s`.
  - POSIX `shm-batch-ping-pong @ max` server averages: C `42.76M item/s`, Rust `38.66M item/s`, Go `25.76M item/s`.
- Server CPU is saturated when Go is the server:
  - POSIX `uds-pipeline-batch-d16` Go server rows show roughly `94%` to `95%` server CPU.
  - Windows `np-pipeline-batch-d16` Go server rows show roughly `90%` to `91%` server CPU.
  - This is not a rate limiter artifact.
- The Go benchmark uses the shared raw server path:
  - `bench/drivers/go/main.go:417` creates `rawsvc.NewServerWithWorkers`.
  - `bench/drivers/go/main.go:418` uses `protocol.MethodIncrement` and `pingPongDispatch()`.
  - `src/go/pkg/netipc/service/raw/server.go:234` dispatches both single and batch requests.
  - `src/go/pkg/netipc/service/raw/server.go:252` creates a `protocol.BatchBuilder` and `src/go/pkg/netipc/service/raw/server.go:254` loops per batch item.
- Go batch helpers do repeated checked arithmetic in the hot loop:
  - `src/go/pkg/netipc/protocol/frame.go:330` through `src/go/pkg/netipc/protocol/frame.go:384` for `BatchItemGet`.
  - `src/go/pkg/netipc/protocol/frame.go:432` through `src/go/pkg/netipc/protocol/frame.go:487` for `BatchBuilder.Add`.
- Rust and C perform the same logical validation, but their hot helper code is visibly leaner:
  - Rust `batch_item_get`: `src/crates/netipc/src/protocol/mod.rs:369`.
  - Rust `BatchBuilder.add`: `src/crates/netipc/src/protocol/mod.rs:435`.
  - C `nipc_batch_item_get`: `src/libnetdata/netipc/src/protocol/netipc_protocol.c:200`.
  - C `nipc_batch_builder_add`: `src/libnetdata/netipc/src/protocol/netipc_protocol.c:245`.

Classification:

- Real Go implementation overhead in shared raw server batch dispatch and protocol batch helpers.
- Not benchmark-only, because the benchmark uses the production raw Go server path.
- Root cause: the Go server validates and walks batch payloads with generic checked helpers per item, then rebuilds the response batch with another generic checked helper per item. This is correct but expensive in the hot path.

Risk if ignored:

- Go Level 2 batch servers stay materially slower than C/Rust for high-throughput batch calls.
- As more services are added, this repeated batch-dispatch overhead may affect production paths, not just benchmarks.

Root-cause evidence:

- Focused POSIX profile: `uds-pipeline-batch-d16,go,go` measured `35,327,058 item/s`; server CPU was the bottleneck.
- `perf record` on the Go server captured 6041 samples with zero lost samples.
- Top server hot symbols:
  - `protocol.(*BatchBuilder).Add`: `22.32%`.
  - `protocol.BatchItemGet`: `15.77%`.
  - `service/raw.(*Server).dispatchServerResponse`: `8.40%`.
  - benchmark increment handler: `6.87%`.
  - `service/raw.(*Server).dispatchSingle`: `4.96%`.
  - `protocol.BatchDirValidate`: `4.58%`.
  - `runtime.memmove`: `3.89%`.
- Relevant source:
  - `src/go/pkg/netipc/service/raw/server.go:252` resets a `BatchBuilder` for each batch response.
  - `src/go/pkg/netipc/service/raw/server.go:254` through `src/go/pkg/netipc/service/raw/server.go:268` loops through every request item, calls `BatchItemGet()`, dispatches one item, and calls `BatchBuilder.Add()`.
  - `src/go/pkg/netipc/protocol/frame.go:330` through `src/go/pkg/netipc/protocol/frame.go:384` implement `BatchItemGet()` with repeated checked arithmetic and bounds validation.
  - `src/go/pkg/netipc/protocol/frame.go:432` through `src/go/pkg/netipc/protocol/frame.go:487` implement `BatchBuilder.Add()` with repeated checked arithmetic, conversion, copy, and directory writes.
  - `src/go/pkg/netipc/transport/internal/framing/receive.go:127` validates inbound batch payloads before raw server dispatch, so part of the per-item server walk is revalidating already-validated structure.

### Investigation 3 - POSIX Lookup Method Go Deltas

Evidence:

- Lookup-method benchmarks are codec/dispatch-only and do not use transport:
  - Go benchmark section: `bench/drivers/go/main.go:1274`.
  - Rust benchmark section: `bench/drivers/rust/src/main.rs:1377`.
  - C benchmark section: `bench/drivers/c/bench_posix.c:1096`.
- Go lookup benchmark is CPU-bound, not rate-limited:
  - `cgroups-lookup-mixed-16`: C `938.2k/s`, Rust `746.4k/s`, Go `454.7k/s`, all around `99%` client CPU.
  - `cgroups-lookup-mixed-256`: C `66.6k/s`, Rust `51.4k/s`, Go `35.7k/s`, all around `99%` client CPU.
  - `apps-lookup-mixed-16`: C `1.03M/s`, Rust `848.1k/s`, Go `589.4k/s`, all around `99%` client CPU.
- Go escape analysis does not show the benchmark label literals escaping in the measured lookup loop:
  - `bench/drivers/go/main.go:1394` label literal does not escape.
  - `bench/drivers/go/main.go:1431` label literal does not escape.
- Go cgroups lookup builder performs repeated semantic validation and checked conversions per item:
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:519` starts the add path.
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:527` validates semantics.
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:548` through `src/go/pkg/netipc/protocol/cgroups_lookup.go:611` checks and converts offsets and lengths.
- Go apps lookup builder has the same pattern:
  - `src/go/pkg/netipc/protocol/apps_lookup.go:599` starts the add path.
  - `src/go/pkg/netipc/protocol/apps_lookup.go:607` validates semantics.
  - `src/go/pkg/netipc/protocol/apps_lookup.go:640` through `src/go/pkg/netipc/protocol/apps_lookup.go:700` checks and converts offsets and lengths.

Classification:

- Real Go codec implementation overhead plus normal Go runtime cost.
- Not a benchmark artifact.
- Root cause: cgroups lookup benchmarks exercise pure codec encode/dispatch/decode loops, and the Go implementation spends CPU in repeated directory/string/label validation and checked builder arithmetic.

Risk if ignored:

- Go lookup paths may be significantly slower at the exact large item counts the project cares about.
- The current benchmark indicates Go is `1.6x` to `2.1x` slower on several lookup variants, including cgroups mixed workloads.

Root-cause evidence:

- Focused POSIX profile: `cgroups-lookup-mixed-16,go,go` measured `448,973/s` at `98.8%` CPU.
- `perf record` captured 6768 samples with zero lost samples.
- Top codec hot symbols:
  - `protocol.lookupStringInto`: `10.12%`.
  - `protocol.(*CgroupsLookupBuilder).add`: `9.40%`.
  - `protocol.lookupPayloadSlice`: `6.96%`.
  - `protocol.writeLookupLabels`: `5.64%`.
  - `protocol.lookupDirEntry`: `5.55%`.
  - `protocol.validateLookupDir`: `5.33%`.
  - `protocol.(*CgroupsLookupBuilder).addUnknown`: `4.86%`.
  - `indexbytebody`: `4.77%`, from NUL scanning.
  - `protocol.EncodeCgroupsLookupRequest`: `4.30%`.
  - `protocol.DecodeCgroupsLookupRequest`: `3.53%`.
  - `protocol.validateLabels`: `3.03%`.
- Relevant source:
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:126` through `src/go/pkg/netipc/protocol/cgroups_lookup.go:164` decode and validate every request directory item and path string.
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:506` through `src/go/pkg/netipc/protocol/cgroups_lookup.go:516` call `req.Item()` for already-decoded request items before appending responses.
  - `src/go/pkg/netipc/protocol/cgroups_lookup.go:519` through `src/go/pkg/netipc/protocol/cgroups_lookup.go:635` build each response item with repeated semantic validation, checked arithmetic, string copy, and label write logic.
  - `src/go/pkg/netipc/protocol/lookup_common.go:262` through `src/go/pkg/netipc/protocol/lookup_common.go:325` validate lookup directories.
  - `src/go/pkg/netipc/protocol/lookup_common.go:341` through `src/go/pkg/netipc/protocol/lookup_common.go:362` validate and construct string views.
  - `src/go/pkg/netipc/protocol/lookup_common.go:388` through `src/go/pkg/netipc/protocol/lookup_common.go:420` validate labels.
  - `src/go/pkg/netipc/protocol/lookup_common.go:505` through `src/go/pkg/netipc/protocol/lookup_common.go:535` write labels.

### Decisions Required

1. Windows Rust `snapshot-shm` anomaly

- A. Design and implement a Rust Windows SHM typed-client receive fast path that preserves timeout and abort semantics without per-call `Instant` deadline work on the already-ready fast path.
  - Pros: directly addresses the measured `10x` anomaly; diagnostic bypass proved typed throughput can return to the `1.1M/s+` range.
  - Cons: requires careful timeout/abort semantics review and Windows validation.
  - Implications: likely touches `src/crates/netipc/src/service/raw/client_call.rs`; may also need tests around abort/timeout behavior.
  - Risks: incorrect fast-path gating could delay abort or timeout handling when the peer is slow or absent.
- B. Treat as benchmark artifact for now.
  - Pros: no implementation risk.
  - Cons: evidence directly rejects this; direct WinSHM and snapshot decode are fast, while typed receive-control is slow.
  - Implications: benchmark report remains misleading and production Rust Windows typed SHM users stay slow.
  - Risks: production Rust Windows snapshot users remain slow.
- Recommendation: A, long-term-best.

2. Go batch/pipeline server anomaly

- A. Design and implement internal Go validated-batch and response-builder fast paths for the shared raw server batch loop.
  - Pros: addresses repeated Go server bottleneck across POSIX and Windows.
  - Cons: requires preserving defensive validation while reducing hot-path overhead.
  - Implications: likely introduces internal helpers used after transport receive has already validated batch structure; public API should not change.
  - Risks: overly aggressive optimization could weaken malformed-batch detection.
- B. Accept as expected Go overhead.
  - Pros: avoids risk and keeps the code simple.
  - Cons: profile evidence shows a fixable production-code hot path, not just runtime overhead.
  - Implications: Go services stay materially slower in batch-heavy workloads.
  - Risks: future Go services inherit this bottleneck.
- Recommendation: A, long-term-best.

3. Go lookup-method anomaly

- A. Design and implement Go lookup codec hot-path improvements that avoid repeated validation/work once a request item or directory has already been validated.
  - Pros: directly targets Level 2 lookup scale work and large item-count usage.
  - Cons: codec builders are correctness-sensitive and already carry overflow/oversized-item logic.
  - Implications: likely changes should be internal helpers, not public API changes.
  - Risks: hoisting checks incorrectly could weaken edge-case guarantees from SOW-0021.
- B. Accept as expected Go overhead.
  - Pros: no implementation risk.
  - Cons: profile evidence shows repeated codec work in project code, not a pure benchmark artifact.
  - Implications: Go lookup consumers may lag C/Rust at HPC-scale item counts.
  - Risks: scale-safe API works functionally but underperforms.
- Recommendation: A, long-term-best.

4. SOW execution model

- A. Keep SOW-0022 as analysis-only, then create or update a separate implementation SOW after user decisions.
  - Pros: preserves the user's explicit "do not implement yet" constraint and keeps decisions clean.
  - Cons: one extra SOW transition before fixes.
  - Implications: no code changes happen until decisions are recorded.
  - Risks: minimal.
- B. Convert SOW-0022 into implementation after decisions.
  - Pros: keeps all anomaly work in one file.
  - Cons: mixes investigation and implementation lifecycle.
  - Implications: larger SOW with multiple unrelated fix surfaces.
  - Risks: easier to lose focus or batch unrelated fixes.
- Recommendation: A, surgical for process control.

## Plan

1. Build a ranked anomaly table from latest POSIX and Windows CSVs.
2. Investigate Windows `snapshot-shm` Rust-client anomaly.
3. Investigate Go server batch/pipeline anomalies across POSIX and Windows.
4. Investigate POSIX lookup-method Go deltas.
5. Separate benchmark-driver artifacts from likely production implementation debt.
6. Present decisions before any code changes.

## Execution Log

### 2026-06-15

- Created this analysis-only SOW.
- Paused SOW-0021 to keep one active SOW slot.
- Recomputed latest POSIX and Windows benchmark deltas from the CSV files.
- Investigated the three strongest anomaly groups with benchmark-driver and library file evidence.
- Recorded user decisions required before any implementation.
- Ran `.agents/sow/audit.sh`; it passed.
- Ran `git diff --check`; it passed.
- User decision: continue analysis inside this SOW until the root causes are definitive enough to choose fixes based on facts; still no implementation approved.
- Reproduced the Windows Rust `snapshot-shm` anomaly with targeted rows against a single Rust server.
- Built temporary diagnostic Rust benchmark variants under `/tmp` and proved direct WinSHM snapshot, header decode, and snapshot decode are fast.
- Used a temporary-only Rust patch to bypass the typed Windows receive-control wrapper and proved the typed client returns to `1.1M/s+`.
- Built the Go benchmark driver to `/tmp/netipc-bench-go` and profiled the Go batch server with `perf record`.
- Profiled the Go `cgroups-lookup-mixed-16` codec-only benchmark with `perf record`.

## Validation

Acceptance criteria evidence:

- Ranked anomaly list recorded in `## Analysis`.
- File/line evidence recorded for:
  - Windows Rust `snapshot-shm` client anomaly.
  - Go batch/pipeline server anomaly.
  - Go lookup-method anomaly.
- Each investigated anomaly has a classification and risk statement.
- No product code, benchmark code, or test code was intentionally changed for this SOW.
- Numbered decisions are recorded in `### Decisions Required`.

Tests or equivalent validation:

- `.agents/sow/audit.sh` passed.
- `git diff --check` passed.
- Targeted Windows Rust diagnostic benchmark was run on `win11` from a `/tmp` source copy and did not change repository source.
- POSIX Go batch server profile was captured with `perf record` into `/tmp/netipc-go-server.perf`.
- POSIX Go lookup-method profile was captured with `perf record` into `/tmp/netipc-go-lookup-cgroups16.perf`.
- Full runtime test execution is not required until a fix SOW is approved because this SOW did not change implementation.

Real-use evidence:

- Latest full benchmark CSV output identified the anomaly groups.
- Targeted benchmark/profiling runs reproduced the key anomalies and isolated root causes.

Reviewer findings:

- No external reviewer run was requested for this analysis-only SOW.

Same-failure scan:

- The anomaly scan grouped rows by scenario, target, client language, and server language to distinguish same-language deltas from client-following and server-following role effects.

Sensitive data gate:

- No `.env` or secret-bearing files were read.
- The SOW records synthetic benchmark measurements and source file references only.

Artifact maintenance gate:

- AGENTS.md: no change needed; this SOW does not change workflow rules.
- Runtime project skills: no change needed; no reusable workflow rule changed yet.
- Specs: no change needed; no protocol, API, wire-format, or behavior change was made.
- End-user/operator docs: no change needed; this is internal benchmark analysis.
- End-user/operator skills: no change needed; no public/operator workflow changed.
- SOW lifecycle: SOW-0021 was paused and SOW-0022 is the active analysis SOW.

Specs update:

- Not needed for analysis-only work. Specs may need updates only if a later implementation changes benchmark policy, performance guarantees, or internal API behavior.

Project skills update:

- Not needed. No durable "how to work here" rule changed.

End-user/operator docs update:

- Not needed. No user-facing behavior changed.

End-user/operator skills update:

- Not needed. No exported operator workflow changed.

Lessons:

- Role-based benchmark matrices are more diagnostic than same-language rows alone: the strongest anomaly follows the Rust Windows client, while the Go batch anomaly follows the Go server.

Follow-up mapping:

- Follow-up requires user decisions in `### Decisions Required`; no implementation follow-up is approved yet.

## Outcome

Completed. The SOW isolated the root causes behind the strongest benchmark language deltas and recorded the user-approved implementation direction.

## Lessons Extracted

- Role-based benchmark matrices identify ownership faster than same-language rows alone.
- Temporary diagnostic binaries are useful when proving whether a hot path is transport, codec, service wrapper, or benchmark harness.
- Before accepting language-runtime overhead as unavoidable, profile the concrete hot path and compare against direct lower-layer measurements.

## Followup

- Implementation is tracked in `.agents/sow/current/SOW-0023-20260615-benchmark-hot-path-fixes.md`.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
