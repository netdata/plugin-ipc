# SOW-0024 - Netdata PR Scanner And CI Cleanup

## Status

Status: completed

Sub-state: source SDK scanner and Go toolchain fixes implemented, validated, and ready to vendor into Netdata PR 22729.

## Requirements

### Purpose

Keep the NetIPC SDK source authoritative while unblocking the downstream Netdata vendor-update PR from real scanner findings and Go toolchain normalization failures.

### User Request

The user asked to check PR comments and CI failures, then approved:

1. Fix the source plugin-ipc SDK first, push it, then re-vendor into Netdata.
2. Leave the Sonar duplication gate as-is for this iteration.
3. Treat the observed Windows CI failure as unrelated runner/toolchain failure unless later evidence proves a NetIPC-specific failure.

### Assistant Understanding

Facts:

- Netdata PR 22729 had no open GitHub review threads.
- Codacy reported two unused variables in the vendored Windows named-pipe handshake.
- SonarCloud reported two C reliability findings in the vendored apps lookup client and minor Go code-smell findings.
- Netdata Go CI failed because the workflow runs `go fix ./...` and the vendored Go code was not normalized for Netdata's current Go toolchain.
- The observed Windows CI failure happened after NetIPC built and failed because the runner did not have `link.exe` / Visual Studio available.

Inferences:

- Fixing the source SDK avoids a Netdata-only vendored patch that would be overwritten by the next vendor sync.
- The C lookup reliability finding is best addressed by making existing positive-count pointer invariants explicit, not by changing lookup semantics.
- The unused Windows batch variables are stale local computations; the protocol deliberately keeps response batch size symmetric with request batch size.

Unknowns:

- Whether Sonar duplication will still block the downstream PR after these fixes; the user explicitly selected leaving duplication unchanged for this iteration.

### Acceptance Criteria

- Source SDK removes the Codacy unused-variable pattern without changing handshake negotiation semantics.
- Source SDK makes the C lookup client positive-count pointer invariants explicit for apps and cgroups lookup.
- Source SDK Go code is normalized by `go fix ./...`.
- Same-failure scans find no remaining exact patterns from the addressed scanner findings.
- Focused C, Rust, and Go validation passes before committing.

## Analysis

Sources checked:

- `docs/level1-wire-envelope.md`
- `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`
- `src/libnetdata/netipc/src/transport/posix/netipc_uds_handshake.c`
- `src/libnetdata/netipc/src/service/netipc_service_apps_lookup.c`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_lookup.c`
- `src/go/pkg/netipc/service/raw/apps_lookup.go`
- `src/go/pkg/netipc/service/raw/cgroups_lookup.go`
- `src/go/pkg/netipc/protocol/lookup_guard_test.go`
- `src/go/pkg/netipc/service/raw/lookup_common_test.go`

Current state:

- `docs/level1-wire-envelope.md` specifies request/response batch-item symmetry for the current handshake.
- POSIX C, Rust, and Go handshakes already derive the agreed response batch item count from the client request batch item count.
- C apps and cgroups lookup clients already allocate arrays only for positive logical request counts, but the invariant was implicit inside the batching loop.
- The Go source needed normal Go toolchain rewrites such as integer-range loops, `min`, and `fmt.Appendf`.

Risks:

- Changing handshake negotiation would be a cross-language protocol behavior change. This SOW avoids that.
- Adding an early zero-item shortcut in C could skip endpoint interaction. This SOW avoids that and preserves the existing call flow.
- Go toolchain rewrites are mechanical but touch shared SDK code, so full Go package tests are required.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The downstream PR exposed three independent cleanup classes:
  - stale unused Windows handshake locals;
  - implicit C lookup invariants that confused static analysis;
  - source not normalized for the downstream Go toolchain's `go fix` step.

Evidence reviewed:

- `docs/level1-wire-envelope.md` records the handshake response-batch symmetry rule.
- POSIX C, Rust, and Go transport code implement the same batch symmetry.
- C lookup code rejects positive counts with null request arrays and allocates item buffers for positive counts before the batching loop.
- Netdata CI log showed `go fix ./...` produced diffs in vendored Go files.

Affected contracts and surfaces:

- Windows C named-pipe server handshake implementation.
- C apps and cgroups lookup client implementations.
- Go protocol/service/transport source formatting and toolchain normalization.
- Downstream Netdata vendored copy after re-vendor.

Existing patterns to reuse:

- Keep handshake response batch size symmetric with request batch size.
- Keep apps and cgroups lookup client logic symmetrical where their flow is equivalent.
- Use `go fix ./...` exactly as downstream CI does.

Risk and blast radius:

- Runtime risk is low because the C handshake edit removes unused locals only.
- Runtime risk is low for lookup invariants because normal positive-count paths already satisfy the new checks.
- Go diffs are mechanical toolchain rewrites; the blast radius is all touched Go SDK packages.

Sensitive data handling plan:

- Do not read `.env` or token files.
- Do not copy secrets, credentials, customer data, private endpoints, personal data, or proprietary operational details into durable artifacts.
- Scanner and CI evidence is summarized by file/rule class only.

Implementation plan:

1. Remove unused Windows handshake batch-default variables while preserving request/response batch symmetry.
2. Add explicit positive-count pointer invariants to C apps and cgroups lookup clients.
3. Run `go fix ./...` and manually remove the exact remaining unnecessary-variable patterns Sonar identified.
4. Validate C, Rust, and Go SDK paths.

Validation plan:

- `go test ./pkg/netipc/...`
- `go fix ./... && git diff --exit-code -- src/go/pkg/netipc`
- `cmake --build build`
- `/usr/bin/ctest --test-dir build --output-on-failure`
- `cargo test`
- Same-failure `rg` scan for the exact fixed patterns.

Artifact impact plan:

- AGENTS.md: no workflow or guardrail change expected.
- Runtime project skills: no reusable workflow change expected.
- Specs: no protocol or public API behavior change expected.
- End-user/operator docs: no operator behavior change expected.
- End-user/operator skills: no output/reference skill change expected.
- SOW lifecycle: complete this narrow SOW in the same commit as the source fixes.

Open-source reference evidence:

- No external open-source references were needed; the authoritative evidence was the local SDK source, docs, and downstream PR feedback.

Open decisions:

- Resolved by the user: fix source first and re-vendor; leave duplication unchanged; treat current Windows failure as unrelated infrastructure.

## Implications And Decisions

1. Source-first fix

- Decision: fix plugin-ipc and re-vendor to Netdata.
- Benefit: keeps the SDK authoritative.
- Risk: requires two commits/pushes, one in the SDK and one downstream.

2. Duplication gate

- Decision: do not change duplication scope or refactor duplicated tests in this iteration.
- Benefit: avoids mixing scanner policy/test refactors into a CI cleanup.
- Risk: downstream Sonar quality gate may still report duplication after these code fixes.

3. Windows CI

- Decision: do not change code or workflow for the observed Windows runner failure.
- Benefit: avoids unrelated infrastructure churn.
- Risk: the downstream PR remains blocked if the runner/toolchain problem persists.

## Plan

1. Patch C scanner findings and Go toolchain normalization in the SDK.
2. Validate source SDK.
3. Commit and push the SDK.
4. Re-vendor the new SDK commit into Netdata PR 22729.

## Execution Log

### 2026-06-15

- Removed unused Windows named-pipe server handshake batch-default variables.
- Added explicit positive-count pointer invariants to C apps and cgroups lookup clients.
- Ran `go fix ./...` in the Go module and kept the generated source rewrites.
- Removed exact remaining unnecessary temporary variables in Go files identified by Sonar.
- Preserved the pre-existing dirty generated benchmark binary as uncommitted local state.

## Validation

Acceptance criteria evidence:

- The Windows handshake now has no `s_req_bat` or `s_resp_bat` locals.
- C lookup positive-count loops now explicitly require non-null request and result storage pointers.
- `go fix ./... && git diff --exit-code -- src/go/pkg/netipc` passed.

Tests or equivalent validation:

- `go test ./pkg/netipc/...` passed.
- `cmake --build build` passed.
- `/usr/bin/ctest --test-dir build --output-on-failure` passed: 48/48 tests.
- `cargo test` in `src/crates/netipc` passed: 375 Rust tests plus binary/doc-test targets.

Real-use evidence:

- Downstream re-vendor and PR CI recheck are handled after this SDK commit; no separate runtime service was started for this source-only cleanup.

Reviewer findings:

- No external reviewer was requested for this narrow scanner/CI cleanup.

Same-failure scan:

- `rg` found no remaining exact occurrences of `s_req_bat`, `s_resp_bat`, the specific `payloadExceededSuffixFits` temporary-variable pattern, or the specific `ensureLookupRequestCapacity` temporary-variable pattern under the touched SDK paths.

Sensitive data gate:

- No `.env`, token files, credentials, customer data, private endpoints, personal data, or proprietary operational details were read or copied into durable artifacts.

Artifact maintenance gate:

- AGENTS.md: no update needed; no workflow or guardrail changed.
- Runtime project skills: no update needed; no reusable workflow changed.
- Specs: no update needed; handshake semantics and lookup API behavior were preserved.
- End-user/operator docs: no update needed; no operator-visible behavior changed.
- End-user/operator skills: no update needed; no output/reference skill changed.
- SOW lifecycle: this SOW is completed and placed under `.agents/sow/done/` in the same commit as the source fixes.

Specs update:

- Not needed; no protocol, wire-format, public API, default, or documented behavior changed.

Project skills update:

- Not needed; no reusable project workflow changed.

End-user/operator docs update:

- Not needed; source cleanup only.

End-user/operator skills update:

- Not needed; no exported/operator skill changed.

Lessons:

- When downstream CI enforces a newer Go toolchain's `go fix`, the source SDK must be normalized before vendoring to avoid recurring downstream-only diffs.

Follow-up mapping:

- Downstream Netdata re-vendor is tracked in Netdata PR 22729 and the Netdata active vendor-update SOW.

## Outcome

Completed.

## Lessons Extracted

- Keep source SDK Go code normalized to the downstream Go toolchain before vendoring into Netdata.

## Followup

None.

## Regression Log

None yet.
