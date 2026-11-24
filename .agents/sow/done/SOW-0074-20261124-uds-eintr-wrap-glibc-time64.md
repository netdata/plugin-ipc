# SOW-0074 - test_uds EINTR wrapper on 64-bit glibc 2.40+

## Status

Status: completed

Sub-state: guard fixed and validated on x86_64 glibc 2.41, i386 legacy and i386 time64; closes with the fix commit.

## Requirements

### Purpose

`test_uds` must build on every glibc the project supports, so CI on `main` and open PRs is green again.

### User Request

Tracked from SOW-0073 (found while validating on Debian 13, glibc 2.41, x86_64). The user asked for it as a separate small PR ahead of plugin-ipc#19, which is blocked by the same CI failure.

### Assistant Understanding

Facts:

- `tests/fixtures/c/uds_eintr_wrap.c` defined `__wrap___sendmsg64` and called `__real___sendmsg64` under `#if defined(__GLIBC__) && defined(__USE_TIME_BITS64)`; `CMakeLists.txt` links `test_uds` with `--wrap=__sendmsg64` on every non-Apple platform. The wrapper came from SOW-0052 (armv7-gnu-time64 fix).
- glibc 2.40 (and 2.39 builds that carry the stable-branch backport) `features-time64.h` defines `__USE_TIME_BITS64` whenever `__TIMESIZE == 64`, i.e. on every 64-bit target, and adds `__USE_TIME64_REDIRECTS` for `__USE_TIME_BITS64 && __TIMESIZE == 32`. `sys/socket.h` redirects `sendmsg` to `__sendmsg64` only under `__USE_TIME64_REDIRECTS`.
- The upstream glibc 2.34-2.39 release tarballs defined `__USE_TIME_BITS64` only for 32-bit targets built with `_TIME_BITS=64`, which is why CI passed until its runners moved to ubuntu-26.04.
- On x86_64 glibc 2.41 the wrapper references `__sendmsg64`, which a 64-bit libc does not export: `undefined reference to '__sendmsg64'`, `test_uds` does not link (reproduced on 1ff116f). CI on `main` (1ff116f) fails the same way in Runtime Safety (ASAN/UBSAN, TSAN, Valgrind, aarch64-gnu), CodeQL (Analyze C/C++ POSIX) and Codacy Coverage; Static Analysis builds library targets only and passes.

Inferences:

- None beyond the facts; Codex confirmed the root cause and the version boundary independently.

Unknowns:

- None.

### Acceptance Criteria

- `test_uds` links and passes on x86_64 glibc 2.41 (beast).
- On a glibc ABI with a default 32-bit `time_t` (i386, armv7) built with `_TIME_BITS=64`, the library still references `__sendmsg64`, the wrapper still intercepts it, and the EINTR injection checks pass.
- Legacy 32-bit `time_t` glibc builds are unaffected.

## Analysis

Sources checked:

- `tests/fixtures/c/uds_eintr_wrap.c`, `CMakeLists.txt` (`test_uds` link options), `tests/fixtures/c/test_uds.c` (injection checks), `tests/run-linux-target.sh`.
- glibc headers: Debian 13 glibc 2.41 and Ubuntu glibc 2.43 `features-time64.h`, `sys/socket.h`; upstream glibc release history for the `__USE_TIME64_REDIRECTS` split (2.40, backported to the 2.39 branch).

Current state:

- Test-only build failure; library code unaffected.

Risks:

- None for production. A wrong guard could silently stop intercepting `__sendmsg64` on 32-bit time64 targets; the `every injected EINTR was hit` check in `test_uds` catches that.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The fixture guards the `__sendmsg64` wrapper on `__USE_TIME_BITS64`, which glibc 2.40+ also defines on 64-bit targets where no redirect happens and libc exports no `__sendmsg64`. The guard must follow the header's own redirect condition.

Evidence reviewed:

- Header text above; red link failure on beast at 1ff116f; CI logs on `main` with the same undefined reference; Codex read-only review of the headers and fixture (agreed root cause, guard and edge cases).

Affected contracts and surfaces:

- Test fixture `tests/fixtures/c/uds_eintr_wrap.c` only. No library, wire, API, spec or doc change.

Existing patterns to reuse:

- The `fallocate`/`fallocate64` wrap pair (`CMakeLists.txt`, `tests/fixtures/c/shm_fallocate_wrap.c`): both names are wrapped unconditionally at link time, and the alternate-name wrapper is compiled only on the libc that provides that name.

Risk and blast radius:

- Test build only. Edge cases checked with Codex: x86_64 and x32 (`__TIMESIZE == 64`, guard off), legacy i386/armv7 without `_TIME_BITS` (off), i386/armv7 with `_TIME_BITS=64` (on), musl (no `__GLIBC__`, off), glibc before 2.34 (no `__USE_TIME_BITS64`, off). `__TIMESIZE` is visible because `sys/socket.h` includes `features.h`/`features-time64.h` first. `--wrap=__sendmsg64` with no references is a no-op, so CMake stays as is.

Performance assessment:

- No changed code runs per request on an established session: the only code change is a preprocessor guard in the test fixture `tests/fixtures/c/uds_eintr_wrap.c:172`, linked only into `test_uds` (`CMakeLists.txt:324-328`). No benchmark run (AGENTS.md Test Cadence: tests are outside the measured paths).

Sensitive data handling plan:

- Evidence is glibc header text, symbol names and test counts; no credentials, hosts or customer data enter the SOW, code comment or PR.

Implementation plan:

1. Change the guard to `defined(__GLIBC__) && defined(__USE_TIME_BITS64) && __TIMESIZE == 32` (the condition glibc uses for `__USE_TIME64_REDIRECTS`, valid on headers that predate that macro) and say why in the comment.

Validation plan:

- Red: `test_uds` link at 1ff116f on x86_64 glibc 2.41. Green: x86_64 full CTest; `tests/run-linux-target.sh i386-gnu c`; an i386 glibc build with `-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64`, checking `nm` for the `__sendmsg64` reference and the wrapper, then CTest. armv7-gnu-time64 runs in CI (beast has no ARM cross compiler).

Artifact impact plan:

- AGENTS.md: unaffected; no workflow change.
- Runtime project skills: unaffected; vendoring copies library sources, not test fixtures.
- Specs: unaffected; test fixture only.
- End-user/operator docs: unaffected; no API or behavior change.
- End-user/operator skills: unaffected; `docs/netipc-integrator-skill.md` does not cover test fixtures.
- SOW lifecycle: this SOW closes as `completed` in `done/` in the fix commit; the pending copy in plugin-ipc#19 is dropped when that PR rebases.

Open-source reference evidence:

- glibc is consulted through installed system headers and its public release history, not a local mirror: `bminor/glibc @ glibc-2.39` `sysdeps/unix/sysv/linux/features-time64.h` (only 32-bit opt-in defines `__USE_TIME_BITS64`); the 2.40 change adding `__USE_TIME64_REDIRECTS` and its 2.39-branch backport.

Open decisions:

- None; the user asked for a separate small PR.

## Implications And Decisions

1. Separate PR ahead of plugin-ipc#19 (user decision, 2026-11-24): this PR owns SOW-0074; #19 rebases onto it after merge.

## Plan

1. Fix the guard; validate on glibc 2.41 x86_64, i386 legacy and i386 time64.

## Execution Log

### 2026-11-24

- `tests/fixtures/c/uds_eintr_wrap.c`: guard changed to `__USE_TIME_BITS64 && __TIMESIZE == 32`, comment explains the glibc 2.40 change.
- Root cause, guard and edge cases agreed with Codex before the edit.

## Validation

Acceptance criteria evidence:

- Red at 1ff116f (beast, Debian 13 glibc 2.41 x86_64): `test_uds` link fails with `undefined reference to '__sendmsg64'`.
- Green with the fix: x86_64 `test_uds` links, 322 passed, 0 failed.
- i386 glibc with `_FILE_OFFSET_BITS=64 _TIME_BITS=64` (static): `libnetipc_uds.a` has two undefined `__sendmsg64` references, `test_uds` contains `__wrap___sendmsg64`, `test_uds` 322 passed including `every injected EINTR was hit` and `every injection was hit`, which fail if the redirected call is not intercepted.
- i386 legacy (`tests/run-linux-target.sh i386-gnu c`): unaffected, all tests pass.

Tests or equivalent validation (2026-11-24, low priority, beast):

- x86_64: build clean, CTest 32/32.
- `tests/run-linux-target.sh i386-gnu c`: CTest 14/14.
- i386 time64 C build: CTest 14/14.
- armv7-gnu-time64 and aarch64-gnu: covered by the CI Linux target jobs on the PR.

Real-use evidence:

- Test-only change; the real use is the CI jobs that failed on `main` (Runtime Safety, CodeQL C/C++ POSIX, Codacy Coverage), rechecked on the PR.

Reviewer findings:

- Codex (root cause and design): agreed with the guard; noted the `||` form with `__USE_TIME64_REDIRECTS` is equivalent and the shorter guard suffices, and that `--wrap=__sendmsg64` without references is harmless.
- Pre-push review on the signed commit: Codex READY with two wording nits (x32 phrasing in the comment and acceptance criteria; the `fallocate` pattern description), both fixed. claude-swarm Kimi K3: READY, no findings. claude-swarm Qwen: C change correct; NOT READY on SOW text only: the CI job list named Static Analysis instead of CodeQL (verified against the 1ff116f runs, fixed) and the performance assessment was missing (added); wording nits on the 2.39 backport and the `NIPC_TEST_REQUIRE_TIME64_32` source fixed. Its `__TIMESIZE` visibility nit for glibc 2.34-2.39 is rejected: `features-time64.h` includes `bits/timesize.h` on the glibc versions that have this redirect (Codex, checked against `bminor/glibc @ glibc-2.39` `sysdeps/unix/sysv/linux/features-time64.h`), and a miss would fail loudly at link time, not silently.

Same-failure scan:

- Other wrapped symbols (`send`, `recv`, `bind`, `accept4`) have no time64 redirect in `sys/socket.h`; `recvmsg` has one (`__recvmsg64`) but is not wrapped.
- `grep -rn 'TIME_BITS\|__TIMESIZE\|TIME64'` over C, Rust, Go and CMake sources: the only feature-macro guard is this one; `test_shm_timeout.c` uses its own `NIPC_TEST_REQUIRE_TIME64_32` define, passed by CI (`runtime-safety.yml`) through `tests/run-shm-timeout-abi.sh`.

Performance assessment:

- Test fixture only; nothing on the measured established-session paths changed, so no benchmark run.

Sensitive data gate:

- No raw secrets, credentials, hosts, customer data or private endpoints in this SOW, the code comment or the PR text.

Artifact maintenance gate:

- `AGENTS.md`: no workflow change.
- Runtime project skills: `project-netdata-vendoring` unaffected; test fixtures are not vendored.
- Specs: no protocol, API or transport change; `.agents/sow/specs/` has no test-fixture rules.
- End-user/operator docs: none affected; no public behavior changed.
- End-user/operator skills: `docs/netipc-integrator-skill.md` unaffected; it covers public APIs and integration, not test fixtures.
- SOW lifecycle: `Status: completed` and the file in `done/`, committed with the fix in one commit.

Specs update:

- None needed: test fixture only.

Project skills update:

- None needed: no reusable workflow change.

End-user/operator docs update:

- None needed: no public behavior changed.

End-user/operator skills update:

- None needed: no docs or spec changed.

Lessons:

- Recorded below.

Follow-up mapping:

- No deferred items.

## Outcome

`test_uds` links again on 64-bit glibc 2.40+, and the `__sendmsg64` wrapper still intercepts the redirected call on 32-bit time64 glibc.

## Lessons Extracted

- Guard a wrapper for a redirected libc symbol on the header's own redirect condition, not on the feature macro that feeds it: glibc changed what `__USE_TIME_BITS64` means in 2.40 without changing the redirect.

## Followup

None.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.
