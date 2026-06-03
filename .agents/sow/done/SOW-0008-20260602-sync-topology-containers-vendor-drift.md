# SOW-0008 - Sync Topology Containers Vendor Drift

## Status

Status: completed

Sub-state: reopened regression, repair, validation, artifact maintenance, and
follow-up mapping are complete.

## Requirements

### Purpose

Keep the upstream `plugin-ipc` repository synchronized with the Netdata vendored
`netipc` copy used by the topology containers work, so future vendor checks show
only meaningful drift and not already-reviewed downstream fixes.

### User Request

The user asked to review the source differences found by
`diff-netdata-vendor.sh` against `~/src/PRs/topology-containers` and bring the
reviewed change into this repository.

### Assistant Understanding

Facts:

- This project uses an initialized SOW system, and source changes with
  behavioral or maintenance impact require a SOW.
- `diff-netdata-vendor.sh` is the project-local comparison script for upstream
  `plugin-ipc` versus Netdata's vendored `netipc` tree.
- The normalized vendor diff reports C and Go source drift only.
- Rust vendored source has no drift after expected workspace/package exclusions.
- The downstream Netdata files are committed and clean in the checked
  `topology-containers` tree.
- The upstream Go module declares `go 1.25`.
- Official Go documentation records range-over-integer support in Go 1.22 and
  `sync.WaitGroup.Go` in Go 1.25.

Inferences:

- The requested change is a vendor-sync backport from the downstream Netdata
  tree into upstream `plugin-ipc`.
- The safe implementation is to apply only the source drift reported after the
  script's expected exclusions and Go import-path normalization.
- The C changes are internal refactors and retry robustness changes; they do not
  alter wire formats or public API contracts.
- The Go changes are source modernization and test cleanup compatible with the
  upstream module's declared Go version.

Unknowns:

- No implementation-blocking unknowns remain after review.

### Acceptance Criteria

- The four files reported by `diff-netdata-vendor.sh` are synchronized with the
  Netdata vendored copy after expected import-path normalization.
- C and Go validation covering the touched surfaces passes.
- `./diff-netdata-vendor.sh ~/src/PRs/topology-containers` reports no C, Rust,
  or Go source differences except the script's documented expected local
  exclusions.
- The SOW records validation, same-failure scan, artifact maintenance, and
  follow-up mapping before completion.

## Analysis

Sources checked:

- `AGENTS.md`
- `.agents/sow/SOW.template.md`
- `.agents/sow/done/SOW-0003-20260524-backport-netdata-go-vendor-cleanup.md`
- `.agents/sow/done/SOW-0006-20260528-shm-lookup-regression-port.md`
- `docs/code-organization.md`
- `docs/codec-apps-lookup.md`
- `docs/codec-cgroups-lookup.md`
- `docs/level2-typed-api.md`
- `docs/netipc-integrator-skill.md`
- `diff-netdata-vendor.sh`
- `src/go/go.mod`
- `netdata/netdata @ 97cd18c03c26ab8ea3fbc400029409e447861b21`
- Go 1.22 release notes: `https://go.dev/doc/go1.22`
- Go 1.25 release notes: `https://go.dev/doc/go1.25`
- Go `sync` package docs: `https://pkg.go.dev/sync`

Current state:

- `git status --short` shows unrelated untracked files:
  - `TODO-perf-parity.md`
  - `tea_debug.log`
- No current SOW file existed before this work.
- No runtime project skills exist under `.agents/skills/`.
- `src/go/go.mod` declares `go 1.25`.
- `netdata/netdata @ 97cd18c03c26ab8ea3fbc400029409e447861b21`
  has `src/go/go.mod` declaring `go 1.26.0`.
- `./diff-netdata-vendor.sh ~/src/PRs/topology-containers --unified` reports:
  - C drift in `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - C drift in `src/libnetdata/netipc/src/service/netipc_service.c`
  - Go drift in `src/go/pkg/netipc/protocol/lookup.go`
  - Go drift in `src/go/pkg/netipc/service/raw/lookup_unix_test.go`
  - no Rust source drift
- The most relevant downstream commits are:
  - `367758427f` - `Vendor NetIPC lookup protocol fixes`
  - `98134c938e` - `fix topology container review issues`
  - `81fb92400f` - `fix topology PR review and CI issues`

Risks:

- Copying beyond the normalized four-file drift would pull unrelated Netdata
  changes into upstream.
- The Go test change uses `sync.WaitGroup.Go`; this would be unsafe for a module
  targeting Go before 1.25, but this module already declares `go 1.25` and
  existing upstream tests already use `wg.Go`.
- The C sleep wrapper changes timing behavior only under signal interruption;
  validation must cover service tests because reconnect and shutdown polling use
  these waits.
- The C protocol helper reduces duplicated overflow code; validation must cover
  lookup codec and service paths to confirm no layout behavior changed.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Downstream Netdata received small review/CI fixes after the last upstream
  `plugin-ipc` sync.
- The upstream repository did not receive those same changes yet.
- The project-local diff script normalizes expected Netdata-local differences:
  C wrappers, Rust workspace/package files, and Go import paths.
- Therefore the remaining four-file diff is real source drift, not expected
  vendoring noise.

Evidence reviewed:

- `diff-netdata-vendor.sh` excludes `netipc_netdata.c` and
  `netipc_netdata.h` before comparing C source trees.
- `diff-netdata-vendor.sh` excludes Rust `Cargo.toml`, `Cargo.lock`,
  `target/`, and `Testing/` before comparing Rust source trees.
- `diff-netdata-vendor.sh` rewrites Netdata Go imports from
  `github.com/netdata/netdata/go/plugins/pkg/netipc` to
  `github.com/netdata/plugin-ipc/go/pkg/netipc` before comparing Go files.
- The C protocol diff adds `lookup_label_storage_add_u64()` and replaces two
  duplicated label storage size calculations in cgroups/apps lookup builders.
- The C service diff adds `service_sleep_us()` and replaces direct `usleep()`
  calls in SHM attach retry, reconnect retry, accept retry, and shutdown drain
  polling paths.
- The Go protocol diff replaces several counted loops with range-over-integer
  loops and adds a short comment plus `//NOSONAR` on the long
  `AppsLookupBuilder.Add` signature.
- The Go service test diff replaces manual `wg.Add(1)` / goroutine /
  `defer wg.Done()` sequences with `wg.Go()` in concurrent lookup tests.
- Official Go 1.22 documentation says `for` loops may range over integers.
- Official Go 1.25 documentation says `sync.WaitGroup.Go` was added in Go
  1.25.
- Existing upstream files already use range-over-integer loops and `wg.Go()` in
  other Go test files.

Affected contracts and surfaces:

- C codec implementation:
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
- C POSIX service implementation:
  - `src/libnetdata/netipc/src/service/netipc_service.c`
- Go lookup codec implementation:
  - `src/go/pkg/netipc/protocol/lookup.go`
- Go POSIX raw service lookup tests:
  - `src/go/pkg/netipc/service/raw/lookup_unix_test.go`
- Vendor synchronization workflow:
  - `diff-netdata-vendor.sh`
- Public C/Rust/Go API contracts, wire formats, service methods, transport
  negotiation, and docs are not expected to change.

Existing patterns to reuse:

- Use `diff-netdata-vendor.sh` as the authoritative sync checker, as done in
  SOW-0003.
- Preserve upstream Go import paths rather than copying Netdata module paths.
- Keep C codec helper changes in the codec layer and service sleep retry
  changes in the service layer, matching `docs/code-organization.md`.
- Use local validation commands listed in `AGENTS.md`.

Risk and blast radius:

- Blast radius is limited to two C source files, one Go codec source file, one
  Go test file, and this SOW.
- Protocol compatibility risk is low because the C and Go codec edits do not
  change constants, item layout, field encoding, or validation rules.
- Runtime service risk is low but non-zero because replacing `usleep()` with
  `nanosleep()` plus `EINTR` retry changes behavior when signals interrupt
  service waits.
- Go compatibility risk is low because the upstream module targets Go 1.25 and
  already uses the same language/library features.
- Security risk is low because the work does not touch authentication,
  credentials, filesystem permissions, socket naming rules, or trust-boundary
  parsing policy.

Sensitive data handling plan:

- Durable artifacts will not include raw secrets, credentials, bearer tokens,
  SNMP communities, customer names, customer identifiers, personal data,
  non-private customer-identifying IP addresses, private endpoints, or
  proprietary incident details.
- Evidence will use repository paths, line numbers, commit hashes, command
  names, and sanitized module paths only.
- No live production data or logs are required.

Implementation plan:

1. Apply the normalized four-file downstream diff into upstream `plugin-ipc`,
   preserving upstream Go import paths.
2. Run formatting if the copied Go files require it.
3. Run focused C and Go validation, then the full project sync diff against
   `topology-containers`.
4. Update this SOW with execution, validation, artifact maintenance, lessons,
   and follow-up mapping.

Validation plan:

- Run `cmake --build build --target test_protocol test_service` if the build
  directory is configured.
- Run `/usr/bin/ctest --test-dir build -R '^(test_protocol|test_service)$' --output-on-failure`
  if the build directory is configured.
- Run `cd src/go && go test ./...`.
- Run `./diff-netdata-vendor.sh ~/src/PRs/topology-containers`.
- Search for remaining same drift using the diff script and targeted `rg`.
- Search the SOW for deferred/follow-up wording before close.
- Check git status and avoid staging unrelated untracked files.

Artifact impact plan:

- AGENTS.md: no source behavior update expected; update command notes if
  validation reveals reusable workflow sequencing knowledge.
- Runtime project skills: no expected update; no runtime `project-*` skills
  exist, and this sync does not create reusable workflow knowledge beyond the
  existing diff script pattern.
- Specs: no expected update; public protocol behavior, wire formats, transport
  behavior, APIs, and operational guarantees are unchanged.
- End-user/operator docs: no expected update; this is internal source sync.
- End-user/operator skills: no expected update; integration guidance is
  unchanged.
- SOW lifecycle: this SOW is the single current SOW for the work and will move
  to `.agents/sow/done/` with `Status: completed` only after validation passes.

Open-source reference evidence:

- `netdata/netdata @ 97cd18c03c26ab8ea3fbc400029409e447861b21`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/libnetdata/netipc/src/service/netipc_service.c`
  - `src/go/pkg/netipc/protocol/lookup.go`
  - `src/go/pkg/netipc/service/raw/lookup_unix_test.go`

Open decisions:

- Resolved decision 1: the user requested bringing the reviewed downstream
  change into this repository, so implementation will apply only the verified
  normalized vendor drift.

## Implications And Decisions

1. Vendor sync scope:
   - A. Apply only the normalized four-file drift reported by
     `diff-netdata-vendor.sh`.
     - Pros: restores sync with minimal blast radius; avoids unrelated Netdata
       changes; matches the project-local workflow.
     - Cons: does not import any unrelated downstream improvements that the
       diff script does not report.
     - Implications: validation can prove this exact sync and the final diff
       script should report no source drift.
     - Risks: if Netdata has related but unreported changes outside vendored
       source directories, they remain out of scope.
   - B. Copy broader Netdata commits that touched these files.
     - Pros: preserves original downstream commit context.
     - Cons: risks pulling unrelated tree changes into upstream; harder to
       prove with the vendor diff script.
     - Implications: broader review and validation would be required.
     - Risks: hidden behavior changes outside the requested vendor sync.

Selected: A.

Reasoning:

- The user asked to review the changes reported by the diff script and bring
  the change here.
- The normalized diff is the evidence-backed scope.
- Prior SOW-0003 used the same sync strategy successfully.

## Plan

1. Apply the four-file normalized vendor drift.
2. Run focused C and Go validation.
3. Re-run the vendor diff script against `topology-containers`.
4. Close the SOW with validation and artifact gates if clean.

## Execution Log

### 2026-06-02

- Created this SOW in `.agents/sow/current/`.
- Reviewed the normalized vendor diff against `topology-containers`.
- Confirmed upstream Go target is `go 1.25` and the local toolchain is
  `go1.26.3-X:nodwarf5`.
- Applied the normalized vendor drift to:
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/libnetdata/netipc/src/service/netipc_service.c`
  - `src/go/pkg/netipc/protocol/lookup.go`
  - `src/go/pkg/netipc/service/raw/lookup_unix_test.go`
- Formatted the touched Go files with `gofmt`.
- Updated `AGENTS.md` with the discovered validation sequencing rule:
  `tests/interop_codec.sh` configures `build/`, so it must not run in parallel
  with CTest or another build-directory validation command.

## Validation

Acceptance criteria evidence:

- `./diff-netdata-vendor.sh ~/src/PRs/topology-containers` reports:
  - C vendored library diff: no differences.
  - Rust vendored source diff: no differences.
  - Go vendored source diff after import normalization: no differences.
- The four reported drift files are synchronized with the Netdata vendored copy
  after the script's expected exclusions and Go import normalization.
- Artifact maintenance, same-failure scan, and SOW lifecycle checks are recorded
  below.

Tests or equivalent validation:

- `gofmt -w src/go/pkg/netipc/protocol/lookup.go src/go/pkg/netipc/service/raw/lookup_unix_test.go`
  completed.
- `git diff --check` passed.
- `cmake --build build --target test_protocol test_service` passed.
- `/usr/bin/ctest --test-dir build -R '^(test_protocol|test_service)$' --output-on-failure`
  passed: 2/2 tests.
- `go test ./...` from `src/go` passed:
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/protocol`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix`
  - packages with no test files reported as such.
- `bash tests/interop_codec.sh` passed:
  - C, Rust, and Go codec binaries built.
  - Cross-decode results were clean.
  - Byte-identical C/Rust/Go comparisons matched.
- `/usr/bin/ctest --test-dir build --output-on-failure` first passed 46/46
  tests, but overlapped with `tests/interop_codec.sh` reconfiguring `build/`.
- `/usr/bin/ctest --test-dir build --output-on-failure` was rerun cleanly and
  passed 46/46 tests in 449.14 seconds.
- `bash .agents/sow/audit.sh` passed before SOW move:
  - SOW framework clean.
  - current SOW gate present.
  - sensitive-data guardrail clean.

Real-use evidence:

- The project-local real-use sync path was exercised by
  `./diff-netdata-vendor.sh ~/src/PRs/topology-containers`; it now reports no
  source drift in C, Rust, or Go after expected Netdata-local normalization.

Reviewer findings:

- Self-review findings before implementation:
  - No blocking issue in the C protocol helper refactor; it preserves the same
    checked additions and overflow result.
  - No blocking issue in the C sleep wrapper; it preserves requested wait
    durations and improves signal interruption handling.
  - No Go compatibility blocker because the upstream module declares Go 1.25
    and the touched Go features are Go 1.22 and Go 1.25 features.
- Post-implementation self-review:
  - The final normalized vendor diff is clean.
  - Full CTest and Go validation passed.
  - No external reviewers were run because the user did not request external
    assistant review.

Same-failure scan:

- `rg -n "usleep\(|add_u64_over_limit\(item_size_u64, labels\[i\]|wg\.Add\(1\)|defer wg\.Done\(\)" src/libnetdata/netipc/src src/go/pkg/netipc/protocol/lookup.go src/go/pkg/netipc/service/raw/lookup_unix_test.go`
  returned no matches.
- The scan found no remaining direct `usleep()` calls in the C netipc source
  tree, no remaining duplicated label storage overflow pattern in the touched C
  protocol builders, and no remaining manual `WaitGroup` add/done pattern in
  the touched Go lookup test.

Sensitive data gate:

- Durable artifacts contain no raw secrets, credentials, bearer tokens, SNMP
  communities, customer names, customer identifiers, personal data, non-private
  customer-identifying IP addresses, private endpoints, or proprietary incident
  details.
- Evidence uses repository paths, command names, commit hashes, and sanitized
  module paths only.

Artifact maintenance gate:

- AGENTS.md: updated with the validation sequencing note for
  `tests/interop_codec.sh` and CTest/build-directory commands.
- Runtime project skills: no update needed; there are still no runtime
  `project-*` skills, and the reusable workflow detail was small enough for the
  project command list in `AGENTS.md`.
- Specs: no update needed; no protocol behavior, public API contract, wire
  format, transport behavior, data format, operational guarantee, or known edge
  case changed.
- End-user/operator docs: no update needed; the work is internal source sync
  plus project-local workflow guidance.
- End-user/operator skills: no update needed; `docs/netipc-integrator-skill.md`
  integration guidance is unchanged.
- SOW lifecycle: status set to `completed`; this SOW is ready to move from
  `.agents/sow/current/` to `.agents/sow/done/` with the implementation and
  artifact updates.

Specs update:

- No spec update needed; the changes do not alter protocol behavior, public API
  contracts, wire formats, transport behavior, data formats, operational
  guarantees, or known edge cases.

Project skills update:

- No runtime project skill update needed; no matching runtime project skills
  exist, and the only reusable workflow detail was recorded in `AGENTS.md`.

End-user/operator docs update:

- No end-user/operator docs update needed; this work does not change published
  commands, APIs, examples, operating procedure for integrators, or user-facing
  behavior.

End-user/operator skills update:

- No end-user/operator skill update needed; the exported integrator skill does
  not cover this internal vendor-sync workflow and no public integration
  behavior changed.

Lessons:

- `tests/interop_codec.sh` configures `build/`; do not run it in parallel with
  CTest or another build-directory validation command.

Follow-up mapping:

- No follow-up work is required for this SOW.
- The only discovered workflow lesson was implemented immediately in
  `AGENTS.md`.

## Outcome

Completed. The upstream repository now matches the `topology-containers`
vendored NetIPC source after the script's expected exclusions and import-path
normalization.

## Lessons Extracted

- Keep `tests/interop_codec.sh` serialized with CTest/build validation because
  it reconfigures the shared `build/` directory.

## Followup

None.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and
later testing or use found broken behavior. Use a dated `## Regression -
YYYY-MM-DD` heading at the end of the file. Never prepend regression content
above the original SOW narrative.

## Regression - 2026-06-02

Status: completed

What broke:

- A new vendor diff appeared after the SOW was completed.
- The drift is limited to
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c`.
- The C apps lookup static assertion still depended on the naturally padded
  `sizeof(nipc_apps_lookup_item_wire_t) == 64u` instead of asserting the fixed
  60-byte wire header boundary directly.

Evidence:

- `./diff-netdata-vendor.sh <topology-containers-tree> --unified` reported
  only this C file after expected exclusions and Go import normalization.
- `ktsaou/netdata @ 8b652640fdeea7533bf81789d5119073c82b3b61`
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c` asserts that
  `reserved1` ends at `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE` and that the C struct
  covers at least that fixed header size.
- `docs/codec-apps-lookup.md` records `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE: 60`
  and explicitly says C must use explicit wire-size constants rather than
  `sizeof(struct)` as the apps item wire length.
- `src/libnetdata/netipc/include/netipc/netipc_protocol.h` defines
  `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE` as `60u`.

Why previous validation missed it:

- Previous validation was correct for the checked downstream revision at SOW
  completion time.
- The checked topology containers tree later moved to
  `8b652640fdeea7533bf81789d5119073c82b3b61`, adding this narrower static
  assertion.

Repair plan:

1. Replace the padded `sizeof == 64u` assertion with explicit fixed-header
   boundary and minimum-coverage assertions.
2. Run focused C protocol validation.
3. Re-run the vendor diff script against the checked topology containers tree.
4. Close this regression only after validation and artifact maintenance are
   recorded.

Validation plan:

- Build `test_protocol` if the existing build tree can compile it.
- Run `ctest --test-dir build -R '^test_protocol$' --output-on-failure`.
- Run `./diff-netdata-vendor.sh <topology-containers-tree>`.
- Check SOW status/directory consistency and unrelated worktree changes.

Artifact updates needed:

- AGENTS.md: no update expected; this does not change project workflow.
- Runtime project skills: no update expected; no runtime `project-*` skills
  exist.
- Specs: no update expected; `docs/codec-apps-lookup.md` already records the
  exact wire-size rule this change enforces.
- End-user/operator docs: no update expected; this is internal assertion
  hygiene with no public behavior change.
- End-user/operator skills: no update expected; public integration guidance is
  unchanged.
- SOW lifecycle: this SOW was reopened in `.agents/sow/current/`, validated,
  marked `Status: completed`, and moved back to `.agents/sow/done/`.

Repair performed:

- Replaced the padded `sizeof(nipc_apps_lookup_item_wire_t) == 64u` assertion
  with:
  - an assertion that `reserved1` ends at `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE`
    byte 60;
  - an assertion that the C struct covers at least the fixed wire header size.

Validation results:

- `cmake --build build --target test_protocol`: passed.
- `ctest --test-dir build -R '^test_protocol$' --output-on-failure`: failed
  before running tests because `~/.local/bin/ctest` is a Python wrapper
  missing the `cmake` module.
- `/usr/bin/ctest --test-dir build -R '^test_protocol$' --output-on-failure`:
  passed; `test_protocol` passed.
- `./diff-netdata-vendor.sh <topology-containers-tree>`: passed; no C, Rust,
  or Go source differences remain after expected exclusions and Go import-path
  normalization.
- Same-failure search:
  `rg -n "sizeof\\(nipc_apps_lookup_item_wire_t\\)|fixed wire header must end|struct must cover the fixed wire header|NIPC_APPS_LOOKUP_ITEM_HDR_SIZE" src/libnetdata/netipc/src/protocol/netipc_protocol.c docs/codec-apps-lookup.md src/libnetdata/netipc/include/netipc/netipc_protocol.h`
  confirmed the old exact `sizeof` assertion is gone and the implementation,
  public header constant, and spec agree on the 60-byte wire header.

Artifact maintenance gate:

- AGENTS.md: no update needed; validation surfaced a local PATH wrapper issue,
  not a reusable project workflow change.
- Runtime project skills: no update needed; no runtime `project-*` skills
  exist.
- Specs: no update needed; `docs/codec-apps-lookup.md` already requires
  explicit wire-size constants and records `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE` as
  60.
- End-user/operator docs: no update needed; this changes an internal C compile
  assertion only.
- End-user/operator skills: no update needed; public integration guidance is
  unchanged.
- SOW lifecycle: reopened from `.agents/sow/done/` to `.agents/sow/current/`,
  marked `Status: completed` after validation, and moved back to
  `.agents/sow/done/` with the source fix.

Follow-up mapping:

- No follow-up work is required.
- The `ctest` PATH wrapper failure is unrelated to this source change; the
  validation was completed with `/usr/bin/ctest`, so no source follow-up is
  required for this SOW.

Outcome:

- Completed. The upstream repository again matches the checked topology
  containers vendored NetIPC source after expected exclusions and Go import-path
  normalization.
