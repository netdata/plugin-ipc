# TODO-rewrite: plugin-ipc Clean Rewrite

## MANDATORY: Read Before Any Work

**STOP. Before doing anything, read every file in `docs/` in full.**
Not skimming. Not pattern-scanning. Every file, every line. The specs
are the authority. Implementation must match them exactly.

Files to read in order:
1. `docs/README.md` — architecture overview
2. `docs/level1-transport.md` — L1 principles and features
3. `docs/level1-wire-envelope.md` — wire format byte layouts
4. `docs/level1-posix-uds.md` — UDS transport contract
5. `docs/level1-posix-shm.md` — POSIX SHM contract
6. `docs/level1-windows-np.md` — Named Pipe contract
7. `docs/level1-windows-shm.md` — Windows SHM contract
8. `docs/codec.md` — Codec principles
9. `docs/codec-cgroups-snapshot.md` — cgroups snapshot wire layout
10. `docs/level2-typed-api.md` — L2 orchestration
11. `docs/level3-snapshot-api.md` — L3 caching
12. `docs/code-organization.md` — module boundaries and file layout

Do not write code until all 12 files are read and understood.

## Current Status

- The specs in `docs/` are closed, reviewed, and validated through 4
  rounds of independent AI review.
- The old implementation exists in git history (committed and pushed
  on branch `main`) but will be deleted in Phase 0.
- The old implementation served its purpose for requirements discovery
  but does not match the specs. A clean rewrite is faster and cleaner
  than refactoring.
- The old code remains accessible via `git log` / `git show` as
  reference for platform-specific edge cases.

## Architecture Summary

```
┌─────────────────────────────────────────────┐
│      Level 3: Caching (client-side only)    │
│           (built on Level 2)                │
├─────────────────────────────────────────────┤
│         Level 2: Orchestration              │
│        (built on Level 1 + Codec)           │
├──────────────────────┬──────────────────────┤
│    Level 1:          │       Codec:          │
│    Transport         │   Wire ↔ Typed        │
│    (opaque bytes)    │   (no I/O)            │
└──────────────────────┴──────────────────────┘
```

- L1 and Codec are parallel — neither depends on the other.
- L2 depends on L1 + Codec.
- L3 is client-side caching on top of L2. Server side is pure L2.

## Rules

### Execution rules

1. Complete one phase fully before moving to the next.
2. Each phase includes: implementation, unit tests, integration tests,
   fuzz tests (where applicable), and validation.
3. Commit after each completed phase. Use descriptive commit messages.
4. Push after each commit.
5. Do not start the next phase until the current phase passes all its
   validation criteria.
6. If a phase goes wrong, `git reset` to the last committed checkpoint
   and retry. Do not accumulate broken state.
7. Between phases, Costa reviews. Do not proceed to the next phase
   without review approval.

### Quality rules

1. 100% test coverage (line + branch) for all library code.
2. Fuzz testing for all decode/parse paths.
3. Cross-language interop tests for all wire contracts.
4. Abnormal path coverage for all failure modes.
5. No exceptions. Nothing integrates into Netdata without this.

### Review rules

After completing each phase, run ALL of these reviewers in parallel
and incorporate their feedback before committing:

```bash
# Codex (gpt-5.4)
timeout 1800 codex exec "[prompt]" --skip-git-repo-check

# GLM-5
timeout 1800 opencode run -m "llm-netdata-cloud/glm-5" --agent code-reviewer "[prompt]"

# Kimi-K2.5
timeout 1800 opencode run -m "llm-netdata-cloud/kimi-k2.5-alibaba" --agent code-reviewer "[prompt]"

# Qwen3.5
timeout 1800 ~/.npm-global/bin/qwen --yolo -p "[prompt]"
```

Prompt template for reviewers:
```
Read all files in docs/ in this repository (the specs).
Then review the implementation in [files changed in this phase].
Check for:
1. Spec compliance — does the implementation match the specs exactly?
2. Wire compatibility — will C, Rust, Go produce identical bytes?
3. Test coverage — are all paths tested, including failure paths?
4. Fuzz coverage — are all decode paths fuzz-tested?
5. Code quality — is the code minimal, clean, and maintainable?
DO NOT MAKE CHANGES. PROVIDE YOUR REVIEW.
```

All reviewers must agree before committing. If any reviewer finds a
real issue, fix it and re-review.

### Windows validation rules

Windows phases are developed locally on Linux, committed, pushed,
then validated on `win11`:

```bash
ssh win11
cd ~/src/plugin-ipc.git
git pull
# compile, test, benchmark
```

If Windows validation fails, fix locally, push, re-validate remotely.

### Language rules

- **Go must be pure Go. No cgo. No exceptions.** This is a hard
  requirement for compatibility with Netdata's `go.d.plugin`.
- Go SHM on Linux uses raw `SYS_FUTEX` syscall, not cgo semaphores.
- All languages share the same wire format. If Go can't do something
  without cgo, find a pure-Go alternative or fall back to baseline.

### Build rules

- **CMake is the top-level build system.** All targets (C, Rust, Go,
  tests, benchmarks, fixtures) must be buildable via CMake.
- `Cargo.toml` and `go.mod` remain as language-native metadata.
- CMake drives `cargo build` and `go build` as custom targets.
- `cmake -S . -B build && cmake --build build` must build everything.

### Performance rules

- SHM max throughput below 1M req/s is a bug — on both POSIX and Windows.
- Reference baselines from the old implementation (on this machine):
  - SHM ping-pong max: C→C ~3.2M, Rust→Rust ~2.9M req/s
  - SHM snapshot refresh max: C→C ~2.5M, Go→Go ~1.2M req/s
  - UDS ping-pong max: C→C ~220k, Rust→Rust ~240k req/s
  - UDS snapshot refresh max: C→C ~194k, Go→Go ~127k req/s
  - Negotiated SHM profile: ~2.8M req/s
  - Local cache lookup: C ~25M, Rust ~23M, Go ~13M lookups/s
- The new implementation must match or exceed these numbers.
- If performance regresses, investigate and fix before committing.

### Benchmark rules

- Benchmarks must cover the **full directed matrix**: all C/Rust/Go
  client-server pairs in both directions (9 pairs for 3 languages).
- Scenarios: max throughput, 100k/s rate-limited, 10k/s rate-limited.
- Each benchmark validates correctness (counter chain verification).
- Benchmark documents (`benchmarks-posix.md`, `benchmarks-windows.md`)
  are generated from complete runs, never hand-edited.

### Code style rules

- Mirror existing Netdata patterns in each language.
- Small files, small functions, single purpose.
- Comments explain "why", not "what".
- Type and function naming follows the Codec spec: `View`, `Builder`,
  `copy`/`materialize`/`to_owned`.
- No over-engineering. No feature flags. No backwards-compatibility
  shims. Just implement the spec.

## Repository Layout

```
src/
  libnetdata/netipc/          # C library
    include/netipc/           # C public headers
    src/
      protocol/               # Codec
      transport/
        posix/                # L1: UDS, SHM
        windows/              # L1: Named Pipe, SHM
      service/                # L2/L3

  crates/netipc/              # Rust crate
    src/
      protocol.rs             # Codec
      transport/
        posix.rs              # L1: UDS, SHM
        windows.rs            # L1: Named Pipe, SHM
      service/                # L2/L3

  go/pkg/netipc/              # Go package
    protocol/                 # Codec
    transport/
      posix/                  # L1: UDS, SHM
      windows/                # L1: Named Pipe, SHM
    service/                  # L2/L3

tests/                        # Test scripts and fixtures
bench/                        # Benchmark drivers and scripts
docs/                         # Specs (already written, do not modify)
```

## Phases

### Phase 0: Clean Slate
- **Goal**: Remove old implementation, set up build skeleton.
- **Actions**:
  - Delete all files under `src/` (old implementation)
  - Delete all files under `tests/` and `bench/` (old test/bench infra)
  - Keep `docs/` intact
  - Keep root build files: `CMakeLists.txt`, `Makefile`, `go.mod`,
    `go.sum`
  - Create empty directory structure per the layout above
  - Create minimal `CMakeLists.txt` that configures but builds nothing
  - Create minimal `Cargo.toml` for the Rust crate
  - Verify `go.mod` is correct
  - Ensure `cmake -S . -B build` succeeds
- **Validation**: clean configure, empty build succeeds.
- **Commit**: "Clean slate: remove old implementation, keep specs"

### Phase 1: Codec + L1 Wire Envelope in C
- **Goal**: Protocol encode/decode in C, matching the wire envelope spec.
- **Scope**:
  - Outer message header encode/decode (32 bytes)
  - Chunk continuation header encode/decode (32 bytes)
  - Batch item directory encode/decode
  - Hello payload encode/decode (44 bytes)
  - Hello-ack payload encode/decode (36 bytes)
  - Cgroups snapshot request encode/decode (4 bytes)
  - Cgroups snapshot response encode/decode (24-byte header + item
    directory + items with 32-byte item headers)
  - Cgroups snapshot response builder
  - All validation rules from the specs
- **Files**:
  - `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
- **Validation**:
  - Unit tests for every encode/decode path
  - Round-trip tests (encode then decode, verify match)
  - Validation rejection tests (truncated, out-of-bounds, missing NUL)
  - Compile-time assertions for struct sizes and field offsets
- **Commit**: "Phase 1: C Codec and L1 wire envelope"

### Phase 2: Codec + L1 Wire Envelope in Rust
- **Goal**: Same protocol encode/decode in Rust.
- **Scope**: Same as Phase 1, in Rust.
- **Files**:
  - `src/crates/netipc/src/protocol.rs`
- **Validation**:
  - Rust unit tests matching Phase 1 coverage
  - Round-trip tests
  - Validation rejection tests
  - File-based C↔Rust interop test: C encodes to file, Rust decodes
    (and vice versa)
- **Commit**: "Phase 2: Rust Codec and L1 wire envelope"

### Phase 3: Codec + L1 Wire Envelope in Go
- **Goal**: Same protocol encode/decode in Go.
- **Scope**: Same as Phase 1, in Go. Pure Go, no cgo.
- **Files**:
  - `src/go/pkg/netipc/protocol/frame.go`
- **Validation**:
  - Go unit tests matching Phase 1 coverage
  - Round-trip tests
  - Validation rejection tests
  - File-based interop test: full C↔Rust↔Go matrix
- **Commit**: "Phase 3: Go Codec and L1 wire envelope"

### Phase 4: Fuzz Testing for Codec
- **Goal**: Fuzz all decode paths in all languages.
- **Scope**:
  - Go: `go test -fuzz` targets for all decode functions
  - Rust: `cargo-fuzz` targets or proptest for all decode functions
  - C: libfuzzer or AFL harness for all decode functions
- **Validation**:
  - Each fuzz target runs for at least 30 seconds without crash/panic
  - Corpus includes: zero bytes, truncated headers, maximum-size
    payloads, corrupt offsets, missing NUL terminators
- **Commit**: "Phase 4: Fuzz testing for all Codec decode paths"

### Phase 5: L1 POSIX UDS Transport in C
- **Goal**: UDS SEQPACKET client/server transport in C.
- **Scope**:
  - Socket path derivation
  - Listener: bind, listen, accept multiple clients
  - Session: handshake (CONTROL messages with hello/hello-ack)
  - Send/receive with transparent chunking
  - Batch assembly/extraction
  - Message_id tracking
  - Stale endpoint recovery
  - Native fd exposure
  - Simple test client and server binaries
- **Files**:
  - `src/libnetdata/netipc/include/netipc/netipc_uds.h`
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`
  - `tests/fixtures/c/` — test client/server
- **Validation**:
  - Single client ping-pong (send request, receive response)
  - Multi-client concurrent sessions
  - Pipelining (send N requests, receive N responses out of order)
  - Batch send/receive
  - Chunking (message larger than packet size)
  - Handshake failure tests (bad auth, profile mismatch)
  - Stale socket recovery test
  - Disconnect with in-flight requests test
- **Commit**: "Phase 5: C POSIX UDS transport"

### Phase 6: L1 POSIX UDS in Rust
- **Goal**: Same UDS transport in Rust.
- **Scope**: Same as Phase 5, in Rust.
- **Validation**:
  - Same test coverage as Phase 5
  - Live C↔Rust interop: C client → Rust server, Rust client → C server
- **Commit**: "Phase 6: Rust POSIX UDS transport"

### Phase 7: L1 POSIX UDS in Go
- **Goal**: Same UDS transport in Go. Pure Go, no cgo.
- **Scope**: Same as Phase 5, in Go.
- **Validation**:
  - Same test coverage as Phase 5
  - Full C↔Rust↔Go live interop matrix (all 9 directed pairs)
- **Commit**: "Phase 7: Go POSIX UDS transport"

### Phase 8: L1 POSIX SHM in C
- **Goal**: Linux SHM transport in C using futex synchronization.
- **Scope**:
  - SHM region creation/destruction (64-byte header)
  - Variable-length request/response publication areas
  - Spin + futex wait/wake protocol
  - SHM file path derivation
  - Profile negotiation upgrade from UDS to SHM
  - Stale region recovery
- **Validation**:
  - SHM client/server round-trip
  - Profile negotiation: both support SHM → upgrades
  - Profile negotiation: only one supports SHM → stays UDS
  - SHM disconnect detection
  - Stale region recovery
- **Commit**: "Phase 8: C POSIX SHM transport"

### Phase 9: L1 POSIX SHM in Rust + Go
- **Goal**: Same SHM transport in Rust and Go.
- **Scope**: Same as Phase 8. Go uses pure-Go futex via raw syscall.
- **Validation**:
  - Same test coverage as Phase 8 in each language
  - Full C↔Rust↔Go SHM interop matrix
  - Negotiated profile interop (mixed SHM/UDS-only peers)
- **Commit**: "Phase 9: Rust and Go POSIX SHM transport"

### Phase 10: L2 in C
- **Goal**: Level 2 client context and managed server in C.
- **Scope**:
  - Client context: initialize, refresh, ready, status, close
  - Typed cgroups snapshot call (using Codec encode/decode)
  - At-least-once retry (must reconnect if previously READY)
  - Check transport_status before decode
  - Managed server: acceptor, fixed worker pool, typed callback
    dispatch, per-connection write serialization
  - Batch dispatch: split across workers, reassemble in order
  - Handler failure → INTERNAL_ERROR with empty payload
- **Validation**:
  - Client lifecycle tests (all state transitions)
  - Typed call success/failure/retry paths
  - Managed server with multiple concurrent clients
  - Batch dispatch correctness
  - Handler failure handling
- **Commit**: "Phase 10: C Level 2 orchestration"

### Phase 11: L2 in Rust + Go
- **Goal**: Same L2 in Rust and Go.
- **Validation**:
  - Same test coverage as Phase 10
  - Cross-language L2 matrix: C client → Rust server, Go client →
    C server, etc.
- **Commit**: "Phase 11: Rust and Go Level 2 orchestration"

### Phase 12: L3 Cache Helpers
- **Goal**: Client-side caching for snapshot services in all languages.
- **Scope**:
  - Refresh orchestration (uses L2 typed calls)
  - Local cache construction from snapshot response
  - Lookup by hash + name
  - Cache preservation on refresh failure
  - Status reporting (empty / populated)
- **Validation**:
  - Full snapshot round-trip: server (L2) → client (L3) → lookup
  - Refresh failure preserves cache
  - Reconnect after server restart rebuilds cache
  - Empty cache lookup returns not-found
  - Large dataset (1000+ items)
  - Cross-language: all producer/consumer pairs
- **Commit**: "Phase 12: Level 3 cache helpers"

### Phase 13: POSIX Benchmarks
- **Goal**: Performance validation on Linux. Full directed matrix.
- **Scope**:
  - UDS ping-pong: full 9-pair C/Rust/Go matrix at max, 100k/s, 10k/s
  - SHM ping-pong: full 9-pair C/Rust/Go matrix at max, 100k/s, 10k/s
  - Negotiated profile: UDS vs SHM comparison (Rust→Rust)
  - Snapshot refresh baseline: full 9-pair matrix at max, 1000/s
  - Snapshot refresh SHM: full 9-pair matrix at max, 1000/s
  - Local cache lookup: C, Rust, Go at max, 1000/s
  - Benchmark document generation
- **Performance floor** (below these = bug, investigate and fix):
  - SHM ping-pong max: ≥ 1M req/s for all pairs
  - SHM snapshot refresh max: ≥ 1M req/s for C/Rust pairs, ≥ 800k for Go pairs
  - UDS ping-pong max: ≥ 150k req/s for all pairs
  - UDS snapshot refresh max: ≥ 100k req/s for all pairs
  - Local cache lookup max: ≥ 10M lookups/s for all languages
- **Reference targets** (match or exceed the old implementation):
  - SHM ping-pong: C→C ~3.2M, Rust→Rust ~2.9M, Go→Go ~1.2M
  - SHM snapshot: C→C ~2.5M, Rust→Rust ~2.0M, Go→Go ~1.2M
  - UDS ping-pong: C→C ~220k, Rust→Rust ~240k, Go→Go ~164k
  - Negotiated SHM: ~2.8M
  - Local lookup: C ~25M, Rust ~23M, Go ~13M
- **Validation**:
  - All benchmarks pass correctness checks (counter chain verification)
  - All pairs meet the performance floor
  - `benchmarks-posix.md` generated from complete run
- **Commit**: "Phase 13: POSIX benchmarks"

### Phase 14: Windows L1 + Codec (Named Pipe)
- **Goal**: Windows Named Pipe transport in C, Rust, Go.
- **Scope**:
  - Pipe name derivation (FNV-1a hash)
  - Named Pipe client/server transport
  - Handshake over Named Pipe
  - Chunking
  - MSYS C build compatibility
- **Development**: local (Linux cross-check where possible)
- **Validation**: `ssh win11`, pull, compile, run tests
  - Same functional coverage as POSIX UDS phases
  - c-native, c-msys, rust-native, go-native interop matrix
- **Commit**: "Phase 14: Windows Named Pipe transport"

### Phase 15: Windows SHM
- **Goal**: Windows SHM transport in C, Rust, Go.
- **Scope**:
  - File mapping with 128-byte header
  - Auto-reset kernel events for SHM_HYBRID
  - Spin + WaitForSingleObject protocol
  - SHM_BUSYWAIT (pure spin) variant
  - Profile negotiation upgrade
- **Validation**: `ssh win11`
  - SHM interop matrix across all 4 implementations
  - Profile negotiation (SHM/baseline mixed peers)
- **Commit**: "Phase 15: Windows SHM transport"

### Phase 16: Windows L2 + L3 + Benchmarks
- **Goal**: Level 2, Level 3, and benchmarks on Windows.
- **Validation**: `ssh win11`
  - Full L2 cross-language matrix
  - L3 snapshot round-trip
  - Full directed benchmark matrix: c-native, c-msys, rust-native,
    go-native (all pairs, all scenarios)
  - `benchmarks-windows.md` generated from complete run
- **Performance floor** (below these = bug):
  - Windows SHM max: ≥ 1M req/s for C/Rust pairs
  - Windows Named Pipe max: baseline sanity, no regression from old
- **Commit**: "Phase 16: Windows L2, L3, and benchmarks"

## Completion Criteria

The rewrite is complete when:
- All 16 phases are committed and pushed
- All specs in `docs/` are satisfied by the implementation
- All cross-language interop tests pass on Linux and Windows
- All fuzz tests pass without crashes
- Benchmark documents are generated from complete runs
- All 4 AI reviewers agree on each phase
- Costa approves the final state
