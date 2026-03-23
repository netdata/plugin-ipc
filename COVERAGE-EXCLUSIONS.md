# Coverage Exclusions

Purpose:

- record what has been measured
- separate ordinary missing test coverage from branches that really need
  special infrastructure
- avoid overstating “coverage complete” when the numbers do not support it

## Verified Measurements

### Linux / POSIX

Verified on `2026-03-23`:

- C:
  - script: `tests/run-coverage-c.sh`
  - result: `90.5%`
  - threshold: `82%`
- Go:
  - script: `tests/run-coverage-go.sh`
  - result: `95.8%`
  - threshold: `85%`
- Rust:
  - script: `tests/run-coverage-rust.sh`
  - result: `90.66%`
  - threshold: `80%`
  - note:
    - the current Linux script used `tarpaulin` on this machine
    - the report still includes Windows-tagged Rust files in the total line count
    - latest ordinary Linux Rust slice raised:
      - `src/service/cgroups.rs` to `686/710`
      - `src/transport/posix.rs` to `388/401`
      - `src/transport/shm.rs` to `347/375`
    - latest slice covered:
      - Linux SHM response `message_id` mismatch on the 3 Rust client call paths
      - Linux managed-server SHM-upgrade rejection when `server_create()` is obstructed on disk
      - direct `poll_fd()` EINTR and closed-descriptor branches
      - direct POSIX helper fallbacks for `detect_packet_size()` and `raw_send()`
      - managed-server worker-capacity rejection in the Rust L2 loop
      - malformed Linux SHM batch-request item decode failure returning `INTERNAL_ERROR`
      - chunked batch-directory invalidation across a real continuation boundary
    - implication:
      - the remaining Linux Rust total is now dominated by helper / fault-injection / Windows-tagged territory
      - one concrete layering fact is now proven:
        - on POSIX baseline, a bad response `message_id` is rejected by L1 before the L2 typed wrappers can map it to `BadLayout`
        - malformed batch directories on POSIX UDS are rejected by L1 before the Rust managed-service loop can map them to `INTERNAL_ERROR`
        - the honest ordinary coverage path for that branch is Linux SHM

Latest Linux Go notes from the current ordinary POSIX slice:

- `service/cgroups/client.go`: `95.9%`
- `transport/posix/shm_linux.go`: `91.9%`
- `transport/posix/uds.go`: `95.6%`
- the latest rerun also exposed two real Unix Go harness bugs, not library regressions:
  - the worker-capacity test used a readiness helper that briefly consumed the only worker slot
  - the non-request termination test relied on one-shot raw connect / refresh assumptions instead of retry-style readiness
- the latest direct SHM guard slice raised:
  - `ShmSend()` to `96.6%`
  - `ShmReceive()` to `96.2%`
- the latest Linux Go service slice raised:
  - `Run()` to `94.7%`
  - `handleSession()` to `92.9%`
  - Linux Go total to `95.8%`

### Windows (`win11`)

Verified on `2026-03-23`:

- C:
  - script: `tests/run-coverage-c-windows.sh 82`
  - result: `83.9%`
  - per-file:
    - `netipc_service_win.c`: `83.1%`
    - `netipc_named_pipe.c`: `85.8%`
    - `netipc_win_shm.c`: `83.2%`
  - status: passes the Linux-matching per-file and total `82%` gates

- Go:
  - script: `tests/run-coverage-go-windows.sh 85`
  - result: `96.7%`
  - selected key files:
    - `service/cgroups/client_windows.go`: `96.7%`
    - `service/cgroups/types.go`: `100.0%`
    - `transport/windows/pipe.go`: `97.1%`
    - `transport/windows/shm.go`: `92.9%`
  - status:
    - reported above the Linux-matching `85%` target
    - the script exits cleanly in noninteractive `ssh`
    - first-class Windows Go service/cache tests now also run under `ctest`
    - the latest transport edge tests, raw WinSHM L2 tests, and the listener shutdown fix materially raised both the Windows transport package and the Windows-only service branches that named pipes cannot reach
    - malformed raw WinSHM request tests now also cover the real SHM server-side teardown / reconnect path
    - the latest create / attach edge tests materially raised the remaining ordinary Windows Go transport file
    - the latest raw I/O, handshake, `Listen()`, chunked batch, and disconnect tests pushed `pipe.go` above `97%` and Windows Go total to `96.7%`

- Rust:
  - script: `tests/run-coverage-rust-windows.sh 80`
  - result: `93.59%` line coverage after excluding Rust bin / benchmark noise from the report
  - key files:
    - `service/cgroups.rs`: `83.83%` line coverage
    - `transport/windows.rs`: `94.43%` line coverage
    - `transport/win_shm.rs`: `87.74%` line coverage
  - status: validated workflow with the same total `80%` threshold policy as Linux Rust coverage
  - caveat: `test_retry_on_failure_windows` is intentionally ignored because the Windows managed-server shutdown/reconnect behavior is still a separate investigation

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
- `src/crates/netipc/src/service/cgroups.rs`

These modules are excluded from Linux builds and cannot be measured by the
current Linux coverage path. The tooling / environment gap is now solved on
Windows. The remaining Windows Rust caveat is no longer the service file; it
is the deferred retry/shutdown investigation plus broader coverage-raising
work.

## What Is Not Justified As “Impossible”

The following are still ordinary missing coverage and should not be treated as
hard exclusions yet.

### Windows C coverage gaps

Current evidence:

- `netipc_service_win.c` is now `83.1%`
- `netipc_named_pipe.c` is `85.8%`
- `netipc_win_shm.c` is `83.2%`

Brutal truth:

- the current Windows C gate is green
- this does not mean Windows C is coverage-complete
- the remaining uncovered branches are still a mix of ordinary missing tests and branches that would need fault injection

### Windows Go coverage gaps

Current evidence:

- Windows Go total is now `96.7%`
- `service/cgroups/client_windows.go` is now `96.7%`
- `transport/windows/pipe.go` is now `97.1%`
- `transport/windows/shm.go` is now `92.9%`
- some malformed named-pipe response cases are filtered by the Windows session layer before they can reach L2 validation branches
- direct raw WinSHM tests now cover the equivalent Windows-only L2 branches
- the remaining uncovered `client_windows.go` blocks are now mostly fixed-size encode guards plus defensive server failure paths

Brutal truth:

- Windows Go is no longer the red gate for the Linux-matching `85%` target
- but it is still not honest to call it coverage-complete
- the remaining ordinary Windows Go work is no longer mainly in `client_windows.go`
- the next honest review target is whether any of the tiny remaining low-level `pipe.go` branches are still worth ordinary testing, plus a final check for any still-reachable `transport/windows/shm.go` residual gap

### Windows Rust coverage gaps

Current evidence:

- Windows Rust now has a validated threshold-enforced workflow
- `service/cgroups.rs` is now `83.83%` line coverage
- `transport/windows.rs` is `94.43%` line coverage
- `transport/win_shm.rs` is `87.74%` line coverage
- one retry/shutdown test is intentionally ignored for now

Brutal truth:

- Windows Rust is no longer a tooling gap
- it is no longer blocked on `service/cgroups.rs` being `0.00%`
- it is now threshold-enforced at the same total `80%` policy as Linux Rust coverage
- `transport/windows.rs` is no longer the remaining weak Windows Rust file
- it still needs more ordinary coverage work, and the retry/shutdown investigation stays outside the normal coverage path

### Linux / POSIX remaining gaps

Even on POSIX, not every remaining uncovered line should be assumed
unreachable. Some are likely still coverable with better malformed-input or
disconnect tests.

Concrete evidence from the latest Linux Go SHM slice:

- typed SHM recovery cases such as batch-handler failure and batch-response
  overflow are ordinary and are now covered
- direct SHM transport guard and timeout paths are ordinary too:
  - invalid service-name entry guards
  - `ShmSend()` bad-parameter guards
  - `ShmReceive()` bad-parameter and timeout paths
  - `ShmCleanupStale()` missing-directory / unrelated-file branches
- result:
  - `transport/posix/shm_linux.go` is now `91.9%`
- raw malformed SHM requests on POSIX are not currently ordinary unit-test
  targets:
  - malformed short request
  - malformed header request
  - unexpected message kind
- reason:
  - they currently block in `ShmReceive(..., 30000)` inside
    `service/cgroups/client.go`, so ordinary tests spend the full SHM receive
    timeout instead of getting a cheap reconnect signal
- implication:
  - these paths should stay out of the ordinary-coverage bucket unless the
    POSIX SHM timeout behavior becomes directly controllable in tests

Concrete evidence from the latest Linux Go service-loop slice:

- ordinary POSIX server-loop cases are still available and are now covered:
  - worker-capacity rejection
  - idle peer disconnect
  - non-request termination
  - truncated raw request recovery
- result:
  - `service/cgroups/client.go` is now `94.3%`
  - `Run()` moved to `86.8%`
  - `handleSession()` moved to `90.6%`
- one real test-harness issue was exposed under coverage slowdown:
  - the Unix Go server helpers were still using blind sleeps before clients
    raced `Refresh()`
  - this is now fixed by waiting for a successful POSIX handshake, not just for
    the socket path to exist
- current honest remaining question in the POSIX service loop:
  - the `session.Send(...)` failure branch after peer close did not reproduce as
    an ordinary Unix-domain packet case in this slice
  - treat it as unproven ordinary work until a deterministic reproduction exists

Concrete evidence from the latest Linux Go SHM transport slice:

- ordinary filesystem-obstruction cases were still available and are now covered:
  - `checkShmStale()` unreadable stale file
  - `checkShmStale()` non-empty directory path (open succeeds, `Mmap` fails)
  - `ShmServerCreate()` retry-create failure when stale recovery cannot remove
    the obstructing path
- one existing SHM transport test-harness race was also real and is now fixed:
  - the direct SHM roundtrip tests were using fixed service names plus blind
    `50ms` sleeps before `ShmClientAttach()`
  - under coverage slowdown this produced both:
    - attach-before-create failures (`ErrShmOpen`)
    - and later server-side futex-wait timeouts
  - the fix was:
    - unique SHM service names per test
    - attach-ready waiting instead of blind sleeps
- result:
  - `transport/posix/shm_linux.go` is now `91.4%`
  - `ShmServerCreate()` moved to `79.2%`
  - `checkShmStale()` moved to `92.6%`
- implication:
  - the remaining `shm_linux.go` gaps are now even more concentrated in true OS
    failure paths such as `Ftruncate`, `Mmap`, `Dup`, `f.Stat`, and atomic-load
    bounds failures after a successful `Mmap`

Concrete evidence from the latest Linux Go UDS transport slice:

- the remaining ordinary UDS batch-directory and first-request state paths were
  still available and are now covered:
  - client `Send()` with `inflightIDs == nil`
  - non-chunked batch-directory underflow validation
  - chunked batch-directory validation after successful reassembly
  - `detectPacketSize()` fallback / success helper behavior
- result:
  - `transport/posix/uds.go` is now `95.6%`
  - `Send()` is now `100.0%`
  - `Receive()` moved to `97.8%`
  - `detectPacketSize()` is now `100.0%`
  - `Listen()` moved to `81.0%`
  - `connectAndHandshake()` moved to `93.2%`
  - `serverHandshake()` moved to `95.3%`
- implication:
  - the remaining weak `uds.go` lines are now much more concentrated in raw
    syscall-failure and short-write territory:
    - `Listen()` raw socket / bind / listen failures
    - `rawSendMsg()` short writes
    - handshake send / receive syscall failures

## Practical Reading Of The Current State

- Linux / POSIX:
  - the scripts are working
  - the current lowered thresholds pass
  - coverage improved meaningfully, especially C
- Windows:
  - coverage measurement now exists and is validated
  - Windows C now passes the Linux-matching `82%` gate
  - Windows Go is above the Linux-matching `85%` target, the script reliability issue is fixed, and Windows Go service/cache tests are now part of `ctest`
  - Windows Go transport coverage is now materially stronger too
  - Windows Rust coverage now has a real threshold-enforced entrypoint
  - more ordinary test work is required before any “coverage parity” claim is honest

## Conclusion

- `100%` overall coverage is not currently achieved.
- Some branches truly need special infrastructure.
- A meaningful part of the remaining Windows coverage gap is still plain missing test work, not a technical impossibility.
