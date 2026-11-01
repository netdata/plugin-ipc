# SOW-0062 - Pipeline+Batch Benchmark Deadlock On Small Socket Buffers

## Status

Status: open

Sub-state: found by the SOW-0059 FreeBSD benchmark run (2026-11-01); needs a user decision on the fix.

## Requirements

### Purpose

Every benchmark row must run on every POSIX platform that runs the benchmark suite, so FreeBSD (and macOS) benchmark runs are complete evidence.

### User Request

Standing rules: zero known bugs; a Netdata-supported configuration that cannot be tested is a blocker. SOW-0059 decision 10A asked for a FreeBSD benchmark run.

### Assistant Understanding

Facts:

- On the FreeBSD 14.3 test VM, `tests/run-posix-bench.sh` row `uds-pipeline-batch` (depth 16) fails for all 9 C/Rust/Go pairs; every other row completes, including `uds-pipeline-d16` (single-item pipelining).
- Reproduction (C client, C batch server): the client blocks for the server's whole lifetime (34 s with a 30 s server) and then fails with `EPIPE` when the server exits. Rust and Go clients fail the same way.
- `truss` of the server: it receives request 1 (8192 + 1440 bytes), sends the first 8192-byte chunk of its response, then blocks sending the rest. The client is still sending requests 2..16 and reads nothing, so both sides block in send: a write-write deadlock.
- The client sends `depth` batches of 2-1000 random items before reading any response (`bench/drivers/c/bench_posix.c` `uds-pipeline-batch-client`, Rust `bench/drivers/rust/src/main.rs` `run_pipeline_batch_client`, Go `bench/drivers/go/main.go` `runPipelineBatchClient`).
- FreeBSD's default SEQPACKET `SO_SNDBUF` is 8192 bytes (SOW-0048); Linux's is about 208 KiB, which holds the bytes in flight, so the row passes there.
- The spec already tells callers about this on macOS (`docs/level1-posix-uds.md`, macOS stream baseline, "Socket buffers": read responses while sending, or keep the bytes in flight small). FreeBSD has no such note.
- No session send timeout exists (`SO_SNDTIMEO` is set only while connecting, `netipc_uds_lifecycle.c`), so the blocking is unbounded until a peer exits.

Inferences:

- The library behaves as specified (blocking sends, ordinary socket backpressure); the defect is the benchmark client's send-everything-then-read pattern on small-buffer platforms.
- macOS (8 KiB stream buffers) most likely deadlocks the same way; not yet run there.

Unknowns:

- None that investigation cannot resolve; the fix direction is a user decision.

### Acceptance Criteria

- `uds-pipeline-batch` completes for all 9 pairs on Linux, FreeBSD and macOS.
- The FreeBSD UDS docs carry the same pipelining/buffer note as macOS.

## Analysis

Fix options (to present to the user):

1. Benchmark clients read responses while sending (send one, drain whatever responses are ready, keep at most `depth` in flight). Same scenario on every platform; Linux numbers may shift, so the Linux floor for this row needs re-evidence.
2. Benchmark clients cap the bytes in flight at what the socket buffers hold (reduce the effective depth on small-buffer platforms, record the effective depth in the row). Linux unchanged; FreeBSD/macOS rows measure a smaller depth.
3. Library: raise session `SO_SNDBUF`/`SO_RCVBUF` on FreeBSD/macOS toward Linux defaults, in C, Rust and Go. On FreeBSD this also raises the SEQPACKET packet size (fewer chunks). Moves the threshold; does not remove the caller constraint; affects packet-size negotiation and needs cross-language interop validation.

## Pre-Implementation Gate

Status: blocked

Blocked on the user's choice among the options above.

## Implications And Decisions

- None yet.

## Plan

After the decision.

## Execution Log

### 2026-11-01

Created from the SOW-0059 FreeBSD benchmark run.

## Validation

Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
