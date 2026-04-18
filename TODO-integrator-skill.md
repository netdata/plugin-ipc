## Purpose

Fit-for-purpose goal: produce one authoritative caller-facing integration skill at `docs/netipc-integrator-skill.md` that lets future assistants and plugin authors add new `plugin-ipc` clients and servers to Netdata correctly, across C, Rust, and Go, on Linux and Windows, with the right choice of L1/L2/L3, typed service integration, failure handling, retries, logging, and refresh strategy.

## TL;DR

- Costa wants a single markdown skill/document for Netdata integrators.
- It must explain how to use L1, L2, and L3 correctly when adding new plugins, clients, and servers.
- It must cover all supported languages and both operating systems, including typed datatype creation, library integration, failure handling, retries, transport/profile logging, and best practices.

## Analysis

- Existing documentation is fragmented across multiple documents instead of giving one integrator-focused workflow:
  - `README.md`
  - `docs/level2-typed-api.md`
  - `docs/level3-snapshot-api.md`
  - `docs/getting-started.md`
- Existing TODO and API work already established important facts that the new skill must reflect:
  - public L2/L3 APIs are intended to be OS-transparent within each language
  - transport details belong to L1/internal layers
  - typed services and typed caches are the intended public integration path for Netdata plugins
- Relevant public integration surfaces already exist in all three languages and must be documented together:
  - C:
    - `src/libnetdata/netipc/include/netipc/netipc_service.h`
    - `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
  - Go:
    - `src/go/pkg/netipc/service/cgroups/`
    - `src/go/pkg/netipc/service/raw/`
  - Rust:
    - `src/crates/netipc/src/service/`
    - `src/crates/netipc/src/protocol/`
- The current documentation is protocol-focused and API-focused, but not integrator-focused:
  - it explains layers and wire contracts
  - it does not yet provide one practical decision tree for:
    - when to choose L1 vs L2 vs L3
    - how to define new typed methods and datatypes
    - how to wire new typed methods into C/Rust/Go libraries
    - how to choose integration points in Netdata plugins
    - how to handle reconnects, snapshot refresh, attach failures, and transport observability
- The skill must be broad enough that an assistant can use it as the single starting point for new integration work, not just as a prose overview.
- Concrete findings from the code/docs audit:
  - The current public typed service facade is `cgroups-snapshot` only.
  - Public typed L2/L3 APIs are present in all three languages:
    - C:
      - `nipc_client_*`
      - `nipc_server_*`
      - `nipc_cgroups_cache_*`
    - Rust:
      - `CgroupsClient`
      - `ManagedServer`
      - `CgroupsCache`
    - Go:
      - `NewClient`
      - `NewServer` / `NewServerWithWorkers`
      - `NewCache`
  - The normative handshake / retry / SHM-attach recovery contract is already documented in:
    - `docs/level1-wire-envelope.md`
    - `docs/level1-transport.md`
    - `docs/level2-typed-api.md`
    - `docs/level3-snapshot-api.md`
  - A critical operational limitation exists today:
    - public L2/L3 status APIs expose state and counters
    - they do **not** expose negotiated `selected_profile`
    - they do **not** expose actual `data_plane` proof
    - therefore transport-profile observability must currently be added at the integration boundary when needed
  - The current docs index did not reference an integrator-focused guide, so the new skill should be linked from:
    - `docs/README.md`
    - `README.md`
- Reviewer validation against a concrete integration scenario (`apps.plugin` as
  provider, `network-viewer.plugin` as consumer) surfaced the same repeated
  gap:
  - the skill is good enough to choose the architecture
  - it is not yet good enough to implement a brand-new typed service without
    deeper source reading
- Concrete evidence from the validation run:
  - `apps.plugin` already computes per-pid CPU values that are suitable for a
    new typed service:
    - `src/collectors/apps.plugin/apps_functions.c:1169`
    - `src/collectors/apps.plugin/apps_functions.c:1184`
  - `network-viewer.plugin` already has the exact row emission and column
    definition points needed for PID-based enrichment:
    - `src/collectors/network-viewer.plugin/network-viewer.c:857`
    - `src/collectors/network-viewer.plugin/network-viewer.c:2910`
    - `src/collectors/network-viewer.plugin/network-viewer.c:2918`
  - the current public C typed API is still effectively `cgroups-snapshot`
    only:
    - `src/libnetdata/netipc/include/netipc/netipc_protocol.h:57`
    - `src/libnetdata/netipc/include/netipc/netipc_protocol.h:494`
    - `src/libnetdata/netipc/include/netipc/netipc_service.h:210`
    - `src/libnetdata/netipc/include/netipc/netipc_service.h:314`
    - `src/libnetdata/netipc/src/service/netipc_service.c:1239`
    - `src/libnetdata/netipc/src/service/netipc_service_win.c:1292`
- Reviewers consistently identified these missing or under-explained areas in
  the skill:
  - no worked example for adding a new typed service end to end
  - no explicit warning that the current public typed facade is cgroups-only
  - no Netdata-specific integration guidance for:
    - `netipc_netdata.h`
    - `netipc_auth_token()`
    - `os_run_dir(true)` vs `os_run_dir(false)`
  - no guidance for function-only / on-demand consumers such as
    `network-viewer.plugin`
  - no explicit guidance about server handler thread-safety against mutable
    plugin-owned state
  - not enough separation between:
    - upstream `plugin-ipc` source-of-truth work
    - Netdata integration-repo-only work
- Reviewer consensus also converged on one important design nuance:
  - a consumer like `network-viewer.plugin`, which emits many rows in one
    function call, will usually want an L3-style snapshot/cache consumer rather
    than per-row L2 calls
  - the current skill hints at this, but should state it more explicitly with a
    worked example
- Second-round reviewer rerun after the skill update reduced the findings
  significantly, but it surfaced one concrete accuracy bug in the document:
  - the skill pointed to `netipc_netdata.h` / `netipc_netdata.c` as if they
    lived in the upstream `plugin-ipc` repo
  - they do not
  - those files are Netdata integration files, not upstream library files
  - upstream source of truth is:
    - `github.com/netdata/plugin-ipc`
- The skill must remain location-independent:
  - it must not hardcode absolute local checkout paths for `plugin-ipc`
  - when it refers to upstream source-of-truth work, it should refer to the
    GitHub repository `github.com/netdata/plugin-ipc`
  - when it refers to Netdata integration files, it should assume the current
    working tree is the Netdata checkout
  - if the caller has a local checkout of `plugin-ipc` elsewhere, the user
    prompt may provide that location explicitly

## Decisions

### Made

- Create a new dedicated task tracker:
  - `TODO-integrator-skill.md`
- The skill/document output file must be:
  - `docs/netipc-integrator-skill.md`
- The deliverable must be a single markdown document.
- The document must cover:
  - L1, L2, and L3 integration options
  - how to choose the right layer
  - how to create new typed L2/L3 datatypes
  - how to integrate those datatypes into the library
  - how to integrate clients and servers into Netdata plugins
  - how to handle failures, retries, logging, refresh strategies, and connection profile visibility
  - all three languages
  - both supported operating systems
- The intended consumer is:
  - future assistants and developers adding new Netdata plugin integrations
- Reviewer feedback should be incorporated into the same skill instead of
  creating more fragmented documentation.
- The skill should be upgraded from a conceptual guide into a practical
  implementation guide for new typed services.
- The next revision of the skill should add:
  - one explicit worked example for a brand-new typed snapshot service
  - one Netdata-specific integration section
  - one section that separates upstream library work from integration-repo-only
    work
  - one section covering function-only consumers and hot-path cache decisions
  - one section covering server-handler synchronization requirements
- The skill must make it explicit that whenever an integration needs a new
  structured data type / service contract, the structured codec must be added
  in all three supported language implementations:
  - C
  - Rust
  - Go
  This is a hard requirement, not an optional best practice.
- The skill must be location-independent:
  - do not state absolute local checkout paths for `plugin-ipc`
  - refer to the upstream source-of-truth repo as:
    - `github.com/netdata/plugin-ipc`
  - assume Netdata integration work happens in the current working tree
  - allow the user prompt to provide a local `plugin-ipc` checkout path when
    needed
- The skill must stay agnostic to the service payload itself:
  - do not describe concrete payload fields or data-model content
  - keep guidance at the service/codec/process level, not at the schema-content
    level
- The skill is Netdata-specific, but generic across Netdata plugins:
  - it should teach reusable Netdata IPC integration patterns
  - it should not teach one concrete plugin pair or one concrete payload
  - it should not tell the integrator how to integrate a specific plugin's
    content
  - instead, it should highlight:
    - common pitfalls
    - best practices
    - areas that require explicit design attention
  - examples of such areas include:
    - data availability differences across Linux and Windows
    - concurrent access to shared provider state in IPC servers
    - concurrent access and lifecycle rules for L3 cache clients
    - identity/key stability
    - truncation / completeness semantics
    - logging / observability expectations
    - startup ordering and retry behavior
- The key library constraints and design assumptions must be crystal clear:
  - assistants and humans who did not read the design history must not guess
    thread-safety, lifecycle, ownership, or transport behavior
  - the skill should state those assumptions explicitly and prominently
  - the skill should include strong reminders near the end so concurrency and
    lifecycle constraints are not missed during implementation
- Latest full-scope reviewer rerun after the generic-scope/concurrency refactor
  concluded:
  - the skill is now correctly scoped and factually aligned with the code
  - the remaining issues are precision issues, not design issues
  - the main remaining clarifications are:
    - say explicitly that L2 clients and L3 caches are not internally
      synchronized
    - warn about concurrent function invocation for on-demand consumers
    - mention L3 cache memory footprint and caller-side refresh backoff
    - strengthen the "upstream first / all three languages first" gate

### Pending

- None at the product-contract level.
- Manual user review of the wording is not required.
- Acceptance should be based on repeated external reviewer convergence plus
  source-backed sanity checks.

## Plan

1. Record the reviewer evidence and turn it into a concrete patch plan for the
   skill.
2. Patch `docs/netipc-integrator-skill.md` to add:
   - explicit current limitation: public typed facade is cgroups-only today
   - Netdata-specific integration rules for auth token, run dir, lifecycle, and
     logging
   - guidance for function-only consumers and when L3 beats direct L2 calls
   - explicit guidance for handler synchronization and mutable producer state
   - a clean ownership split: upstream `plugin-ipc` first, Netdata integration
     second
   - location-independent wording:
     - upstream repo identified as `github.com/netdata/plugin-ipc`
     - Netdata integration paths described relative to the current working tree
     - no false claim that `netipc_netdata.*` exists in upstream `plugin-ipc`
   - one prominent section that states the library constraints and assumptions
   - one prominent section for concurrent use of:
     - managed servers
     - provider-owned shared state
     - L2 clients
     - L3 caches and their returned views/items
   - one strong final reminder checklist so these constraints are not missed
3. Revalidate the revised skill against the current code paths in:
   - `src/libnetdata/netipc/include/netipc/`
   - `src/libnetdata/netipc/src/service/`
   - `src/go/pkg/netipc/service/raw/`
   - `src/crates/netipc/src/service/cgroups.rs`
   - `src/libnetdata/netipc/netipc_netdata.*`
   - `src/libnetdata/os/run_dir.*`
4. Summarize the revised skill and any remaining gaps.
5. Apply the latest reviewer precision fixes:
   - make the lack of internal synchronization explicit in the main constraints
     section
   - add guidance for concurrent function invocations in function-only
     consumers
   - add cache memory footprint and refresh-backoff guidance
   - strengthen the "do not continue until the codec exists in C, Rust, and
     Go" rule

## Current Status

- Completed:
  - created the new integrator guide at:
    - `docs/netipc-integrator-skill.md`
  - documented:
    - when to use L1 vs L2 vs L3
    - current public integration surfaces in C, Rust, and Go
    - failure and retry expectations
    - SHM attach failure recovery semantics
    - logging and observability best practices
    - workflow for adding a new typed service to the library
    - workflow for integrating new clients and servers into Netdata
    - anti-patterns and an assistant execution checklist
  - linked the new guide from:
    - `docs/README.md`
    - `README.md`
  - revised the guide with the reviewer-driven patch plan
  - added:
    - explicit warning that the public typed facade is cgroups-only today
    - Netdata-specific glue guidance for:
      - `netipc_netdata.h`
      - `netipc_auth_token()`
      - `os_run_dir(true)` / `os_run_dir(false)`
    - guidance for function-only / on-demand consumers
    - explicit upstream-vs-integration ownership split
    - provider synchronization guidance for handler threads
  - reran external reviewers on the revised skill with the same scope
  - confirmed the main remaining documentation defect is:
    - wrong repository ownership/path description for `netipc_netdata.h` /
      `netipc_netdata.c`
  - removed the concrete plugin-specific worked example
  - rewrote the guide around generic Netdata IPC constraints, assumptions, best
    practices, and pitfalls
  - added:
    - explicit library constraints and assumptions section
    - explicit client/cache concurrency guidance
    - explicit platform data-availability guidance
    - strong end-of-document reminders
- revalidated the revised sections against:
  - `src/libnetdata/netipc/include/netipc/`
  - `src/libnetdata/netipc/src/service/`
  - `src/go/pkg/netipc/service/raw/`
  - `src/crates/netipc/src/service/cgroups.rs`
  - `src/libnetdata/netipc/netipc_netdata.*`
  - `src/libnetdata/os/run_dir.*`
- Pending:
  - user review of the revised guide
  - fold in the latest reviewer precision fixes and do one final sanity check

## Implied decisions

- The skill should be practical and action-oriented, not just architectural theory.
- The skill should state best practices and anti-patterns explicitly.
- The skill should distinguish stable public integration APIs from internal/raw/test-only surfaces.
- The skill should explain the philosophy behind the layers so assistants can reason correctly about new integrations.
- The skill should make it hard for assistants to miss that a new typed service
  usually starts as upstream `plugin-ipc` work, not as direct edits inside a
  vendored integration repo.
- The skill should warn that server handlers run against live plugin-owned
  state, so thread-safety is part of the integration contract.
- The skill should not assume a fixed local filesystem layout for the upstream
  `plugin-ipc` checkout.
- The skill should assume Netdata integration file paths are relative to the
  current Netdata working tree.
- The core skill should remain generic and should not teach one concrete
  service payload or one concrete plugin pair as part of the reusable guidance.
- The core skill should explicitly teach integrators what to think about, not
  what one concrete integration must look like.
- The skill should clearly state the library's concurrency and lifecycle
  assumptions instead of leaving readers to infer them from the code.
- The skill should prefer explicit "not internally synchronized" wording over
  weaker hints about ownership when concurrent access would be unsafe.

## Testing requirements

- Validate every API/example/reference in the skill against the current codebase before finalizing.
- Ensure the skill does not instruct callers to use deprecated, raw, or internal-only paths when a public typed path exists.
- Ensure the skill reflects the current negotiated-transport and reconnect contract accurately.
- Ensure the revised skill does not imply that a brand-new typed service can be
  implemented in Netdata without upstream `plugin-ipc` changes first.
- Ensure the revised skill does not misstate whether a file belongs to:
  - the upstream `github.com/netdata/plugin-ipc` repo
  - the current Netdata integration working tree
- Ensure the revised skill does not:
  - embed a concrete payload schema
  - rely on plugin-specific implementation details as reusable library guidance
- Ensure the revised skill does:
  - call out platform-specific data availability concerns
  - call out provider-side synchronization concerns
  - call out consumer-side cache concurrency/lifecycle concerns
  - call out identity and correctness pitfalls
  - stay generic across all Netdata plugins

## Documentation updates required

- Create:
  - `docs/netipc-integrator-skill.md`
- Check whether the following should later cross-link to the new skill:
  - `README.md`
  - `docs/getting-started.md`
  - `docs/level2-typed-api.md`
  - `docs/level3-snapshot-api.md`
