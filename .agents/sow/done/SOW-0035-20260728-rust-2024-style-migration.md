# SOW-0035 - Coordinated Rust 2024 Style Migration

## Status

Status: completed

Sub-state: delivered 2026-10-31 in plugin-ipc; the Netdata half is delivered by SOW-0050's vendoring (decision 2).

## Requirements

### Purpose

Migrate plugin-ipc and Netdata's corresponding vendored NetIPC Rust source to
the Rust 2024 style edition without mixing formatting churn into the semantic
edition migration.

### User Request

When rustfmt 1.97.1 exposed a repository-wide style migration during SOW-0034,
the user selected a focused semantic migration first and a separately tracked,
coordinated formatting migration afterward.

### Assistant Understanding

Facts:

- Rust language and style editions can be configured independently.
- With language edition 2024 and no style override, rustfmt 1.97.1 proposes 98
  formatting hunks across 40 standalone files.
- Thirty-six matching Netdata vendored source files also fail the same
  rustfmt-2024 check.
- SOW-0034 preserves style edition 2021 explicitly to avoid obscuring semantic
  review and creating unplanned vendor drift.

Inferences:

- A coordinated formatting-only change can be reviewed and validated more
  reliably than a mixed semantic-and-formatting diff.
- Netdata vendoring preflight is mandatory because this follow-up intentionally
  changes vendored source formatting.

Unknowns:

- The source and Netdata baseline commits at the time this SOW is activated.
- Whether later rustfmt releases or intervening source changes alter the exact
  formatting diff.

### Acceptance Criteria

- Plugin-ipc selects Rust 2024 style explicitly and all Rust targets pass
  formatting checks.
- The formatting diff is behavior-neutral and separated from semantic changes.
- Netdata vendoring preflight passes and its vendored NetIPC source uses the
  same formatting without overwriting Netdata-only files.
- Plugin-ipc and Netdata validation pass on their supported POSIX and Windows
  paths.

## Analysis

Sources checked:

- `src/crates/netipc/Cargo.toml`
- Rustfmt 1.97.1 formatting diagnostics from SOW-0034
- `.agents/skills/project-netdata-vendoring/SKILL.md`
- Netdata's vendored `src/crates/netipc/` source

Current state:

- SOW-0034 is the only executing SOW and explicitly preserves style edition
  2021.
- This formatting migration is intentionally queued until the semantic Rust
  2024 migration completes.

Risks:

- A large formatting diff can hide accidental semantic edits.
- Updating plugin-ipc alone would create avoidable downstream vendor drift.
- Netdata CI/toolchain changes between SOWs may change the expected formatting.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- `rustfmt.toml` pins style edition 2021 while the crate uses language edition 2024. The pin was a deliberate stopgap from SOW-0034; this SOW removes it.

Evidence reviewed:

- SOW-0034 has completed (`done/`).
- With `style_edition = "2024"`, rustfmt 1.9.0 (stable) reformats 42 of the 61 tracked `.rs` files; rustfmt 1.8.0 (Rust 1.91.0, the MSRV) produces the identical result (`cargo fmt --check` passes with both after the change).
- Netdata (`ktsaou/netdata @ ae9113f2cc`): workspace `edition = "2024"` (`src/crates/Cargo.toml:66`), no `rustfmt.toml` in its tree and no `cargo fmt` CI check, so Netdata's own Rust code formats in style 2024.

Affected contracts and surfaces:

- Rust source formatting only; `rustfmt.toml`; contributor notes in AGENTS.md and README.md. No runtime, API or wire change.

Existing patterns to reuse:

- CI `cargo fmt --all --check` (`.github/workflows/static-analysis.yml:88`).

Risk and blast radius:

- A large mechanical diff could hide a semantic edit; the formatting-only proof below rules that out.

Performance assessment:

- Formatting only; no compiled-code change. No benchmark run.

Sensitive data handling plan:

- Public source and command results only. Covers this SOW, docs and code comments.

Implementation plan:

1. Set `style_edition = "2024"` in `rustfmt.toml`; run `cargo fmt --all`.
2. Prove the diff is formatting-only.
3. Build and test; update AGENTS.md and README.md.

Validation plan:

- `cargo fmt --check` with stable and 1.91.0; `rustfmt --edition 2024 --check` over every tracked `.rs` file; formatting-only proof; `cargo test` on Linux; builds for Linux, FreeBSD, the Windows target and MSRV; clippy gates.

Artifact impact plan:

- AGENTS.md and README.md style notes; no spec, skill or end-user doc change (no behavior change).

Open-source reference evidence:

- Netdata paths above (`ktsaou/netdata @ ae9113f2cc`).

Open decisions:

- None (decisions below are assistant-applied under the user's autonomy instruction).

## Implications And Decisions

1. **Ordering - selected by the user: semantic migration before formatting.**
   - Keep SOW-0034 focused and coordinate style changes in this SOW afterward.
2. **Netdata half moves to SOW-0050 (2026-10-31, assistant-applied, long-term-best).** SOW-0050 vendors every plugin-ipc change since the last baseline with the full preflight; a separate vendoring for formatting alone would repeat that preflight and touch the same files twice. SOW-0050 records the requirement that the vendored Rust copy passes `rustfmt --edition 2024 --check`.

## Plan

The gate's implementation plan.

## Execution Log

### 2026-07-28

- Created as the real tracked follow-up required by the user's SOW-0034
  formatting-scope decision.
- No implementation started.

### 2026-10-31

- `rustfmt.toml`: `style_edition = "2024"`; `cargo fmt --all` reformatted 42 files (219 insertions, 239 deletions).
- AGENTS.md and README.md style notes updated; SOW-0050 records the Netdata half.

## Validation

Acceptance criteria evidence:

- Style 2024 selected explicitly (`rustfmt.toml`); `cargo fmt --all --check` passes with stable (rustfmt 1.9.0) and Rust 1.91.0 (rustfmt 1.8.0); `rustfmt --edition 2024 --check` passes for all 61 tracked `.rs` files, including ones cargo does not reach.
- Formatting-only proof (scripted comparison of each changed file against a pre-change snapshot):
  - outside `use` statements, all 42 files are identical after removing whitespace and trailing commas, except 4 files (`transport/posix.rs`, `shm.rs`, `win_shm.rs`, `windows.rs`) whose only difference is a `;` added after a tail `return ...` expression (a `return` diverges, so the semicolon changes nothing); the script checked that every added `;` ends a `return` statement;
  - `use` statements import the same multiset of names in every file (only order changed: style 2024 version-sorting).
- Netdata half: delivered by SOW-0050 (decision 2).

Tests or equivalent validation:

- Linux `cargo test`: 387 passed. Windows (MSYS2 host) `cargo test`: 266 passed.
- Builds: Rust 1.91.0 `--all-targets` (Linux), FreeBSD `--all-targets`, clippy CI gate on Linux and `x86_64-pc-windows-gnu`: all clean.

Real-use evidence:

- No runtime path changed; the test suite and builds above are the evidence.

Reviewer findings:

- No external review; the scripted formatting-only proof replaces a manual read of the 42-file diff.

Same-failure scan:

- No other style pin: `rustfmt.toml` is the only rustfmt config in the repository (`find . -name rustfmt.toml`).

Sensitive data gate:

- Public source paths and command results only.

Artifact maintenance gate:

- AGENTS.md: Rust style note updated (style 2024, check with both toolchains).
- Runtime project skills: `project-netdata-vendoring` unchanged; SOW-0050 carries the formatting requirement.
- Specs: no product behavior change.
- End-user/operator docs: README.md "Building And Running" style note updated.
- End-user/operator skills: `docs/netipc-integrator-skill.md` exposes no formatting command; unchanged.
- SOW lifecycle: moved to `done/`; Netdata half tracked in SOW-0050.

## Outcome

plugin-ipc formats its Rust code with style edition 2024, matching its language edition and Netdata's workspace. The change is formatting-only, proven per file.

## Lessons Extracted

- A whitespace-stripped comparison plus an import-name multiset check is enough to prove a rustfmt edition migration is behavior-neutral; the only other construct style 2024 changes is the tail-`return` semicolon.

## Followup

- Netdata vendored copy: SOW-0050.
- Search of this SOW for defer/later/follow-up/future/TODO/pending: only the SOW-0034/SOW-0050 references above and historical text.

## Regression Log

None yet.
