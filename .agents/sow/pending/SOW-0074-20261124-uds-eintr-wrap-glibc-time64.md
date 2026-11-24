# SOW-0074 - test_uds EINTR wrapper on 64-bit glibc 2.41

## Status

Status: open

Sub-state: found during SOW-0073 validation; not started.

## Requirements

### Purpose

`test_uds` must build on every glibc the project supports.

### User Request

Tracked from SOW-0073 (found while validating on Debian 13, glibc 2.41, x86_64).

### Assistant Understanding

Facts:

- `tests/fixtures/c/uds_eintr_wrap.c` defines `__wrap___sendmsg64` and calls `__real___sendmsg64` under
  `#if defined(__GLIBC__) && defined(__USE_TIME_BITS64)`; `CMakeLists.txt` links `test_uds` with
  `--wrap=__sendmsg64` on every non-Apple platform.
- glibc 2.41 (`bits/features-time64.h`) defines `__USE_TIME_BITS64` whenever `__TIMESIZE == 64`, i.e. on every
  64-bit target. `sys/socket.h` redirects `sendmsg` to `__sendmsg64` only under `__USE_TIME64_REDIRECTS` (32-bit
  `time_t` targets built with `_TIME_BITS=64`).
- So on x86_64 glibc 2.41 the wrapper references `__sendmsg64`, which libc does not export:
  `undefined reference to '__sendmsg64'`, and `test_uds` does not link (reproduced on 1ff116f1dc).

Inferences:

- The guard should follow the header's own condition (`__USE_TIME64_REDIRECTS`, falling back to `__TIMESIZE == 32`
  with `__USE_TIME_BITS64` for glibc versions that predate that macro).

Unknowns:

- Which glibc version started defining `__USE_TIME_BITS64` on 64-bit targets (CI's Ubuntu glibc still links).

### Acceptance Criteria

- `test_uds` links and passes on x86_64 glibc 2.41 and on the 32-bit time64 targets of `tests/run-linux-target.sh`.

## Analysis

Sources checked:

- `tests/fixtures/c/uds_eintr_wrap.c`, `CMakeLists.txt` (`test_uds` link options), glibc 2.41 headers on Debian 13.

Current state:

- Test-only build failure; library code unaffected.

Risks:

- None for production; the EINTR injection tests do not run on affected hosts until fixed.

## Pre-Implementation Gate

Status: blocked

Not started: fill the gate before implementation.

## Implications And Decisions

None yet.

## Plan

1. Fix the guard; validate on glibc 2.41 x86_64 and a 32-bit time64 target.

## Execution Log

None yet.

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
