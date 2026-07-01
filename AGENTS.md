# AGENTS.md

## Goals

`plugin-ipc` is a cross-language IPC library for Netdata plugins and helper services.

Success means:

- one authoritative specification under `docs/`;
- one interoperable protocol stack;
- typed service APIs for C, Rust, and Go;
- POSIX and Windows transport coverage;
- cross-language interoperability as a release requirement;
- benchmarks and tests that prove behavior and performance claims.

## SOW System

This project uses a local Statement of Work system.

The SOW system is self-contained in this repository. Normal SOW work must not depend on `~/.agents`, `~/.AGENTS.md`, global skills, global templates, or global scripts. Use this `AGENTS.md`, project-local SOW files, project-local specs, project-local skills, and the active SOW.

### Roles

- **User responsibilities:** purpose, scope decisions, design forks, risk acceptance, destructive approvals, and final product judgment.
- **Assistant responsibilities:** investigation, evidence, implementation, tests or equivalent validation, reviews, documentation, memory updates, and concise reporting.

### Required First Checks

Before non-trivial work:

1. Read pending/current SOWs for overlap, contradictions, and existing decisions.
2. Read relevant specs under `.agents/sow/specs/`.
3. Inspect `.agents/skills/project-*/SKILL.md` if any exist, and load every runtime project skill whose trigger matches the work.
4. Inspect the authoritative docs under `docs/` and the relevant C, Rust, Go, test, and benchmark code.
5. Ask the user only for irreducible product/design/risk decisions.

### Git Worktrees

Assistants must not create git worktrees on their own. Create a git worktree only when the user explicitly asks for it or approves it.

### Sensitive Data In Durable Artifacts

SOWs, specs, documentation, project skills, agent instructions, and code comments are commit-ready artifacts. Treat them as public unless a repository-specific policy explicitly says otherwise.

CRITICAL: Never write raw sensitive data to durable artifacts. This includes passwords, API keys, bearer tokens, SNMP communities, private keys, connection strings with embedded credentials, session cookies, community member names, customer names, customer identifiers, personal data, non-private IP addresses that can identify customers, private endpoints, account IDs, and proprietary incident details.

Write only sanitized evidence:

- use placeholders such as `[REDACTED_SECRET]`, `[CUSTOMER]`, `[ACCOUNT]`, `[PRIVATE_ENDPOINT]`;
- use stable aliases such as `customer-a` only when the real mapping is not stored in the repository;
- cite file paths, line numbers, command names, schema fields, or error classes instead of copying sensitive values;
- summarize logs and traces; include only minimal redacted snippets.

If sensitive data is required to continue, stop and ask the user for a secure handling path. If sensitive data is found in a durable artifact, sanitize it before any commit. If sensitive data was already committed, tell the user and do not rewrite history without explicit approval.

### Open-Source Reference Evidence

When SOW evidence comes from local mirrored or cloned open-source repositories, cite the upstream repository and checked commit instead of the workstation absolute path.

Use:

```text
owner/repo @ commit
relative/path/inside/repo:line
```

Resolve `owner/repo` from the repository remote, record the checked commit, and keep paths relative to the upstream repository root. Never write workstation absolute paths for external open-source evidence into SOW evidence.

### Pre-Implementation Gate

Implementation must not begin until the active SOW contains a concrete `## Pre-Implementation Gate` section. Before moving a SOW from `pending/open` to `current/in-progress`, or before continuing implementation in an existing current SOW that lacks this section, fill the gate.

The gate must record the problem/root-cause model, evidence reviewed, affected contracts and surfaces, existing patterns to reuse, risk and blast radius, sensitive data handling plan, implementation plan, validation plan, artifact impact plan, and open decisions. The sensitive data plan must cover SOWs, specs, documentation, project skills, agent instructions, and code comments. Generic placeholders such as `TBD`, `N/A`, or "to be checked later" are invalid unless the SOW explains why the item truly does not apply. If the gate exposes an unknown that cannot be resolved by investigation, stop and ask the user before implementation.

### When A SOW Is Required

Create or reuse a SOW for non-trivial work:

- feature work;
- bug fixes with behavioral impact;
- refactors;
- migrations;
- documentation or content changes with product/business impact;
- process changes;
- regressions;
- spec hygiene;
- project skill changes;
- benchmark/performance investigations;
- any work with unclear risk.

Trivial work does not need a SOW:

- typo fixes;
- formatting-only changes;
- mechanical rename with no behavior change;
- simple search/replace with low risk.

When unsure, treat the work as non-trivial.

### SOW Locations

- Pending: `.agents/sow/pending/`
- Current: `.agents/sow/current/`
- Done: `.agents/sow/done/`
- Specs: `.agents/sow/specs/`
- Template for new SOWs: `.agents/sow/SOW.template.md`
- Local audit: `.agents/sow/audit.sh`

Create new SOW files from `.agents/sow/SOW.template.md`. The template is project-local and may be customized for this repository.

Empty SOW directories must contain `.gitkeep` or `.keep` so the committed repository preserves the full SOW layout after clone/checkout.

Filename:

```text
SOW-NNNN-YYYYMMDD-{slug}.md
```

Status and directory must agree:

- `open` lives in `pending/`
- `in-progress` lives in `current/`
- `paused` lives in `current/`
- `completed` lives in `done/`
- `closed` lives in `done/`

### SOW Completion And Commit

The successful terminal SOW status is `completed`. `done` is a directory name, not a status value. Never write `Status: done` or `Status: complete`.

When a SOW's work is ready to close:

1. Finish implementation, docs, specs, skills, validation, and follow-up mapping.
2. Update the SOW to `Status: completed`.
3. Move the SOW file to `.agents/sow/done/`.
4. Commit the work, artifact updates, SOW status change, and SOW move together as one commit, unless the user explicitly requested a different commit split.

Do not create a separate commit just to mark or move the SOW. Do not claim a SOW is completed while the implementation and the SOW lifecycle change live in separate uncommitted or separately committed states.

### One SOW At A Time

Never execute multiple SOWs as one batch.

If work overlaps:

- merge or consolidate before implementation; or
- split into separate SOWs and complete one before starting the next.

Progress reports are not stop points. Once a SOW is in progress, continue until it is delivered, failed with evidence, blocked on a real user decision/approval, or superseded by newer user instructions.

### User Decisions

When user decisions are needed:

1. Present concrete evidence with files/lines or source references.
2. Provide numbered options.
3. Explain pros, cons, implications, and risks.
4. Recommend one option with reasoning.
5. Record the user's decision in the SOW before implementation.

### Followup Discipline

"Deferred" is not a terminal outcome.

Before a SOW can close, every valid deferred item must be:

- implemented in the current SOW; or
- explicitly rejected as not worth doing, with evidence; or
- represented by a real pending/current SOW file.

Pre-close, search the SOW for:

```text
defer|later|follow-up|future|TODO|pending
```

Map every remaining item to implemented, rejected, or tracked.

### Regressions

A regression is discovered after a SOW was considered completed or closed, later testing or use finds broken behavior, and the original SOW's claimed outcome is no longer true.

When behavior that a completed SOW claimed working stops working:

1. Find the original SOW in `done/`.
2. Move it back to `current/`.
3. Mark it `in-progress` with a regression note in `## Status`.
4. Append a new dated `## Regression - YYYY-MM-DD` section at the end of the file, after the original outcome, lessons, and follow-up content.
5. In that appended section, record what broke, evidence, why previous validation missed it, the repair plan, validation, and updates needed to specs, skills, docs, audits, or follow-up SOWs.
6. Fix and validate there.

Never prepend regression content above the original SOW narrative. The original requirements, analysis, plan, validation, outcome, lessons, and follow-up must remain readable first.
Do not create a new SOW for a true regression.

### Validation Gate

A SOW cannot be completed until Validation records:

- acceptance criteria evidence;
- tests or equivalent validation;
- real-use evidence when a runnable path exists;
- reviewer findings and how they were handled;
- same-failure search results;
- artifact maintenance gate for `AGENTS.md`, runtime project skills, specs, end-user/operator docs, end-user/operator skills, and SOW lifecycle;
- SOW status/directory consistency;
- spec update or specific reason no spec update was needed;
- project skill update or specific reason no skill update was needed;
- end-user/operator docs update or evidence-backed reason none were affected;
- end-user/operator skill update or evidence-backed reason none were affected by docs/spec changes;
- lessons extracted or specific reason there were none;
- follow-up mapping.

Generic "N/A" is invalid.

### Artifact Maintenance Gate

Every SOW close must explicitly record whether each durable artifact class was updated or why no update was needed:

- `AGENTS.md` - workflow, responsibility, local framework, project-wide guardrails.
- Runtime project skills - `.agents/skills/project-*/SKILL.md` for HOW to work here.
- Specs - `.agents/sow/specs/` for WHAT the project does.
- End-user/operator docs - README, docs site, runbooks, published guides, help text, or other human-facing documentation.
- End-user/operator skills - output/reference skills copied or consumed outside normal repo work.
- SOW lifecycle - split, merge, status, directory, deferred work, regression reopening, and follow-up mapping.

This is an assistant responsibility. If a SOW changes behavior, docs, specs, commands, schemas, defaults, workflows, examples, or operating procedure, the assistant must update every affected artifact in the same SOW, or record the evidence-backed reason an artifact is unaffected.

### Specs

Specs are memory of WHAT this project does.

The authoritative product and protocol specifications currently live under `docs/`. SOW specs under `.agents/sow/specs/` should capture durable project decisions, cross-cutting behavioral rules, and SOW-discovered facts that are not already represented in the public specs.

Update specs when shipped work changes:

- protocol behavior;
- public API contracts;
- wire formats;
- transport behavior;
- data formats;
- operational guarantees;
- known edge cases.

Specs describe current reality, not aspiration. If specs and code disagree, record the discrepancy in the active SOW and resolve or track it.

### Project Skills

Project skills are memory of HOW to work here.

Runtime input project skills should live under `.agents/skills/project-*/SKILL.md`. The `project-` prefix is the generic hook meaning "agents working in this repo must consider this skill." Before non-trivial work, inspect those skill descriptions and load every matching runtime skill.

Do not create generic `project-*` skills only to make the framework look complete. Project skills will be added incrementally when concrete reusable workflow knowledge exists.

Output/reference skills may also exist as docs or exported artifacts. Do not rename, shorten, or change their descriptions only to satisfy runtime discovery. Instead, list them separately below and update them when the related public/operator workflow changes.

### Project Skills Index

Runtime input skills:

- `.agents/skills/project-netdata-vendoring/SKILL.md`
  Trigger: any work that copies, syncs, vendors, merges, or checks NetIPC source changes into a Netdata checkout.
  Purpose: require source `plugin-ipc` CI and GitHub code/security scanner preflight before touching Netdata's vendored NetIPC copy.

Output/reference skills:

- `docs/netipc-integrator-skill.md`
  Consumer: downstream assistants or operators integrating NetIPC services.
  Update when: public APIs, examples, service workflow, transport support, validation commands, or integration guidance changes.

### Project-specific commands

- Configure/build C plus available Rust and Go pieces: `make`
- Explicit configure/build: `cmake -S . -B build` then `cmake --build build`
- Run CTest after configure/build: `ctest --test-dir build --output-on-failure`
- POSIX coverage: `bash tests/run-coverage-c.sh`, `bash tests/run-coverage-rust.sh`, `bash tests/run-coverage-go.sh`
- POSIX interop examples: `bash tests/interop_codec.sh`, `bash tests/test_uds_interop.sh`, `bash tests/test_shm_interop.sh`, `bash tests/test_service_interop.sh`
- `tests/interop_codec.sh` configures `build/`; do not run it in parallel with CTest or another build-directory validation command.
- Windows validation scripts are under `tests/run-*-windows*.sh` and `tests/test_*_win*.sh`; use the specific script matching the touched transport/API.
- Benchmark generation: `bash tests/generate-benchmarks-posix.sh` or `bash tests/generate-benchmarks-windows.sh`
- Netdata vendor diff: `bash ./diff-netdata-vendor.sh /path/to/netdata`
- Netdata vendor copy: `bash ./vendor-to-netdata.sh /path/to/netdata`

### Project-specific overrides

- The public specs under `docs/` are authoritative. When implementation and specs disagree, treat the specs as right unless a SOW explicitly revises them.
- Preserve the layer boundaries in `docs/code-organization.md`: Codec owns wire format; transport owns connection/framing/profile mechanics; service owns typed orchestration and snapshot helpers.
- Cross-language interoperability is mandatory across C, Rust, and Go. Do not change one language implementation without checking the corresponding contract and other language implementations.
- Go must remain pure Go without `cgo`.
- Performance claims require benchmark evidence. Do not adjust benchmark floors or accept parity gaps without a fact-based analysis.
- Before any Netdata vendoring work, load `.agents/skills/project-netdata-vendoring/SKILL.md` and complete its preflight even if the user did not explicitly ask for CI, scanner, or drift checks. Do not modify Netdata's vendored NetIPC copy until the active SOW records: the candidate `plugin-ipc` source commit; GitHub Actions/check-run evidence; GitHub code-scanning, Dependabot, and secret-scanning evidence; the last source-to-Netdata vendoring baseline; a two-way gap analysis of what changed in `plugin-ipc` since that baseline and what changed in Netdata's vendored NetIPC copy since that baseline; and a migration plan for every drift class. Failing, pending, cancelled, timed-out, or untriaged open scanner findings block vendoring unless they are fixed, documented as evidence-backed false positives, or explicitly risk-accepted by the user in the SOW. Unknown baseline, unexplained downstream drift, or no migration plan also blocks vendoring.
- Archived TODO history under `.agents/sow/todo-history/TODO-*` is existing project memory. Do not move, delete, or rewrite those archived notes without explicit user approval; track future classification or migration through a SOW.

### Preservation Notes

- No pre-existing `AGENTS.md` or tool-specific instruction file was found during SOW bootstrap.
- Existing public docs under `docs/` were preserved as the authoritative specification surface.
- Existing root `TODO*.md` files were archived under `.agents/sow/todo-history/` by explicit user request and tracked through SOW-0002.
- No runtime `project-*` skills were created during bootstrap by user request.

Project SOW status: initialized
