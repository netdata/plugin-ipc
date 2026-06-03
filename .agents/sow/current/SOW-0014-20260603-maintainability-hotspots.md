# SOW-0014 - Maintainability Hotspots

## Status

Status: in-progress

Sub-state: implementing the approved first remediation target: Go raw L3 cache duplication.

## Requirements

### Purpose

Improve repository hygiene by reducing real source complexity and duplication without weakening useful Codacy maintainability signals.

### User Request

The user selected keeping complexity and duplication metrics active and treating real hotspots as source remediation work, then approved proceeding with SOW-0014.

### Assistant Understanding

Facts:

- Codacy reports repository-level complexity and duplication percentages even when the current issue backlog is zero.
- Local Lizard and JSCPD approximations found real maintainability pressure in source code, not only test or fixture scope.
- Coverage upload work is tracked separately in `SOW-0013`.
- No runtime project skills exist for this repository.
- No SOW specs exist beyond `.agents/sow/specs/.gitkeep`; public docs under `docs/` remain the relevant contract surface.
- `docs/code-organization.md` requires preserving protocol, transport, and service layer boundaries.

Inferences:

- Some duplication is intentional cross-language parity or POSIX/Windows parity and should not be removed mechanically.
- The useful remediation target is high-risk source duplication and complexity that obscures protocol, transport, or service behavior.

Unknowns:

- Which hotspots are worth changing after maintainers evaluate protocol clarity, parity, and performance tradeoffs.

### Acceptance Criteria

- Identify source complexity and duplication hotspots with file-level evidence.
- Separate intentional parity duplication from avoidable implementation duplication.
- Propose user decisions before refactoring protocol, transport, or service code.
- Reduce selected real hotspots without changing public protocol/API behavior.
- Validate C, Rust, and Go tests and relevant interoperability scripts after changes.

## Analysis

Sources checked:

- `SOW-0013` local Lizard and JSCPD findings.
- `docs/code-organization.md`
- `docs/level2-typed-api.md`
- `.codacy/codacy.config.json`
- Codacy local/cloud skill guidance.

Current state:

- Highest local source complexity appeared in lookup builders/decoders, service session loops, and transport send/receive paths.
- Largest local source duplicate groups included POSIX/Windows wrapper parity, typed lookup service wrappers, and language/service parity.
- Codacy Cloud for commit `655eb62e1aa7395e3a8ec70728e7215c77306948` reports:
  - issues: 0.
  - coverage: 88%.
  - complex files: 43%.
  - duplicated files: 46%.
  - LOC: 106819.
  - `fileComplexityValueThreshold`: 20.
  - `fileDuplicationBlockThreshold`: 1.
- Enabled Codacy Cloud tools are Checkov, Cppcheck, Opengrep, Revive, ShellCheck, Spectral, and Trivy.
- Codacy Analysis CLI local run with the current config found 0 issues.
- Official Codacy documentation confirms these are file-level metrics:
  - file complexity is the sum of method cyclomatic complexity, and repository complexity is the percentage of files above the configured complexity goal.
  - file duplication is the number of clones in the file, and repository duplication is the percentage of duplicated files.

Risks:

- Refactoring protocol and transport code can break wire compatibility.
- Removing intentional duplication can make cross-language parity harder to audit.
- Performance-sensitive hot paths need benchmark evidence before accepting structural changes.

## Pre-Implementation Gate

Status: passed for the approved first target.

Problem / root-cause model:

- Working theory: Codacy's maintainability percentages include real complexity and duplication, but not all duplication is bad. Some repeated code preserves explicit cross-language or cross-platform protocol parity.
- The approved first source change is limited to the Go raw L3 cache duplicate implementation.

Evidence reviewed:

- `SOW-0013` Codacy metrics investigation.
- Local Lizard and JSCPD approximations recorded in `SOW-0013`.
- `docs/code-organization.md` layer-boundary rules.
- `docs/level2-typed-api.md` typed API ownership and lifetime rules.
- `.codacy/codacy.config.json` current local Codacy configuration.

Affected contracts and surfaces:

- C, Rust, and Go protocol, transport, and service implementations.
- Cross-language interoperability tests and benchmarks.
- Public protocol/API documentation if any behavior changes are proposed.

Existing patterns to reuse:

- Preserve layer boundaries documented in `docs/code-organization.md`.
- Preserve cross-language parity tests and benchmark scripts.

Risk and blast radius:

- Medium to high depending on selected hotspots.
- Protocol and transport refactors can affect all SDK consumers.
- Performance changes need benchmark validation.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details into durable artifacts.

Implementation plan:

1. Reproduce local complexity and duplication reports with current tools and exact thresholds.
2. Record the user's selected first hotspot group.
3. Refactor only the selected Go raw L3 cache duplicate.
4. Run unit and build validation appropriate to touched Go code.

Validation plan:

- JSCPD before/after comparison for the selected duplicate group.
- Go tests for the touched raw service package.
- Cross-platform Go build validation for Linux and Windows package variants.
- Broader C/Rust validation is not required for this target unless the change escapes Go raw Level 3 cache code.

Artifact impact plan:

- AGENTS.md: likely unaffected unless workflow rules change.
- Runtime project skills: likely unaffected unless reusable maintainability workflow emerges.
- Specs: update only if behavior/contract changes.
- End-user/operator docs: likely unaffected because public API and behavior should stay unchanged.
- End-user/operator skills: likely unaffected because public/operator guidance should stay unchanged.
- SOW lifecycle: moved from `pending/` to `current/` after user approval to proceed; the user approved the Go raw L3 cache first target on 2026-06-04.

Open-source reference evidence:

- Not checked yet; future implementation should use local mirrored repositories only where comparable refactor patterns are relevant.

Open decisions:

- After the Go raw L3 cache target is validated, decide whether to continue with larger typed service-facade duplication or stop.

## Implications And Decisions

- User decision from `SOW-0013`: keep complexity and duplication metrics active; fix real source hotspots instead of weakening useful rules.

1. First remediation target

Evidence gathered on 2026-06-03:

- Lizard 1.23.0 production-source pass:
  - 51 analyzed production source files.
  - 32 files have summed cyclomatic complexity above 20.
  - 1208 functions analyzed.
  - 39 functions have cyclomatic complexity above 20.
- Highest production-source complexity by summed file complexity:
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`: file sum 484; `nipc_apps_lookup_builder_add` has CCN 63 at lines 1982-2198; `apps_lookup_decode_item_bytes` has CCN 49 at lines 1744-1858.
  - `src/go/pkg/netipc/protocol/lookup.go`: file sum 439; `AppsLookupBuilder.Add` has CCN 71 at lines 1253-1491; `decodeAppsLookupItem` has CCN 52 at lines 1111-1222.
  - `src/crates/netipc/src/service/raw.rs`: file sum 431; POSIX and Windows session loops have CCN 36 and 32 at lines 2272-2530 and 2050-2268.
  - `src/libnetdata/netipc/src/service/netipc_service.c`: file sum 396; `server_handle_session` has CCN 47 at lines 1135-1346.
  - `src/libnetdata/netipc/src/service/netipc_service_win.c`: file sum 389; `server_handle_session` has CCN 50 at lines 965-1182.
- JSCPD 4.2.4 production-source pass, excluding Go/Rust in-package tests and the Rust interop utility:
  - 37 analyzed source files.
  - 45 exact clones.
  - 1031 duplicated lines.
  - 12.74% duplicated lines.
  - service-layer duplicate groups: 29 clones, 872 duplicated lines.
  - transport duplicate groups: 14 clones, 192 duplicated lines.
  - protocol duplicate groups: 2 clones, 12 duplicated lines.
- Largest production duplicate pairs:
  - Rust typed lookup facades: `src/crates/netipc/src/service/apps_lookup.rs` and `src/crates/netipc/src/service/cgroups_lookup.rs`, 224 duplicated lines across 7 clones. Shared config and transport conversion start at `apps_lookup.rs:21` and `cgroups_lookup.rs:23`; shared client/server wrapper shape starts at `apps_lookup.rs:89` and `cgroups_lookup.rs:91`.
  - Go raw cache POSIX/Windows: `src/go/pkg/netipc/service/raw/cache.go` and `src/go/pkg/netipc/service/raw/cache_windows.go`, 131 duplicated lines across 5 clones. Shared cache types and refresh/lookup logic start at `cache.go:13` and `cache_windows.go:17`.
  - Go typed lookup facades: apps/cgroups and POSIX/Windows wrappers, including `src/go/pkg/netipc/service/apps_lookup/client.go:11`, `src/go/pkg/netipc/service/cgroups_lookup/client.go:11`, `src/go/pkg/netipc/service/apps_lookup/client_windows.go:11`, and `src/go/pkg/netipc/service/cgroups_lookup/client_windows.go:11`.
  - Windows SHM C transport internal duplication: `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c`, 79 duplicated lines across 4 clones around server-create and client-attach validation/name setup at lines 258 and 469.

Options:

- A. Start with typed service-facade duplication in Rust and Go.
  - Scope: extract shared wrapper/config conversion patterns while preserving public package/module names and exported API shapes.
  - Pros: attacks the largest production duplicate category; mostly Level 2 facade code; lower behavior risk than protocol or transport loops.
  - Cons: may require generics, traits, or small internal helpers; over-abstraction can make the typed API less obvious.
  - Implications: tests should cover Rust service facade compilation and Go POSIX/Windows package builds; public docs likely unchanged if exported APIs remain identical.
  - Risks: accidental API drift in service constructors or platform build tags.

- B. Start with Go raw L3 cache POSIX/Windows duplication.
  - Scope: move common cache state, refresh, lookup, status, and close logic into shared code; keep only platform-specific client construction separate.
  - Pros: smallest isolated production duplicate group; low protocol/transport risk.
  - Cons: reduces fewer duplicated lines than option A; improves one Go cache surface only.
  - Implications: Go tests should cover POSIX locally and Windows via existing Windows validation path.
  - Risks: interface extraction around platform-specific client construction could add indirection for a narrow win.

- C. Start with lookup codec builder/decoder complexity.
  - Scope: split validation, layout calculation, label writing, and item header writing in C/Rust/Go lookup codecs.
  - Pros: largest complexity reduction opportunity; directly improves wire-code readability and testability.
  - Cons: high blast radius because codec code owns wire format encode/decode.
  - Implications: requires C/Rust/Go codec tests, cross-language fixture checks, and interop validation.
  - Risks: subtle wire compatibility regression, borrowed-view lifetime bugs, or performance regression.

- D. Start with raw service session loops and transport send/receive complexity.
  - Scope: split receive, dispatch, response-status mapping, and send paths in raw managed service loops and selected transports.
  - Pros: addresses high-complexity hot paths that are hard to reason about.
  - Cons: highest behavioral and performance risk; these loops encode retry, batching, SHM/baseline selection, and close semantics.
  - Implications: broad C/Rust/Go tests, POSIX interop, Windows validation, and benchmarks are required.
  - Risks: connection lifecycle regressions, performance regressions, or cross-platform drift.

Recommendation: A first.

Reasoning:

- It targets the largest production duplicate category: service-layer duplicates are 872 of 1031 production duplicated lines.
- It avoids the wire-format and transport hot paths that carry the highest regression risk.
- It should reduce Codacy duplicated-file pressure without weakening rules or hiding intentional cross-language parity.
- After A, re-run metrics and decide whether B or C is the next best return/risk tradeoff.

Decision update on 2026-06-04:

- After reviewing the refined duplication composition, the user approved the plan to start with the Go raw L3 cache duplicate implementation.
- This supersedes the earlier broad recommendation to start with typed service facades.
- Implementation scope is limited to `src/go/pkg/netipc/service/raw/cache.go`, `src/go/pkg/netipc/service/raw/cache_windows.go`, and a shared raw-cache helper file if needed.
- Public `NewCache()` signatures and platform build tags must remain unchanged.

2. Duplication composition refinement

Context:

- The goal is not zero duplication. The goal is to remove duplication only where it improves maintainability without hiding protocol, language, or platform parity.
- JSCPD duplicate line counts by pair are an upper-bound gain estimate. They are not strictly additive because the same block can participate in more than one clone pair.

Whole repository composition from the `src tests bench` JSCPD report:

- External fixtures/tests: 3204 clone-pair lines, 149 clones, 67 pairs.
  - Interpretation: mostly validation scaffolding and cross-language fixture parity.
  - Recommendation: keep explicit unless a specific fixture maintenance problem appears.
  - Risk of refactoring: high clarity risk; tests become less direct and less diagnostic.
- In-package tests under `src/`: 1020 clone-pair lines, 68 clones, 23 pairs.
  - Interpretation: mostly repeated test setup/assertion patterns.
  - Recommendation: keep most of it; extract only tiny local helpers when a file is actively touched.
  - Risk of refactoring: medium; over-abstracted tests become harder to read.
- Production source: 45 clones, 21 pairs, 1031 duplicated lines in the filtered production-source report.
  - Interpretation: the only scope worth considering for source remediation in this SOW.

Production-source duplication groups, with gain and risk:

- Go typed service facade/platform wrappers: 384 clone-pair lines, 11 clones, 8 pairs.
  - Evidence examples: `src/go/pkg/netipc/service/cgroups/client.go:27` vs `src/go/pkg/netipc/service/cgroups/client_windows.go:27`; `src/go/pkg/netipc/service/apps_lookup/client.go:5` vs `src/go/pkg/netipc/service/cgroups_lookup/client.go:5`.
  - Potential gain: high for duplicated production service wrapper files.
  - Risk: medium; must preserve package names, exported API, build tags, and public constructor behavior.
  - Recommendation: fix only if the extraction stays small and internal.
- Rust apps/cgroups typed lookup facades: 224 clone-pair lines, 7 clones, 1 pair.
  - Evidence: `src/crates/netipc/src/service/apps_lookup.rs:19` vs `src/crates/netipc/src/service/cgroups_lookup.rs:21`.
  - Potential gain: high; one large isolated facade duplicate.
  - Risk: medium; traits/generics can make a simple typed API harder to inspect.
  - Recommendation: fix with a small shared internal helper only if public facade readability remains good.
- Go raw L3 cache POSIX/Windows duplicate implementation: 131 clone-pair lines, 5 clones, 1 pair.
  - Evidence: `src/go/pkg/netipc/service/raw/cache.go:11` vs `src/go/pkg/netipc/service/raw/cache_windows.go:15`.
  - Potential gain: medium.
  - Risk: low to medium; platform-specific client construction must remain separate.
  - Recommendation: good candidate if the shared cache logic can move to platform-neutral code.
- Rust cgroups snapshot vs cgroups lookup facade overlap: 86 clone-pair lines, 3 clones, 1 pair.
  - Evidence: `src/crates/netipc/src/service/cgroups.rs:8` vs `src/crates/netipc/src/service/cgroups_lookup.rs:6`.
  - Potential gain: medium.
  - Risk: medium; snapshot and lookup are different service contracts.
  - Recommendation: consider only together with the Rust typed-facade cleanup.
- C Windows SHM internal repeated setup/receive logic: 79 clone-pair lines, 4 clones, 1 pair.
  - Evidence: `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:474` vs `src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c:265`.
  - Potential gain: medium.
  - Risk: medium to high; transport setup and recovery paths are platform-sensitive.
  - Recommendation: do not start here unless Windows SHM code is already being changed.
- Go Windows SHM internal repeated setup/receive logic: 56 clone-pair lines, 5 clones, 1 pair.
  - Evidence: `src/go/pkg/netipc/transport/windows/shm.go:334` vs `src/go/pkg/netipc/transport/windows/shm.go:171`.
  - Potential gain: low to medium.
  - Risk: medium to high; Windows SHM behavior is sensitive.
  - Recommendation: keep explicit for now.
- C POSIX SHM internal repeated setup/receive logic: 27 clone-pair lines, 2 clones, 1 pair.
  - Evidence: `src/libnetdata/netipc/src/transport/posix/netipc_shm.c:536` vs `src/libnetdata/netipc/src/transport/posix/netipc_shm.c:369`.
  - Potential gain: low.
  - Risk: high relative to gain.
  - Recommendation: keep explicit.
- Small local duplicates: 67 clone-pair lines across Go raw service types, Go/POSIX SHM, Rust service/protocol local helpers, C POSIX/Windows SHM parity, and Go protocol frame helpers.
  - Evidence examples: `src/go/pkg/netipc/service/raw/types.go:213`, `src/go/pkg/netipc/protocol/frame.go:346`, `src/crates/netipc/src/protocol/cgroups.rs:449`.
  - Potential gain: low.
  - Risk: low to high depending on file, but poor return overall.
  - Recommendation: leave alone unless touched for another reason.

Refined recommendation:

- First useful target: Go raw L3 cache or typed service facades, depending on desired blast radius.
- Best balance: Go raw L3 cache first if the goal is a safe first cleanup; typed service facades first if the goal is maximum duplication reduction.
- Do not refactor transport or codec code only to satisfy duplication metrics.

## Plan

1. Refresh metric evidence.
2. Present hotspot options.
3. Implement selected source remediation.
4. Validate behavior and metrics.

## Execution Log

### 2026-06-03

- Created as follow-up from `SOW-0013`.
- Moved to `current/` after the user approved proceeding.
- Loaded Codacy local/cloud skill guidance for local and remote analysis workflow.
- Checked project-local SOW specs and runtime project skills:
  - `.agents/sow/specs/` contains only `.gitkeep`.
  - no `.agents/skills/project-*/SKILL.md` files exist.
- Read `docs/code-organization.md` and `docs/level2-typed-api.md` to preserve layer boundaries and typed API contracts during hotspot analysis.
- Confirmed `codacy` and `codacy-analysis` are installed.
- Confirmed `lizard` and `jscpd` are not currently on `PATH`; local report generation will use tool-local runners or user-local installations without modifying repository dependencies.

### 2026-06-04

- The user approved the refined plan to start with the Go raw L3 cache duplicate implementation.
- Extracted shared raw cache state, refresh, lookup, status, and close logic into `src/go/pkg/netipc/service/raw/cache_common.go`.
- Kept POSIX and Windows `NewCache()` constructors in the existing build-tagged files:
  - `src/go/pkg/netipc/service/raw/cache.go`
  - `src/go/pkg/netipc/service/raw/cache_windows.go`
- Preserved public `NewCache()` signatures and platform-specific transport configuration types.
- Kept platform-specific client construction in the build-tagged files; the shared helper accepts the already constructed raw `Client`.
- The shared cache logic uses the checked `uint32` conversion path that was already present in the POSIX implementation.

## Validation

Evidence-refresh validation:

- `codacy repository gh netdata plugin-ipc --output json` succeeded and reported commit `655eb62e1aa7395e3a8ec70728e7215c77306948`.
- `codacy issues gh netdata plugin-ipc --branch main --overview --output json` succeeded and reported no current issue categories, levels, languages, tags, patterns, or authors.
- `codacy-analysis discover --output-format json` succeeded and detected C, Go, JSON, Markdown, Rust, Shell, and YAML.
- `codacy-analysis analyze --inspect --output-format json` succeeded; Checkov, Semgrep, Trivy, cppcheck, shellcheck, and spectral were ready, with no unavailable tools.
- `codacy-analysis analyze --output-format json` succeeded and found 0 issues.
- `uvx --from lizard lizard --version` reported Lizard 1.23.0.
- `npx --yes jscpd --version` reported JSCPD 4.2.4.
- Lizard production-source report was generated with CCN threshold 20.
- JSCPD production-source report was generated with min-lines 5, min-tokens 50, and 2 MB max source size.
- `git diff --check` passed after recording the evidence.
- SOW-sensitive scan returned no matches for personal/workstation strings, raw Codacy token assignments, bearer tokens, or private-key markers in this SOW.

Implementation validation:

- `gofmt -w src/go/pkg/netipc/service/raw/cache_common.go src/go/pkg/netipc/service/raw/cache.go src/go/pkg/netipc/service/raw/cache_windows.go` passed.
- `go test ./pkg/netipc/service/raw` passed in `src/go`:
  - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw 77.561s`
- `GOOS=linux GOARCH=amd64 go test -c ./pkg/netipc/service/raw -o /tmp/plugin-ipc-raw-linux.test` passed.
- `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/raw -o /tmp/plugin-ipc-raw-windows.test.exe` passed.
- `go test ./pkg/netipc/service/cgroups` passed:
  - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups 0.304s`
- `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/cgroups -o /tmp/plugin-ipc-cgroups-windows.test.exe` passed.
- `go test ./pkg/netipc/protocol ./pkg/netipc/transport/posix ./pkg/netipc/service/raw ./pkg/netipc/service/cgroups` passed.
- `go test ./...` passed in `src/go`.
- JSCPD production-source after the raw cache change:
  - command scope matched the baseline: `src`, min-lines 5, min-tokens 50, max source size 2 MB, formats C/C++/Rust/Go, excluding Go/Rust in-package tests and `src/crates/netipc/src/bin/**`.
  - baseline before selected change: 45 exact clones, 1031 duplicated lines, 12.74%.
  - after selected change: 40 exact clones, 905 duplicated lines, 11.38%.
  - detailed clone check found no remaining duplicate involving `service/raw/cache*`.
- `codacy-analysis analyze --output-format json` passed:
  - Checkov: 0 issues.
  - Opengrep/Semgrep: 0 issues.
  - Trivy: 0 issues.
  - cppcheck: 0 issues.
  - ShellCheck: 0 issues.
  - Spectral: 0 issues.
  - total: 0 issues, 0 errors.
- `bash .agents/sow/audit.sh` passed and reported SOW initialization complete and clean.
- `git diff --check` passed.

Sensitive data gate:

- `.env` was not read.
- No raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details were written to this SOW.
- Local report artifacts remain under temporary paths and are summarized here without sensitive values.

## Outcome

First remediation target complete; the overall maintainability SOW remains in progress pending the next target decision.

## Lessons Extracted

Pending.

## Followup

- Decide whether to continue with typed service-facade duplication, switch to complexity hotspots, or stop after the raw cache cleanup.

## Regression Log

None yet.
