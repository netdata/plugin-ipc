# Coverage Exclusions

Purpose:

- record what has been measured
- separate ordinary missing test coverage from branches that really need
  special infrastructure
- avoid overstating “coverage complete” when the numbers do not support it

## Verified Measurements

### Linux / POSIX

Verified on `2026-03-18`:

- C:
  - script: `tests/run-coverage-c.sh`
  - result: `90.5%`
  - threshold: `82%`
- Go:
  - script: `tests/run-coverage-go.sh`
  - result: `86.1%`
  - threshold: `85%`
- Rust:
  - script: `tests/run-coverage-rust.sh`
  - result: `81.46%`
  - threshold: `80%`

### Windows (`win11`)

Verified on `2026-03-18`:

- C:
  - script: `tests/run-coverage-c-windows.sh 80`
  - result: `67.5%`
  - per-file:
    - `netipc_service_win.c`: `63.9%`
    - `netipc_named_pipe.c`: `66.0%`
    - `netipc_win_shm.c`: `76.8%`
  - status: below target

- Go:
  - script: `tests/run-coverage-go-windows.sh 80`
  - result: `52.4%`
  - selected low-coverage files:
    - `service/cgroups/cache_windows.go`: `0.0%`
    - `service/cgroups/client_windows.go`: `37.7%`
    - `transport/windows/pipe.go`: `5.8%`
    - `transport/windows/shm.go`: `72.5%`
  - status: below target

- Rust:
  - no validated Windows-native coverage workflow yet

## What Is Actually Excluded

These categories genuinely need special infrastructure beyond ordinary tests.

### 1. Integer-overflow guard branches

Examples:

- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:146`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:203`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:223`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:248`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:453`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:489`

These require absurd sizes such as `item_count > SIZE_MAX / N` or wire values
that are not produced by normal encoders. They are reasonable candidates for
fault-injection or synthetic corruption harnesses.

### 2. Allocation-failure branches

Examples:

- `malloc` / `calloc` / `realloc` failure cleanup in C service and transport code
- low-memory allocation paths in Windows C `netipc_service_win.c`
- low-memory branches in Go and Rust transport scratch growth

These require deterministic allocation-failure injection to cover reliably.

### 3. OS / kernel failure branches

Examples:

- socket creation failures
- `mmap` / `CreateFileMapping` / `MapViewOfFile` failures
- `pthread_create` / Win32 handle creation failures
- named-pipe creation / handshake API failures

These are not “ordinary missing tests”. They require fault injection,
resource exhaustion, or environment simulation.

### 4. Timing / race branches

Examples:

- futex timeout and EINTR races
- TOCTOU stale-cleanup paths
- mid-send or mid-receive disconnect timing
- Windows accept / shutdown timing edges

These need race orchestration or deterministic hooks.

### 5. Windows-only Rust coverage

Facts:

- `src/crates/netipc/src/transport/windows.rs`
- `src/crates/netipc/src/transport/win_shm.rs`

These modules are excluded from Linux builds and cannot be measured by the
current Linux coverage path. This is a tooling / environment gap, not proof
that the remaining lines are untestable.

## What Is Not Justified As “Impossible”

The following are still ordinary missing coverage and should not be treated as
hard exclusions yet.

### Windows C coverage gaps

Current evidence:

- `netipc_service_win.c` is only `63.9%`
- `netipc_named_pipe.c` is only `66.0%`

Brutal truth:

- this is too low to explain away as “only fault injection remains”
- there are still many normal branches and scenarios that simply do not have
  tests yet

### Windows Go coverage gaps

Current evidence:

- `service/cgroups/cache_windows.go`: `0.0%`
- `service/cgroups/client_windows.go`: `37.7%`
- `transport/windows/pipe.go`: `5.8%`

Brutal truth:

- these numbers prove the Windows Go side is far from coverage-complete
- Phase 2 is not blocked only by impossible branches; it is still missing a
  large amount of ordinary test coverage

### Linux / POSIX remaining gaps

Even on POSIX, not every remaining uncovered line should be assumed
unreachable. Some are likely still coverable with better malformed-input or
disconnect tests.

## Practical Reading Of The Current State

- Linux / POSIX:
  - the scripts are working
  - the current lowered thresholds pass
  - coverage improved meaningfully, especially C
- Windows:
  - coverage measurement now exists and is validated
  - the numbers are far below the draft targets
  - more ordinary test work is required before any “coverage parity” claim is honest

## Conclusion

- `100%` overall coverage is not currently achieved.
- Some branches truly need special infrastructure.
- A large part of the remaining Windows coverage gap is still plain missing test work, not a technical impossibility.
