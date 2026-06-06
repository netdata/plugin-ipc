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

## Validation

Acceptance criteria evidence:

- Pending.

Tests or equivalent validation:

- Pending.

Real-use evidence:

- Pending.

Reviewer findings:

- No external reviewer used yet.

Same-failure scan:

- Pending.

Sensitive data gate:

- Pending final scan.

Artifact maintenance gate:

- AGENTS.md: pending.
- Runtime project skills: pending.
- Specs: pending.
- End-user/operator docs: pending.
- End-user/operator skills: pending.
- SOW lifecycle: pending.

Specs update:

- Pending.

Project skills update:

- Pending.

End-user/operator docs update:

- Pending.

End-user/operator skills update:

- Pending.

Lessons:

- Pending.

Follow-up mapping:

- Pending.

## Outcome

Pending.

## Lessons Extracted

Pending.

## Followup

None yet.

## Regression Log

None yet.
