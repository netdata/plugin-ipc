# SOW-0003 - Backport Netdata Go Vendor Cleanup

## Status

Status: completed

Sub-state: implementation and validation are complete; this SOW is closed with
the scoped source backport while unrelated staged repository setup files are
intentionally excluded.

## Requirements

### Purpose

Keep the upstream `plugin-ipc` repository and the Netdata vendored `netipc`
copy synchronized so that future vendor checks expose only real protocol,
transport, or API drift.

### User Request

The user asked whether to port Netdata-side Go maintainer cleanup changes back
to this repository so the vendored Netdata copy and upstream `plugin-ipc` are
in sync. The user selected option A: backport the 7-file Netdata diff into
`plugin-ipc`.

### Assistant Understanding

Facts:

- The project SOW system is initialized and source changes require a SOW.
- `diff-netdata-vendor.sh` is the project-local sync checker.
- The checker reports no C drift after excluding Netdata-local wrappers.
- The checker reports no Rust source drift after excluding workspace/package
  files.
- The checker reports 7 Go files differ after Netdata import paths are
  normalized.
- The Netdata-side drift is contained in `netdata/netdata` commit
  `151d78c63cc350a94aedc1d9b2dea58215671adb`.

Inferences:

- This is a safe, focused source sync if the exact Netdata diff is applied and
  the Go package tests plus the vendor diff pass.
- The changes are mostly language-modernization cleanup, not protocol contract
  changes.

Unknowns:

- No unresolved implementation or lifecycle unknowns remain after validation
  and closure.

### Acceptance Criteria

- The 7 Go files reported by `diff-netdata-vendor.sh` are synchronized with the
  Netdata vendored copy after import-path normalization.
- `cd src/go && go test ./...` passes.
- `bash ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git` reports
  no C, Rust, or Go source differences except the script's documented expected
  local exclusions.
- The SOW records validation, artifact maintenance, and follow-up mapping.

## Analysis

Sources checked:

- `AGENTS.md`
- `.agents/sow/SOW.template.md`
- `.agents/sow/pending/SOW-0002-20260501-root-todo-classification.md`
- `docs/code-organization.md`
- `docs/netipc-integrator-skill.md`
- `diff-netdata-vendor.sh`
- `TODO-netdata-master-backport.md`
- `src/go/go.mod`
- `netdata/netdata @ 1268945e2ca08a98e7d9eacae632d56d9e3c08d5`
- Go 1.21 release notes: `https://go.dev/doc/go1.21`
- Go 1.22 release notes: `https://go.dev/doc/go1.22`
- Go 1.25 release notes: `https://go.dev/doc/go1.25`
- Go specification: `https://go.dev/ref/spec`

Current state:

- `plugin-ipc` `src/go/go.mod` declares `go 1.25`.
- `netdata/netdata` `src/go/go.mod` declares `go 1.26.0`.
- Official Go documentation says `min` and `max` were added in Go 1.21.
- Official Go documentation says range-over-integer loops were added in Go
  1.22.
- Official Go documentation says `sync.WaitGroup.Go` was added in Go 1.25.
- The local toolchain is `go version go1.26.3-X:nodwarf5 linux/amd64`.

Risks:

- The backport uses newer Go syntax and library helpers; the project already
  requires Go 1.25, so the features are compatible with the declared upstream
  Go version.
- Test-only loop cleanup has low behavioral risk, but the production helper
  simplification in `service/raw/client.go` must be validated.
- This work does not change wire formats, transport framing, public service
  names, or cross-language protocol behavior.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The Netdata vendored Go tree received a cleanup commit after the prior
  upstream/vendor sync.
- The upstream `plugin-ipc` copy did not receive the same 7-file Go cleanup.
- Because `diff-netdata-vendor.sh` normalizes Netdata Go import paths before
  comparison, the remaining 7-file diff is real source drift rather than module
  path noise.

Evidence reviewed:

- `diff-netdata-vendor.sh` excludes Netdata-only C wrappers and Rust
  packaging/workspace files before comparing source trees.
- `diff-netdata-vendor.sh` rewrites Netdata Go imports from
  `github.com/netdata/netdata/go/plugins/pkg/netipc` to
  `github.com/netdata/plugin-ipc/go/pkg/netipc` before comparing Go files.
- The normalized diff reports these files:
  - `src/go/pkg/netipc/service/cgroups/cgroups_unix_test.go`
  - `src/go/pkg/netipc/service/raw/cache_test.go`
  - `src/go/pkg/netipc/service/raw/client.go`
  - `src/go/pkg/netipc/service/raw/client_test.go`
  - `src/go/pkg/netipc/service/raw/more_unix_test.go`
  - `src/go/pkg/netipc/service/raw/ping_pong_test.go`
  - `src/go/pkg/netipc/service/raw/stress_test.go`
- `netdata/netdata @ 1268945e2ca08a98e7d9eacae632d56d9e3c08d5` contains
  commit `151d78c63cc350a94aedc1d9b2dea58215671adb`, subject
  `feat(go.d): add vnode-scoped metrics for Azure Monitor workloads (#22402)`.
- That commit changes exactly the 7 Go files reported by the normalized sync
  checker.

Affected contracts and surfaces:

- Go source under `src/go/pkg/netipc/service/cgroups/` and
  `src/go/pkg/netipc/service/raw/`.
- Upstream/vendor synchronization workflow using `diff-netdata-vendor.sh`.
- No C API, Rust API, Go public API, wire format, transport name, or published
  documentation contract is expected to change.

Existing patterns to reuse:

- Use the project-local `diff-netdata-vendor.sh` checker rather than ad-hoc
  file comparison.
- Keep Netdata import paths out of upstream `plugin-ipc`; preserve the upstream
  module path in Go files.
- Keep the repository's mirrored Netdata destination layout described in
  `docs/code-organization.md`.

Risk and blast radius:

- Blast radius is limited to 7 Go files in the service layer and tests.
- Production code risk is limited to replacing explicit cap logic with `min`
  and a loop with range-over-integer in batch decode.
- Test code risk is limited to equivalent loop and goroutine helper
  modernization.
- Security risk is low because no auth, secrets, parsing trust boundary, or
  transport permissions are changed.
- Performance risk is low; no benchmark claim is being made or changed.

Sensitive data handling plan:

- Durable artifacts will not include secrets, credentials, bearer tokens, SNMP
  communities, customer names, customer identifiers, personal data,
  non-private customer-identifying IP addresses, private endpoints, or
  proprietary incident details.
- Evidence will cite repository paths, command names, commit hashes, and
  sanitized module paths only.
- No logs containing sensitive runtime data are needed for this backport.

Implementation plan:

1. Apply the exact normalized 7-file Go cleanup from the Netdata vendored tree
   to upstream `plugin-ipc`, preserving upstream Go import paths.
2. Run Go formatting if needed.
3. Run focused and sync validation.
4. Update this SOW with validation evidence and artifact maintenance results.

Validation plan:

- Run `cd src/go && go test ./...`.
- Run `bash ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git`.
- Search the SOW for deferred/follow-up wording before close.
- Check git status without staging unrelated files.

Artifact impact plan:

- AGENTS.md: no expected update; workflow rules are unchanged.
- Runtime project skills: no expected update; there are no runtime
  `project-*` skills and this does not create reusable workflow knowledge.
- Specs: no expected update; protocol behavior, wire formats, transports, and
  public API contracts are unchanged.
- End-user/operator docs: no expected update; this is internal source sync.
- End-user/operator skills: no expected update; integration guidance is
  unchanged.
- SOW lifecycle: this current SOW tracks the backport and will be completed or
  left current with evidence if validation fails.

Open-source reference evidence:

- `netdata/netdata @ 1268945e2ca08a98e7d9eacae632d56d9e3c08d5`
  - `src/go/pkg/netipc/service/cgroups/cgroups_unix_test.go`
  - `src/go/pkg/netipc/service/raw/cache_test.go`
  - `src/go/pkg/netipc/service/raw/client.go`
  - `src/go/pkg/netipc/service/raw/client_test.go`
  - `src/go/pkg/netipc/service/raw/more_unix_test.go`
  - `src/go/pkg/netipc/service/raw/ping_pong_test.go`
  - `src/go/pkg/netipc/service/raw/stress_test.go`

Open decisions:

- Resolved decision 1: user selected option A, backport the 7-file Netdata diff
  into `plugin-ipc`.

## Implications And Decisions

1. Backport strategy:
   - A. Backport the 7-file Netdata diff into `plugin-ipc`.
     - Pros: restores vendor sync, keeps maintainer cleanup, reduces future
       vendor diff noise.
     - Cons: requires validation because the diff uses newer Go constructs.
     - Implications: upstream will use `min`, range-over-integer loops, and
       `sync.WaitGroup.Go` where Netdata already does.
     - Risks: Go compatibility must be verified against the declared
       `plugin-ipc` `go 1.25` module version.
   - B. Do nothing.
     - Pros: no churn.
     - Cons: vendor drift remains.
     - Implications: future vendor sync checks remain noisy.
     - Risks: real future drift may be harder to identify.
   - C. Revert Netdata to match `plugin-ipc`.
     - Pros: removes drift.
     - Cons: drops the Netdata Go cleanup.
     - Implications: works against maintainer-owned Netdata cleanup.
     - Risks: likely creates unnecessary Netdata churn.
   - User selection: A.
   - Recommendation recorded before selection: A, because the diff is focused,
     compatible with Go 1.25 features, and preserves the maintainer cleanup.

## Plan

1. Backport the 7 Go file changes only.
2. Run Go formatting if file content requires it.
3. Run `cd src/go && go test ./...`.
4. Run `bash ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git`.
5. Update validation, artifact maintenance, lessons, and follow-up mapping.

## Execution Log

### 2026-05-24

- Created this SOW and recorded the user-selected backport decision before
  editing source files.
- Backported the 7 Go file changes from the Netdata vendored tree into
  upstream `plugin-ipc`, preserving upstream Go import paths.
- Ran `gofmt` on the changed Go files.
- Validated with `go test ./...` under `src/go`.
- Validated with `bash ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git`.
- Ran `git diff --check`.

## Validation

Acceptance criteria evidence:

- The 7 Go files reported by the initial normalized vendor diff were updated:
  - `src/go/pkg/netipc/service/cgroups/cgroups_unix_test.go`
  - `src/go/pkg/netipc/service/raw/cache_test.go`
  - `src/go/pkg/netipc/service/raw/client.go`
  - `src/go/pkg/netipc/service/raw/client_test.go`
  - `src/go/pkg/netipc/service/raw/more_unix_test.go`
  - `src/go/pkg/netipc/service/raw/ping_pong_test.go`
  - `src/go/pkg/netipc/service/raw/stress_test.go`
- `bash ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git` now
  reports:
  - C vendored library diff: `No differences.`
  - Rust vendored source diff: `No differences.`
  - Go vendored source diff after import normalization: `No differences.`

Tests or equivalent validation:

- `gofmt -w` completed on all changed Go files.
- `cd src/go && go test ./...` passed:
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/protocol`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw`
  - `github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix`
- `git diff --check` passed with no whitespace errors.

Real-use evidence:

- The project-local real-use sync checker was run against the local Netdata
  checkout and reported a clean normalized sync state.

Reviewer findings:

- No external reviewer was requested for this focused sync backport.

Same-failure scan:

- `rg` across the 7 changed files for the old modernization patterns found
  remaining explicit loops and `sync.WaitGroup` usage in unrelated sections of
  `src/go/pkg/netipc/service/raw/client.go`.
- The normalized vendor diff is clean, so those remaining patterns are not
  upstream/vendor drift and were not changed in this focused backport.

Sensitive data gate:

- Passed. This SOW and the code changes contain no raw secrets, credentials,
  bearer tokens, SNMP communities, customer names, customer identifiers,
  personal data, non-private customer-identifying IP addresses, private
  endpoints, or proprietary incident details.

Artifact maintenance gate:

- AGENTS.md: no update needed; repository workflow and guardrails are
  unchanged.
- Runtime project skills: no update needed; there are still no runtime
  `project-*` skills and this did not create reusable workflow knowledge.
- Specs: no update needed; protocol behavior, wire formats, transports, and
  public API contracts are unchanged.
- End-user/operator docs: no update needed; this is internal source
  synchronization with no operator-visible behavior change.
- End-user/operator skills: no update needed; `docs/netipc-integrator-skill.md`
  integration guidance is unchanged.
- SOW lifecycle: this SOW is marked `Status: completed` and moved to
  `.agents/sow/done/`. The scoped source changes and SOW lifecycle update are
  committed together, while unrelated staged repository setup files are
  intentionally excluded.

Specs update:

- No spec update needed. The change does not alter protocol behavior, public
  APIs, wire formats, transport behavior, data formats, operational guarantees,
  or known edge cases.

Project skills update:

- No runtime project skill update needed. No repeatable project workflow was
  discovered beyond the existing `diff-netdata-vendor.sh` sync checker.

End-user/operator docs update:

- No end-user/operator docs update needed. The change is a source-level sync of
  equivalent Go implementation style.

End-user/operator skills update:

- No end-user/operator skill update needed. The public integration workflow and
  examples are unchanged.

Lessons:

- Existing vendor sync scripts are sufficient to prove upstream/vendor parity
  after Go import-path normalization; no new project skill is needed for this
  narrow workflow.

Follow-up mapping:

- No functional follow-up remains.

## Outcome

The Netdata Go cleanup backport is implemented and validated. The vendored
Netdata copy and this repository are now in sync for C, Rust source, and Go
after the expected script exclusions and Go import-path normalization.

## Lessons Extracted

Use the project-local vendor diff script before and after any Netdata sync
backport. A clean normalized diff is the strongest acceptance signal for this
class of maintenance work.

## Followup

None.

## Regression Log

None yet.
