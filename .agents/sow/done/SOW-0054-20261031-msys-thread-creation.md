# SOW-0054 - MSYS-safe thread creation in the Windows C library

## Status

Status: completed

Sub-state: library and benchmark driver start MSYS threads through pthread_create; validated on Windows.

## Requirements

### Purpose

NetIPC ships in Netdata's Windows build, which runs on the MSYS2 (Cygwin) runtime. Threads that the C library starts must be ones that runtime supports, so servers never stall or die under load.

### User Request

Zero-known-bug policy (AGENTS.md): confirmed bugs are fixed, not documented. Every Netdata-supported platform must build and test.

### Assistant Understanding

Facts:

- Under `__MSYS__`, `nipc_service_win_beginthreadex()` (`src/libnetdata/netipc/src/service/netipc_service_win.c:96-119`) starts session threads with raw `CreateThread`.
- SOW-0053 reproduced, on costa-win11 under `ctest -j8` load, a C fixture whose accept thread was started with `CreateThread`: the thread never ran (a raw `WriteFile` at its entry never appeared within 100-170 ms while the main thread kept looping), then the process died silently with exit status 0, no `atexit` handler run and no crash event. Clients saw `ERROR_BROKEN_PIPE` during the handshake. Rate: 1-5 failures per 120 runs across about 8 runs.
- The same fixture with that one thread started by `pthread_create` had 0 failures in 240 runs under the same load.
- Cygwin maintainers state that threads not started through Cygwin's own mechanisms are not supported (cygwin mailing list, 2009-10, "Thread related crash"); Cygwin finds per-thread TLS through the TIB, which such threads lack.
- The library passes session thread handles to `CancelSynchronousIo` and `CloseHandle` (`netipc_service_win_server_session.c:432,483`, `netipc_service_win_server.c:248`), so a replacement must still expose a real Win32 thread handle.
- `bench/drivers/c/bench_windows.c` also starts threads with `CreateThread`.

Inferences:

- Library session threads are started from an already running thread, which may be why 240 runs showed no session-thread failure; the runtime still does not support them.

Unknowns:

- None that investigation cannot resolve.

### Acceptance Criteria

- Under `__MSYS__` the library starts every thread with `pthread_create` and still hands back a real Win32 thread handle for cancellation and joins; native (non-MSYS) Windows builds keep `_beginthreadex`. (Revised at activation: the MSYS runtime has no `pthread_getw32threadhandle_np`; see decision 1.)
- The C benchmark driver uses the same safe path.
- A deterministic test proves the library's threads are runtime threads under MSYS, and fails on the previous code.
- A Windows load run (repeated `ctest -j8`) shows no silent server death.
- Full Windows ctest, Rust and Go suites pass.

## Analysis

Sources checked:

- `src/libnetdata/netipc/src/service/netipc_service_win.c`
- `src/libnetdata/netipc/src/service/netipc_service_win_server.c`
- `src/libnetdata/netipc/src/service/netipc_service_win_server_session.c`
- `tests/fixtures/c/test_sync.h` (`test_create_thread`, added by SOW-0053)

Risks:

- Handle ownership: a handle from `pthread_getw32threadhandle_np` belongs to the pthread; the code must not close it the way it closes a `CreateThread` handle. Joins must use `pthread_join` or a completion event.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Under `__MSYS__`, `nipc_service_win_beginthreadex()` starts threads with raw `CreateThread`, which the Cygwin/MSYS runtime does not support (no runtime TLS for the thread). SOW-0053 saw such a thread never run and the process die silently under load.

Evidence reviewed:

- `src/libnetdata/netipc/src/service/netipc_service_win.c:93-115` (the only library thread creation; caller `netipc_service_win_server.c:206`), handle uses `netipc_service_win_server_session.c:431-432` (`CancelSynchronousIo`) and `:482-483`, `netipc_service_win_server.c:247-248` (wait and close).
- `bench/drivers/c/bench_windows.c:538,669,755` (`CreateThread`), joined with `WaitForSingleObject` and `CloseHandle` at `:550-553,679-680,704-705,766-767,791-792`.
- Probe on costa-win11 (MSYS2 runtime `msys-2.0.dll`): `pthread_getw32threadhandle_np` is not declared in `/usr/include/pthread.h` nor exported. A handle the thread duplicates from `GetCurrentThread()` is signaled when the detached pthread returns (109 ms for a 100 ms sleeper), `CancelSynchronousIo` on it ends a blocked `ReadFile` (error 995), and 2000 consecutive starts never failed. In a `CreateThread` thread `pthread_getattr_np(pthread_self())` returns ESRCH (3); in pthreads and the main thread it returns 0.

Affected contracts and surfaces:

- Internal only: `nipc_service_win_beginthreadex()` (not public), the Windows C benchmark driver, one new test. No public API, wire format or docs change.

Existing patterns to reuse:

- `tests/fixtures/c/test_sync.h` `test_create_thread` (SOW-0053) for the trampoline shape.

Risk and blast radius:

- Every managed-server session thread in MSYS builds (Netdata's Windows build). Native builds unchanged. Creation now waits until the new thread has duplicated its handle, a few microseconds per accepted session; that wait cannot hang because `pthread_create` success means the thread runs.

Performance assessment:

- Thread creation happens once per accepted connection (`netipc_service_win_server.c:206`), outside established-session request handling. No per-request code changes; no benchmark run is needed. The benchmark driver change affects only how its threads start, not what it measures.

Sensitive data handling plan:

- No secrets involved; the Windows host is named by lab hostname only. Applies to this SOW and code comments; no docs or skills change.

Implementation plan:

1. Library: under `__MSYS__`, start the thread with `pthread_create` through a trampoline that duplicates its own thread handle, reports it with its thread id, then runs the start routine; the creator waits for the report and detaches the pthread.
2. Benchmark driver: same pattern in a local helper (it uses public headers only).
3. Test in `test_win_service_extra.c`: a thread started by the helper is a runtime thread (`pthread_getattr_np` succeeds, MSYS only), reports the id the helper returned, and its handle is signaled when it returns.

Validation plan:

- New test fails on the previous code and passes now; full Windows ctest, Rust and Go suites; repeated `ctest -j8` load run on Windows; Linux build unaffected (the code is Windows-only), checked by a Linux build.

Artifact impact plan:

- AGENTS.md: no change expected.
- Runtime project skills: no change expected.
- Specs: none; internal threading only.
- End-user/operator docs: none; no behaviour or API change.
- End-user/operator skills: none.
- SOW lifecycle: pending -> current -> done in one commit.

Open-source reference evidence:

- None; evidence is the local runtime probe.

Open decisions:

- None; decisions below were assistant-applied under the user's autonomy instruction.

## Implications And Decisions

1. Real handle from the thread itself (long-term-best): the new pthread duplicates `GetCurrentThread()` and hands the handle to the creator before running the routine. Reason: MSYS has no `pthread_getw32threadhandle_np`; this keeps every caller's `CancelSynchronousIo`, `WaitForSingleObject` and `CloseHandle` unchanged. Alternative rejected: an event signaled on return (the test fixture approach) cannot serve `CancelSynchronousIo`.
2. The benchmark driver gets a local helper with the same pattern, because it builds against public headers only.
3. The test fixture helper in `test_sync.h` stays as it is: test threads only need join semantics, which its event provides.

## Plan

As in the Pre-Implementation Gate implementation plan.

## Execution Log

### 2026-10-31

- `src/libnetdata/netipc/src/service/netipc_service_win.c`: `nipc_service_win_beginthreadex()` under `__MSYS__` starts a pthread through `service_win_thread_trampoline`, which duplicates its own handle and reports it with its thread id before running the routine; the creator detaches the pthread and returns that handle.
- `bench/drivers/c/bench_windows.c`: `bench_create_thread()` with the same pattern under `__MSYS__` (`CreateThread` otherwise) for the timer and server threads.
- `tests/fixtures/c/test_win_service_extra.c`: `test_library_thread_creation`.

## Validation

Acceptance criteria evidence:

- `nipc_service_win_beginthreadex()` under `__MSYS__` uses `pthread_create` and returns a real thread handle; native builds keep `_beginthreadex` (unchanged `#else` branch).
- The benchmark driver starts its timer and server threads through `bench_create_thread()`.
- `test_library_thread_creation` (in `test_win_service_extra`) passes: the thread is a runtime thread, its handle is signaled on return, and the reported thread id and `GetThreadId(handle)` match the thread that ran. Built against the previous `netipc_service_win.c` (scratch tree on costa-win11) it fails exactly "library thread is a runtime thread" (791 passed, 1 failed).
- Load: the full Windows ctest followed by 5 back-to-back `ctest -j8` runs, each 20/20, with no server death.

Tests or equivalent validation:

- Windows (costa-win11, MSYS2): `test_win_service_extra` 792 passed; full ctest 20/20 six times.
- Rust and Go suites were not rerun: no Rust or Go file changed, and they do not link the C library; the C/Rust/Go interop pairs run inside the Windows ctest.
- Linux: `cmake --build build` clean; the changed code is Windows-only.

Real-use evidence:

- The Windows C benchmark driver ran a 1 s `np-ping-pong` smoke (client about 1000 req/s, server stopped by its timer thread, exit 0), exercising both helper-started threads. Not a benchmark measurement.

Reviewer findings:

- No external review requested; the change mirrors the SOW-0053 fixture helper and the runtime probe.

Same-failure search:

- `grep -rn "CreateThread\|_beginthreadex"` over `src/`, `bench/` and `tests/`: the library and driver now go through the helpers; the remaining `CreateThread` uses are the native-build branch of `bench_create_thread()` and `test_sync.h`'s native branch. Rust and Go create threads through their own runtimes.

Sensitive data gate:

- This SOW and the code comments contain no secrets, credentials, customer or personal data, or private endpoints; the Windows host is named by lab hostname only.

Artifact maintenance gate:

- AGENTS.md: no change; no workflow changed.
- Runtime project skills: no change; vendoring steps unaffected.
- Specs: no change; thread creation is internal and not part of any `docs/` contract.
- End-user/operator docs: no change; no API, behaviour or command changed.
- End-user/operator skills: `docs/netipc-integrator-skill.md` unaffected (no public surface changed).
- SOW lifecycle: pending -> current -> done in this commit; the acceptance criterion naming `pthread_getw32threadhandle_np` was revised with evidence (decision 1).

## Outcome

Under MSYS, every thread the Windows C library and the C benchmark driver start is a runtime pthread with a real Win32 handle, so the runtime-unsupported `CreateThread` path that let a server die silently under load is gone.

## Lessons Extracted

- Check that a platform API exists on the actual runtime before writing it into acceptance criteria: `pthread_getw32threadhandle_np` belongs to pthreads-win32, not Cygwin/MSYS.
- `pthread_getattr_np(pthread_self())` returning ESRCH is a deterministic test for "thread not started by the Cygwin runtime", turning a load-dependent failure into a one-shot test.

## Followup

- None: no deferred items remain.

## Regression Log

None yet.
