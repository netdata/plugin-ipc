# SOW-0015 - Codacy Scope And Maintainability

## Status

Status: in-progress

Sub-state: Codacy test/bench exclusion and refreshed production-source maintainability baseline in progress.

## Requirements

### Purpose

Keep Codacy complexity and duplication signals useful for production SDK hygiene by excluding test/benchmark code from Codacy maintainability metrics, then continue improving real production files one file at a time without hiding useful rules.

### User Request

The user approved proceeding after the assistant reported that test/bench exclusion was not properly completed and recommended committing/importing Codacy global excludes for `tests/**` and `bench/**`, reanalyzing Codacy, then continuing file-by-file on remaining production files.

### Assistant Understanding

Facts:

- Codacy Cloud currently reports 0 issues, 88% coverage, 42% complex files, and 41% duplicated files for commit `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- `.codacy/codacy.config.json` currently has top-level `"exclude": []`, so test and benchmark paths are not globally excluded in the committed local Codacy configuration.
- The installed Codacy Cloud CLI skill states that committing `.codacy/codacy.config.json` does not by itself change Codacy Cloud; `codacy tools ... --import` is required.
- Codacy's official Cloud configuration file documentation says Cloud path ignores come from a repository-root `.codacy.yml` or `.codacy.yaml` file with `exclude_paths`, not from `.codacy/codacy.config.json`.
- `SOW-0013` recorded the decision to keep complexity and duplication metrics active and fix real source hotspots.
- `SOW-0014` already implemented substantial production-source organization work, including C protocol splits, Rust/Go lookup codec splits, Rust raw service splits, and C service splits.

Inferences:

- Excluding test and benchmark paths should make Codacy maintainability percentages closer to the production SDK surface the user wants to improve.
- Remaining production-source complexity and duplication should be selected from fresh Codacy data after the exclusion is applied and reanalysis completes.

Unknowns:

- Which production files remain above Codacy's complexity and duplication goals after Codacy reanalyzes with `tests/**` and `bench/**` excluded.
- Whether Codacy Cloud reports file-level metric contributors through the CLI; if not, local Lizard/JSCPD approximations will be used for the next file decision.

### Acceptance Criteria

- `.codacy/codacy.config.json` excludes `tests/**` and `bench/**` at global scope.
- The Codacy configuration is imported to Codacy Cloud.
- Codacy Cloud reanalysis is triggered and checked after import.
- Refreshed metrics are recorded.
- The next production-file maintainability target is selected with evidence.
- Validation passes for changed configuration and SOW state.

## Analysis

Sources checked:

- `.codacy/codacy.config.json`
- `.github/codeql.yml`
- `.agents/sow/done/SOW-0013-20260603-codacy-metrics-investigation.md`
- `.agents/sow/done/SOW-0014-20260603-maintainability-hotspots.md`
- `~/.agents/skills/configure-codacy/SKILL.md`
- `~/.agents/skills/codacy-cloud-cli/SKILL.md`
- Codacy Cloud CLI repository query for `gh/netdata/plugin-ipc`

Current state:

- Codacy Cloud latest analyzed commit is `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- Codacy Cloud metrics are: 0 issues, 107960 LOC, 88% coverage, 42% complex files, and 41% duplicated files.
- Codacy Cloud goals are: max duplicated files 10%, max complex files 10%, file duplication block threshold 1, and file complexity value threshold 20.
- `.codacy/codacy.config.json` has partial Opengrep tool-specific excludes for a few Windows fixture files, but top-level global excludes are empty.
- Codacy Cloud API `listIgnoredFiles` reports `hasCodacyConfigurationFile: false` for commit `5a55246b9b4698317bdf8440c898369034a1f408`, proving Cloud did not treat `.codacy/codacy.config.json` as the repository-level Cloud configuration file.
- CodeQL is intentionally broader and still scans `src`, `tests`, and `bench`; this SOW is about Codacy maintainability scope, not weakening GitHub code scanning.

Risks:

- Global Codacy excludes may remove Codacy issue scanning for test/bench paths, not only complexity/duplication metrics.
- If Codacy Cloud import changes more than the global excludes, the Cloud configuration could drift unexpectedly.
- If Codacy does not expose file-level complexity/duplication contributors through the CLI, the next-file decision needs local approximation plus dashboard confirmation.

## Pre-Implementation Gate

Status: ready.

Problem / root-cause model:

- Codacy maintainability percentages are still calculated with committed global exclusions set to an empty list.
- Test and benchmark code is large and intentionally repetitive; including it in Codacy maintainability metrics makes production-source hygiene harder to read.
- Previous production-source cleanup started and reduced real complexity/duplication, but follow-on target selection should use fresh metrics after Codacy scope is corrected.

Evidence reviewed:

- `.codacy/codacy.config.json:8185` has top-level `"exclude": []`.
- `.codacy/codacy.config.json:7154` has only tool-specific Opengrep excludes for selected fixtures.
- `SOW-0013` records the decision to keep complexity and duplication metrics active and treat real source hotspots as remediation work.
- `SOW-0014` records completed C/Rust/Go production-source organization work and says further complexity or duplication work should start from fresh Codacy/GitHub evidence.
- Codacy Cloud CLI repository query reports current metrics for commit `dbf77a8595b01e3e335db41883d2d5f8b72dfac7`.
- Codacy Cloud CLI skill states `.codacy/codacy.config.json` is local-only until imported with `codacy tools ... --import`.
- Codacy official docs state Cloud repository configuration uses root `.codacy.yml` or `.codacy.yaml`, starting with `---`, and path ignores are defined under `exclude_paths`.
- Codacy Cloud API file queries after commit `5a55246b9b4698317bdf8440c898369034a1f408` still list `tests/**`, `bench/**`, Go `*_test.go`, and Rust `*_tests.rs` files in complexity and duplication data.

Affected contracts and surfaces:

- Local Codacy configuration file `.codacy/codacy.config.json`.
- Cloud Codacy configuration file `.codacy.yml`.
- Codacy Cloud repository configuration after import.
- Codacy Cloud maintainability metrics and issue scope.
- GitHub Codacy local-analysis workflow trigger paths.
- SOW lifecycle records.
- No protocol, API, wire format, runtime behavior, or public SDK behavior should change during the exclusion step.

Existing patterns to reuse:

- Preserve strong CodeQL and GitHub static-analysis coverage; do not alter `.github/codeql.yml` in this SOW unless evidence proves it is necessary.
- Preserve Codacy rules and tools; this scope change should exclude non-production paths rather than disable useful rules.
- Follow SOW-0014's one-file-at-a-time maintainability workflow for production-source remediation.

Risk and blast radius:

- Low runtime risk for the exclusion step because no product code changes are expected.
- Medium quality-reporting risk because global Codacy excludes may also suppress Codacy issues in tests/bench paths.
- Low to medium future implementation risk for the next production-file cleanup, depending on the selected file.

Sensitive data handling plan:

- Do not read `.env`.
- Do not write raw secrets, credentials, tokens, customer data, personal data, private endpoints, or proprietary details into durable artifacts.
- Codacy CLI output will be summarized without recording account tokens or credentials.

Implementation plan:

1. Update `.codacy/codacy.config.json` global excludes to include `tests/**` and `bench/**`.
2. Add root `.codacy.yml` with Cloud-native `exclude_paths` for test and benchmark files.
3. Add `.codacy.yml` and `.codacy.yaml` to the Codacy local-analysis workflow path filters.
4. Validate JSON/YAML syntax and run local Codacy analysis enough to prove the config is accepted.
5. Commit and push the configuration and SOW record.
6. Import the committed Codacy tool configuration into Codacy Cloud and trigger reanalysis.
7. Record refreshed Codacy metrics.
8. Select the next production-file maintainability target from refreshed Codacy data or local production-source approximation if Codacy does not expose contributors.

Validation plan:

- `jq empty .codacy/codacy.config.json`
- YAML parser check for `.codacy.yml`.
- `codacy-analysis analyze --output-format json` with the updated configuration.
- `codacy tools gh netdata plugin-ipc --import -y`
- `codacy repository gh netdata plugin-ipc --reanalyze`
- Re-query Codacy Cloud metrics after reanalysis completes.
- `git diff --check`
- `bash .agents/sow/audit.sh`

Artifact impact plan:

- AGENTS.md: no workflow or guardrail change expected.
- Runtime project skills: no reusable workflow change expected.
- Specs: no protocol/API behavior change expected.
- End-user/operator docs: no public SDK docs change expected.
- End-user/operator skills: no exported/operator skill change expected.
- SOW lifecycle: new current SOW because this is a new Codacy scope correction plus next maintainability pass.

Open-source reference evidence:

- No external open-source reference is needed; this is repository-specific Codacy configuration and local source hygiene work.

Open decisions:

- Resolved: the user approved proceeding with Codacy test/bench exclusion and continued production-source maintainability work.

## Implications And Decisions

1. Codacy scope

- Decision: exclude `tests/**` and `bench/**` from Codacy global analysis scope.
- Benefit: Codacy complexity and duplication percentages should better represent production SDK files.
- Implication: Codacy will likely stop reporting issues from test and benchmark paths too, not only maintainability metrics.
- Mitigation: GitHub CodeQL and static-analysis workflows continue scanning test and benchmark paths separately.

2. Production-source maintainability

- Decision: continue one production file at a time after refreshed metrics.
- Benefit: avoids broad mechanical refactors and keeps review/validation tractable.
- Implication: metric reduction will be incremental, not a single bulk cleanup.
- Mitigation: each target will use file-level evidence and validation appropriate to the touched language/runtime surface.

## Plan

1. Correct Codacy scope and import it to Cloud.
2. Reanalyze Codacy and record refreshed metrics.
3. Build the next production-file candidate list.
4. Start the next low-risk production-file cleanup only after evidence shows the target and the intended refactor.

## Execution Log

### 2026-06-05

- Started SOW after user approval to proceed.
- Updated `.codacy/codacy.config.json` global excludes from `[]` to:
  - `bench/**`
  - `tests/**`
- Verified this is a Codacy-only scope change; `.github/codeql.yml` still scans `src`, `tests`, and `bench`.
- Ran local Codacy analysis with the updated config:
  - total issues: 0.
  - Checkov: 10 files, 0 issues.
  - Opengrep/Semgrep: 264 files, 0 issues.
  - Trivy: 265 files, 0 issues.
  - cppcheck: 45 files, 0 issues.
  - ShellCheck: 3 files, 0 issues.
  - Spectral: 10 files, 0 issues.
- Committed and pushed `5a55246b9b4698317bdf8440c898369034a1f408`.
- Imported `.codacy/codacy.config.json` to Codacy Cloud.
- The import temporarily disabled Cloud Revive because the local file did not contain Revive. Revive was immediately re-enabled in Cloud.
- Added Revive back to `.codacy/codacy.config.json` with the 21 enabled Cloud Revive patterns so future tool imports do not disable it again.
- Verified local Codacy Analysis CLI `0.8.0` is the latest npm-published version available locally, but its Revive adapter reports a non-fatal `findings is not iterable` invocation error even when scanning one Go file. The Revive binary itself runs, and Cloud Revive remains enabled.
- Checked Codacy Cloud after commit `5a55246b9b4698317bdf8440c898369034a1f408`:
  - latest analyzed commit: `5a55246b9b4698317bdf8440c898369034a1f408`.
  - metrics stayed at 0 issues, 88% coverage, 42% complex files, and 41% duplicated files.
  - `listIgnoredFiles` reported `hasCodacyConfigurationFile: false` and only one ignored file: `bench/drivers/go/go.mod`.
- Queried Codacy Cloud file metrics through the generated API client:
  - `bench/drivers/go/main_windows.go`: complexity 321, duplication 1706.
  - `bench/drivers/c/bench_windows.c`: complexity 292, duplication 1357.
  - `tests/fixtures/c/test_uds.c`: complexity 281, duplication 2445.
  - `src/go/pkg/netipc/transport/posix/uds_test.go`: complexity 257, duplication 4091.
  - `src/crates/netipc/src/service/raw_unix_tests.rs`: complexity 329.
- Added Cloud-native `.codacy.yml` with `exclude_paths` for:
  - `bench/**`
  - `benchmarks-*`
  - `tests/**`
  - `**/*_test.go`
  - `**/*_tests.rs`
  - `**/tests.rs`
- Added `.codacy.yml` and `.codacy.yaml` to the Codacy local-analysis workflow path filters.
- Validated the new local state:
  - `.codacy.yml` parses as YAML and contains the intended `exclude_paths`.
  - `.codacy/codacy.config.json` parses as JSON and now includes Revive with 21 patterns.
  - `git diff --check` passed.
  - `bash .agents/sow/audit.sh` passed.
  - `codacy-analysis analyze . --output-format json` exited with Codacy analysis status 0, produced 0 issues, and produced 1 known Revive adapter error:
    - Checkov: success, 11 files, 0 issues.
    - Opengrep/Semgrep: success, 265 files, 0 issues.
    - Trivy: success, 266 files, 0 issues.
    - cppcheck: success, 45 files, 0 issues.
    - ShellCheck: success, 3 files, 0 issues.
    - Spectral: success, 11 files, 0 issues.
  - Revive: partial, 90 files, 0 issues, 1 `findings is not iterable` invocation error.
  - The previous pushed commit `5a55246b9b4698317bdf8440c898369034a1f408` has green CI for CodeQL, Static Analysis, Supply Chain Security, Codacy Local Analysis, Codacy Coverage, and the Codacy GitHub check.
- Committed and pushed `36c06554e1a617e314574ab1cfeae17547285f14`.
- Imported `.codacy/codacy.config.json` again after adding Revive locally:
  - 7 tools reconfigured: Checkov, Opengrep, Trivy, Cppcheck, ShellCheck, Spectral, Revive.
  - 2727 patterns enabled.
  - No tool was disabled.
- Triggered Codacy Cloud reanalysis after the push.
- Checked GitHub CI for `36c06554e1a617e314574ab1cfeae17547285f14`:
  - Supply Chain Security: success.
  - Codacy Local Analysis: success.
  - Static Analysis: success.
  - CodeQL: success.
  - Codacy Coverage: success.
  - Code Quality push check: success.
- Checked Codacy Cloud after `.codacy.yml` reached `main`:
  - `listIgnoredFiles` reports `hasCodacyConfigurationFile: true`.
  - 133 files are now listed as ignored, including `bench/**`, `tests/**`, Go `*_test.go`, Rust `*_tests.rs`, and Rust `tests.rs` files.
  - Repository headline metrics still report the previous full-analysis commit `5a55246b9b4698317bdf8440c898369034a1f408` and still show 42% complex files and 41% duplicated files. This appears to be Codacy metric lag or commit-selection lag after a config-only commit.
- Built a production-source-only file list by filtering the current Codacy file API data through the new ignored-path rules.
- Top production complexity files after filtering ignored test/bench paths:
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`: complexity 227, duplication 422.
  - `src/go/pkg/netipc/transport/windows/pipe.go`: complexity 214, duplication 773.
  - `src/go/pkg/netipc/transport/posix/uds.go`: complexity 182, duplication 745.
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`: complexity 179, duplication 166.
  - `src/go/pkg/netipc/protocol/apps_lookup.go`: complexity 148, duplication 175.
- Selected `src/libnetdata/netipc/src/transport/posix/netipc_uds.c` as the next production maintainability target because it is the highest-complexity production file after excluding test/bench paths.
- Read `src/libnetdata/netipc/src/transport/posix/netipc_uds.c` in full.
- `netipc_uds.c` currently mixes several separate responsibilities in one compilation unit:
  - path validation and stale endpoint recovery.
  - low-level SEQPACKET send/receive.
  - client handshake.
  - server handshake.
  - listener/connect/accept/close lifecycle.
  - client-side in-flight message ID tracking.
  - send chunking.
  - receive chunk reassembly and batch validation.
- Selected low-risk implementation shape:
  - keep the public `netipc_uds.h` API unchanged.
  - keep `netipc_uds.c` as shared low-level/common helpers.
  - add a private `netipc_uds_internal.h`.
  - move lifecycle/stale recovery, handshake, in-flight tracking, send, and receive into separate POSIX UDS implementation files.
  - split send/receive helper logic only where it removes obvious nested branches without changing protocol behavior.
- Implemented the POSIX UDS split:
  - `netipc_uds.c`: shared low-level send/receive and packet-size helpers.
  - `netipc_uds_internal.h`: private POSIX UDS declarations and constants.
  - `netipc_uds_handshake.c`: client/server handshake and negotiation.
  - `netipc_uds_lifecycle.c`: listen/connect/accept/close and stale socket recovery.
  - `netipc_uds_inflight.c`: client-side message ID tracking.
  - `netipc_uds_send.c`: outbound limit checks, single-packet send, and chunked send.
  - `netipc_uds_receive.c`: inbound validation, chunk reassembly, and batch validation.
- Updated `CMakeLists.txt` so the `netipc_uds` static library builds all POSIX UDS implementation files.
- Validation after the split:
  - `cmake --build build`: passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed in 449.75 seconds.
  - `git diff --check`: passed.
  - `bash .agents/sow/audit.sh`: passed.
  - `codacy-analysis analyze . --output-format json`: exit status 0, 0 issues, 1 known Revive adapter error.
    - Checkov: success, 11 files, 0 issues.
    - Opengrep/Semgrep: success, 271 files, 0 issues.
    - Trivy: success, 272 files, 0 issues.
    - cppcheck: success, 51 files, 0 issues.
    - ShellCheck: success, 3 files, 0 issues.
    - Spectral: success, 11 files, 0 issues.
  - Revive: partial, 90 files, 0 issues, 1 `findings is not iterable` invocation error.
- Note: plain `ctest` resolves to a broken user-local Python wrapper at `~/.local/bin/ctest`; system `/usr/bin/ctest` was used for validation.
- Committed and pushed `50c16fa17e4aa4df0b5fb1211b4239f5d71ecc28`.
- Remote validation for `50c16fa17e4aa4df0b5fb1211b4239f5d71ecc28`:
  - GitHub Static Analysis: success.
  - GitHub CodeQL: success.
  - GitHub Codacy Local Analysis: success.
  - GitHub Codacy Coverage: success.
  - GitHub Runtime Safety: success.
  - GitHub Supply Chain Security: success.
  - Codacy push quality check: success.
- Codacy Cloud analyzed `50c16fa17e4aa4df0b5fb1211b4239f5d71ecc28`:
  - issues: 0.
  - LOC: 41001.
  - complex files: 29%.
  - duplicated files: 27%.
- POSIX UDS file-level Codacy result after the split:
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c`: complexity 70, duplication 96.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_handshake.c`: complexity 59, duplication 57.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c`: complexity 50, duplication 0.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_send.c`: complexity 42, duplication 12.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c`: complexity 15, duplication 0.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_inflight.c`: complexity 12, duplication 31.
- Top production complexity files after the POSIX UDS split:
  - `src/go/pkg/netipc/transport/windows/pipe.go`: complexity 214, duplication 773.
  - `src/go/pkg/netipc/transport/posix/uds.go`: complexity 182, duplication 745.
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`: complexity 179, duplication 160.
  - `src/go/pkg/netipc/protocol/apps_lookup.go`: complexity 148, duplication 175.
  - `src/go/pkg/netipc/transport/posix/shm_linux.go`: complexity 147, duplication 39.
- Selected `src/go/pkg/netipc/transport/windows/pipe.go` as the next file to read because it is now the top production complexity and duplication hotspot.
- Read `src/go/pkg/netipc/transport/windows/pipe.go` in full and compared its function map with `src/go/pkg/netipc/transport/posix/uds.go`.
- `pipe.go` mixes these responsibilities in one file:
  - Win32 syscall wrappers and constants.
  - pipe name derivation and service-name validation.
  - role/config/session/listener types.
  - client connection lifecycle.
  - session wait/close behavior.
  - send chunking.
  - receive chunk reassembly and batch validation.
  - listener accept/close behavior.
  - client and server handshakes.
- Selected low-risk implementation shape:
  - keep package name, exported API, types, errors, and behavior unchanged.
  - split the Windows named-pipe package by goal inside the same package.
  - do not introduce a shared cross-platform abstraction yet; POSIX and Windows package boundaries make that higher risk.
  - create separate files for session/client lifecycle, send, receive, listener, and handshake.
- Implemented the Windows Named Pipe Go split:
  - `pipe.go`: Win32 constants/syscall wrappers, pipe-name/service-name helpers, common low-level I/O, and shared utility helpers.
  - `pipe_session.go`: role/config/session types, session wait/close behavior, and client connect lifecycle.
  - `pipe_send.go`: outbound logical-message send and chunked-send flow.
  - `pipe_receive.go`: receive path, inbound limit checks, response tracking, chunk reassembly, and batch payload validation.
  - `pipe_listener.go`: listener lifecycle, accept/close behavior, and pipe instance creation.
  - `pipe_handshake.go`: client/server HELLO negotiation, compatibility detection, rejection, and HELLO_ACK send.
- Validation after the Windows Named Pipe Go split:
  - `gofmt` on the touched Windows pipe files: passed.
  - `git diff --check`: passed.
  - `cd src/go && go test ./...`: passed.
  - `cd src/go && GOOS=windows GOARCH=amd64 go test -c -o /tmp/netipc-transport-windows.test.exe ./pkg/netipc/transport/windows`: passed.
  - `win11`, isolated validation copy, `cd src/go && MSYSTEM=MSYS go test ./pkg/netipc/transport/windows`: passed.
  - `bash .agents/sow/audit.sh`: passed.
  - `codacy-analysis analyze . --output-format json`: exit status 0, 0 issues, 1 known Revive adapter invocation error:
    - Revive error: `Failed to run revive: findings is not iterable`.
- Committed and pushed `ce9c5edeee80732d60daf4e6654dd253a49711a5`.
- Remote validation for `ce9c5edeee80732d60daf4e6654dd253a49711a5`:
  - GitHub CodeQL C/C++ POSIX, C/C++ Windows, Go POSIX, Go Windows, and Rust: success.
  - GitHub Static Analysis C, Go, Rust, workflow/shell, and bench/fixture Go checks: success.
  - GitHub Runtime Safety ASAN/UBSAN, TSAN, Go Race Detector, and Windows MSYS2 Runtime: success.
  - GitHub Supply Chain Security OSV-Scanner, Semgrep Secrets, and OpenSSF Scorecard: success.
  - GitHub Codacy Local Analysis: success.
  - GitHub C, Rust, and Go Coverage: success.
  - Valgrind: skipped.
- Codacy Cloud analyzed `ce9c5edeee80732d60daf4e6654dd253a49711a5`:
  - issues: 0.
  - LOC: 41123.
  - coverage: 88%.
  - complex files: 30%.
  - duplicated files: 28%.
- Windows Named Pipe file-level Codacy result after the split:
  - `src/go/pkg/netipc/transport/windows/pipe.go`: complexity 58, duplication 98.
  - `src/go/pkg/netipc/transport/windows/pipe_handshake.go`: complexity 48, duplication 213.
  - `src/go/pkg/netipc/transport/windows/pipe_listener.go`: complexity 31, duplication 14.
  - `src/go/pkg/netipc/transport/windows/pipe_receive.go`: complexity 46, duplication 32.
  - `src/go/pkg/netipc/transport/windows/pipe_send.go`: complexity 19, duplication 55.
  - `src/go/pkg/netipc/transport/windows/pipe_session.go`: complexity 26, duplication 79.
- Top production complexity files after the Windows Named Pipe split:
  - `src/go/pkg/netipc/transport/posix/uds.go`: complexity 182, duplication 532.
  - `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`: complexity 179, duplication 160.
  - `src/go/pkg/netipc/protocol/apps_lookup.go`: complexity 148, duplication 175.
  - `src/go/pkg/netipc/transport/posix/shm_linux.go`: complexity 147, duplication 39.
  - `src/crates/netipc/src/transport/posix.rs`: complexity 143, duplication 0.
- Selected `src/go/pkg/netipc/transport/posix/uds.go` as the next file to read because it is now the top production complexity and duplication hotspot.
- Read `src/go/pkg/netipc/transport/posix/uds.go` in full.
- `uds.go` mixes these responsibilities in one file:
  - POSIX constants, errors, utility helpers, low-level SEQPACKET I/O, and socket path validation.
  - role/config/session types.
  - client connection lifecycle.
  - session close behavior.
  - send chunking.
  - receive chunk reassembly, inbound limit checks, response tracking, and batch validation.
  - listener lifecycle, accept, close, bind/listen, and socket unlink.
  - stale endpoint recovery.
  - client and server handshakes.
- Selected low-risk implementation shape:
  - keep package name, exported API, types, errors, and behavior unchanged.
  - split the POSIX UDS package by goal inside the same package.
  - do not introduce a shared POSIX/Windows abstraction yet; the two transports have different syscall and stale-endpoint behavior.
  - create separate files for session/client lifecycle, send, receive, listener, stale endpoint recovery, and handshake.
- Implemented the POSIX UDS Go split:
  - `uds.go`: constants, errors, path/service helpers, packet-size helper, low-level SEQPACKET I/O, and shared utility helpers.
  - `uds_session.go`: role/config/session types, session close behavior, and client connect lifecycle.
  - `uds_send.go`: outbound logical-message send and chunked-send flow.
  - `uds_receive.go`: receive path, inbound limit checks, response tracking, chunk reassembly, and batch payload validation.
  - `uds_listener.go`: listener lifecycle, accept/close behavior, bind/listen, and socket unlink.
  - `uds_stale.go`: stale endpoint detection and safe stale socket unlink policy.
  - `uds_handshake.go`: client/server HELLO negotiation, compatibility detection, rejection, and HELLO_ACK send.
- Validation after the POSIX UDS Go split:
  - `gofmt` on the touched POSIX UDS files: passed.
  - `git diff --check`: passed.
  - `cd src/go && go test ./pkg/netipc/transport/posix`: passed.
  - `cd src/go && go test ./...`: passed.
  - `bash .agents/sow/audit.sh`: passed.
  - `codacy-analysis analyze . --output-format json`: exit status 0, 0 issues, 1 known Revive adapter invocation error:
    - Revive error: `Failed to run revive: findings is not iterable`.

### 2026-06-06 - GitHub AI Findings Triage

- Opened `https://github.com/netdata/plugin-ipc/security/quality/ai-findings` with the authenticated Playwright browser.
- GitHub displayed 15 AI findings across 5 files, not 5 individual findings:
  - `src/crates/netipc/src/service/raw/server_windows.rs`: 2 findings.
  - `src/go/pkg/netipc/protocol/lookup_common.go`: 5 findings.
  - `src/go/pkg/netipc/transport/posix/uds_stale.go`: 3 findings.
  - `src/go/pkg/netipc/transport/windows/pipe_session.go`: 2 findings.
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds_send.c`: 3 findings.
- Local verification found stale or false-positive protocol findings:
  - `src/go/pkg/netipc/protocol/lookup_common.go:19` already has the `LookupLabelView` doc comment.
  - `src/go/pkg/netipc/protocol/lookup_common.go:452` already uses `count := int(itemCount)` after the prior overflow check.
  - `src/go/pkg/netipc/protocol/frame.go:75` defines package-level `ne = binary.NativeEndian`.
  - `src/go/pkg/netipc/protocol/frame.go:100` defines package-level `Align8`.
- Accepted real low-risk findings:
  - Simplify redundant Rust Windows SHM finalization match.
  - Document the Rust Windows SHM cleanup order.
  - Add a doc comment for unexported Go `maxIntValue` to satisfy the scanner without behavior change.
  - Cache effective UID before POSIX stale-unlink directory stat.
  - Use a bounded UDS stale-candidate dial timeout and retry transient `ECONNREFUSED` before unlinking.
  - Add idiomatic Windows Go `Role()` accessors while preserving deprecated `GetRole()` wrappers.
  - Replace the Windows named-pipe spin-loop magic number with a named constant.
  - Split C UDS send compound conditions and local min expressions without changing send behavior.
- Same-pattern search found `src/go/pkg/netipc/transport/windows/shm.go:163` also exposed only `GetRole()` while POSIX SHM exposes `Role()`; added the same additive `Role()` API there and kept `GetRole()` for compatibility.
- Validation for this AI-findings pass:
  - `gofmt` on touched Go files: passed.
  - `cargo fmt --manifest-path src/crates/netipc/Cargo.toml`: passed.
  - `git diff --check`: passed.
  - `cd src/go && go test ./pkg/netipc/protocol ./pkg/netipc/transport/posix`: passed.
  - `cd src/go && go test ./...`: passed.
  - `cd src/go && GOOS=windows GOARCH=amd64 go test -c -o /tmp/netipc-transport-windows.test.exe ./pkg/netipc/transport/windows`: passed.
  - `ssh win11 'cd /tmp/plugin-ipc-ai-findings/src/go && MSYSTEM=MSYS go test ./pkg/netipc/transport/windows'`: passed.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`: passed, 332 Rust unit tests.
  - `cmake --build build`: passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure`: passed, 46/46 tests.
  - `bash .agents/sow/audit.sh`: passed.
  - `codacy-analysis analyze --files ... --output-format json`: exit status 0, 0 issues, 1 known Revive adapter error:
    - Revive error: `Failed to run revive: findings is not iterable`.

### 2026-06-07 - C UDS Send AI Follow-up

- GitHub AI findings on `src/libnetdata/netipc/src/transport/posix/netipc_uds_send.c` reported:
  - ambiguous `min_size` helper name.
  - casts from `size_t` to `uint32_t` for payload length, chunk payload length, and total message length whose invariants were not explicit.
  - ceil division using `(remaining + chunk_payload_budget - 1)`, which could overflow in the abstract.
- Local verification found one issue stronger than the GitHub wording:
  - `src/libnetdata/netipc/src/transport/posix/netipc_uds.c:nipc_uds_header_payload_len()` only checked `SIZE_MAX` overflow on 32-bit builds and did not reject framed total message lengths above `UINT32_MAX` on 64-bit builds.
  - `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c:header_payload_len()` had the same helper shape, so the same contract issue existed in the Windows C transport.
- Implemented follow-up:
  - renamed POSIX helper `min_size` to `min_of_size_t`.
  - added assertions at the narrowing cast sites.
  - changed POSIX and Windows C total-message helpers to reject `NIPC_HEADER_LEN + payload_len > UINT32_MAX`.
  - changed POSIX and Windows C chunk-count calculation to `1 + ((remaining - 1) / chunk_payload_budget)` to avoid forming a potentially overflowing addition.
  - changed chunk-budget checks from equality to `session->packet_size <= NIPC_HEADER_LEN` to avoid unsigned underflow if a malformed session reaches send.
  - added POSIX C regression coverage for underflow chunk budget and oversized framed total-message length.
- Benchmark runner follow-up:
  - Full Windows benchmark startup failed before running benchmarks because `tests/run-windows-bench.sh` passed `/mingw64/bin/gcc` and `/mingw64/bin/g++` to CMake.
  - CMake 4.3.0 in the current MSYS environment translated those to `C:/msys64/mingw64/bin/gcc` and rejected them as non-existing full paths.
  - Updated the runner to pass Windows-style `.exe` paths from `cygpath -m`, for example `C:/msys64/mingw64/bin/gcc.exe`.
- POSIX benchmark failure root cause:
  - Full POSIX benchmark initially failed deterministically on `uds-batch-ping-pong rust->c @ max`.
  - Manual focused repro failed before processing item zero.
  - Root cause: the Rust POSIX batch benchmark client/server configs raised batch item counts and response payload size, but did not raise request payload size. Large 2..1000 item batch requests could exceed the negotiated request payload limit before reaching the C server.
  - Fixed `bench/drivers/rust/src/main.rs` so POSIX Rust batch client and server configs set all four batch/request/response limits consistently.
- Go benchmark determinism follow-up:
  - Local `go doc math/rand.Seed` for the installed Go toolchain says package-level random state is seeded randomly at startup when `Seed` is not called, and since Go 1.24 package-level `Seed` is a no-op.
  - C and Rust benchmark batch-size streams were deterministic xorshift32 with seed `12345`; Go was not.
  - Replaced Go package-global `math/rand` batch sizing in POSIX and Windows benchmark drivers with a local xorshift32 sequence matching the C/Rust seed.
- Windows benchmark stability-policy follow-up:
  - Batch benchmark rows report item throughput, while `target_rps` controls batch request rate.
  - The Windows runner previously compared `target_rps` directly to prior max item throughput when deciding whether a fixed-rate row was oversubscribed.
  - Added fixed-rate target throughput calculation for batch rows using the 2..1000 batch-size midpoint, so oversubscription classification compares item-throughput to item-throughput.
  - Moved oversubscription handling before stable-ratio failure handling so an intentionally overloaded fixed-rate row is classified consistently.
- Windows benchmark timeout evidence:
  - First fresh full Windows run reached `shm-ping-pong c->go @ max` and recorded one hard failure: the C client was killed by the runner outer `timeout` with exit 124 while the Go server had printed `READY`.
  - The C client produced no stderr because the outer timeout was shorter than the benchmark client's own 30s SHM receive timeout.
  - Fixed `tests/run-windows-bench.sh` by adding `NIPC_BENCH_CLIENT_TIMEOUT_GRACE_SEC`, default `35`, and using `duration + CLIENT_TIMEOUT_GRACE_SEC` for client process timeouts.
  - Stopped the invalid full Windows run and cleaned up only the exact remote benchmark PIDs tied to `/tmp/plugin-ipc-bench-full-results-fixed4` and `/tmp/netipc-bench-88189`.
  - Focused Win11 diagnostic for `shm-ping-pong c->go @ max` with diagnostics enabled passed: throughput `2714891/s`, p50 `3.700us`, p95 `8.900us`, p99 `16.200us`, stable ratio `1.017852`, no warnings.
  - A fresh full Windows benchmark rerun with the improved timeout (`fixed5`) was stopped after the first row failed the stability policy, not correctness: `np-ping-pong c->c @ max` samples were `75452`, `74311`, `52814`, `66071`, and `51296`, with no client/server errors.
  - Focused Win11 diagnostic for `np-ping-pong c->c @ max` with the same default durations passed: throughput `77680/s`, p50 `13.200us`, p95 `41.800us`, p99 `82.600us`, stable ratio `1.093959`, no warnings.
  - The final full Win11 benchmark rerun with diagnostics enabled passed with exit status 0:
    - output CSV: `/tmp/plugin-ipc-bench-full-results-final2/benchmarks-windows.csv`
    - row count: 202 lines.
    - hard failure scan: no `client failed`, `server failed`, `did not stop`, `error`, `One or more Windows benchmark scenarios failed`, or `Diagnostic rerun failed` matches.
    - one stability-only row was recovered by a successful diagnostic rerun: `np-batch-ping-pong go->go @ 10000/s`, first attempt stable ratio `1.849425`, diagnostic stable ratio `1.000165`, recovered median throughput `5040434/s`.
    - remaining warnings were raw repeated-throughput outliers or oversubscribed fixed-rate rows kept by the benchmark policy, with stable trimmed cores or intentional overload evidence.
- Vendor script follow-up:
  - `vendor-to-netdata.sh` still had a hard-coded C file list from before the C source splits.
  - Updated it to rsync the full vendored C include tree and C source tree, preserving Netdata-specific root wrapper files outside those synced subtrees.
  - This is required before updating the NetIPC vendored copy in `~/src/netdata-ktsaou.git`; otherwise split C files would be silently missed.
- Validation completed so far for this follow-up:
  - `bash -n tests/run-windows-bench.sh vendor-to-netdata.sh`: passed.
  - `git diff --check`: passed.
  - `cmake --build build`: passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed in 449.56 seconds.
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`: 332 tests passed; benchmark and interop test targets built and had 0 tests.
  - `cd src/go && go test ./...`: passed.
  - `cd bench/drivers/go && go test ./...`: passed.
  - `cd bench/drivers/go && GOOS=windows GOARCH=amd64 go test -c -o /tmp/plugin-ipc-go-bench-windows.test.exe .`: passed.
  - `bash -n tests/run-windows-bench.sh tests/test_windows_bench_stability_policy.sh vendor-to-netdata.sh`: passed.
  - Local targeted Codacy Analysis CLI scan with JSON output on the changed files: 0 issues, 1 known Revive adapter invocation error (`findings is not iterable`); latest output `/tmp/plugin-ipc-codacy-final6.json`.
  - Full POSIX benchmark rerun: 298 CSV lines, no warnings or failures, output at `/tmp/plugin-ipc-bench-full-posix-final/benchmarks-posix.csv`.
  - Full Win11 benchmark rerun: 202 CSV lines, no hard failures, output at `/tmp/plugin-ipc-bench-full-results-final2/benchmarks-windows.csv`.

### 2026-06-07 - Netdata Re-vendor Plan

The next repository step is to update the vendored NetIPC copy in `~/src/netdata-ktsaou.git` after this SDK commit is pushed.

Plan:

1. Read `~/src/netdata-ktsaou.git/AGENTS.md` and any active Netdata SOW instructions before editing that repository.
2. Create or switch to a dedicated Netdata branch for the vendor update.
3. Run `./vendor-to-netdata.sh ~/src/netdata-ktsaou.git` from this repository.
4. Run `./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git --unified` and inspect the result so only expected vendored NetIPC files changed.
5. If the script reports required Go import rewrites, run the exact rewrite command in the Netdata repository and re-check the diff.
6. Update Netdata build integration if the split C source files require changed source lists.
7. Validate Netdata build/tests for the vendored NetIPC surface, including Windows compile/unit validation if the Netdata workflow exposes it locally.
8. Commit only explicit Netdata vendor/update files, push the branch, and open a PR against Netdata.

### 2026-06-07 - PR 22649 Review And SonarCloud Follow-up

Netdata PR #22649 review context was checked before editing:

- GitHub review threads had two unresolved, non-outdated, actionable findings:
  - `src/crates/netipc/src/service/raw/server.rs:95` equivalent vendored Windows raw server shutdown risk: only the initially stored listener handle was available to `stop()`, while later accepted pipe instances could keep blocking.
  - `src/go/pkg/netipc/service/raw/cgroups_lookup.go:123` service dispatch had a dead `builder.Finish() == 0` check after builder error and item-count checks.
- Same-pattern service dispatch review was checked in Go raw apps lookup; the same dead service-level guard existed there.
- Protocol-level Go lookup dispatch was also checked. The guard looked similar, but `cd src/go && go test -count=1 ./...` proved it is required: `TestLookupDispatchCoverage` intentionally corrupts builder state and expects `ErrOverflow`. The protocol-level guards were restored.
- SonarCloud PR query reported six open issues:
  - Go parameter-count findings in `src/go/pkg/netipc/protocol/apps_lookup.go`.
  - Go unnecessary recovered variable findings in `src/go/pkg/netipc/service/raw/server_unix.go` and `src/go/pkg/netipc/service/raw/server_windows.go`.
  - C `openat()` descriptor finding in `src/libnetdata/netipc/src/transport/posix/netipc_shm.c`.
  - C memcpy bounds finding in `src/libnetdata/netipc/src/transport/posix/netipc_uds_receive.c`.
- GitHub AI quality findings also reported:
  - ignored `clock_gettime` syscall result in `bench/drivers/go/main.go`.
  - Go integer `range` compatibility concerns in Go service/raw and stale-dial code.
  - missing rationale around the Go UDS 32-bit total-length limit.
  - maintainability issues in `vendor-to-netdata.sh`.

Implemented SDK follow-up:

- Aligned all plugin-ipc Go modules to Netdata's Go version, `go 1.26.0`.
- Ran Go's `go fix` modernization on the affected Go module surfaces, including benchmark driver loop and slice cleanups.
- Changed Go benchmark CPU-time helper to check the `clock_gettime` syscall error path and return zero on failure.
- Added the missing Go UDS comment explaining the protocol's 32-bit framed total-length contract.
- Hardened `vendor-to-netdata.sh`:
  - validates source `go.mod` presence and parses module paths from the `module` directive.
  - avoids fragile status-line spacing.
  - counts only relevant C, Rust, and Go source files.
  - prints an import-rewrite review command after the suggested `sed` command.
- Updated Rust Windows raw server to refresh the stored listener handle around each accept and clear it before joining session threads, so `stop()` can target the currently blocking listener handle.
- Removed dead service-level Go lookup dispatch `builder.Finish() == 0` checks in raw apps lookup and raw cgroups lookup.
- Reduced SonarCloud Go parameter count in apps lookup semantic validators by passing a private `appsLookupSemantics` struct.
- Removed unnecessary recovered-value variables from Go raw server panic-recovery paths.
- Avoided `openat()` with an invalid directory descriptor in POSIX SHM stale cleanup.
- Added explicit chunk and first-packet bounds checks in POSIX UDS receive before copying chunk payload data into the assembled receive buffer.

Validation completed for this follow-up:

- `cd bench/drivers/go && go fix ./... && go test ./...`: passed.
- `git diff --check`: passed.
- `cd src/go && go test -count=1 ./...`: passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml`: 332 tests passed.
- `cmake --build build`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed.
- Win11 temp-copy Rust validation: `cargo test --manifest-path src/crates/netipc/Cargo.toml service::raw -- --test-threads=1`: 22 Windows raw-service tests passed.
- Win11 temp-copy Go validation: `cd src/go && CGO_ENABLED=0 go test -count=1 ./pkg/netipc/service/raw ./pkg/netipc/transport/windows`: passed.

### 2026-06-07 - PR 22649 SonarCloud Hotspot Follow-up

Pre-push Netdata PR #22649 sync found nine SonarCloud security hotspots on the old PR head:

- `src/libnetdata/netipc/src/transport/posix/netipc_uds_lifecycle.c`: four `strncpy` hotspot reports around socket path copies.
- `src/libnetdata/netipc/src/service/netipc_service_common.c`: one `strlen` hotspot in service context field copying.
- `src/libnetdata/netipc/src/service/netipc_service_posix_server.c`: two `strlen` hotspots in server config field copying.
- `src/libnetdata/netipc/src/service/netipc_service_win_server.c`: two `strlen` hotspots in Windows server config field copying.

Implemented SDK follow-up:

- Replaced POSIX UDS `strncpy` socket/path copies with a checked NUL-terminated bounded copy helper and a `sockaddr_un` fill helper.
- Replaced repeated POSIX and Windows service server `strlen`/`memcpy` config-field copies with `nipc_service_common_copy_cstr_field()`.
- Changed the common service copy helper to avoid open-ended `strlen` while preserving previous truncating field-copy behavior.
- Same-pattern search found two additional `strncpy` copies in POSIX SHM context path storage; those were changed to checked bounded copies in the same increment.

Validation completed for this hotspot follow-up:

- `git diff --check`: passed.
- `rg "strncpy\\(|strlen\\(run_dir\\)|strlen\\(service_name\\)|size_t len = strlen" src/libnetdata/netipc/src src/libnetdata/netipc/include`: only the unrelated Windows named-pipe hash input remains.
- `cmake --build build`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed.
- Win11 temp-copy focused C validation: MSYS CMake build of `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits` passed; CTest for those three tests passed.

### 2026-06-07 - PR 22649 Review And Duplication Follow-up

Netdata PR #22649 was rechecked after the previous SDK re-vendor commit.

Facts from GitHub and SonarCloud:

- GitHub review threads: two total, two resolved, zero open.
- SonarCloud issue API: zero unresolved issues on the PR.
- SonarCloud hotspot API: zero unresolved hotspots on the PR.
- SonarCloud quality gate still failed because new-code duplication is 10.5%, above the configured 3% threshold.
- Cubic added a new top-level review on the current PR head with six findings and no new unresolved inline review threads.

Validated and fixed Cubic findings in SDK source:

- Rust POSIX stale endpoint recovery leaked the probe socket when `ECONNREFUSED` was treated conservatively; the early return was removed so the common close path always runs.
- Go Windows named-pipe `Accept()` read listener config without the listener mutex; it now takes a locked config snapshot before calling `AcceptWithConfig()`.
- Go Windows named-pipe `Close()` could leave the accepting pipe handle open if the wake-up `CreateFile()` failed; it now marks the stored handle invalid and closes the accepting handle as fallback.
- Rust raw string-reverse client now uses checked size arithmetic before allocating the request scratch buffer.
- Go cgroups cache bucket sizing now rejects item counts that would overflow `nextPowerOf2U32()` or the platform `int` length before allocation.
- Go POSIX UDS and Windows named-pipe send helpers now calculate total framed length in `uint64` and check both the protocol `u32` limit and platform `int` limit before returning an `int`.

Same-pattern fixes and tests:

- The Go total-length helper pattern was fixed in both POSIX UDS and Windows named-pipe transports.
- Added Go unit tests for cgroups cache bucket-count overflow edges.
- Added Go unit tests for POSIX and Windows send total-length bounds.

Validation completed for this review follow-up:

- `git diff --check`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/service/raw ./pkg/netipc/transport/posix`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/...`: passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml string_reverse -- --test-threads=1`: 21 POSIX string-reverse-related tests passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml -- --test-threads=1`: 332 tests passed.
- Win11 temp-copy Go validation: `cd src/go && go test -count=1 ./pkg/netipc/transport/windows ./pkg/netipc/service/raw`: passed.
- Win11 temp-copy Rust validation: `cargo test --manifest-path src/crates/netipc/Cargo.toml string_reverse -- --test-threads=1`: 14 Windows string-reverse-related tests passed.

SonarCloud duplication composition for Netdata PR #22649:

- `src/go/pkg/netipc/transport/windows/pipe_receive.go`: 189 duplicated lines.
- `src/go/pkg/netipc/transport/posix/uds_receive.go`: 189 duplicated lines.
- `src/go/pkg/netipc/service/raw/server_unix.go`: 160 duplicated lines.
- `src/go/pkg/netipc/service/raw/server_windows.go`: 159 duplicated lines.
- `src/go/pkg/netipc/transport/windows/pipe_handshake.go`: 141 duplicated lines.
- `src/go/pkg/netipc/transport/posix/uds_handshake.go`: 141 duplicated lines.
- `src/libnetdata/netipc/src/service/netipc_service_posix_client_call.c`: 74 duplicated lines.
- `src/libnetdata/netipc/src/service/netipc_service_win_client_call.c`: 74 duplicated lines.
- `src/libnetdata/netipc/src/service/netipc_service_posix_server.c`: 61 duplicated lines.
- `src/libnetdata/netipc/src/service/netipc_service_win_server.c`: 61 duplicated lines.

The duplication is a real POSIX/Windows paired-implementation signal, not unresolved Sonar line findings. It will be handled after this review-finding fix is committed and re-vendored.

### 2026-06-07 - SonarCloud Duplication Reduction

Live Netdata PR #22649 recheck before this increment:

- GitHub review threads: eight total, eight resolved, zero open.
- SonarCloud issue API: zero unresolved issues on the PR.
- SonarCloud hotspot API: zero unresolved hotspots on the PR.
- SonarCloud quality gate status: failed only because new-code duplication is 10.5%, above the configured 3% threshold.
- Latest SonarCloud duplication component tree still reports 1,826 duplicated new lines across 29 files.

Selected low-risk production-source refactors:

- Extract Go POSIX/Windows transport receive framing from `uds_receive.go` and `pipe_receive.go` into a shared internal framing receiver. This targets the top two SonarCloud contributors, 189 duplicated lines each.
- Extract the Go raw-service per-session request/dispatch/response loop from `server_unix.go` and `server_windows.go` into shared raw-service helpers while leaving OS-specific accept, readiness, receive, send, and SHM cleanup code local. This targets the next two contributors, 160 and 159 duplicated lines.
- Extract Go POSIX/Windows transport send chunking and HELLO/HELLO_ACK protocol helpers into shared internal framing helpers while keeping socket/pipe I/O and session construction platform-local. This targets the next Go transport contributors: handshake and send pairs.
- Extract C client refresh, raw-call envelope validation, and retry/reconnect policy into common service helpers with platform callbacks. This targets the POSIX/Windows C client-call and client lifecycle duplication without changing transport send/receive functions.

Risk controls:

- Do not merge POSIX and Windows accept loops; they differ in readiness, listener shutdown, and SHM setup behavior.
- Keep transport-specific receive/send as callbacks so OS-specific error and timeout semantics remain local.
- Validate POSIX Go packages locally and Windows Go packages on Win11 after the refactor.
- Validate C common helper changes on POSIX and Win11 because both platform service client paths now call common retry/raw-call helpers.

Validation completed for this duplication-reduction increment:

- `git diff --check`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/transport/internal/framing ./pkg/netipc/transport/posix ./pkg/netipc/service/raw`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/...`: passed.
- `cmake --build build`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed.
- Win11 temp-copy Go validation: `cd src/go && go test -count=1 ./pkg/netipc/transport/windows ./pkg/netipc/service/raw`: passed.
- Win11 temp-copy C validation: MSYS CMake build of `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits` passed; CTest for those three tests passed.
- `bash .agents/sow/audit.sh`: passed.

### 2026-06-07 - Netdata PR #22649 post-push scanner pass

- Rechecked Netdata PR #22649 after vendoring SDK commit `1c6093540d0c` into Netdata commit `50a9c100ec0c`.
- GitHub review state:
  - review threads: 8 total, 0 open.
  - review decision remains `REVIEW_REQUIRED`.
- SonarCloud state:
  - open issues: 0.
  - open hotspots: 0.
  - quality gate still reports duplication failure, but `project_pull_requests/list` shows SonarCloud's latest PR analysis is for old Netdata commit `86c0aa015f73`, not current PR head `50a9c100ec0c`.
  - The stale SonarCloud duplication component list still names files that were changed by the latest duplication cleanup, so it must not be treated as current evidence until SonarCloud reanalyzes the latest head.
- Codacy state on current Netdata PR head:
  - GitHub check `Codacy Static Code Analysis`: `ACTION_REQUIRED`.
  - Repo-local Codacy API fetch returned 155 live `Added` issues and 25 already `Fixed` issues.
  - Live `Added` issues are:
    - 84 `cppcheck_missingIncludeSystem` reports against standard/POSIX/Windows system headers such as `<stdint.h>`, `<string.h>`, `<unistd.h>`, and `<windows.h>`.
    - 66 `cppcheck_unusedStructMember` reports against C wire-layout/internal operation structs.
    - 5 `cppcheck_knownConditionTrueFalse` reports against split C server init paths.
  - Already `Fixed` Codacy reports include all `flawfinder_strncpy`, `flawfinder_usleep`, and `cppcheck_unreadVariable` findings from older PR commits.
- Verified the 5 live `cppcheck_knownConditionTrueFalse` reports are real but low risk: both split server init functions reject `NULL` `config` before the flagged assignments.
- Fixed the redundant split-server `config` guards in:
  - `src/libnetdata/netipc/src/service/netipc_service_posix_server.c`
  - `src/libnetdata/netipc/src/service/netipc_service_win_server.c`
- Validation for the split-server guard cleanup:
  - `git diff --check`: passed.
  - POSIX focused build of `test_service`, `test_service_extra`, and `test_service_payload_limits`: passed.
  - `/usr/bin/ctest --test-dir build --output-on-failure -R 'test_service|test_service_extra|test_service_payload_limits'`: 9/9 matching service tests passed.
  - Win11 MSYS focused build of `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits`: passed.
  - Win11 MSYS CTest for the same three targets: 3/3 passed.
  - `bash .agents/sow/audit.sh`: passed.
- Scanner-policy decision still needed before the Netdata PR can become Codacy-clean:
  - `cppcheck_missingIncludeSystem` appears non-actionable in Codacy because it cannot see ordinary system headers in the analyzer environment.
  - `cppcheck_unusedStructMember` is noisy for NetIPC wire-layout structs and internal callback tables, but disabling it affects the whole Netdata repository unless a narrower suppression path is chosen.

### 2026-06-07 - PR 22649 latest review-thread pass

Netdata PR #22649 was rechecked after vendoring SDK commit `04f8b09` into Netdata commit `d60e6bc19a`.

Facts from GitHub, SonarCloud, and CI:

- GitHub review threads: ten total, eight resolved, two open.
- SonarCloud PR issue API: zero unresolved issues.
- SonarCloud PR hotspot API: zero unresolved hotspots.
- Netdata CI: zero failed checks at the snapshot; 103 checks still running and one CLA check pending.

Open GitHub review threads:

- `src/go/pkg/netipc/transport/internal/framing/handshake.go`: cubic claimed the negotiated request payload should follow `min(max(client, server), 256MB)`.
  - Current public spec evidence says otherwise: `docs/level1-wire-envelope.md` defines proposals above `1 MiB` as `LIMIT_EXCEEDED` and says accepted request payload limits are echoed unchanged.
  - Same-language evidence: C POSIX, C Windows, Rust POSIX, Rust Windows, and Go all implement the same `1 MiB` reject plus echo behavior.
  - Decision for this increment: do not change the handshake contract as a PR-comment side effect. Add source clarification if needed and report the thread as stale/contract-conflicting unless the user explicitly chooses to revise the protocol across docs, C, Rust, Go, and tests.
- `src/go/pkg/netipc/transport/internal/framing/receive.go`: cubic claimed packets whose received length exceeds the declared frame length should be rejected instead of accepted.
  - Local verification found the finding is real and same-pattern across all packet transports:
    - Go shared framing accepted `n >= totalMsg`.
    - C POSIX UDS accepted `(size_t)n >= total_msg`.
    - C Windows Named Pipe accepted `n >= total_msg`.
    - Rust POSIX UDS accepted `n >= total_msg`.
    - Rust Windows Named Pipe accepted `n >= total_msg`.
  - Decision for this increment: fix the whole class so exact-length packets are accepted, shorter packets enter the chunked path, and longer packets fail as framing/protocol errors.

Implemented SDK follow-up:

- Added a Go shared-framing comment clarifying the current Level 1 handshake contract: request payload limits are client-proposed, proposals above the wire cap are rejected, and accepted values are echoed unchanged.
- Changed non-chunked receive completion in Go shared framing, C POSIX UDS, C Windows Named Pipe, Rust POSIX UDS, and Rust Windows Named Pipe from `received_len >= declared_total` to:
  - reject `received_len > declared_total` as a protocol/framing error.
  - accept only `received_len == declared_total` as a complete non-chunked message.
  - keep `received_len < declared_total` as the chunked-message path.
- Added regression tests for packets that declare one payload byte but contain two bytes in the same packet:
  - Go shared framing unit test.
  - POSIX C UDS integration test.
  - Windows C Named Pipe integration test.
  - POSIX Rust UDS socketpair test.
  - Windows Rust Named Pipe test.

Validation completed for this increment:

- `cd src/go && go test -count=1 ./pkg/netipc/transport/internal/framing`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/transport/internal/framing ./pkg/netipc/transport/posix`: passed.
- `cd src/go && go test -count=1 ./pkg/netipc/...`: passed.
- `cd src/go && GOOS=windows GOARCH=amd64 go test -c -o /tmp/netipc-transport-windows.test.exe ./pkg/netipc/transport/windows`: passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml test_receive_packet_longer_than_declared_payload -- --test-threads=1`: passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml transport::posix -- --test-threads=1`: 60 tests passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml -- --test-threads=1`: 333 tests passed.
- `cmake --build build --target test_uds`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure -R '^test_uds$'`: passed.
- `cmake --build build`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 tests passed.
- Win11 MSYS C validation: `test_named_pipe` built and passed.
- Win11 Go validation: `cd src/go && MSYSTEM=MSYS CGO_ENABLED=0 go test -count=1 ./pkg/netipc/transport/internal/framing ./pkg/netipc/transport/windows`: passed.
- Win11 Rust validation: `MSYSTEM=MSYS cargo test --manifest-path src/crates/netipc/Cargo.toml transport::windows -- --test-threads=1`: 26 Windows transport tests passed.
- `git diff --check`: passed.

## Validation

Acceptance criteria evidence:

- Codacy global scope correction was implemented earlier in this SOW and pushed.
- The C UDS send AI finding was addressed in `src/libnetdata/netipc/src/transport/posix/netipc_uds_send.c`:
  - helper renamed to `min_of_size_t`.
  - narrowing casts now have explicit assertions and safety-contract comments.
  - send chunk count no longer forms `(remaining + chunk_payload_budget - 1)`.
- Same-pattern C transport issue was addressed in `src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c`.
- Vendor script now copies the full vendored C include/source subtrees so split C files are not missed.
- Netdata PR #22649 review and SonarCloud findings were verified against SDK source and addressed in the SDK before re-vendoring.
- Plugin-ipc Go modules now match Netdata's Go version, `go 1.26.0`.
- Netdata PR #22649 SonarCloud security hotspots for C string/path copying were verified and addressed in the SDK before re-vendoring.

Tests or equivalent validation:

- `cmake --build build`: passed.
- `/usr/bin/ctest --test-dir build --output-on-failure`: 46/46 passed.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml`: 332 passed.
- `cd src/go && go test ./...`: passed.
- `cd bench/drivers/go && go test ./...`: passed.
- `cd src/go && go test -count=1 ./...`: passed after restoring the required protocol-level lookup dispatch guard.
- Win11 temp-copy Rust raw-service validation: 22 tests passed.
- Win11 temp-copy Go service/raw and transport/windows validation: passed.
- Win11 temp-copy C service validation: `test_win_service`, `test_win_service_extra`, and `test_win_service_payload_limits` built and passed under MSYS.
- `git diff --check`: passed.
- `bash -n tests/run-windows-bench.sh tests/test_windows_bench_stability_policy.sh vendor-to-netdata.sh`: passed.
- `codacy-analysis analyze --files ... --output-format json`: 0 issues; known local Revive adapter invocation error remains.

Real-use evidence:

- Full POSIX benchmark: 298 CSV lines, no warnings or failures, artifact `/tmp/plugin-ipc-bench-full-posix-final/benchmarks-posix.csv`.
- Full Win11 benchmark: 202 CSV lines, exit status 0, no hard failure scan matches, artifact `/tmp/plugin-ipc-bench-full-results-final2/benchmarks-windows.csv`.
- Win11 diagnostic rerun recovered one stability-only row and published the diagnostic row.

Reviewer findings:

- GitHub AI findings for `netipc_uds_send.c` were manually verified and addressed.
- GitHub review-thread findings from Netdata PR #22649 were manually verified and addressed where they were real.
- SonarCloud PR findings for Netdata PR #22649 were queried and addressed in the SDK source before re-vendoring.
- SonarCloud PR security hotspots for C `strncpy` and open-ended config field length/copy patterns were queried and addressed in the SDK source before re-vendoring.
- No external AI reviewer was used for this increment.

Same-failure scan:

- Same C total-message-width helper pattern was found and fixed in the Windows named-pipe transport.
- Same chunk-count overflow-prone ceil pattern was found and fixed in POSIX C, Windows C, POSIX Rust, Windows Rust, POSIX Go, and Windows Go transport paths.
- Same benchmark batch sizing nondeterminism pattern was found and fixed in POSIX and Windows Go benchmark drivers.
- Same dead Go service-level lookup dispatch guard was found and removed in apps lookup and cgroups lookup.
- Similar Go protocol-level lookup dispatch guards were tested and kept because regression coverage proves they still guard an overflow state.
- Same C `strncpy` path-copy pattern was found beyond the reported UDS lifecycle file in POSIX SHM context path storage and fixed in the same increment.

Sensitive data gate:

- `.env` was not read or committed.
- SOW evidence records local artifact paths, command names, and sanitized tool output only; no secrets or customer data were written.

Artifact maintenance gate:

- AGENTS.md: no workflow or guardrail change needed.
- Runtime project skills: no reusable project workflow changed.
- Specs: no protocol/API spec update needed; wire fields were already `u32`, and the implementation now enforces that existing wire contract consistently.
- End-user/operator docs: no public usage behavior changed.
- End-user/operator skills: no exported/operator workflow changed.
- SOW lifecycle: SOW remains `in-progress`; this increment records validation and the Netdata re-vendor plan.

Specs update:

- No spec update needed for this increment; the implementation now matches the existing `u32` wire-field constraints.

Project skills update:

- No project skill update needed; no reusable repo workflow changed beyond the current SOW evidence.

End-user/operator docs update:

- No end-user/operator docs update needed; public API and documented usage did not change.

End-user/operator skills update:

- No end-user/operator skill update needed.

Lessons:

- Benchmark runners must distinguish correctness failures from stability-only noise and must preserve first-attempt evidence before any diagnostic recovery.
- Vendor scripts should copy source subtrees after source-file organization splits; hard-coded file lists go stale quickly.

Follow-up mapping:

- Netdata re-vendor work is planned in this SOW and will be performed after the SDK commit is pushed.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
