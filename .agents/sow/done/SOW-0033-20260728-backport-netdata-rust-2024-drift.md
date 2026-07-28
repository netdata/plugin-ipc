# SOW-0033 - Backport Netdata Rust 2024 Vendor Drift

## Status

Status: completed

Sub-state: Backport, validation, review, follow-up mapping, and source/vendor parity restoration are complete.

## Requirements

### Purpose

Restore source ownership and normalized equality for valid Rust compatibility changes made in Netdata's vendored NetIPC copy.

### User Request

The user approved backporting the five identified Rust source differences from Netdata into `plugin-ipc` and committing the result.

### Assistant Understanding

Facts:

- `plugin-ipc` is the authoritative source for vendored C, Rust, and Go NetIPC implementations.
- The latest verified exact-sync baseline is plugin-ipc commit `37cce82d4b0e1e9d1fbee2ff2ab561cc9920ffa6` and Netdata commit `d5796cb0a841adaabdd68d427664155d8e30d46e`.
- Current plugin-ipc commit `44cfb8a999abcd649a1fa21db5ecc9d01c5fffa7` has no vendored-path changes after the source baseline.
- Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb` introduced five source-owned Rust compatibility changes and one Netdata-local Cargo packaging change after the baseline.
- The normalized comparison reports no C or Go differences and exactly five Rust source differences.
- The source crate remains on Rust edition 2021. Netdata uses edition 2024 with a workspace MSRV of Rust 1.91.

Inferences:

- The five source changes are valid upstream improvements because explicit unsafe operations, safe naming, and the lint-compatible function-pointer cast also compile under edition 2021.
- Backporting them prevents the next source-to-Netdata vendor copy from removing Netdata's edition-2024 compatibility fixes.

Unknowns:

- No implementation-blocking unknown remains. Exact downstream hunks, their introducing commit, their edition rationale, and the affected source files are established from repository history and direct comparison.

### Acceptance Criteria

- The five source-owned Rust files match Netdata after the backport.
- `src/crates/netipc/Cargo.toml` remains on edition 2021; Netdata workspace packaging is not copied upstream.
- Normalized C, Rust, and Go vendor comparison reports no differences.
- Rust formatting, native tests, and available Windows-target checks pass at low scheduler priority.
- Same-pattern searches find no remaining matching edition-2024 compatibility drift in the affected Rust source.
- Only the five Rust files, this completed SOW, and the user-approved pending Rust 2024 follow-up SOW are committed.

## Analysis

Sources checked:

- `.agents/skills/project-netdata-vendoring/SKILL.md`
- `.agents/sow/current/SOW-0027-20260629-netdata-vendor-memory-safety-update.md`
- `.agents/sow/done/SOW-0030-20260712-shm-sigbus-on-full-backing-store.md`
- `diff-netdata-vendor.sh`
- `src/crates/netipc/Cargo.toml`
- The five affected Rust source files in plugin-ipc and Netdata
- Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb`

Pre-implementation state:

- plugin-ipc `44cfb8a999abcd649a1fa21db5ecc9d01c5fffa7` is clean and matches its local `origin/main` tracking ref.
- Netdata `01d56fddc20ac4a73ea74da19e49420cab764ad0` has no uncommitted NetIPC vendor-path changes.
- The prior exact-sync pair is recorded in SOW-0030 and independently confirmed by history.
- Netdata's only post-baseline NetIPC source commit is `3f646983eac1f001ab652b34383a20aaff35f3eb`.

Risks:

- Copying Netdata's `Cargo.toml` would incorrectly import downstream workspace policy into the standalone source crate.
- Missing a Windows-only unsafe FFI hunk would leave future Netdata Windows builds vulnerable to edition-2024 compilation failures.
- Broad file replacement could copy unrelated downstream formatting or future drift; implementation must apply only the reviewed five-file patch.
- Validation on only the native POSIX target would miss Windows-gated syntax unless an installed Windows target is checked.

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- Netdata migrated its Rust workspace to edition 2024 after the last exact NetIPC vendor sync. That migration required explicit unsafe FFI declarations and operations, renaming the newly reserved identifier `gen`, and a lint-compatible function-pointer cast. Those valid fixes were made downstream because the standalone source crate remains edition 2021, creating five-file source drift that a future vendor copy would overwrite.

Evidence reviewed:

- SOW-0030 records exact post-merge equality at plugin-ipc `37cce82d4b0e1e9d1fbee2ff2ab561cc9920ffa6` and Netdata `d5796cb0a841adaabdd68d427664155d8e30d46e`.
- `git diff 37cce82d4b0e1e9d1fbee2ff2ab561cc9920ffa6..44cfb8a999abcd649a1fa21db5ecc9d01c5fffa7` reports no source-side changes in the vendored C, Rust, or Go paths.
- `netdata/netdata @ 01d56fddc20ac4a73ea74da19e49420cab764ad0`, commit `3f646983eac1f001ab652b34383a20aaff35f3eb`, changes:
  - `src/crates/netipc/src/service/raw/server.rs:110`
  - `src/crates/netipc/src/service/raw_unix_tests.rs:4143`
  - `src/crates/netipc/src/transport/shm.rs:803-807,903-912,1146-1149`
  - `src/crates/netipc/src/transport/win_shm.rs:29,277-336`
  - `src/crates/netipc/src/transport/windows.rs:50`
- `netdata/netdata @ 01d56fddc20ac4a73ea74da19e49420cab764ad0`, `src/crates/Cargo.toml:66-67`, selects edition 2024 and Rust 1.91; `src/crates/netipc/Cargo.toml:4` inherits that workspace edition.
- The project-local normalized diff reports no C or Go differences and exactly the five Rust source differences above.

Affected contracts and surfaces:

- Rust NetIPC source compilation under editions 2021 and 2024.
- POSIX SHM implementation and one POSIX signal-interruption test.
- Windows service, Named Pipe, and SHM FFI declarations and calls.
- Source-to-Netdata vendor equality.
- No public API, ABI, protocol, wire-format, runtime behavior, or configuration contract changes.

Existing patterns to reuse:

- Source-first downstream drift backports recorded in SOW-0027.
- Exact normalized comparison through `diff-netdata-vendor.sh`.
- Low-priority validation through `tests/run-low-priority.sh`.
- Netdata's reviewed mechanical patch from commit `3f646983eac1f001ab652b34383a20aaff35f3eb`.

Risk and blast radius:

- Low behavioral risk: identifier renames and explicit unsafe syntax do not change generated protocol behavior.
- Medium portability risk if a reviewed Windows-only hunk is omitted; mitigate with exact vendor comparison and a Windows-target check when the target is installed.
- No service, database, configuration, environment, package, network, or production runtime changes are involved.
- No Netdata checkout will be modified by this SOW.

Sensitive data handling plan:

- The work requires only public source paths, commit hashes, compiler/test output, and repository history.
- SOWs, specs, docs, skills, instructions, and code comments will contain no credentials, secrets, customer identifiers, personal data, private endpoints, or production incident details.

Implementation plan:

1. Apply only the five reviewed Rust source changes from Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb`.
2. Preserve the standalone source crate's edition-2021 manifest and make no Netdata checkout changes.
3. Run formatting, native Rust tests, an available Windows-target check, normalized vendor comparison, same-pattern searches, diff checks, sensitive-data audit, and review.
4. Create the approved pending Rust 2024 migration SOW, complete and move this SOW, then commit exactly the five Rust files and both SOW artifacts.

Validation plan:

- Run `cargo fmt --check` for the standalone NetIPC crate at low priority.
- Run the standalone Rust test suite at low priority.
- Inspect installed Rust targets and run an all-targets Windows check when available.
- Run `diff-netdata-vendor.sh` and require no normalized C, Rust, or Go differences.
- Search the affected Rust source for remaining plain Windows `extern "system"` blocks, reserved `gen` identifiers, and the direct signal-handler-to-integer cast.
- Run `git diff --check`, the changed-file sensitive-data audit, and `.agents/sow/audit.sh`.
- Review the final diff against Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb` and inspect same-pattern occurrences beyond the original five hunks.

Artifact impact plan:

- AGENTS.md: no update expected because source ownership and vendoring rules already cover this case.
- Runtime project skills: no update expected because `project-netdata-vendoring` already requires two-way drift classification and source-first handling.
- Specs: no update expected because the patch changes compiler compatibility syntax, not behavior or contracts.
- End-user/operator docs: no update expected because no API, configuration, behavior, or operating procedure changes.
- End-user/operator skills: no update expected because integration guidance is unchanged.
- SOW lifecycle: this new post-baseline drift is isolated from paused SOW-0027; complete and move SOW-0033 with the implementation commit, while the broader user-approved edition migration is tracked by pending SOW-0034.

Open-source reference evidence:

- `netdata/netdata @ 01d56fddc20ac4a73ea74da19e49420cab764ad0`
  - `src/crates/Cargo.toml:66-67`
  - `src/crates/netipc/Cargo.toml:4,10,13`
  - `src/crates/netipc/src/service/raw/server.rs:110`
  - `src/crates/netipc/src/service/raw_unix_tests.rs:4143`
  - `src/crates/netipc/src/transport/shm.rs:803-807,903-912,1146-1149`
  - `src/crates/netipc/src/transport/win_shm.rs:29,277-336`
  - `src/crates/netipc/src/transport/windows.rs:50`

Open decisions:

- Resolved by the user on 2026-07-28: backport and commit the five source-owned Rust changes.
- Resolved scope: preserve plugin-ipc's edition-2021 manifest and Netdata's workspace-owned packaging rather than conflating source compatibility with an edition migration.
- Resolved after validation exposed broader standalone-target work: finish and commit this surgical vendor-drift backport first, then execute a separate full Rust 2024 migration SOW.

## Implications And Decisions

1. **Source ownership - selected: backport the five changes to plugin-ipc.**
   - This is the long-term-best option because it prevents future vendoring from erasing valid downstream compiler-compatibility fixes.
2. **Packaging scope - selected: do not copy Netdata's Cargo manifest changes.**
   - Netdata's manifest inherits workspace edition and dependencies; plugin-ipc is a standalone crate and retains its existing edition-2021 contract.
3. **Destination scope - selected: change plugin-ipc only.**
   - The Netdata vendored source already contains the fixes and requires no modification.
4. **Broader Rust 2024 work - selected: A then B.**
   - Complete this exact five-file backport as the surgical first commit.
   - Track benchmark, fixture, drop-order, edition, and MSRV migration work in a separate pending SOW so it receives its own design analysis and validation.

## Plan

1. Apply the exact five-file Rust compatibility patch with no manifest or behavior changes.
2. Validate native compilation, exact Windows-source parity with the reviewed downstream patch, normalized vendor equality, formatting, tests, same-pattern safety, and repository hygiene.
3. Create pending SOW-0034 for the broader Rust 2024 migration.
4. Complete the SOW lifecycle and commit only the five Rust files, completed SOW-0033, and pending SOW-0034.

## Execution Log

### 2026-07-28

- Completed read-only baseline reconstruction and two-way vendor drift classification.
- The user approved the source-first five-file backport and requested a commit.
- Applied the exact five source-owned Rust hunks from Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb`.
- `cargo fmt --check` passed and the native standalone Rust suite passed 380/380 tests.
- The normalized vendor comparison now reports no C, Rust, or Go differences.
- A broad `RUSTFLAGS='-D rust-2024-compatibility' cargo check --all-targets` exposed pre-existing edition-migration work outside the vendored source:
  - reserved `gen` identifiers in `bench/drivers/rust/src/main.rs` and `bench/drivers/rust/src/bench_windows.rs`;
  - Rust 2024 temporary drop-order warnings in benchmark and fixture connection loops.
- Stopped before committing and presented the scope evidence. The user selected the surgical backport first, followed by a separate full Rust 2024 migration.
- Created pending SOW-0034 to track the full edition migration, including benchmark/fixture diagnostics, destructor-order analysis, MSRV policy, and native Windows validation.
- Scoped Rust 2024 compatibility linting for the vendored library and its unit tests passed.
- Same-pattern searches found no plain C/system extern blocks, `gen` declarations, or direct signal-handler-to-integer casts under `src/crates/netipc/src`.
- An independent review found no code blockers, confirmed all five files are byte-for-byte identical to Netdata's reviewed source, and classified every unsafe/name/cast hunk as behavior-preserving.

## Validation

Acceptance criteria evidence:

- All five source-owned Rust files are byte-for-byte identical to `netdata/netdata @ 01d56fddc20ac4a73ea74da19e49420cab764ad0` after the backport.
- `src/crates/netipc/Cargo.toml` is unchanged and still selects edition 2021.
- `diff-netdata-vendor.sh` reports no normalized C, Rust, or Go differences.
- The five Rust files, completed SOW-0033, and pending SOW-0034 are the only intended commit paths.

Tests or equivalent validation:

- `tests/run-low-priority.sh cargo fmt --manifest-path src/crates/netipc/Cargo.toml -- --check`: passed.
- `tests/run-low-priority.sh cargo test --manifest-path src/crates/netipc/Cargo.toml --lib --tests`: passed; 380/380 library tests passed and all declared zero-test binary harnesses completed successfully.
- `RUSTFLAGS='-D rust-2024-compatibility' tests/run-low-priority.sh cargo test --manifest-path src/crates/netipc/Cargo.toml --lib --no-run`: passed for the vendored library and unit-test source.
- `git diff --check`: passed.
- No Windows Rust target is installed locally. The Windows-only files are byte-for-byte identical to Netdata commit `3f646983eac1f001ab652b34383a20aaff35f3eb`, whose recorded validation ran `cargo check --target x86_64-pc-windows-gnu -p netipc --all-targets` with zero errors or warnings.

Real-use evidence:

- The native standalone Rust test suite exercised the POSIX service, UDS, SHM, cache, retry, abort, stale-recovery, and protocol paths successfully.
- The project-local consumer comparison reports exact normalized equality for all C, Rust, and Go vendored source trees.

Reviewer findings:

- Independent review found no blocking code, safety, portability, or behavior issue.
- Review confirmed the five files exactly match Netdata's reviewed patch.
- Review confirmed explicit unsafe blocks and `unsafe extern` declarations only make existing FFI obligations explicit; identifier and cast changes preserve behavior.
- Review identified SOW commit-scope, historical-state, and follow-up wording contradictions; all were corrected before closure.

Same-failure scan:

- No plain `extern "system"` or `extern "C"` block remains under `src/crates/netipc/src`.
- No declaration using the edition-2024 reserved identifier `gen` remains under `src/crates/netipc/src`.
- No direct `noop_signal_handler as usize` cast remains.
- Review confirmed all six `unsafe fn` bodies in the vendored Rust source explicitly scope their unsafe operations.
- Broader non-vendored benchmark/fixture edition findings are not deferred informally; they are tracked by `.agents/sow/pending/SOW-0034-20260728-rust-2024-migration.md`.

Sensitive data gate:

- `SOW_AUDIT_SENSITIVE_CHANGED=1 bash .agents/sow/audit.sh` scanned the changed durable artifacts and reported no sensitive-data patterns.
- Source and SOW changes contain only public repository paths, public commit hashes, and sanitized validation summaries.

Artifact maintenance gate:

- AGENTS.md: no update needed; existing source-ownership, vendoring, SOW, and low-priority validation rules directly covered this work.
- Runtime project skills: no update needed; `project-netdata-vendoring` already requires baseline reconstruction, two-way drift classification, and source-first backports.
- Specs: no update needed; the patch changes Rust compiler compatibility syntax without changing API, ABI, wire format, transport behavior, or operational guarantees.
- End-user/operator docs: no update needed; no public command, configuration, behavior, or integration procedure changed.
- End-user/operator skills: no update needed; `docs/netipc-integrator-skill.md` already directs changes to the source repository and no integration contract changed.
- SOW lifecycle: SOW-0033 will be marked `completed` and moved to `done/` with the implementation commit; full edition migration is represented by pending SOW-0034.

Specs update:

- No spec update was needed because the five changes are behavior-preserving Rust syntax and naming adjustments.

Project skills update:

- No project skill update was needed because the existing vendoring skill identified and governed the exact drift class successfully.

End-user/operator docs update:

- No end-user/operator documentation changed because build commands, APIs, configuration, and runtime behavior remain unchanged.

End-user/operator skills update:

- No output/reference skill update was needed because the public integration workflow and requirements remain unchanged.

Lessons:

- A downstream workspace edition migration can create valid source-owned drift even when protocol and runtime behavior remain unchanged.
- Rust edition validation must include every manifest-declared target; library parity alone does not prove the standalone benchmarks and fixtures are migration-ready.

Follow-up mapping:

- Full standalone Rust 2024 migration, including benchmark/fixture findings, drop-order analysis, MSRV policy, and Windows validation: `.agents/sow/pending/SOW-0034-20260728-rust-2024-migration.md`.
- No other valid deferred item remains.

## Outcome

The five valid Netdata Rust compatibility changes are source-owned again. Normalized C, Rust, and Go vendor trees are equal, native tests and scoped Rust 2024 compatibility checks pass, and the broader edition migration is isolated in pending SOW-0034.

## Lessons Extracted

- Periodically compare normalized vendor trees even after an exact sync; downstream toolchain migrations can introduce source-worthy compatibility fixes.
- Keep a surgical parity restoration separate from a full language-edition migration when the latter exposes broader runtime-lifetime and support-policy decisions.

## Followup

- `.agents/sow/pending/SOW-0034-20260728-rust-2024-migration.md` tracks the user-approved full Rust 2024 migration.

## Regression Log

None yet.
