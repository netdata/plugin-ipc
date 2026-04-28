## Purpose

Assess the current Coverity findings in the `netipc` C implementation,
separate real bugs from weak/static-analysis-only reports, and decide the
minimum safe fixes needed without changing the public protocol contract.

## TL;DR

- Review the six reported Coverity findings in:
  - `src/libnetdata/netipc/src/service/netipc_service.c`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`
- Classify each as:
  - real bug
  - hardening gap
  - static-analysis false positive / cosmetic cleanup
- If code changes are needed, mirror any platform-parity fixes where the same
  pattern exists in the Windows counterpart.
- Use the Netdata Coverity audit skill from
  `/home/costa/src/PRs/memory-corruption-hunting/.agents/skills/coverity-audit/SKILL.md`
  to fetch the live Coverity table/details and map each finding to its CID
  before implementation.

## Source of truth

- Costa confirmed the Coverity `.env` is already prepared in
  `/home/costa/src/PRs/memory-corruption-hunting/`.
- Costa confirmed the Coverity keepalive is already running.
- Fetched current outstanding Coverity table into:
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/raw/outstanding-20260428-085558-all.json`
- Fetched current per-defect details into:
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/details/outstanding-20260428-085558/`
- Prepared per-CID bundles under:
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503341/`
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503342/`
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503343/`
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503344/`
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503345/`
  - `/home/costa/src/PRs/memory-corruption-hunting/.local/audits/coverity/triage/outstanding/cid-503346/`

## Coverity CID mapping

- CID 503341:
  - checker: `OVERRUN`
  - type: `Out-of-bounds access`
  - file/function: `netipc_service.c:server_handle_session`
  - event: `netipc_service.c:1006` copies `response_len`; Coverity models
    `response_len == UINT32_MAX` while `msg` still points at `stack_msg`.
  - updated conclusion:
    - on 64-bit, the exact path is infeasible because `msg_len` exceeds the
      stack threshold
    - on 32-bit, `NIPC_HEADER_LEN + response_len` can overflow `size_t`
    - harden all service SHM send paths before computing `msg_len`
- CID 503342:
  - checker: `TOCTOU`
  - type: `Time of check time of use`
  - file/function: `netipc_uds.c:check_and_recover_stale`
  - events:
    - `netipc_uds.c:498` calls `stat(path)`
    - `netipc_uds.c:522` calls `unlink(path)` after the check
  - updated conclusion:
    - real stale-cleanup hardening issue
    - current code can also delete a regular file at the socket path because
      `connect(AF_UNIX SOCK_SEQPACKET)` to a regular file returns
      `ECONNREFUSED`
- CID 503343:
  - checker: `TOCTOU`
  - type: `Time of check time of use`
  - file/function: `netipc_shm.c:check_shm_stale`
  - events:
    - `netipc_shm.c:153` calls `stat(path)`
    - `netipc_shm.c:158` calls `unlink(path)` after the check
    - `netipc_shm.c:162` calls `open(path)` after the check
  - updated conclusion:
    - real stale-cleanup hardening issue
    - replace pre-open `stat()` with `open()` plus `fstat()`
    - avoid following symlinks during stale inspection
- CID 503344:
  - checker: `REVERSE_INULL`
  - type: `Dereference before null check`
  - file/function: `netipc_service.c:server_init_raw`
  - events:
    - `netipc_service.c:1181` dereferences `config`
    - `netipc_service.c:1183` checks `config`
  - updated conclusion:
    - real raw/test entrypoint hardening bug
    - mirrored in `netipc_service_win.c`
- CID 503345:
  - checker: `DEADCODE`
  - type: `Logically dead code`
  - file/function: `netipc_protocol.c:nipc_string_reverse_encode`
  - event:
    - `netipc_protocol.c:756` cannot be reached on the 64-bit Coverity build
  - updated conclusion:
    - not a runtime bug on 64-bit
    - rewrite the overflow guard so it is compiled only where meaningful
- CID 503346:
  - checker: `CONSTANT_EXPRESSION_RESULT`
  - type: `Operands don't affect result`
  - file/function: `netipc_protocol.c:nipc_string_reverse_encode`
  - event:
    - `netipc_protocol.c:755` is always false on the 64-bit Coverity build
  - updated conclusion:
    - same root cause as CID 503345

## Analysis

### Finding 1: out-of-bounds access in `netipc_service.c:1006`

- Reported line:
  - `memcpy(msg + NIPC_HEADER_LEN, resp_buf, response_len);`
- Current evidence:
  - `resp_buf` is allocated once per session before `server_handle_session()`
    runs:
    - `src/libnetdata/netipc/src/service/netipc_service.c:1037-1045`
  - `resp_size` is at least `session.max_response_payload_bytes`, with a floor
    of `1024`:
    - `src/libnetdata/netipc/src/service/netipc_service.c:1038-1043`
  - `response_len` is clamped before the copy:
    - if `response_len > session.max_response_payload_bytes`, the code changes
      the response to `LIMIT_EXCEEDED` and forces `response_len = 0`
    - `src/libnetdata/netipc/src/service/netipc_service.c:949-960`
- Working conclusion:
  - no concrete out-of-bounds path is established under the current
    server-handler contract
  - likely a Coverity false positive unless the handler contract itself is
    violated

### Finding 2: dereference before null check in `netipc_service.c:1183`

- Stronger issue than the reported line:
  - `server->base_config = *config;`
  - `src/libnetdata/netipc/src/service/netipc_service.c:1181`
- Current evidence:
  - `server_init_raw()` never rejects `config == NULL`
  - the later ternary checks at `1183-1188` are too late
  - the public typed entrypoint always passes a stack config produced by
    `service_server_config_to_transport()`:
    - `src/libnetdata/netipc/src/service/netipc_service.c:1231-1238`
  - the internal raw/test entrypoint forwards `config` directly:
    - `src/libnetdata/netipc/src/service/netipc_service.c:1248-1258`
- Working conclusion:
  - this is a real hardening bug in the raw initializer path
  - likely mirrored in the Windows file too

### Findings 3 and 4: dead code / operands don't affect result in `netipc_protocol.c:755-756`

- Reported code:
  - `if (str_len > SIZE_MAX - NIPC_STRING_REVERSE_HDR_SIZE - 1)`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c:755-756`
- Current evidence:
  - `str_len` is `uint32_t`
  - `NIPC_STRING_REVERSE_HDR_SIZE` is `8u`
    - `src/libnetdata/netipc/include/netipc/netipc_protocol.h:441`
  - on 64-bit builds, the condition is effectively dead because `SIZE_MAX` is
    much larger than any `uint32_t` payload length
  - on 32-bit builds, the condition is still meaningful near `UINT32_MAX`
- Working conclusion:
  - not a functional bug
  - valid as a Coverity cleanliness issue on 64-bit analysis
  - can be rewritten as an explicit 32-bit-only overflow guard

### Finding 5: TOCTOU in `netipc_uds.c:498`

- Reported code starts with:
  - `if (stat(path, &st) != 0) return -1;`
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c:494-529`
- Current evidence:
  - the code:
    - `stat()`s the socket path
    - then creates a probe socket
    - then `connect()`s
    - then may `unlink(path)` on stale-socket outcomes
  - a path replacement can happen between `stat()`, `connect()`, and `unlink()`
- Working conclusion:
  - this is a real filesystem race in principle
  - the practical risk depends on the trust model of the run directory, but the
    report itself is not bogus
  - a better structure is possible

### Finding 6: TOCTOU in `netipc_shm.c:153`

- Reported code starts with:
  - `if (stat(path, &st) != 0) return -1;`
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c:150-197`
- Current evidence:
  - the code:
    - `stat()`s first
    - checks `st_size`
    - then `open()`s the path
    - then `mmap()`s and validates the opened file
    - may `unlink(path)` on invalid/stale outcomes
  - the initial size decision is made before the file is opened
  - path replacement can happen before `open()` or before the later `unlink()`
- Working conclusion:
  - this is a stronger TOCTOU report than the UDS one
  - `fstat()` on the opened fd should replace the initial `stat()` size check
  - unlink safety still needs thought if we want to avoid deleting a newly
    replaced path

## Decisions

### Applied

1. Coverity-cleanup scope
   - Decision:
     - fix all six outstanding netipc findings
   - Rationale:
     - CID 503341 is not a valid 64-bit OOB path, but it exposed a real
       same-class 32-bit addition overflow hardening gap
     - CIDs 503345 and 503346 are cosmetic on 64-bit, but the code can be
       rewritten to keep the 32-bit guard without dead 64-bit code
     - leaving any of the six outstanding would keep the Coverity queue noisy

2. Raw server-init null handling
   - Decision:
     - reject `config == NULL` explicitly in raw init on POSIX and Windows
   - Rationale:
     - the typed public API still accepts `NULL` typed config because it
       synthesizes a concrete transport config before calling raw init
     - the raw/test entrypoint now has a simpler non-null contract

3. TOCTOU remediation level
   - Decision:
     - stronger race hardening
   - Rationale:
     - UDS stale recovery now refuses to unlink non-socket paths
     - SHM stale recovery now opens first, uses `fstat()` on the opened file,
       avoids following symlinks, and compares path metadata before stale unlink
     - POSIX still cannot atomically unlink a pathname only if it is still the
       same inode, so the final unlink remains path-based by OS design

## Plan

1. Harden header+payload additions before allocation/copy in:
   - POSIX L2 service
   - Windows L2 service
   - POSIX UDS L1 transport
   - Windows named-pipe L1 transport
2. Reject `NULL` raw server configs in POSIX and Windows raw L2 init.
3. Rework stale UDS recovery to avoid deleting regular files at socket paths.
4. Rework stale SHM recovery to use `open()`/`fstat()` and refuse symlinks.
5. Guard SHM region-size arithmetic in POSIX and Windows SHM transports.
6. Rewrite the string-reverse size guard so the 32-bit-only guard is not dead
   code in 64-bit Coverity builds.

## Implied decisions

- Do not change the public Level 2 typed server API contract unless evidence
  shows the public path is also unsafe.
- Keep behavior-compatible stale cleanup semantics unless the race fix requires
  tightening unlink conditions.

## Testing requirements

- Added targeted POSIX tests:
  - `test_uds`: regular file at stale socket path is not unlinked
  - `test_shm`: symlink at stale SHM path is not unlinked
  - `test_service`: raw init rejects `NULL` config
- Added targeted Windows C test coverage:
  - `test_win_service_extra`: raw init rejects `NULL` config
- Passed targeted POSIX binaries:
  - `./build/bin/test_uds`: `166 passed, 0 failed`
  - `./build/bin/test_shm`: `104 passed, 0 failed`
  - `./build/bin/test_service`: `222 passed, 0 failed`
- Passed POSIX C coverage gate:
  - `tests/run-coverage-c.sh`
  - total coverage: `91.9%`
  - all covered files above the `90%` threshold

## Final execution plan

1. Commit and push the upstream `plugin-ipc.git` Coverity hardening changes.
2. Pull the pushed commit on `win11:~/src/plugin-ipc.git`.
3. Run the relevant Windows test coverage from that checkout.
   - First `win11` run executed the Windows C test suite but failed the
     coverage threshold because new impossible-on-64-bit `size_t` guard
     branches reduced `netipc_service_win.c` to `89.6%`.
   - Follow-up fix:
     - compile the `size_t` header+payload overflow guard only on platforms
       where `size_t` can overflow for `uint32_t`-bounded payloads
     - add Windows tests for the real `uint32_t` SHM capacity overflow guards
4. Verify C benchmark performance is not regressed:
   - Linux C benchmarks from this checkout.
   - Windows C benchmarks from `win11:~/src/plugin-ipc.git`.
5. Vendor the pushed upstream library into:
   - `/home/costa/src/PRs/memory-corruption-hunting`
6. Make exactly one Netdata-side commit in
   `/home/costa/src/PRs/memory-corruption-hunting`.

## Documentation updates required

- None expected if fixes stay internal and behavior-compatible.
- Update docs only if the raw init contract or stale-cleanup behavior becomes
  observably stricter.
