# SOW-0054 - MSYS-safe thread creation in the Windows C library

## Status

Status: open

Sub-state: discovered during SOW-0053; test fixtures already fixed there, library and C benchmark driver remain.

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

- Under `__MSYS__` the library starts every thread with `pthread_create` and obtains the Win32 handle with `pthread_getw32threadhandle_np` for cancellation and joins; native (non-MSYS) Windows builds keep `_beginthreadex`.
- The C benchmark driver uses the same safe path.
- A Windows load reproduction (many server starts under `ctest -j8`) shows no silent server death.
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

Status: blocked

To be filled before implementation.

## Implications And Decisions

None yet.

## Plan

To be written.

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
