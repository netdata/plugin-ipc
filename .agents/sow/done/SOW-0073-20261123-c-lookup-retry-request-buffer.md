# SOW-0073 - C lookup retry resends an intact request

## Status

Status: completed

Sub-state: delivered as one commit; draft PR against netdata/plugin-ipc.

## Requirements

### Purpose

A C APPS_LOOKUP or CGROUPS_LOOKUP call that reconnects inside the call must resend the same request bytes, as
`docs/level2-typed-api.md` section 5 (at-least-once, "resend the request") requires.

### User Request

Fix the C client lookup retry that resends an overwritten request (found by a Netdata review of the vendored copy,
reproduced with a standalone program), with a separate request buffer, as a local change: no push, no PR.

### Assistant Understanding

Facts:

- The lookup clients encode each subcall into `ctx->send_buf` (`src/service/netipc_service_apps_lookup.c:233-240`,
  `netipc_service_cgroups_lookup.c:328-335`) and pass that pointer as the request of every attempt
  (`apps_lookup.c:66-78`, `:255`; `cgroups_lookup.c:350`).
- On an SHM session `nipc_service_common_client_prepare_shm_request` (`netipc_service_common.c:270-303`) moves the
  payload to `send_buf + 32` and writes the header at offset 0, i.e. it frames the request in place.
- `nipc_service_common_call_with_retry` (`netipc_service_common.c:425-470`) reconnects and runs the attempt again on
  an ordinary failure and in the overflow-resize loop. The retry then sends the framed bytes again as the payload:
  the server rejects it (`NIPC_ERR_BAD_LAYOUT`, handler not called).
- A reconnect that negotiates SHM or a larger request size resizes `send_buf`
  (`netipc_service_posix_client_connect.c:29-38`, `netipc_service_win_client_connect.c:33-41`); the retry then reads
  the request through the old pointer.
- Trigger: a server restart while the client stays up (fails fast since SOW-0044 dead-peer detection; before it,
  an SHM call timed out instead of retrying).
- The overflow-resize retry in the same function resends too. A response that does not fit is normally answered
  with `PAYLOAD_EXCEEDED` markers and split into further subcalls (`apps_lookup.c:275-328`), so a server with the
  default 64 KiB response size does not reach it while requests stay within a 4 KiB request cap (Netdata's
  configuration; a much larger request cap could make the markers alone exceed 64 KiB). It is reached when even the markers do not fit the server's
  response limit (`nipc_dispatch_apps_lookup`, `netipc_protocol_apps_lookup.c:738-790`, then `LIMIT_EXCEEDED` at
  `netipc_service_common.c:663-672`): e.g. 16 PIDs need about 1.2 KiB against a 1 KiB server limit. The client
  raises its own response size, reconnects and resends (`netipc_service_common.c:460-482`); on 1ff116f1dc the
  resend is the framed bytes, so the call ends in `NIPC_ERR_BAD_LAYOUT` instead of `NIPC_ERR_OVERFLOW`.
- Rust and Go keep a separate request buffer and SHM frame buffer (`src/crates/netipc/src/service/raw/client.rs:77-79`,
  `client_call.rs:192-273`; Go `service/raw/client_unix.go`, `client_windows.go`), so they are not affected.
- `cgroups_snapshot` encodes into a stack buffer inside each attempt (`netipc_service_cgroups_snapshot.c:41-56`):
  not affected.

Inferences:

- None beyond the facts; the design was cross-checked with Codex (agreement recorded under Execution Log).

Unknowns:

- None.

### Acceptance Criteria

- New C robustness tests (POSIX `test_robustness`, Windows `test_win_robustness`) restart an APPS_LOOKUP and a
  CGROUPS_LOOKUP server under a READY SHM client; the next call succeeds with exactly one reconnect and the replacement
  handler sees the exact items, for SHM->SHM and baseline-only->SHM restarts. A third case sends an APPS_LOOKUP
  whose response exceeds a fixed 1 KiB server limit: the server must see the same items twice and the call must end
  in `NIPC_ERR_OVERFLOW`. They fail on 1ff116f1dc and pass after, on Linux and Windows.
- Existing C suites pass (CTest) on Linux and the Windows service suites on MSYS; the ASAN suites, now including
  `test_robustness` (`tests/run-sanitizer-asan.sh`), report nothing.

## Analysis

Sources checked:

- The files and lines under Facts; `docs/level2-typed-api.md` sections 4-5; the fault-site tests
  `tests/fixtures/c/test_service.c:3821`, `:3870`, `test_service_extra.c:505`, `test_win_service_extra.c:3684`,
  `:3715`, `:3778`.

Current state:

- C lookups violate the resend contract on SHM sessions whenever the in-call retry runs.

Risks:

- `nipc_client_ctx_t` is a public struct (fields marked internal); appending fields changes its size. Consumers that
  vendor and rebuild together (Netdata) are unaffected; mixing headers and library builds is already unsupported.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The request and the SHM frame share one buffer: framing rewrites the request in place, and a reconnect can resize
  it, while the retry state keeps the request pointer. Evidence: Facts above; standalone reproduction on Linux
  (SHM restart -> `NIPC_ERR_BAD_LAYOUT`; baseline->SHM restart -> sanitizer report on the old pointer).

Evidence reviewed:

- Code paths under Facts, Rust/Go client buffers, Level 2 docs sections 4-5, existing fault-site tests.

Affected contracts and surfaces:

- `nipc_client_ctx_t` (new `request_buf`, `request_buf_size`), test fault-site enums (new
  `*_CLIENT_REQUEST_BUF_REALLOC`, appended), internal `nipc_service_platform_ensure_client_request_buffer`
  (renamed from `..._send_buffer`), C lookup clients, C SHM request framing. No wire or API behaviour change other than
  the retry now working.

Existing patterns to reuse:

- `nipc_service_posix_ensure_buffer` / `nipc_service_win_ensure_buffer` with fault sites; Rust `request_buf` /
  `send_buf` split; robustness test server helpers.

Risk and blast radius:

- Client-side only, C only. Extra memory: one lazily grown buffer per client, sized like the largest lookup
  subrequest.
- Performance assessment (Test Cadence): per request on an established SHM session, lookups now encode into
  `request_buf` and copy into `send_buf` (`netipc_service_common.c:296`), where 1ff116f1dc encoded into `send_buf`
  and moved the bytes 32 bytes up in place. The bytes copied are the same; the request now touches two buffers
  instead of one overlapping region, so the working set grows by one request (at most the 4 KiB request cap in
  Netdata's configuration). Baseline (UDS) sessions and `cgroups_snapshot` are unchanged. The effect is expected to
  be below benchmark noise. The benchmark matrix has no row for this path (`lookup-method-bench` times codec and
  dispatch in-process, `bench/drivers/c/bench_posix.c:1282-1410`, without a client or transport), so the user chose
  a short targeted A/B instead (Validation).

Sensitive data handling plan:

- No sensitive data: library code, synthetic PIDs and cgroup paths in tests.

Implementation plan:

1. Add `request_buf` to the client context, its ensure helper (POSIX, Windows) and fault site; free it on close.
2. Lookups encode into `request_buf`; `prepare_shm_request` copies it into `send_buf` with memcpy.
3. Retarget the lookup fault-site tests to the request site; keep the SHM frame fault tests on `send_buf`.
4. Add POSIX and Windows robustness reproduction tests (restart SHM->SHM and baseline->SHM; apps and cgroups) and
   run `test_robustness` in the ASAN script, so the baseline->SHM case catches a stale pointer in CI.

Validation plan:

- Red: new tests on 1ff116f1dc (Linux, plain and ASAN). Green: full CTest, `tests/run-sanitizer-asan.sh` on Linux.
  Windows suites: built and run on a Windows host if available, otherwise recorded as a gap.
- Performance: a short A/B of the C typed lookup client over an established SHM_FUTEX session, 1ff116f1dc vs the
  fix (user decision 2026-11-24; the benchmark matrix has no row for this path).
- Independent review before closing.

Artifact impact plan:

- AGENTS.md: unaffected (no workflow change).
- Runtime project skills: unaffected (vendoring workflow unchanged).
- Specs: `docs/level2-typed-api.md` already requires resending the request; no change.
- End-user/operator docs: unaffected (no public behaviour beyond the fix).
- End-user/operator skills: `docs/netipc-integrator-skill.md` does not describe client buffers; unaffected.
- SOW lifecycle: this SOW closes with the commit.

Open-source reference evidence:

- None: only this repository was checked.

Open decisions:

- None: the fix shape (separate request buffer) was the user's request.

## Implications And Decisions

1. User decision 2026-11-23: fix with a separate request buffer, keep the work local (no push, no PR).
2. User decision 2026-11-24: a short benchmark run instead of the full matrix; run as a targeted lookup-client A/B
   because no matrix row exercises the changed path.

## Plan

1. Library change (Implementation plan 1-2).
2. Tests (Implementation plan 3-4), red on 1ff116f1dc, green after.
3. Independent review, then close.

User decision 3 (2026-11-24): squash into one commit and open a draft PR against netdata/plugin-ipc.

## Execution Log

### 2026-11-23

- Codex agreed the root cause (including the overflow-retry path and Windows), the separate buffer over re-encoding,
  no other affected callers, the struct change for source-vendored consumers, and asked for Windows tests, a response
  overflow case and deterministic restart synchronization.
- Implemented: `request_buf` + `nipc_service_platform_ensure_client_request_buffer` (POSIX, Windows) + fault site
  `*_CLIENT_REQUEST_BUF_REALLOC` (appended to the public and internal enums); lookups encode into it;
  `prepare_shm_request` memcpy; `close_buffers` frees it. Lookup fault tests retargeted to the request site
  (`test_service.c`, `test_win_service_extra.c`); the SHM-connect fault test stays on `send_buf`.
- Tests: N12 in `test_robustness.c` and `test_win_robustness.c`; no sleeps (the restart is a stop + join, the
  replacement listens before the call returns from init, handler observations are read under a lock).
- Review round 1 (Codex READY, three SHOULD-FIX): added the fixed-server-limit APPS_LOOKUP case (the response
  growth variant tried first used the 64 KiB default and never overflowed; a CGROUPS_LOOKUP of 16 short paths fits
  1 KiB, and it shares the retry code); the new test fixtures now check thread creation and destroy the server on
  failure; the performance assessment above now names the two-buffer working set.
- `tests/run-sanitizer-asan.sh` now runs `test_robustness` (it ran clean under ASAN with leak detection).

## Validation

Acceptance criteria evidence:

- Red, 1ff116f1dc + tests only, Linux x86_64 gcc (Debian, glibc 2.41): all five N12 cases fail; the restarts
  return `NIPC_ERR_BAD_LAYOUT` (6) with one reconnect, the limit case returns `NIPC_ERR_BAD_LAYOUT` with the handler
  called once (89 passed, 14 failed, three runs); ASAN build: `heap-use-after-free` READ in `memmove` in
  `prepare_shm_request` at the baseline->SHM case.
- Red, Windows Server 2022, MSYS gcc 16.2: the SHM->SHM and limit cases fail every run; the baseline->SHM cases fail
  only when the reconnect's `realloc` moves `send_buf` (one run: all four restarts failed; a later run: the cgroups
  baseline->SHM case passed; the final fixtures failed 14 checks in each of three runs). That case reads the request through the old pointer, so a non-sanitizer build sees it
  only when the block moves; the Linux ASAN run (now part of `tests/run-sanitizer-asan.sh`) catches it every time.
- Green, final fixtures: N12 passes on Linux plain and ASAN (103 passed, 0 failed, three runs each) and on Windows
  (81 passed, 0 failed, three runs); the limit case returns `NIPC_ERR_OVERFLOW` (11) after exactly two reconnects
  with the server seeing the same 16 PIDs twice.

Tests or equivalent validation:

- Linux, on a2c4437 (the review fixes touch only the N12 fixtures, rerun above):
  `ctest --test-dir build -E '^test_uds$'` 31/31 passed (C, Rust, Go and interop suites).
  `test_uds` does not link on this host on main either: `uds_eintr_wrap.c` wraps `__sendmsg64` under
  `__USE_TIME_BITS64`, which glibc 2.41 defines on 64-bit time too, while only `__USE_TIME64_REDIRECTS` redirects
  `sendmsg`; unrelated to this change, tracked in SOW-0074.
- Linux ASAN+UBSAN with leak detection (`tests/run-sanitizer-asan.sh` flags, run per binary because of the same
  `test_uds` link failure): test_protocol, test_shm, test_service, test_service_extra, test_cache,
  test_multi_server, test_robustness exit 0, no sanitizer reports.
- Windows MSYS, with the review fixes: `test_win_service`, `test_win_service_extra`,
  `test_win_service_payload_limits`, `test_win_robustness`, `test_win_service_guards`,
  `test_win_service_guards_extra` 6/6 passed.

Performance evidence (2026-11-24, beast, idle, client and server pinned to 2 CPUs, `-O2`, 5 interleaved rounds of
4 s per row, calls/s median and range):

| Row | 1ff116f1dc | fix | delta |
|---|---|---|---|
| apps, 16 PIDs | 39812 (36818-43352) | 38523 (34142-41694) | -3.2% |
| apps, 256 PIDs | 9898 (8931-10475) | 10086 (9053-10300) | +1.9% |
| cgroups, 16 paths | 42510 (37495-47148) | 39429 (37295-44514) | -7.2% |
| cgroups, 256 paths | 9646 (8762-10141) | 9584 (9478-9806) | -0.6% |

- Run-to-run spread within one variant is 12-26%, and the medians move in both directions, so no difference is
  resolved at this length. A call costs 22-110 us, dominated by the futex round trip and the codec; the added work is
  one buffer of at most a few KiB in the working set, with the same bytes copied. A smaller effect than the noise
  would need the full benchmark cadence with longer runs.

Real-use evidence:

- Standalone apps-lookup program against the public API (server restart under a live client): failed on
  1ff116f1dc as above; the N12 tests reproduce the same sequence in the suite.

Reviewer findings:

- Codex (read-only, on a2c4437): READY; SHOULD-FIX 1, a response over a fixed server limit reaches the overflow
  retry: fixed (new limit case, red/green above); SHOULD-FIX 2, new fixtures ignored thread-creation failure:
  fixed; SHOULD-FIX 3, performance assessment should name the two-buffer working set: fixed in the gate, benchmark
  left to the user.
- Codex round 2 (on f7078f8): READY; SHOULD-FIX the limit case accepted `calls >= 2`, so a refused final reconnect
  could pass: fixed (4 server workers, assert exactly 2 reconnects and 2 handler calls); NIT qualify the 64 KiB claim
  with the request cap: fixed.
- claude-swarm GLM-5.3 lead (on a2c4437): READY; NIT unused `n` parameter (removed with the round-1 changes); NIT
  `memcpy` overlap would be undefined for a future aliasing caller: no change (internal function, every caller passes
  a separate buffer, the comment at the copy states it); NIT thread creation unchecked: fixed.
- claude-swarm Kimi K3 (on a2c4437): READY; NIT second server stop unchecked: fixed (checks added); NIT
  function-static path buffers: no change (single-threaded fixture, no defect).

Same-failure scan:

- `grep -rn 'send_buf' src/libnetdata/netipc`: after the change only the SHM frame paths use it
  (`prepare_shm_request`, client connect on POSIX and Windows, `close_buffers`). `do_raw_call` callers: lookups
  (now `request_buf`) and `cgroups_snapshot` (stack buffer, encoded per attempt). Rust and Go already split the
  buffers.

Sensitive data gate:

- No sensitive data: synthetic PIDs and paths; no hosts, credentials or customer data in code, tests or this SOW.

Artifact maintenance gate:

- `AGENTS.md`: no workflow change; the validation commands it lists are unchanged.
- Runtime project skills: `project-netdata-vendoring` unaffected (vendoring workflow unchanged).
- Specs: `docs/level2-typed-api.md` section 5 already requires resending the request; `.agents/sow/specs/` holds
  no decision this changes.
- End-user/operator docs: no public behavior change beyond the fix; no doc describes the client scratch buffers.
- End-user/operator skills: `docs/netipc-integrator-skill.md` does not describe client buffers; unaffected.
- SOW lifecycle: this SOW moves to `done/` as `completed` in the same commit as the work; SOW-0074 (pending)
  tracks the `test_uds` link failure found during validation.

Specs update:

- None needed: the public spec already states the contract the fix restores.

Project skills update:

- None needed.

End-user/operator docs update:

- None needed.

End-user/operator skills update:

- None needed.

Lessons:

- See Lessons Extracted.

Follow-up mapping:

- `test_uds` link failure on 64-bit glibc 2.41: tracked, SOW-0074 (pending).
- Forced `realloc` relocation for the baseline->SHM case without a sanitizer: rejected; the ASAN run of
  `test_robustness` catches it every time, and the SHM->SHM and limit cases fail deterministically without it.
- Full benchmark matrix: rejected for this change; the matrix has no row for the lookup client path and the
  targeted A/B showed no difference above noise.

## Outcome

C lookup clients resend the exact request when a call reconnects and retries, on POSIX and Windows, matching
Rust and Go. N12 reproduction tests cover server restarts (SHM->SHM, baseline->SHM) and a response over a fixed
server limit; `test_robustness` now runs under ASAN.

## Lessons Extracted

- A buffer that a lower layer rewrites in place cannot also be the input a retry replays: the retry state must own,
  or point to, storage nothing below it touches.
- A path judged unreachable from stock configuration can still be reached by a non-default configuration: the
  overflow retry is reached by a server with a small fixed response limit.
- A reproduction that depends on `realloc` moving a block is not deterministic without a sanitizer; keep the case
  under ASAN in CI rather than relying on plain runs.

## Followup

- SOW-0074 (pending): `test_uds` EINTR wrapper guard on 64-bit glibc 2.41.
