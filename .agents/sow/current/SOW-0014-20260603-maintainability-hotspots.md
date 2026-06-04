# SOW-0014 - Maintainability Hotspots

## Status

Status: in-progress

Sub-state: continuing useful low-risk complexity hotspot cleanup after the raw-cache, Go facade, apps lookup, and cgroups lookup targets.

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

- After low-risk typed service-facade cleanup is validated, switch to complexity hotspots.

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

Decision update on 2026-06-04, C lookup codec target:

- The user confirmed that `src/libnetdata/netipc/src/protocol/netipc_protocol.c` file complexity and POSIX/Windows service duplication are real hygiene issues worth improving when the change stays reasonable.
- Continue in this existing SOW instead of opening a new SOW.
- First target: reduce `netipc_protocol.c` lookup response builder complexity by extracting private C codec helpers around item layout, label layout, string writing, and directory entry writing.
- Preserve the public C protocol API, public header declarations, wire format, offsets, alignment, error codes, and cross-language fixture bytes.
- Defer POSIX/Windows service common extraction to a separate target-specific decision because it has higher lifecycle and platform-risk surface.

### Target Gate - C Lookup Codec Builder Cleanup

Problem / root-cause model:

- `netipc_protocol.c` is a high-complexity file because it contains multiple protocol families and two dense lookup response builders.
- The remaining builder complexity is concentrated in repeated fixed-string offset calculation, label table sizing, bounds checks, wire header writes, string writes, label writes, and directory updates.
- Working theory: helper extraction can improve reviewability and reduce function-level complexity without changing the wire contract.

Evidence reviewed:

- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1721` `nipc_cgroups_lookup_builder_add`.
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:2063` `nipc_apps_lookup_builder_add`.
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1006` shared label storage sizing helper.
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1186` shared label writer.
- `docs/code-organization.md` codec boundary: protocol code owns wire format encode/decode/builders and must not import transport/service logic.
- `tests/interop_codec.sh` cross-language byte-identity and decode validation covers cgroups/apps lookup response fixtures.

Affected contracts and surfaces:

- C codec internals only.
- Public C protocol header and function signatures remain unchanged.
- Cross-language C/Rust/Go fixture byte identity is an acceptance requirement.

Existing patterns to reuse:

- Keep private static helpers in `netipc_protocol.c`.
- Keep error returns as `NIPC_ERR_*`.
- Keep existing `lookup_write_labels`, `lookup_finish_common`, `align8_u64_over_limit`, and `add_u64_over_limit` helper style.

Risk and blast radius:

- Medium. The code owns wire layout, offsets, alignment, label table placement, and builder error behavior.
- Main regression risks are off-by-one NUL placement, wrong directory length, wrong relative offsets after finish, or changed overflow/error classification.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details into code, docs, skills, specs, or SOW artifacts.

Implementation plan:

1. Extract a private layout struct for lookup builder item placement.
2. Extract shared fixed-string layout calculation for cgroups and apps builders.
3. Extract shared label layout calculation using existing label validation and sizing rules.
4. Extract shared directory-entry write and fixed-string write helpers.
5. Keep cgroups/apps-specific wire header construction local and explicit.

Validation plan:

- Build C code with `cmake --build build`.
- Run focused protocol tests with `/usr/bin/ctest --test-dir build --output-on-failure -R protocol`.
- Run `bash tests/interop_codec.sh` serially after the build/protocol test.
- Run `codacy-analysis analyze --output-format json`.
- Run `git diff --check`, SOW audit, and sensitive-data scan on touched files.

Artifact impact plan:

- `AGENTS.md`: no workflow or guardrail change expected.
- Runtime project skills: no reusable workflow change expected.
- Specs: no public protocol or API behavior change expected; update only if validation reveals a contract discrepancy.
- End-user/operator docs: no public usage change expected.
- End-user/operator skills: no exported/operator workflow change expected.
- SOW lifecycle: record this target and keep SOW in progress after the narrow cleanup unless the user asks to close it.

Decision update on 2026-06-04, C service common target:

- The user approved continuing in this SOW after the C lookup codec cleanup.
- Proceed with the POSIX/Windows C service duplication cleanup next.
- Target a private service-common module for platform-neutral L2/L3 logic; do not merge the POSIX and Windows session loops.
- Preserve platform-specific transport, SHM, wait, cancel, close, wakeup, and thread behavior.

### Target Gate - C Service Common Extraction

Problem / root-cause model:

- `src/libnetdata/netipc/src/service/netipc_service.c` and `src/libnetdata/netipc/src/service/netipc_service_win.c` duplicate substantial service orchestration code.
- Some duplication is intentional because POSIX and Windows transports differ in polling, wakeup, cancellation, SHM object preparation, and thread lifecycle.
- Working theory: extracting platform-neutral service helpers can reduce real duplication while keeping the critical platform paths readable and explicit.

Evidence reviewed:

- `src/libnetdata/netipc/src/service/netipc_service.c:144` and `src/libnetdata/netipc/src/service/netipc_service_win.c:141` duplicate power-of-two sizing helpers.
- `src/libnetdata/netipc/src/service/netipc_service.c:157` and `src/libnetdata/netipc/src/service/netipc_service_win.c:154` duplicate buffer sizing helpers.
- `src/libnetdata/netipc/src/service/netipc_service.c:587` and `src/libnetdata/netipc/src/service/netipc_service_win.c:578` duplicate call retry state-machine structure but differ in sleep/drain timing.
- `src/libnetdata/netipc/src/service/netipc_service.c:703` and `src/libnetdata/netipc/src/service/netipc_service_win.c:693` duplicate lookup request-size helpers.
- `src/libnetdata/netipc/src/service/netipc_service.c:1039` and `src/libnetdata/netipc/src/service/netipc_service_win.c:1231` duplicate typed dispatch.
- `src/libnetdata/netipc/src/service/netipc_service.c:1876` and `src/libnetdata/netipc/src/service/netipc_service_win.c:1871` duplicate L3 cgroups cache logic with only allocator fault-site and monotonic-clock differences.
- `docs/code-organization.md` confirms service modules own Level 2 retry/managed server orchestration and Level 3 cache helpers, while transport mechanics remain below the service layer.

Affected contracts and surfaces:

- C private service implementation and CMake build inputs.
- Public C service API signatures remain unchanged.
- POSIX and Windows behavior must remain platform-specific where the transport lifecycle differs.

Existing patterns to reuse:

- Keep platform files as the owner of transport calls and OS primitives.
- Keep common code private under `src/libnetdata/netipc/src/service/`.
- Keep fault-injection sites preserved through platform-provided allocator callbacks for shared cache code.

Risk and blast radius:

- Medium. Service code owns connection retry, typed calls, managed server dispatch, and cache refresh behavior.
- Main regression risks are changed reconnect timing, changed error mapping, changed response overflow handling, lost fault-injection coverage, or Windows/POSIX lifecycle drift.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details into code, docs, skills, specs, or SOW artifacts.

Implementation plan:

1. Add private `netipc_service_common.h/.c`.
2. Move platform-neutral sizing, default payload, request-size, response-status, typed-dispatch, client status/close-buffer, and L3 cache helpers into the common module.
3. Keep platform files responsible for fault injection wrappers, transport config conversion, connection attempts, send/receive, session loops, accept loops, and OS-specific shutdown.
4. Link the common source into both POSIX and Windows service libraries.

Validation plan:

- Build C code with `cmake --build build`.
- Run focused C service tests with `/usr/bin/ctest --test-dir build --output-on-failure -R 'service|cache'`.
- Run POSIX interop scripts affected by service/cache: `bash tests/test_service_interop.sh`, `bash tests/test_service_shm_interop.sh`, `bash tests/test_cache_interop.sh`, and `bash tests/test_cache_shm_interop.sh` when available on the local platform.
- Run `codacy-analysis analyze --output-format json`.
- Run `git diff --check`, SOW audit, and sensitive-data scan on touched files.

Artifact impact plan:

- `AGENTS.md`: no workflow or guardrail change expected.
- Runtime project skills: no reusable workflow change expected.
- Specs: no public protocol/API behavior change expected; update only if validation reveals a contract discrepancy.
- End-user/operator docs: no public usage change expected.
- End-user/operator skills: no exported/operator workflow change expected.
- SOW lifecycle: record this target and keep SOW in progress unless the user asks to close it.

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
- Committed and pushed the raw-cache cleanup as `e762aef` with message `Refactor Go raw cache implementation`.
- GitHub reported the direct push bypassed the pull-request rule for `main`, as requested by the user.
- Reviewed typed service-facade duplication for obvious low-risk and useful cleanup:
  - Useful/low-risk: same-package Go POSIX/Windows wrappers in `src/go/pkg/netipc/service/cgroups/`, `src/go/pkg/netipc/service/apps_lookup/`, and `src/go/pkg/netipc/service/cgroups_lookup/`.
  - Reason: common wrapper code can move to untagged files inside the same package, while platform transport config conversion remains in build-tagged files.
  - Rejected for now: cross-service Go abstraction between apps lookup and cgroups lookup, because it would require generic/internal indirection across public service packages for modest readability gain.
  - Rejected for now: Rust apps/cgroups lookup facade abstraction, because it would likely require traits/generics/macros around service-specific public types and would make the facade less direct.
- Applied the low-risk Go typed-facade cleanup:
  - `src/go/pkg/netipc/service/cgroups/client_common.go`
  - `src/go/pkg/netipc/service/cgroups/cache_common.go`
  - `src/go/pkg/netipc/service/apps_lookup/client_common.go`
  - `src/go/pkg/netipc/service/cgroups_lookup/client_common.go`
- Left build-tagged Go files responsible only for POSIX/Windows transport config conversion and platform-specific `NewCache()` construction where needed.
- Committed and pushed the typed-facade cleanup as `ef9fcef` with message `Refactor Go service facade wrappers`.
- GitHub reported the direct push bypassed the pull-request rule for `main`, as requested by the user.
- Switched to the lookup codec complexity target.
- Extracted apps lookup semantic validation into one local helper per language:
  - `src/go/pkg/netipc/protocol/lookup.go`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/crates/netipc/src/protocol/lookup.rs`
- The codec change intentionally did not change layout offsets, byte-order writes, label table construction, directory entries, or packed-data compaction.
- The extracted helpers preserve the existing validation order: status domain, cgroup-status domain, comm length, unknown-pid metadata rules, known-pid/cgroup-state rules, then source-string validation in builder paths.
- Continued the lookup codec complexity target with cgroups lookup builders:
  - `src/go/pkg/netipc/protocol/lookup.go`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/crates/netipc/src/protocol/lookup.rs`
- Extracted cgroups lookup semantic validation into one local helper per language.
- Extracted cgroups lookup label-table writing into one local helper per language.
- The cgroups lookup change intentionally did not change item offsets, label table layout, string storage order, directory entries, response finishing, or packed-data compaction.
- Label source validation remains in the existing layout calculation path before label storage is written.
- Investigated the failing GitHub Actions check `Go Static Analysis (src/go)` on commit `879c521`.
- The failing job reported that `gosec` found issues and uploaded SARIF; GitHub code scanning exposed five open `G115` alerts:
  - alerts `7574`, `7575`, and `7576` at `pkg/netipc/protocol/lookup.go:1192`.
  - alerts `7577` and `7578` at `pkg/netipc/protocol/lookup.go:757`.
  - all five were `integer overflow conversion int -> uint64` from helper-call casts in the Go lookup codec.
- Fixed the Go code-scanning findings by changing the Go lookup semantic helper length/count parameters to `int`, removing the introduced `int -> uint64` casts instead of suppressing `G115`.
- Continued the apps lookup builder complexity target:
  - reused the extracted lookup label writer from both cgroups and apps lookup builders in Go, C, and Rust.
  - kept item offsets, label-table layout, string storage order, directory entries, response finishing, and packed-data compaction unchanged.
- User decision on 2026-06-04: proceed with option B, one more low-risk cleanup by splitting apps lookup semantic validation, then stop and re-check Codacy metrics.
- Implemented option B by splitting apps lookup semantic validation into local domain, unknown-pid, and known-pid helpers in:
  - `src/go/pkg/netipc/protocol/lookup.go`
  - `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
  - `src/crates/netipc/src/protocol/lookup.rs`
- The apps lookup semantic validation split preserves the existing order: status domain, cgroup-status domain, comm length, unknown-pid zero-field checks, then known-pid cgroup-state checks.
- The apps lookup semantic validation split intentionally did not change decode offsets, builder layout, label table storage, directory entries, response finishing, serialization, or public APIs.
- Continued with the approved C service common target.
- Added private common service implementation files:
  - `src/libnetdata/netipc/src/service/netipc_service_common.c`
  - `src/libnetdata/netipc/src/service/netipc_service_common.h`
- Linked the common source into both C service libraries in `CMakeLists.txt`.
- Moved platform-neutral C service helpers into the common module:
  - payload-size and power-of-two growth helpers.
  - default cgroups request/response payload sizes.
  - common client initialization, status, and buffer cleanup.
  - request-size calculations for cgroups lookup and apps lookup.
  - response transport-status mapping.
  - typed server dispatch and response header/result handling.
  - L3 cgroups cache refresh, lookup, status, and close logic.
- Kept platform files responsible for OS-specific behavior:
  - POSIX and Windows transport config conversion.
  - connection setup, reconnect timing, send/receive calls, and SHM attachment.
  - server session loops, accept loops, drain/stop/destroy lifecycle, wakeups, cancellation, and thread handling.
- Preserved platform-specific cache allocation fault sites through callback tables in `netipc_service.c` and `netipc_service_win.c`.
- Did not merge POSIX and Windows session loops because they still encode different lifecycle, wait, wakeup, and cancellation behavior.

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
- Low-risk typed-facade validation:
  - `gofmt -w` passed for all touched Go facade files.
  - `go test ./pkg/netipc/service/apps_lookup ./pkg/netipc/service/cgroups_lookup ./pkg/netipc/service/cgroups` passed in `src/go`.
  - `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/apps_lookup -o /tmp/plugin-ipc-apps-lookup-windows.test.exe` passed.
  - `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/cgroups_lookup -o /tmp/plugin-ipc-cgroups-lookup-windows.test.exe` passed.
  - `GOOS=windows GOARCH=amd64 go test -c ./pkg/netipc/service/cgroups -o /tmp/plugin-ipc-cgroups-windows-b.test.exe` passed.
  - `go test ./...` passed in `src/go`.
  - JSCPD production-source after the Go typed-facade cleanup:
    - previous state after raw-cache cleanup: 40 exact clones, 905 duplicated lines, 11.38%.
    - after same-package Go typed-facade cleanup: 36 exact clones, 679 duplicated lines, 8.76%.
  - Remaining service-facade clones are cross-service config/type shapes and small constructor similarities; these were rejected for now because removing them would require broader public-package abstraction.
  - `codacy-analysis analyze --output-format json` passed:
    - Checkov: 0 issues.
    - Opengrep/Semgrep: 0 issues.
    - Trivy: 0 issues.
    - cppcheck: 0 issues.
    - ShellCheck: 0 issues.
    - Spectral: 0 issues.
    - total: 0 issues, 0 errors.
- Option B apps lookup semantic validation split:
  - `gofmt -w src/go/pkg/netipc/protocol/lookup.go` passed.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml` passed.
  - Lizard focused lookup-code comparison:
    - Go `validateAppsLookupSemantics`: CCN 35 before, below the CCN 20 warning threshold after.
    - C `apps_lookup_validate_semantics`: CCN 35 before, below the CCN 20 warning threshold after.
    - Rust `validate_apps_lookup_semantics`: CCN 32 before, below the CCN 20 warning threshold after.
  - Local reproduction of the Go static-analysis lane passed in `src/go`:
    - `go test ./...` passed.
    - `go vet ./...` passed.
    - `staticcheck ./...` passed.
    - `govulncheck ./...` passed with no vulnerabilities.
    - `gosec -quiet -fmt sarif -out /tmp/plugin-ipc-gosec-src-go-b.sarif -exclude=G404 ./...` passed with status `0`.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml` passed: 329 Rust unit tests passed, plus bin/doc test targets.
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R protocol` passed: 4/4 protocol tests.
  - `bash tests/interop_codec.sh` passed after CTest, serially:
    - Rust decoded C output: 89 passed, 0 failed.
    - Go decoded C output: 90 passed, 0 failed.
    - C decoded Rust output: 101 passed, 0 failed.
    - Go decoded Rust output: 90 passed, 0 failed.
    - C decoded Go output: 101 passed, 0 failed.
    - Rust decoded Go output: 89 passed, 0 failed.
    - C, Rust, and Go generated byte-identical protocol fixture files, including all apps lookup response variants.
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
- Lookup codec complexity validation:
  - `gofmt -w src/go/pkg/netipc/protocol/lookup.go` passed.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml` passed.
  - `go test ./pkg/netipc/protocol` passed.
  - `go test ./...` passed in `src/go`.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml` passed: 329 Rust unit tests passed, plus bin/doc test targets.
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R protocol` passed: 4/4 protocol tests.
  - `bash tests/interop_codec.sh` passed after CTest, serially:
    - Rust decoded C output: 89 passed, 0 failed.
    - Go decoded C output: 90 passed, 0 failed.
    - C decoded Rust output: 101 passed, 0 failed.
    - Go decoded Rust output: 90 passed, 0 failed.
    - C decoded Go output: 101 passed, 0 failed.
    - Rust decoded Go output: 89 passed, 0 failed.
    - C, Rust, and Go generated byte-identical protocol fixture files.
  - Lizard focused lookup-code comparison:
    - Go `decodeAppsLookupItem`: CCN 52 before, 19 after.
    - Go `AppsLookupBuilder.Add`: CCN 71 before, 39 after.
    - C `apps_lookup_decode_item_bytes`: CCN 49 before, 16 after.
    - C `nipc_apps_lookup_builder_add`: CCN 63 before, 31 after.
    - Rust `decode_apps_item`: CCN 43 before, 13 after.
    - Rust `AppsLookupBuilder::add`: CCN 56 before, 27 after.
    - New semantic helpers remain intentionally high-complexity state machines: Go CCN 35, C CCN 35, Rust CCN 32.
    - Focused lookup-code average CCN moved from 7.4 to 6.8; NLOC warning ratio moved from 0.32 to 0.26.
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
  - Sensitive-data scan across the touched SOW and codec files returned no matches.
- CI/code-scanning repair and apps lookup builder validation:
  - GitHub Actions failing check inspected:
    - run `26915313092`, job `79403453980`, check `Go Static Analysis (src/go)`.
    - root cause: `gosec` exited with status 1 after uploading SARIF.
    - GitHub code scanning showed five open `G115` alerts on `pkg/netipc/protocol/lookup.go`.
  - Local reproduction of the failing Go static-analysis job passed in `src/go`:
    - `go test ./...` passed.
    - `go vet ./...` passed.
    - `staticcheck ./...` passed.
    - `govulncheck ./...` passed with no vulnerabilities.
    - `gosec -quiet -fmt sarif -out /tmp/plugin-ipc-gosec-src-go.sarif -exclude=G404 ./...` passed with status `0`.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml` passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml` passed: 329 Rust unit tests passed, plus bin/doc test targets.
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R protocol` passed: 4/4 protocol tests.
  - `bash tests/interop_codec.sh` passed after CTest, serially:
    - Rust decoded C output: 89 passed, 0 failed.
    - Go decoded C output: 90 passed, 0 failed.
    - C decoded Rust output: 101 passed, 0 failed.
    - Go decoded Rust output: 90 passed, 0 failed.
    - C decoded Go output: 101 passed, 0 failed.
    - Rust decoded Go output: 89 passed, 0 failed.
    - C, Rust, and Go generated byte-identical protocol fixture files, including all apps lookup response variants.
  - Lizard focused lookup-code comparison for the apps lookup builder target:
    - Go `AppsLookupBuilder.Add`: CCN 39 before, 28 after.
    - C `nipc_apps_lookup_builder_add`: CCN 31 before, 29 after.
    - Rust `AppsLookupBuilder::add`: CCN 27 before, below the CCN 20 warning threshold after.
  - `codacy-analysis analyze --output-format json` passed:
    - Checkov: 0 issues.
    - Opengrep/Semgrep: 0 issues.
    - Trivy: 0 issues.
    - cppcheck: 0 issues.
    - ShellCheck: 0 issues.
    - Spectral: 0 issues.
    - total: 0 issues, 0 errors.
- Cgroups lookup complexity validation:
  - `gofmt -w src/go/pkg/netipc/protocol/lookup.go` passed.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml` passed.
  - `go test ./pkg/netipc/protocol` passed.
  - `go test ./...` passed in `src/go`:
    - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/protocol (cached)`
    - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups 0.304s`
    - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw 77.588s`
    - `ok github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix 0.101s`
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml` passed: 329 Rust unit tests passed, plus bin/doc test targets.
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R protocol` passed: 4/4 protocol tests.
  - `bash tests/interop_codec.sh` passed after CTest, serially:
    - Rust decoded C output: 89 passed, 0 failed.
    - Go decoded C output: 90 passed, 0 failed.
    - C decoded Rust output: 101 passed, 0 failed.
    - Go decoded Rust output: 90 passed, 0 failed.
    - C decoded Go output: 101 passed, 0 failed.
    - Rust decoded Go output: 89 passed, 0 failed.
    - C, Rust, and Go generated byte-identical protocol fixture files, including all cgroups lookup response variants.
  - Lizard focused lookup-code comparison for the cgroups lookup target:
    - Go `CgroupsLookupBuilder.Add`: CCN 42 before, 23 after.
    - C `nipc_cgroups_lookup_builder_add`: CCN 32 before, 24 after.
    - Rust `CgroupsLookupBuilder::add`: CCN 34 before, below the CCN 20 warning threshold after.
    - Go `decodeCgroupsLookupItem`: CCN 21 before, below the CCN 20 warning threshold after.
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
  - Sensitive-data scan across the touched SOW and codec files returned no matches.
- C lookup codec builder validation:
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R protocol` passed: 4/4 protocol tests.
  - `bash tests/interop_codec.sh` passed after the build/protocol test, serially:
    - Rust decoded C output: 89 passed, 0 failed.
    - Go decoded C output: 90 passed, 0 failed.
    - C decoded Rust output: 101 passed, 0 failed.
    - Go decoded Rust output: 90 passed, 0 failed.
    - C decoded Go output: 101 passed, 0 failed.
    - Rust decoded Go output: 89 passed, 0 failed.
    - C, Rust, and Go generated byte-identical protocol fixture files, including all cgroups/apps lookup response variants.
  - Codacy-managed Lizard with `-l c -C 20 src/libnetdata/netipc/src/protocol/netipc_protocol.c` passed with no CCN threshold warnings:
    - `nipc_cgroups_lookup_builder_add`: CCN 8.
    - `nipc_apps_lookup_builder_add`: CCN 8.
    - No function exceeded CCN 20.
    - Note: this improves function-level complexity and reviewability, but same-file helper extraction only modestly reduces file-level summed complexity. Further per-file Codacy score reduction requires a separate protocol-family file split decision.
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
  - Sensitive-data scan across the touched SOW and C codec file returned no matches.
- C service common extraction validation:
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R 'service|cache'` passed: 13/13 focused service/cache tests.
  - `bash tests/test_service_interop.sh` passed: 9/9 C/Rust/Go service interop pairs.
  - `bash tests/test_cache_interop.sh` passed: 9/9 C/Rust/Go cache interop pairs.
  - `bash tests/test_service_shm_interop.sh` passed: 9/9 C/Rust/Go SHM service interop pairs.
  - `bash tests/test_cache_shm_interop.sh` passed: 9/9 C/Rust/Go SHM cache interop pairs.
  - Codacy-managed Lizard with `-l c -C 20 src/libnetdata/netipc/src/service/netipc_service.c src/libnetdata/netipc/src/service/netipc_service_win.c src/libnetdata/netipc/src/service/netipc_service_common.c` reported:
    - `netipc_service.c`: NLOC 1290, 53 functions.
    - `netipc_service_win.c`: NLOC 1315, 53 functions.
    - `netipc_service_common.c`: NLOC 503, 30 functions.
    - Remaining CCN warnings are only the platform-specific session loops:
      - POSIX `server_handle_session`: CCN 32.
      - Windows `server_handle_session`: CCN 33.
    - This confirms the common extraction reduced duplicated helper/cache code while leaving the higher-risk lifecycle loops explicit.
  - `npx --yes jscpd --min-lines 5 --min-tokens 50 --max-size 2mb --format c,cpp,rust,go --reporters json --output /tmp/plugin-ipc-jscpd-current src` produced a filtered production-source approximation:
    - 36 production clone pairs after excluding Go/Rust tests and `src/crates/netipc/src/bin/**`.
    - 715 production clone-pair lines.
    - zero remaining clone pairs involving `src/libnetdata/netipc/src/service/netipc_service.c`, `src/libnetdata/netipc/src/service/netipc_service_win.c`, or `src/libnetdata/netipc/src/service/netipc_service_common.c`.
    - largest remaining production duplicate surfaces are Rust typed facades and SHM transport internals, not the C POSIX/Windows service files targeted here.
  - Windows/MSYS validation on remote host `win11`:
    - The current uncommitted tree was copied to a fresh remote validation directory with `.git/`, build outputs, `.env`, and unrelated local scratch files excluded.
    - Environment: `MSYSTEM=MSYS`, `/usr/bin/gcc`, `/usr/bin/g++`, `/usr/bin/cmake`, `/usr/bin/ninja`, Windows Cargo from the Windows user-profile Cargo directory, and Go from `/c/Program Files/Go/bin`.
    - Targeted Windows/MSYS build passed for named-pipe, WinSHM, service, cache, stress, and C/Rust/Go interop targets; this compiled `netipc_service_common.c` and `netipc_service_win.c` into `libnetipc_service_win.a`.
    - Targeted Windows/MSYS CTest slice passed: 12/12 tests:
      - `test_named_pipe`
      - `test_named_pipe_interop`
      - `test_win_shm`
      - `test_win_service`
      - `test_win_service_extra`
      - `test_win_service_payload_limits`
      - `test_win_stress`
      - `test_win_shm_interop`
      - `test_service_win_interop`
      - `test_service_win_shm_interop`
      - `test_cache_win_interop`
      - `test_cache_win_shm_interop`
    - Full Windows/MSYS `cmake --build build-msys-validation -j12` passed.
    - Full Windows/MSYS `ctest` initially reported 32/34 passing. The two failures were investigated:
      - `interop_codec` failed because the first remote copy included a stale local Linux Rust `src/crates/netipc/target/debug/interop_codec` artifact; after deleting the remote temp Cargo target directory, `ctest -R '^interop_codec$'` passed.
      - `go_FuzzDecodeCgroupsLookupResponse` failed once with `context deadline exceeded` after the 20-second fuzz deadline; immediate rerun with `ctest -R '^go_FuzzDecodeCgroupsLookupResponse$'` passed.
- Windows/MSYS service/cache tests and cross-language interop are therefore validated for this change. The dirty-transfer artifact is not a repository failure; the single fuzz timeout is recorded as a timing flake in the full Windows smoke suite.

### 2026-06-04 Code Scanning Repair

- After commit `94907bf`, GitHub CodeQL reported two open `cpp/constant-comparison` alerts:
  - alert `7579`: `src/libnetdata/netipc/src/service/netipc_service_common.c`, apps lookup request-size additive overflow guard.
  - alert `7580`: `src/libnetdata/netipc/src/service/netipc_service_common.c`, apps lookup request-size additive overflow guard.
- The alerts are valid for 64-bit builds because `pid_count` is `uint32_t` and the apps lookup directory/key constants are small enough that the additive `SIZE_MAX` comparisons cannot become true on 64-bit `size_t`.
- The extracted common helper accidentally made the additive guard unconditional; the original POSIX helper guarded equivalent checks with `#if SIZE_MAX <= UINT32_MAX`.
- Repair plan: restore the conditional 32-bit-only additive overflow guard and keep the existing multiplication guards plus caller-side `req_size > UINT32_MAX` checks.
- Risk: low. The change affects only a static-analysis-visible overflow guard in the private C service helper. It does not change request encoding, public APIs, transport behavior, or the 64-bit runtime path.
- Repair implemented by guarding the apps lookup additive overflow check with `#if SIZE_MAX <= UINT32_MAX`.
- Validation:
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R 'service|cache'` passed: 13/13 focused service/cache tests.
  - `codacy-analysis analyze --output-format json` passed with 0 issues across Checkov, Opengrep/Semgrep, Trivy, cppcheck, ShellCheck, and Spectral.
  - Windows/MSYS rebuild on `win11` passed for `netipc_service_win`, `test_win_service`, `test_win_service_extra`, `test_win_service_payload_limits`, `interop_service_win_c`, and `interop_cache_win_c`.
  - Windows/MSYS CTest service slice passed: `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits`.
  - `git diff --check` passed.
  - `bash .agents/sow/audit.sh` passed.

### 2026-06-04 GitHub AI Quality Findings Review

- The GitHub quality AI findings page reported seven findings across typed Go service wrappers and C Windows service code.
- Response-batch mapping findings:
  - Reported files include `src/go/pkg/netipc/service/apps_lookup/client.go`, `src/go/pkg/netipc/service/cgroups/client.go`, `src/go/pkg/netipc/service/cgroups_lookup/client.go`, `src/go/pkg/netipc/service/cgroups_lookup/client_windows.go`, and `src/libnetdata/netipc/src/service/netipc_service_win.c`.
  - Same-failure search also found the equivalent Go Windows apps lookup wrapper and POSIX C service mapping.
  - Decision: reject these as false positives, but clarify the code.
  - Evidence: public typed Go configs expose `MaxRequestBatchItems` and `MaxResponsePayloadBytes`, but no `MaxResponseBatchItems`; C `nipc_client_config_t` and `nipc_server_config_t` likewise expose only `max_request_batch_items` and `max_response_payload_bytes`.
  - Evidence: `docs/level1-wire-envelope.md` says `max_response_batch_items` is kept for wire symmetry and the agreed response batch count must equal the agreed request batch count.
  - Implementation plan: keep behavior unchanged, but route response-batch assignments through private helpers named as typed response-batch symmetry helpers so future reviewers do not interpret the mapping as accidental copy-paste.
- Lookup request-payload default finding:
  - Reported file: `src/libnetdata/netipc/src/service/netipc_service_win.c`.
  - Same-failure search found the equivalent POSIX C lookup server initializers.
  - Decision: reject as intentional behavior, but clarify the code.
  - Evidence: `docs/level2-typed-api.md` says typed callers do not provide `max_request_payload_bytes`; the library derives the initial proposal internally from method schema, batch size, and dynamic-field assumptions.
  - Evidence: lookup requests contain dynamic path/PID arrays, and C lookup servers pre-size accepted-session SHM resources from `learned_request_payload_bytes` before serving typed requests.
  - Risk of applying the suggested change: changing lookup server request defaults from the larger lookup default to the generic request default could add avoidable reconnect churn or undersized SHM setup for lookup workloads.
  - Implementation plan: keep behavior unchanged, but replace direct `cgroups_response_payload_default()` calls in lookup request-default sites with a private helper named as a lookup request-payload default.
  - Implemented:
    - C POSIX and Windows typed service config conversion now routes response-batch symmetry through `nipc_service_common_typed_response_batch_items()`.
    - C POSIX and Windows lookup server request defaults now route through `nipc_service_common_lookup_request_payload_default()`.
    - Go POSIX and Windows typed service wrappers for apps lookup, cgroups lookup, and cgroups snapshot now route response-batch symmetry through per-package `typedResponseBatchItems()` helpers.
  - Same-failure verification: no remaining direct `MaxResponseBatchItems: config.MaxRequestBatchItems` or `max_response_batch_items = config->max_request_batch_items` assignments in typed service wrappers.
  - `cmake --build build` passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R 'service|cache'` passed: 13/13 focused service/cache tests.
  - POSIX Go typed-service package tests passed for apps lookup, cgroups lookup, and cgroups snapshot.
  - Windows Go typed-service package compilation/tests passed for apps lookup, cgroups lookup, and cgroups snapshot.
  - Windows/MSYS rebuild passed for `netipc_service_win`, `test_win_service`, `test_win_service_extra`, `test_win_service_payload_limits`, `interop_service_win_c`, and `interop_cache_win_c`.
  - Windows/MSYS CTest service slice passed: `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits`.
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
  - Sensitive-data scan across the touched SOW, Go service wrappers, and C service files returned no matches.

Sensitive data gate:

- `.env` was not read.
- No raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details were written to this SOW.
- Local report artifacts remain under temporary paths and are summarized here without sensitive values.

## Outcome

Raw cache, Go typed-facade, apps lookup builder, cgroups lookup builder, apps lookup semantic validation, five Go code-scanning findings, C lookup builder function-level complexity, and C POSIX/Windows service helper duplication are locally remediated; the overall maintainability SOW remains in progress pending the next selected maintainability target and Codacy metric re-check after push.

### 2026-06-04 Windows CI And CodeQL Coverage

- User decision: add Windows CI coverage after discovering that current CodeQL coverage is Linux-shaped, not a CodeQL product limit.
- Purpose: make Windows runtime and Windows static-analysis coverage first-class CI signals for this cross-platform SDK.
- Evidence:
  - `.github/workflows/codeql.yml` currently runs every matrix entry on `ubuntu-latest`.
  - The C/C++ CodeQL job currently builds only POSIX targets: `netipc_protocol`, `netipc_uds`, `netipc_shm`, and `netipc_service`.
  - The Go CodeQL job currently runs `go test ./...` on Linux, so Windows-only Go files guarded by `//go:build windows` are not extracted.
  - `CMakeLists.txt` already has Windows runtime targets guarded by `NETIPC_WINDOWS_RUNTIME`.
  - Existing Windows validation scripts and local validation use MSYS2/MinGW, not MSVC.
- Decision:
  - Add GitHub-hosted Windows CI on `windows-latest`.
  - Use MSYS2/MinGW for the Windows runtime build because it matches the existing tested Windows scripts.
  - Keep heavyweight Windows coverage and benchmark jobs out of push/PR CI for now; they remain explicit scripts.
  - Add Windows CodeQL jobs for C/C++ and Go so Windows-only sources are extracted.
- Risk:
  - Windows hosted runners may expose timing flakiness in named-pipe/SHM tests.
  - Adding MSYS2 introduces another pinned CI action and package-install surface.
  - CodeQL Windows jobs increase CI time and may surface a new backlog of valid Windows-only findings.
- Validation plan:
  - Run local action/workflow linting.
  - Run relevant local CMake/Go validation.
  - Push and verify GitHub Windows runtime and CodeQL jobs.
- Implemented:
  - Added a `windows-latest` MSYS2/MinGW runtime job to `.github/workflows/runtime-safety.yml`.
  - Expanded CodeQL into distinct POSIX and Windows categories for C/C++ and Go.
  - Expanded POSIX C/C++ CodeQL build targets so test, interop, cache, stress, hardening, and benchmark C sources are compiled during extraction.
  - Added Windows C/C++ CodeQL build targets for named pipe, Windows SHM, Windows service, Windows interop, guard, stress, and benchmark C sources.
  - Added Windows Go CodeQL build execution so Windows build-tagged Go packages are extracted.
- Local validation:
  - YAML parsing passed for `.github/workflows/codeql.yml` and `.github/workflows/runtime-safety.yml`.
  - `actionlint .github/workflows/codeql.yml .github/workflows/runtime-safety.yml` passed.
  - POSIX expanded C/C++ CodeQL target list built locally with `cmake --build build`.
  - Windows/MSYS runtime build command passed on the Windows validation host.
  - Windows/MSYS runtime CTest slice passed on the Windows validation host: 12/12 targeted Windows tests.
  - Windows Go CodeQL build command passed on the Windows validation host for `src/go`, `tests/fixtures/go`, and `bench/drivers/go`.
  - Windows C/C++ CodeQL-only `bench_windows_c` target built on the Windows validation host.
  - `git diff --check` passed.
  - `bash .agents/sow/audit.sh` passed.
  - `codacy-analysis analyze --output-format json` passed with 0 issues and 0 errors across Checkov, Opengrep/Semgrep, Trivy, cppcheck, ShellCheck, and Spectral.

## Lessons Extracted

Pending.

## Followup

- Continue only with complexity or duplication targets that have clear maintainability gain and low enough behavior risk.
- Current remaining high-complexity targets include the POSIX/Windows service session loops and transport send/receive/reconnect paths; these have higher behavioral and performance risk than the helper/cache extraction completed here.

## Regression Log

None yet.
