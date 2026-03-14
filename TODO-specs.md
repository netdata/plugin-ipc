# TODO-specs

## Purpose
- Fit-for-purpose goal:
  - create four authoritative architecture/specification guides for `plugin-ipc`
  - use them as the project "GOD" before any further architectural rewrite
  - base them on the current active TODO plus archived history, with latest decisions overriding earlier ones

## TL;DR
- Costa requested:
  - create `docs/`
  - write four spec files there:
    - level 1 transport API
    - level 2 strongly typed API
    - level 3 snapshot API
    - code organization guide
- Current immediate requirement:
  - read every `docs/*.md` file in full
  - do not skim or pattern-scan them
  - treat the docs as the current review baseline before any further analysis
- These specs must be derived from:
  - `TODO-plugin-ipc.md`
  - `TODO-plugin-ipc.history.md`
- Where older and newer decisions conflict:
  - the latest decisions win

## Analysis
- Fact (2026-03-14): `docs/` currently contains 12 markdown files and 2696 total lines.
- Fact (2026-03-14): all current `docs/*.md` files were read in full, not skimmed, and now form the active documentation baseline for further discussion.
- Current known sources of truth:
  - active spec / execution plan:
    - `TODO-plugin-ipc.md`
  - archived decision/history transcript:
    - `TODO-plugin-ipc.history.md`
  - current conversation context
  - this client's persisted session/thread data under `~/.codex`
- Current architectural risk:
  - the codebase does not consistently reflect the agreed 3-level structure yet
  - therefore the spec documents must be stronger than the current public API shape
- Verbatim-response extraction sources for disambiguation:
  - primary:
    - current conversation context
  - primary persisted source:
    - per-thread rollout history under:
      - `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`
    - this is the best persisted source for verbatim turns/messages
    - evidence from Codex source:
      - `~/src/codex.git/codex-rs/app-server/README.md`
        - `thread/read` with `includeTurns` loads stored turns
      - `~/src/codex.git/codex-rs/core/src/rollout/recorder.rs`
        - rollout files are written as `rollout-...jsonl`
      - `~/src/codex.git/codex-rs/core/src/rollout/list.rs`
        - session layout is `~/.codex/sessions/YYYY/MM/DD/rollout-...jsonl`
  - metadata index for locating rollout files:
    - SQLite state DB under:
      - `~/.codex/state_5.sqlite`
    - this stores thread metadata plus `rollout_path`
    - it does **not** by itself store the full transcript/turn content
    - evidence from Codex source:
      - `~/src/codex.git/docs/config.md`
        - SQLite state DB lives under `sqlite_home` / `CODEX_SQLITE_HOME`, defaulting to `CODEX_HOME`
      - `~/src/codex.git/codex-rs/state/src/lib.rs`
        - current state DB filename/version is `state_5.sqlite`
      - `~/src/codex.git/codex-rs/state/src/runtime.rs`
        - exact path builder uses `codex_home.join(state_db_filename())`
      - local schema check:
        - `threads` table contains `id`, `rollout_path`, and metadata fields
  - weaker fallback source:
    - text-only global history:
      - `~/.codex/history.jsonl`
    - evidence from Codex source:
      - `~/src/codex.git/codex-rs/core/src/message_history.rs`
        - global append-only message history lives at `~/.codex/history.jsonl`
- Local environment facts relevant to extraction:
  - `CODEX_HOME` and `CODEX_SQLITE_HOME` were not set in this shell when checked
  - therefore the effective local persistence root is the default:
    - `~/.codex`
- Extraction rule for this task:
  - when TODO/history wording is ambiguous, use this precedence:
    1. Costa's current conversation replies
    2. Costa's verbatim replies in the relevant `~/.codex/sessions/.../rollout-*.jsonl`
    3. latest binding wording in `TODO-plugin-ipc.md`
    4. older wording in `TODO-plugin-ipc.history.md`
  - if newer verbatim Costa wording conflicts with older TODO/history wording:
    - newer Costa wording wins
- Required evidence gathering:
  - extract all binding architectural decisions from:
    - active TODO
    - archived history
    - verbatim Costa replies from current session / persisted rollout history
  - identify conflicts and resolve them by recency
  - ensure the resulting specs describe:
    - required behavior
    - forbidden patterns
    - layering boundaries
    - client and server responsibilities

## Decisions
- Made:
  - create `TODO-specs.md`
  - create `docs/`
  - write the specs in sequence, not all at once
  - first draft and close:
    - level 1 transport API spec
    - level 2 strongly typed API spec
    - level 3 snapshot API spec
  - only after 1-3 are drafted and reviewed:
    - derive the code-organization guide from them
    - discuss it explicitly before treating it as normative
  - latest decisions override previous ones when conflicts exist
  - the first pass should attempt to produce fully closed specs with no open questions left inside them
  - if real unanswered questions are discovered during drafting:
    - stop treating the affected spec as final
    - surface the exact unresolved questions
    - resolve them before the spec becomes normative
  - extract Costa's prior responses verbatim where they clarify ambiguous TODO/history wording
  - specs for level 1, level 2, and level 3 must require:
    - 100% testing coverage
    - fuzz testing / fuzziness coverage
    - explicit corner-case and abnormal-path coverage
    - no exceptions
  - the testing requirement is not cosmetic:
    - the explicit purpose is to make malformed IPC, corner cases, and abnormal situations unable to crash Netdata
- Pending:
  - none before drafting the first spec set

## Plan
1. Read every existing `docs/*.md` file in full as the current authoritative documentation baseline.
2. Audit `TODO-plugin-ipc.md` for the current binding decisions.
3. Audit `TODO-plugin-ipc.history.md` for earlier decisions that may still matter.
4. Extract Costa's prior responses verbatim where they clarify ambiguous architecture wording:
   - current conversation context first
   - then `~/.codex/sessions/.../rollout-*.jsonl`
   - use `~/.codex/state_5.sqlite` only to locate the right rollout path when needed
   - use `~/.codex/history.jsonl` only as a weaker fallback
5. Extract conflicts and resolve them by latest-decision-wins.
6. Draft `docs/level1-transport.md` as a closed spec.
7. Draft `docs/level2-typed-api.md` as a closed spec.
8. Draft `docs/level3-snapshot-api.md` as a closed spec.
9. Identify any genuine unanswered questions exposed by those 3 specs.
10. Resolve those questions before the affected specs become normative.
11. Only then derive and discuss `docs/code-organization.md`.
12. Cross-check the specs against the current codebase.
13. Summarize the resulting gap-analysis baseline.

## Implied Decisions
- The specs are normative:
  - code must adapt to the specs
  - the specs do not adapt to accidental current code shape
- The specs must be concrete and enforceable:
  - not vague architecture prose
- “Unresolved question” means:
  - a real architectural or behavioral ambiguity that cannot be derived from the latest approved decisions
  - not mere uncertainty caused by implementation drift or incomplete recollection
- Netdata integration quality bar:
  - nothing should be integrated unless the resulting code has:
    - complete test coverage expected by the specs
    - fuzz coverage where protocol/state-machine parsing or framing is involved
    - strong abnormal-path coverage proving it will not crash Netdata under malformed or failing IPC conditions
    - explicit evidence that request/response, chunking, pipelining, reconnect, and shutdown corner cases are exercised

## Testing Requirements
- No runtime behavior change is intended from this task alone.
- Validation for this task is:
  - internal consistency across the 4 specs
  - consistency with latest approved decisions
  - no contradiction with still-binding protocol/transport facts already implemented

## Documentation Updates Required
- Create:
  - `docs/level1-transport.md`
  - `docs/level2-typed-api.md`
  - `docs/level3-snapshot-api.md`
  - `docs/code-organization.md` only after 1-3 are closed
- Update references from:
  - `TODO-plugin-ipc.md`
  - `TODO-specs.md`
