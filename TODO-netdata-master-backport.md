## Purpose

Fit-for-purpose goal: compare upstream `plugin-ipc` against the vendored
`netipc` trees in Netdata master at `~/src/netdata-ktsaou.git`, identify real
library deltas that may need backporting upstream, and provide a repeatable
diff script so future checks do not depend on ad-hoc manual commands.

## TL;DR

- Costa suspects that Netdata master may now contain vendored `netipc` changes
  that are not yet present upstream in `plugin-ipc`.
- We need a repeatable script that diffs all vendored trees:
  - C: `src/libnetdata/netipc`
  - Rust: `src/crates/netipc`
  - Go: `src/go/pkg/netipc`
- The script must separate expected Netdata-local differences from real library
  drift, especially:
  - Netdata-only C wrappers:
    - `netipc_netdata.c`
    - `netipc_netdata.h`
  - Rust workspace-local files:
    - `Cargo.toml`
    - `Cargo.lock`
  - Go import-path rewrites from upstream module paths to the Netdata module
    path

## Analysis

### Current repository facts

- Upstream repo:
  - `/home/costa/src/plugin-ipc.git`
- Netdata master checkout to compare against:
  - `/home/costa/src/netdata-ktsaou.git`
- Existing vendoring helper already exists at the upstream repo root:
  - `vendor-to-netdata.sh`
- Vendored trees in Netdata master are present at:
  - `src/libnetdata/netipc`
  - `src/crates/netipc`
  - `src/go/pkg/netipc`

### Current raw diff facts

- C raw diff:
  - no library-source drift was found in the common vendored C files
  - only Netdata-local wrapper files exist on the Netdata side:
    - `src/libnetdata/netipc/netipc_netdata.c`
    - `src/libnetdata/netipc/netipc_netdata.h`
- Rust raw diff:
  - no drift was found in the Rust `src/` tree
  - raw differences are repo-local packaging files and local build artifacts:
    - upstream-only `src/crates/netipc/Cargo.lock`
    - differing `src/crates/netipc/Cargo.toml`
    - upstream-only local directories:
      - `src/crates/netipc/Testing`
      - `src/crates/netipc/target`
- Go raw diff:
  - many files differ
  - sampled evidence shows at least two categories of change:
    - expected Netdata-specific import-path rewrite noise, for example:
      - upstream:
        - `github.com/netdata/plugin-ipc/go/pkg/netipc/...`
      - Netdata:
        - `github.com/netdata/netdata/go/plugins/pkg/netipc/...`
    - at least one apparent substantive code change, for example:
      - `src/go/pkg/netipc/protocol/frame.go`
      - `for i := 0; i < count; i++ {`
      - vs
      - `for i := range count {`

### Current normalized diff facts

- Added helper script at the upstream repo root:
  - `diff-netdata-vendor.sh`
- The script:
  - excludes Netdata-only C wrapper files
  - excludes Rust workspace/package files from source-drift comparison
  - normalizes Netdata Go import paths before comparing vendored Go sources
- Current normalized result against `~/src/netdata-ktsaou.git`:
  - C vendored library diff:
    - no differences
  - Rust vendored source diff:
    - no differences
  - Go vendored source diff:
    - exactly 7 files differ:
      - `src/go/pkg/netipc/protocol/codec_edge_test.go`
      - `src/go/pkg/netipc/protocol/frame.go`
      - `src/go/pkg/netipc/protocol/frame_test.go`
      - `src/go/pkg/netipc/transport/posix/shm_linux.go`
      - `src/go/pkg/netipc/transport/posix/shm_linux_test.go`
      - `src/go/pkg/netipc/transport/posix/uds.go`
      - `src/go/pkg/netipc/transport/posix/uds_test.go`

### Netdata-side git history facts

- Netdata master contains a single recent commit that explains the 7-file Go
  drift:
  - commit:
    - `9e9d16ac849853fde77269a898b211058da41e99`
  - subject:
    - `chore(go): go fix (#22248)`
  - date:
    - `2026-04-22 12:10:07 +0300`
- The patch is limited to those same 7 Go files.
- Verified content patterns in that commit:
  - integer loop rewrites:
    - `for i := 0; i < count; i++` -> `for i := range count`
  - helper rewrites:
    - explicit `if`-based min/max logic -> built-in `min()` / `max()`
  - goroutine helper rewrites in tests:
    - `wg.Add(1); go func(){ defer wg.Done() ... }()` -> `wg.Go(func() { ... })`
  - string buffer rewrites in tests:
    - `[]byte(fmt.Sprintf(...))` -> `fmt.Appendf(nil, ...)`

### Interpretation

- Fact:
  - this is not a new C or Rust vendored library change on Netdata master
- Fact:
  - this is not a broad Go library divergence either
- Fact:
  - the current Netdata-side drift is one Go-only cleanup/modernization commit
    affecting 7 files
- Open question for later backport work:
  - whether upstream `plugin-ipc` should adopt this `go fix` cleanup as-is, or
    keep the current explicit style

### Implications

- A naive raw `diff -rq` is not enough for Go because it overreports expected
  import-path rewrites.
- The useful comparison must normalize those path rewrites before reporting
  substantive Go drift.
- We need one repeatable script at the upstream repo root, next to
  `vendor-to-netdata.sh`, because this is the same maintenance workflow.

## User Decisions

### Made

- Netdata master to compare against:
  - `~/src/netdata-ktsaou.git`
- Deliverable:
  - create a script that diffs all vendored files
  - run it to identify current differences

### Pending

- None at this stage.

## Plan

1. Add a root helper script in `plugin-ipc` that:
   - compares vendored C, Rust, and Go trees against a Netdata checkout
   - separates expected Netdata-local differences from real drift
   - normalizes Go import-path rewrites before the substantive comparison
2. Run the script against `~/src/netdata-ktsaou.git`.
3. Report:
   - the expected Netdata-local differences
   - the real vendored-library differences
   - any files that may need backporting upstream
4. If Costa wants the Netdata Go cleanup upstreamed, backport commit
   `9e9d16ac849853fde77269a898b211058da41e99` into the upstream Go tree and
   then rerun the diff script to confirm the vendored trees match again.

## Implied Decisions

- The script should live at the upstream repo root, mirroring the existing
  `vendor-to-netdata.sh` workflow helper.
- The script should default to `~/src/netdata-ktsaou.git`, but accept a custom
  Netdata checkout path as an argument.

## Testing Requirements

- Run the script against the default Netdata master checkout:
  - `~/src/netdata-ktsaou.git`
- Verify that the script:
  - reports the expected C wrapper files separately
  - reports Rust workspace/package drift separately from Rust source drift
  - normalizes Go import-path rewrites and reports the remaining substantive Go
    diffs

## Documentation Updates Required

- None expected.
