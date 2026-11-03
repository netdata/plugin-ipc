# SOW-0064 - Windows Named Pipe Pipelining Without Deadlock

## Status

Status: open

Sub-state: created from SOW-0062 decision 6A (2026-11-02); after SOW-0062.

## Requirements

### Purpose

A client that pipelines over the Windows named-pipe baseline must not deadlock when the bytes in flight exceed the pipe buffers, matching the POSIX guarantee from SOW-0062.

### User Request

2026-11-02, decision 6A: give Windows the same guarantee with overlapped I/O, in its own SOW.

### Assistant Understanding

Facts (SOW-0062 Analysis, "Windows named pipes"):

- C, Rust and Go create synchronous, message-mode pipes with 64 KiB buffers per direction (`netipc_named_pipe.c:58-63`, `:218-222`, `:986-990`; Rust `windows.rs:1164-1176`; Go `pipe_listener.go:268-276`); each packet is a blocking `WriteFile` with no timeout.
- With more than about 64 KiB in flight, client and server can both block in `WriteFile`.
- The Windows benchmark clients avoid it with a byte budget (`bench_windows.c:2007`, `:2052-2063`; Go and Rust Windows drivers mirror it).
- A synchronous pipe handle has no per-call non-blocking write; progress needs overlapped I/O (`FILE_FLAG_OVERLAPPED` on both ends) or `PIPE_NOWAIT` (kept by Microsoft only for LAN Manager compatibility).
- Netdata's Windows consumers use typed one-in-flight calls only.

Unknowns:

- The exact overlapped design per language (event per operation, cancellation, interaction with the existing `PeekNamedPipe` receive timeout), to be analyzed.

### Acceptance Criteria

- A Windows client pipelining beyond the pipe buffers completes in C, Rust and Go, with interop.
- One-in-flight calls keep their current behavior and performance (benchmark evidence).
- The interim Windows caller rule added by SOW-0062 is replaced by the real behavior in the docs.
- The Windows benchmark clients' byte budget is removed or justified.
- If responses read during a stalled send are queued in the session, the Windows session exposes the same has-pending check as POSIX (SOW-0062 decision 1: C `nipc_uds_session_has_pending`, Rust `UdsSession::has_pending`, Go `Session.HasPending`), and the handle accessor docs say to check it before waiting on the handle.

## Analysis

Pending.

## Pre-Implementation Gate

Status: blocked

Pending analysis.

## Implications And Decisions

- Mechanism (2026-11-02, user: "A"): overlapped I/O, own SOW, after SOW-0062.

## Plan

After analysis.

## Execution Log

### 2026-11-02

Created from SOW-0062.

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
