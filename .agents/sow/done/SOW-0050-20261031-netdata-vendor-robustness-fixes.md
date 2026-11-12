# SOW-0050 - Netdata Vendor Robustness Fixes

## Status

Status: completed

Sub-state: delivered 2026-11-12. netdata/netdata#24633 vendors plugin-ipc `792eb3c` (SOW-0072 review fixes) with the consumer fixes; its CI passes except the SonarCloud duplication condition. Previously delivered 2026-11-09 with `d3c94a5`. Merging is the user's and the maintainers' decision. netdata/netdata#24633 vendors plugin-ipc `d3c94a5` with the consumer fixes; its CI passes except the triaged SonarCloud gate. Merging is the user's and the maintainers' decision.

## Requirements

### Purpose

Deliver the SOW-0044 (dead peer detection) and SOW-0043 (handshake, timeouts, shutdown) fixes to Netdata, whose plugins consume NetIPC as vendored source.

### User Request

From SOW-0040 decisions (2026-10-29, decision 3a): the robustness fixes are vendored into Netdata together in one PR after SOW-0043, through the vendoring preflight. Pushing and opening the Netdata PR need explicit user approval.

### Assistant Understanding

Facts:

- Netdata consumes NetIPC through vendored copies of `src/libnetdata/netipc`, `src/crates/netipc` and `src/go/pkg/netipc` (`vendor-to-netdata.sh`, `diff-netdata-vendor.sh`).
- The current Netdata consumers are C on Linux using SHM_FUTEX with a 5 s call timeout (survey recorded in SOW-0044, `ktsaou/netdata @ ae9113f2cc`). N5 (SHM client waits the full timeout after the server dies) affects them today.
- `.agents/skills/project-netdata-vendoring/SKILL.md` is mandatory before touching a Netdata checkout.
- Netdata consumer fixes carried from SOW-0021's downstream plan (checked 2026-10-31 against `ktsaou/netdata @ ae9113f2cc`; the statuses are already vendored there, the consumers were never adapted):
  - confirmed bug: `src/collectors/network-viewer.plugin/network-viewer-apps-lookup-client.c:503-537` special-cases only `NIPC_PID_LOOKUP_UNKNOWN`. An `NIPC_PID_LOOKUP_OVERSIZED_ITEM` item carries `cgroup_status == KNOWN (0)`, `starttime == 0` and empty strings (`docs/codec-apps-lookup.md`, "Field Semantics"), so it is cached as a known PID with empty data. It must be handled as not enriched and not cached; an escaped `PAYLOAD_EXCEEDED` must be treated as a contract failure, not as data;
  - `src/collectors/apps.plugin/apps-cgroups-lookup-client.c:381-403`: `NIPC_CGROUP_LOOKUP_OVERSIZED_ITEM` already ends as a final non-retry state (only `UNKNOWN_RETRY_LATER` is re-requested) but is counted as a retry miss; count it separately;
  - `src/collectors/cgroups.plugin/cgroup-lookup-test/cgroup-lookup-test.c`: print names for `PAYLOAD_EXCEEDED` and `OVERSIZED_ITEM`.
- SOW-0035 (2026-10-31) moved plugin-ipc to rustfmt style edition 2024, formatting-only. Its Netdata half (the same formatting in the vendored `src/crates/netipc/`) is delivered by this vendoring; the vendored copy must pass `rustfmt --edition 2024 --check`. Netdata's workspace already uses edition 2024 and has no `rustfmt.toml` (`ktsaou/netdata @ ae9113f2cc`, `src/crates/Cargo.toml:66`).
- SOW-0045 (2026-10-31) added `src/libnetdata/netipc/src/transport/windows/netipc_win_security.c` (Windows access policy) and links it against `advapi32`. Netdata lists Windows NetIPC sources by hand: `netipc/src/transport/windows/netipc_win_security.c` must be added to `NETIPC_WINDOWS_TRANSPORT_FILES` (`ktsaou/netdata @ ae9113f2cc`, `src/libnetdata/SourceLists.cmake:472-475`). `advapi32` is already linked on Windows (`src/libnetdata/CMakeLists.txt:145`). The Windows plugins (apps.plugin server, network-viewer.plugin client) both run as LocalSystem, which the new access policy accepts.
- SOW-0058 (2026-10-31) moved plugin-ipc to `go 1.27.0` (Netdata's version). Its Codacy re-check needs the branch pushed: the preflight here records the Codacy Trivy `go.mod` standard-library findings after the push (33 open on `main` `63919ecb7d` before the bump).

Inferences:

- The SOW-0044 performance assessment found no established-session per-request change beyond one `Option` read in Rust, so its part of the vendored diff needs no benchmark run; SOW-0043's assessment decides the rest. SOW-0051 adds two monotonic clock reads per call (call start and remaining-time computation), so the vendored diff includes an established-session per-request change and the pre-vendoring benchmark run covers it. SOW-0062 adds one branch per client packet send and one queue-empty check per packet receive on the POSIX baseline, and a non-blocking progress send for pipelining clients; the pre-vendoring POSIX run covers its rows (`uds-pipeline-d16`, `uds-pipeline-batch`, and the single-in-flight UDS rows).

Unknowns:

- The last source-to-Netdata vendoring baseline must be re-established by the preflight at execution time.

### Acceptance Criteria

- The vendoring preflight (CI, code/secret/dependency scanning, baseline, two-way gap analysis, migration plan, performance assessment) is recorded here and passes.
- The Netdata checkout builds and its NetIPC consumers pass the targeted tests after vendoring; the post-vendor diff matches the expected normalized diff.
- The Netdata PR is opened only after user approval.

## Analysis

Sources checked:

- SOW-0044 Followup and Outcome; SOW-0043 scope; `.agents/skills/project-netdata-vendoring/SKILL.md`.

Current state:

- Both fix SOWs are on branch `sow-0040-robustness-repro-tests` in `plugin-ipc`. The branch was pushed on 2026-11-04 with user approval (candidate commit `2eee40a`, after SOW-0064 and SOW-0056).

Risks:

- Downstream drift in Netdata's vendored copy since the last vendoring may conflict with these fixes; the preflight's two-way analysis covers it.

## Pre-Implementation Gate

Status: ready (preflight passed 2026-11-06)

Problem / root-cause model:

- Netdata's vendored NetIPC lacks the fixes of the SOWs completed since the last vendoring. The first vendoring (2026-11-06) used source commit `4261919` (branch `sow-0040-robustness-repro-tests`), after `2eee40a` and the later SOW-0066 and SOW-0067 fixes; the re-vendoring after SOW-0070 uses that SOW's commit. Each candidate's preflight evidence is in the Execution Log.

Evidence reviewed:

- The vendoring preflight in the Execution Log: CI and check runs, code-scanning, Dependabot and secret-scanning results with each alert's disposition, the reconstructed baseline and two-way gap analysis, the migration plan, and the same-host benchmark A/B (decisions of 2026-11-04 and 2026-11-06).

Affected contracts and surfaces:

- Netdata vendored NetIPC trees (C, Rust, Go) and the Netdata plugins that call NetIPC.

Existing patterns to reuse:

- `vendor-to-netdata.sh`, `diff-netdata-vendor.sh`, the `project-netdata-vendoring` preflight, SOW-0027 as the previous vendoring record.

Risk and blast radius:

- Every Netdata plugin using NetIPC; the new disconnected error code is a new public enum value that Netdata callers must treat as a failed call (they already treat any non-OK result as failure).

Sensitive data handling plan:

- Cite Netdata evidence as `owner/repo @ commit` with repo-relative paths; no workstation paths, tokens or private endpoints in this SOW, Netdata commit messages or the PR.

Implementation plan:

1. Run the vendoring preflight and record it here.
2. Vendor, diff, build and test in the Netdata checkout.
3. Ask the user before pushing and opening the PR.

Validation plan:

- Preflight evidence; post-vendor diff; Netdata build and targeted NetIPC consumer tests.

Artifact impact plan:

- Expected none in `plugin-ipc` beyond this SOW; revisit at close.

Open-source reference evidence:

- Netdata checkout commit recorded at activation.

Open decisions:

- Push and PR approval: resolved (2026-11-06, user; netdata/plugin-ipc#18 and netdata/netdata#24633 are open).
- Remaining close condition: Netdata PR CI passes on the re-vendored commit. Met 2026-11-09 (Validation), with one exception: the SonarCloud quality gate fails on a triaged false positive (c:S3519) and on duplication 3.2% against 3% (Validation).

## Implications And Decisions

Ordering (2026-10-31, user): the full POSIX and Windows benchmark run happens after SOW-0045 and SOW-0058 land, and before this vendoring. SOW-0057 waits until after this vendoring.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

- CI on `d54b22c` (SOW-0066, dispatched on the branch): Runtime Safety success (all Linux targets), CodeQL success (all five languages, Go Windows included), Codacy Local Analysis success, Static Analysis failure only in "Go Static Analysis (src/go) / Report gosec status", Supply Chain Security failure only in OpenSSF Scorecard (default branch only; Semgrep and OSV-Scanner pass), Codacy Coverage failure only in the upload (decision 5).
  - Open code-scanning alerts on the branch: 7766 gosec G115 (`src/go/pkg/netipc/service/raw/client.go:71`, `uint32(ms)` after the `ms > ^uint32(0)` check and `left > 0`, so bounded: false positive); 7764 and 7765 Semgrep `chmod` (stale: the calls are gone, and the codacy-local analysis of `d54b22c` reports 0 results under a different tool name, so GitHub does not close them). Dismissal with evidence comments awaits user approval (the dismiss call was refused by the session's permission policy).
- Preflight decisions (2026-11-04, user accepted every recommendation):
  1. CI fixes land in a separate SOW (SOW-0066) with its own commit, which becomes the vendoring candidate.
  2. The macOS `chmod` findings are fixed with fd-relative calls (SOW-0066).
  3. CodeQL alerts 7759 to 7763 are dismissed with evidence comments (SOW-0066).
  4. Semgrep is pinned (SOW-0066).
  5. The Codacy coverage upload failure ("No Write permission on Project" for the `CODACY_API_TOKEN` account) is accepted for this vendoring; the user fixes the token permission later.
  6. Pushing SOW-0066's commit to the branch and rerunning the workflows is approved.
  7. The POSIX benchmarks rerun after SOW-0066, on a quiet workstation.
  8. Performance is settled with same-host full runs of `63919ec` (what Netdata runs today) and of the candidate, on Windows and on a quiet POSIX host; the candidate's runs become the committed `benchmarks-*.csv`.
  9. The Windows scratch tree holding the `63919ec` baseline build is kept for decision 8.
  The Windows power plan stays unchanged; changing it was not approved.
- Performance and vendoring decisions (2026-11-06, user: "I like your picks"):
  1. A: the possible Windows snapshot-baseline cost (about 5%, paired geometric mean 0.951) and the possible Go-client SHM batch cost on POSIX (about 2%, pinned) are accepted. Snapshot refresh runs about once per second per plugin against a capacity of about 50k/s.
  2. A: the committed `benchmarks-*.csv` (2026-06-15) are not replaced in this vendoring; the same-host A/B in this SOW is the performance evidence. The host-wide drop on both benchmark hosts and the re-baseline of CSVs and floors are tracked by SOW-0069. This supersedes the "commit the candidate's runs" part of decision 8.
  3. A: push `4261919` and rerun the workflows; it is the vendoring candidate.
  4. A: vendor into Netdata. The user asked for everything to be on GitHub for approvals (a plugin-ipc PR and a Netdata PR), and for the vendoring to use a separate git worktree of the user's Netdata checkout.
- Vendoring (2026-11-06): a git worktree of the user's Netdata checkout, branch `netipc-vendor-20261106` from `netdata/netdata @ 3483002881` (upstream master; its vendored trees still have only the two downstream commits analysed above). `diff-netdata-vendor.sh` before: 201 difference lines; `vendor-to-netdata.sh` from plugin-ipc `4261919`; Go import paths rewritten to `github.com/netdata/netdata/go/plugins` (113 files); after: no C, Rust or Go differences. Changes stay inside `src/libnetdata/netipc`, `src/crates/netipc` and `src/go/pkg/netipc` (180 modified, 60 added, the 2 `shm_linux*.go` removed by the `shm_posix*.go` rename) plus the two build files: `netipc_win_security.c` added to `NETIPC_WINDOWS_TRANSPORT_FILES` (the only vendored `.c` missing from the lists; headers are not listed by convention) and `target_link_libraries(netipc PUBLIC advapi32)` for Windows (libnetdata links advapi32 before netipc, which a static link cannot resolve). Rust: the crate needs only `libc`, already a workspace dependency; Go: Netdata's `go.mod` already requires `golang.org/x/sys v0.49.0`.
- Netdata validation on Linux (worktree build directory, the main checkout's `ENABLE_*` options, submodules initialized): configure and full build pass with no warning in NetIPC sources; `apps-lookup-protocol-test`, `apps-lookup-netipc-lock-test`, `apps-cgroups-lookup-client-abort-test`, `cgroup-lookup-netipc-test` pass; `go test ./pkg/netipc/...` and `go vet` pass; `cargo test -p netipc` 401 passed; `cargo fmt -p netipc --check` and `cargo doc -p netipc` (0 warnings) clean.
- Netdata commit `42313ad410` pushed to the user's fork; PRs opened at the user's request: netdata/plugin-ipc#18 (branch `sow-0040-robustness-repro-tests`) and netdata/netdata#24633. Netdata builds `netipc` on Linux and Windows only (`src/libnetdata/CMakeLists.txt`), so the library's FreeBSD and macOS support is not used by Netdata yet; this vendoring keeps that scope.
- SOW-0067 (`bbc55a2`, 2026-11-04) fixes the endpoint lock and SHM region file under a restrictive umask, found during SOW-0066 validation; it becomes the vendoring candidate. Pushing it and dismissing alerts 7764 to 7766 approved (2026-11-05, user, "1a 2a").
- Alerts dismissed with evidence comments (2026-11-05): 7764 and 7765 as stale (the `chmod` calls were removed in `d54b22c`; last reported at `2eee40a`), 7766 as a false positive (bounded conversion).
- CI on `bbc55a2` (dispatched): Runtime Safety, CodeQL and Codacy Local Analysis succeed. Codacy Coverage failed earlier than the accepted upload failure: `llvm-profdata merge` crashed on a mode 0200 profile written by SOW-0067's Rust umask test child; repaired as a SOW-0067 regression (`292c6dc`). Static Analysis still fails in "Report gosec status": the step fails on any gosec finding, and dismissing alert 7766 does not change gosec's own result; `80a56de` adds the repository's usual `#nosec G115` justification at `client.go:71` (local gosec 2.29.0 does not flag the line; CI installs `gosec@latest`). Supply Chain Security fails only in OpenSSF Scorecard (default branch only).
- `tests/run-coverage-go.sh` reports 88.4% against its 90% threshold; `d54b22c` gives 88.3%, so the shortfall predates SOW-0066 and SOW-0067. No workflow runs this threshold. Tracked by SOW-0068 after this vendoring (user, 2026-11-05, "2a"). Push of `292c6dc` and `80a56de` and the workflow rerun approved (user, "1a").
- CI on `80a56de` (the vendoring candidate; dispatched): Runtime Safety (all Linux targets), CodeQL, Codacy Local Analysis and Static Analysis succeed (the gosec status step passes with the annotation). Codacy Coverage fails only in "Upload C coverage to Codacy" (decision 5; report generation passes). Supply Chain Security fails only in OpenSSF Scorecard (default branch only). Open code-scanning, Dependabot and secret-scanning alerts: 0, 0, 0.
- Decision 8A runs started 2026-11-05: on the Windows test host, full `tests/run-windows-bench.sh` runs of `63919ec` and then `80a56de` from `git archive` exports, one after the other on the idle host (about 11% CPU from the resident Netdata agent, as in earlier runs). Extended fuzz and stress suites started on the Linux workstation on `80a56de` plus uncommitted SOW files only. The POSIX A/B waits for a quiet workstation (load average 15 to 33 on 112 cores from unrelated workloads on 2026-11-05).
- Extended suites on `80a56de` (Linux, 2026-11-05): `tests/run-extended-fuzz.sh` 15 passed, 0 failed (12 Go fuzz targets at 60 s each, the C harness with 1 MB random input and with 1000 x 1 KB inputs, the Rust proptest at 100000 cases). `tests/run-extended-stress.sh` 7 of 7 passed: `test_windows_bench_stability_policy`, `test_stress` (C), `test_chaos_extended`, `test_lookup_scale_interop`, `test_stress_go`, `test_stress_rust`, `test_lookup_scale_shm_interop`.
- Windows decision 8A, first attempt: the `63919ec` run finished (07:29 to 12:26 host time, 197 rows); the `80a56de` run stopped at its first run directory (`mkdir -m 0700` fails on MSYS). Repaired as a SOW-0066 regression (`4261919`, Windows bench script only); the candidate run restarted on a `git archive` export of `4261919`, which becomes the vendoring candidate (the other files equal `80a56de`).
- `63919ec` Windows rows lost: shm-ping-pong rust->c, rust->rust and rust->go and snapshot-shm rust->rust fail the stability gate (as on every earlier run on this host); snapshot-baseline rust->rust repeat 2 reports `client: 1 errors`, a failed call during measurement, and its diagnostic rerun is a raw-ratio outlier (1.357 against 1.35). To be compared with the candidate run before any conclusion.
- POSIX decision 8A started 2026-11-05 10:3x UTC on the Linux workstation (CPU pressure about 2%, no memory or IO pressure, load average about 10 on 112 cores): full `tests/run-posix-bench.sh` runs of `63919ec` and then `4261919` from `git archive` exports, both under umask 022 (the `63919ec` script creates its run directory with a plain `mkdir`); load and pressure sampled every minute alongside.
- Windows full A/B result (`63919ec` 197 rows, `4261919` 200 rows; 195 common): geometric mean of throughput ratios 0.964, with max-rate ratios from 0.58 to 1.42 in both directions, including rows whose code did not change (shm-ping-pong c->c 1.42, np-pipeline-d16 c->c 1.30). Two sequential five-hour runs cannot resolve differences of that size on this host. Consistent: np-pipeline-batch-d16 at about 0.82 to 0.95 (the cost accepted in SOW-0064 decision 8A); lookup 1.09 to 1.24. To resolve: snapshot-baseline (c->c 0.63, rust->c 0.58) and np-batch-ping-pong (0.81 to 1.00). The 100k/s Named Pipe rows are capacity-bound in both runs (no client reaches 100k/s), so they behave as unchecked max-rate rows and move in both directions. The candidate run reports no client error; `63919ec`'s `client: 1 errors` did not recur. A targeted alternating A/B (snapshot-baseline and np-batch-ping-pong, max rate, two rounds of base then candidate) is running.
- The `63919ec` POSIX run fails its floors on today's workstation (uds-ping-pong c->c 72k/s against a 120k floor; the committed CSV has 156k), so the POSIX host also runs well below its committed numbers.
- POSIX full A/B (`63919ec` and `4261919`, 345 common rows, both under similar load: median load average 13.0 and 12.4, CPU pressure at most 5%): geometric mean 1.048. Lookups flat (0.97 to 1.07); Go rows up strongly (SOW-0063: snapshot-shm rust->go 3.70, uds-ping-pong go->go 1.66). Below 0.9: 9 scattered rows (shm-ping-pong rust->c 0.63, shm-batch-ping-pong go->c 0.72, uds-batch-ping-pong go->c 0.78, ...). Floors missed after retries: `63919ec` 31 rows, `4261919` 23 rows (uds-ping-pong about 65k to 78k/s on both against a 120k floor; committed c->c 156k).
- POSIX row-interleaved A/B (each row runs base then candidate, 10 s max rate, low priority; `.tmp` runner calling the bench binaries directly, since `tests/run-posix-bench.sh` has no row filter): shm-ping-pong, shm-batch-ping-pong and uds-batch-ping-pong, 27 rows x 3 rounds, geometric mean 1.114; the scattered full-run drops do not reproduce (shm-ping-pong rust->c 1.08, shm-batch-ping-pong go->c 1.04). Four rows below 0.9 got 6 more rounds (9 in all): uds-batch-ping-pong c->c 0.98, uds-batch-ping-pong go->rust 1.07, shm-ping-pong rust->rust 1.10, shm-batch-ping-pong go->rust 0.89 (baseline bimodal: 16M or 20M to 21M). Pinned to CPUs 51 (server) and 55 (client), separate cores sharing one L3, 6 rounds: shm-batch-ping-pong go->rust 0.984 and go->c 0.980, tight spreads. The 0.89 was CPU placement; the Go-client SHM batch rows may carry a cost of about 2%, at the edge of what 6 samples resolve. Code check: the SHM dead-peer probes added by `b450b40` run only after an idle receive timeout (C client and Rust server), not per request.
- Windows alternating A/B (snapshot-baseline and np-batch-ping-pong, max rate, `63919ec` then `4261919`, two rounds; with the full runs, three measurements per row): np-batch-ping-pong geometric mean 1.111 (the full-run 0.81 to 1.00 was noise). snapshot-baseline, paired by adjacent runs: 0.820, 0.902 and 1.143 (all 26 paired rows: 0.951, 18 below 1); the candidate is steady (means 49k, 54k, 54k/s) while the baseline swings (59k, 59k, 48k/s). A cost of about 5% is possible and has a plausible source: SOW-0064 made the Named Pipe client handle overlapped and measured a higher client CPU per call; a snapshot call reads a large response. SOW-0064 recorded one-in-flight rows as equal or higher, so this cost was not part of its decision 8A.
- Report generation on the candidate runs: `tests/generate-benchmarks-posix.sh` reports 26 floor violations (uds-ping-pong about 65k to 78k/s against 120k on all 9 pairs; snapshot-baseline, snapshot-shm and two shm-ping-pong rows); `tests/generate-benchmarks-windows.sh` rejects the CSV as incomplete (199 of 201 rows: shm-ping-pong c->rust and snapshot-shm rust->rust failed the stability gate). The committed CSVs date from 2026-06-15 (`95c53ef`; uds-ping-pong c->c 156k/s, p50 4 us). Workstation today: `schedutil` governor, STIBP always on, kernel 7.0.0-42; the June state is not recorded, so the cause of the host-wide drop is unknown.

- Code-scanning alerts on `278125c` (2026-11-09, user, "1b": dismiss all four, no code change), all in `netipc_uds_lifecycle.c`: 7769 `cpp/path-injection` (line 404, the endpoint lock path from the caller's `run_dir`, the same finding as the dismissed 7761 and 7767) dismissed as a false positive; 7770 and 7771 `cpp/stack-address-escape` (lines 348 and 338, the test-only hook setters; the tests in `tests/fixtures/c/test_uds.c` reset each hook to NULL before returning) dismissed as used in tests; 7768 Semgrep `c_race_rule-chmod` (line 376, from a scan before `3fb1bef` removed the `chmod`) dismissed as stale. Open code-scanning alerts after the dismissals: 0.
- Codacy Coverage upload on `278125c`: the same "No Write permission on Project" refusal as decision 5. The repository secret `CODACY_API_TOKEN` was last updated on 2026-06-03, and the reporter version is 14.1.3 in both the last passing run (main, 2026-09-27) and the failing runs (since 2026-11-04). Cause (user, 2026-11-09): Codacy revoked every token without an expiration date and now requires tokens to expire within one year; the revoked token produces this refusal. The repair is a new token with an expiration date, stored in the repository secret by an account with Codacy access. Decision 5 still applies to this vendoring.
- Re-vendoring (2026-11-09) into the same worktree (branch `netipc-vendor-20261106`). Downstream: no commit on `netdata/netdata` master touched the vendored trees or the NetIPC build lists since `3483002881`. Vendoring `278125c` built with a new NetIPC warning (`link()` unused result under `_FORTIFY_SOURCE`), fixed in plugin-ipc `d3c94a5` (SOW-0070 regression 2026-11-09); `d3c94a5` vendored, Go import paths rewritten by hand (the script only prints the command), `diff-netdata-vendor.sh` reports no C, Rust or Go differences, and no vendored `.c` file is missing from the build lists.
- Netdata validation on `d3c94a5` (Linux): full build with no warning in NetIPC sources; `apps-lookup-protocol-test`, `apps-lookup-netipc-lock-test`, `apps-cgroups-lookup-client-abort-test`, `cgroup-lookup-netipc-test`, `network-viewer-apps-lookup-client-test` pass; `go test ./pkg/netipc/...` and `go vet` pass; `cargo test -p netipc` 413 passed; `cargo fmt --check` and `cargo doc` (0 warnings) clean. `GOOS=windows go vet` reports `possible misuse of unsafe.Pointer` on the `MapViewOfFile` address conversions in `transport/windows/shm.go`; the same conversions exist in `63919ec`, and CI vets Linux only.
- Found during this validation: the three lookup tests left their `mkdtemp` run directory behind, because the endpoint lock file (kept on disk by design) made `rmdir` fail. They now remove `{run_dir}/{service}.lock` first, and a rerun leaves no directory. Netdata commit `d2343836cc` (local) holds the re-vendor, the consumer fixes (items above) and this test cleanup.

## Plan

See the implementation plan above.

## Execution Log

### 2026-11-04

- Source branch pushed (user approval, "1a"). The workflows trigger on `main` and PRs only, so they were started with `workflow_dispatch` on the branch at `2eee40a`: Codacy Local Analysis, Codacy Coverage, CodeQL, Runtime Safety, Static Analysis, Supply Chain Security. `macos.yml` cannot be dispatched: it does not exist on the default branch yet (GitHub 404); macOS was validated on the macOS test host (SOW-0056). `dependency-review.yml` runs on pull requests only.
- Full benchmark runs started on `2eee40a` (Test Cadence; covers SOW-0051, SOW-0062, SOW-0063, SOW-0064 and SOW-0056): `tests/run-posix-bench.sh` on the Linux workstation (low priority; load average about 25 at start) and `tests/run-windows-bench.sh` on the Windows test host, both with default durations as for the committed reports.
- Vendoring baseline and two-way drift (read-only analysis):
  - Baseline: `netdata/netdata @ 22f029781de1a0d817bad31fbc3c49915094b86d` (#24049) vendors plugin-ipc `5c1e145` (PR #17 branch); its squash on main is `63919ec`, with an identical vendored tree. The normalized `diff -rq` of the Netdata tree against `63919ec` is empty across 228 files.
  - Upstream since `63919ec`: 25 commits touch the vendored trees (210 files, +20161/-5980), SOW-0031 through SOW-0064 and SOW-0056.
  - Downstream since the baseline: two commits. `netdata/netdata @ 95167c963a` (#24164) is rustfmt-only; plugin-ipc HEAD already matches it (rustfmt on HEAD makes zero changes), so it is obsolete drift. `netdata/netdata @ a80f0e3dc0` (#24123) changes one rustdoc line, `src/crates/netipc/src/protocol/mod.rs` (`[directory]` to `` `directory` ``), which removes the only `cargo doc` warning; it must be backported to plugin-ipc first.
  - Netdata-only files (wrappers `netipc_netdata.c/h`, the workspace `Cargo.toml`, the Go module path) did not change since the baseline.
  - Build lists: add `netipc/src/transport/windows/netipc_win_security.c` to `NETIPC_WINDOWS_TRANSPORT_FILES` in Netdata's `src/libnetdata/SourceLists.cmake`. The file calls advapi32; Netdata's `netipc` target links nothing on Windows, so link `advapi32` to it (or prove the Netdata Windows build links as is). Rust and Go need no list change; Go additions, removals and the `shm_linux*.go` to `shm_posix*.go` rename ride the rsync `--delete`.
  - Consumers: C plugins compare only ABORTED/TIMEOUT/OVERFLOW/BAD_ITEM_COUNT; the new DISCONNECTED/TRANSPORT codes compile and fall into their generic failure branches. No Netdata code outside the vendored trees uses the Rust or Go packages.
- CI on `2eee40a`: all six dispatched workflows failed. Triage:
  - Product or test defects in this branch:
    1. Go `TestPipeDACLOwnerOnly`, `TestWinShmObjectsDACLOwnerOnly` (CodeQL Go Windows build step): the runner account is the built-in Administrator (RID 500), which SDDL prints as `LA`; the tests build the expected string from the full SID. The Rust tests `test_pipe_dacl_owner_only` and `test_server_objects_dacl_owner_only_windows` compare the same way; the C helper compares SIDs and is correct. The product DACLs are right.
    2. C `test_robustness` CLOEXEC case, every Linux target: it checks every fd from 3 up, and the runner passes two inherited pipes (fds 142 and 145) that NetIPC did not create.
    3. Rust build on `armv7-gnu` and `armv7-gnu-time64`: CI's Rust 1.99.1 links `+crt-static` armv7 gnueabihf binaries as static-pie, which needs `rcrt1.o`; Ubuntu's `libc6-dev-armhf-cross` ships none (the local Rust 1.97.1 does not hit this).
    4. staticcheck U1000: `service/raw/large_helpers_unix_test.go` helpers are used only by the `netipc_extended` stress tests; the no-op `setNoSigpipe` in `uds_platform_other.go` is unused on Linux and FreeBSD (accept4 path).
    5. flawfinder level 5 and Semgrep `c_race_rule-chmod` (code-scanning 7764, 7765): the two `chmod()` calls in the macOS-only `bind_owner_only_socket` (`netipc_uds_lifecycle.c:127,132`, SOW-0049).
  - CodeQL alerts on the branch (none open on main): 7761 `cpp/path-injection` (`netipc_uds_lifecycle.c:321`, the endpoint lock path built from the caller's `run_dir`), 7763 `cpp/stack-address-escape` (`:299`, the listen test hook), 7759 `cpp/toctou-race-condition` (`tests/fixtures/c/test_uds.c:1349`), 7760 `cpp/world-writable-file-creation` (`tests/fixtures/c/uds_eintr_wrap.c:139`, the umask probe), 7762 `cpp/long-switch` note (`test_uds.c:342`).
  - Not code: OpenSSF Scorecard runs on the default branch only. Semgrep Secrets crashes (`semgrep-core exited with -11`) with the unpinned latest semgrep, also on main since at least 2026-10-19. Codacy Coverage upload is refused with "No Write permission on Project" for the `CODACY_API_TOKEN` account (it last succeeded on main on 2026-09-27). Codacy Local Analysis fails because of the chmod finding (item 5).
  - Dependabot: 0 open. Secret scanning: 0 open.
- POSIX benchmark run 1 is invalid: `tests/run-posix-bench.sh` creates its run directory with a plain `mkdir` (lines 576, 905); under the session umask `0002` it is group-writable, and the C driver (`tests/fixtures/c/interop_path.h:94`) refuses it, so every scenario with a C server or C client failed. The script fix is `mkdir -m 0700`. A rerun with umask `022` was stopped after its first row: the workstation load average was 93 from unrelated workloads, and c->c ping-pong read 53k against the committed 156k.
- Windows benchmark run on `2eee40a` (host idle at start): 190 of 196 rows published; 6 SHM rows (shm-ping-pong rust->c, go->c, c->rust, rust->go, go->go; snapshot-shm c->go) failed the stability gate (stable ratio up to 2.6) and their diagnostic reruns did not recover. Against the committed `benchmarks-windows.csv` (`95c53ef`, 2026-06-15) almost every max-rate row is 0.3x to 0.4x, including the in-process `lookup` rows that use no transport, which points to host drift since June rather than to code. A same-host A/B of `95c53ef` against `2eee40a` (lookup, shm-ping-pong, np-pipeline-batch-d16; C clients; two alternating rounds) is running to separate them.
- Windows A/B result (same host, rounds base, head, base, head; max-rate throughput, C clients):

  | Row | 95c53ef r1 | 95c53ef r2 | 2eee40a r1 | 2eee40a r2 | head/base |
  |---|---|---|---|---|---|
  | lookup c | 59.2M | 53.9M | 46.3M | 44.0M | 0.80 |
  | np-pipeline-batch-d16 c->c | 16.8M | 18.2M | 16.9M | 15.7M | 0.93 |
  | np-pipeline-batch-d16 c->rust | 18.2M | 17.3M | 16.7M | 15.4M | 0.90 |
  | np-pipeline-batch-d16 c->go | 14.8M | 12.5M | 13.0M | 12.9M | 0.95 |
  | shm-ping-pong c->c | 1.29M | 2.99M | 2.48M | failed | noisy |
  | shm-ping-pong c->rust | failed | 2.78M | failed | 2.59M | noisy |
  | shm-ping-pong c->go | 1.78M | 1.98M | failed | 1.23M | noisy |

  - Host drift is confirmed: the June commit itself now runs at 0.35x to 0.45x of its own committed numbers (lookup 122M then, 54M to 59M now; np-pipeline-batch c->c 46.7M then, 16.8M to 18.2M now). The committed `benchmarks-windows.csv` is not a valid reference on this host.
  - The lookup gap comes from the benchmark itself: SOW-0025 (`cf8eaf9`, 2026-06-29, after `95c53ef`, already vendored to Netdata through SOW-0027) changed every lookup driver to take the cache read guard per pass of 16 lookups.
  - The np-pipeline-batch gap is the cost accepted in SOW-0064 decision 8A.
  - SHM ping-pong fails the stability gate on both commits on this host, so it gives no signal either way. Since the last vendoring (`63919ec`), the Windows SHM C and Rust code changed only in `nipc_win_shm_server_create` (DACL, SOW-0045) and in formatting; the Go change is SOW-0063's hot-path work.
  - The vendoring reference is `63919ec` (what Netdata runs today), not `95c53ef`.

### 2026-11-06

- PR checks on netdata/plugin-ipc#18 (head `9568f4e`, library sources equal to `4261919`): 41 pass, 4 skipped, 1 failure, "C, Rust, and Go Coverage" in its Codacy upload step only (decision 5). CodeQL first failed on new alert 7767 (`cpp/path-injection`, `netipc_uds_lifecycle.c:352`), the same finding as the dismissed 7761 at lines moved by SOW-0067; dismissed as a false positive (user, "1a"); CodeQL then passes and open code-scanning alerts are 0.
- PR checks on netdata/netdata#24633 (head `42313ad410`): 124 pass, 20 skipped, 1 failure, the SonarCloud quality gate. "Build Windows", "Test Windows installer" and the Go `windows/amd64` builds pass, which proves the `netipc_win_security.c` list entry and the `advapi32` link.
- SonarCloud findings, triaged and posted on the Netdata PR (user, "2a"):
  - BLOCKER c:S3519 (`netipc_named_pipe.c:1784`): false positive. `recv_buf` is grown to at least `payload_len` (`ensure_recv_buf`, lines 1600-1611, called at 1718); a first read longer than the message is rejected (1699); continuation chunks are checked for header size (1752), header fields (1765-1768) and payload length (1775); `assembled + chunk_data > payload_len` is rejected before the copy (1780).
  - 8 MAJOR c:S2753 (`netipc_win_shm.c` lines 40-163): test-only fault injection; the file-local enum has the same enumerators and values as the public test enum under `NIPC_INTERNAL_TESTING`, which Netdata never defines; the code is unchanged since `63919ec`, only shifted.
  - Duplication on new code 3.2% against a 3% limit: vendored transport and test code; no refactor in a vendoring PR.

### 2026-11-12

- Re-vendoring of `ce6834e` (SOW-0070 regression 2026-11-11) as Netdata `b53818fab6`, recorded late: it was pushed to the PR on 2026-11-11 and not recorded here then.
- Re-vendoring for SOW-0072 (review findings on both PRs): Netdata `b41982210f` vendors plugin-ipc `142f289` with the network-viewer test fix (cache mutex initialized around `run_overflow_status_test()`), and `6aaaeea23a` vendors `792eb3c` (the second review round).
- PR thread replies: 15 threads on netdata#24633 and 29 on plugin-ipc#18 answered one by one, with the fix commit or the evidence for a rejection; 0 unresolved threads on either PR afterwards.

## Validation

Acceptance criteria evidence:

- Vendoring preflight recorded and passing: candidate `4261919` (CI on `80a56de` and `4261919`, same library sources), code scanning, Dependabot and secret scanning 0 open, baseline `63919ec` / `netdata/netdata @ 22f029781d`, two-way gap analysis, migration plan, performance assessment with same-host A/B and user acceptance (decisions 2026-11-06), extended fuzz 15/15 and stress 7/7 (Implications And Decisions).
- Post-vendor normalized diff: no C, Rust or Go differences; Netdata-only wrappers, workspace manifest and Go module path preserved.
- Netdata builds and its NetIPC consumers pass on Linux; the Windows build passes in the Netdata PR CI (2026-11-06 log).
- PRs opened only at the user's request (decision 4A).
- Re-vendoring (2026-11-09): candidate `d3c94a5`. Preflight: plugin-ipc#18 checks on `d3c94a5` pass except "C, Rust, and Go Coverage" (Codacy upload, decision 5; the token was revoked by Codacy) and GitHub's managed "Code Quality: PR #18" job, which failed on an HTTP 504 downloading the CodeQL bundle and cannot be retried (it passed on every earlier commit; CodeQL itself passed after a rerun of the same 504). Code scanning, Dependabot and secret scanning: 0, 0, 0 open. Downstream drift: none since `3483002881`. Performance: `3fb1bef`, `278125c` and `d3c94a5` change no established-session per-request path (lock creation runs once per listen, the zero in-flight bound applies only with nothing in flight, the receive change is whitespace); the benchmark evidence of 2026-11-06 stands. Extended suites: the 2026-11-05 runs stand; the later changes touch lock creation, a send bound with nothing in flight, `StopListening` and the Go stalled-reader poll, which the stress suites run without change in coverage, and no codec or decoder.

- Re-vendoring (2026-11-12): candidate `792eb3c` (first `142f289`). Preflight: plugin-ipc#18 checks on `142f289` and `792eb3c` pass, macOS included, except "C, Rust, and Go Coverage" (Codacy upload, decision 5). Code scanning, Dependabot and secret scanning: 0, 0, 0 open. Downstream drift: no `netdata/netdata` master commit since `3483002881` touches the vendored trees, the NetIPC build lists or the NetIPC consumers (the only network-viewer change, `00cfa2052e`, is in the topology code). Performance: the changes run once per message chunk only in the new `chunk_index < chunk_count` comparison (SOW-0072 decision 9, assessment only); everything else is handshake, connect, attach, listen or failure paths, so the benchmark evidence of 2026-11-06 stands. Extended suites, replacing the 2026-11-09 "no codec or decoder" assessment, since SOW-0072 changes the receive path: `tests/run-extended-fuzz.sh` 15/15 on the `04f71b1` tree (the later commits change no codec or decoder) and `tests/run-extended-stress.sh` 7/7 on `04f71b1` and on `5e68489`, which contains the `792eb3c` attach-loop change. The vendor diff after the copy and the Go import rewrite: no C, Rust or Go differences; no vendored file added or removed, so the build lists need no change.

Tests or equivalent validation:

- Re-vendored `792eb3c` (Netdata `6aaaeea23a`, and `142f289` as `b41982210f` before it): Linux build with no warning in NetIPC sources; the five lookup tests pass; `go test ./pkg/netipc/...`, `go vet`, `cargo test -p netipc` 416, `cargo fmt --check`, `cargo doc` 0 warnings; the fortified `-O2 -D_FORTIFY_SOURCE=3` library build has no library warning. Netdata PR CI on `6aaaeea23a`: 123 pass, 21 skipped, 1 failure (SonarCloud, below); "Build Windows", "Test Windows installer" and the Go `windows/amd64` builds pass.

- Netdata worktree on Linux: full build without NetIPC warnings; `apps-lookup-protocol-test`, `apps-lookup-netipc-lock-test`, `apps-cgroups-lookup-client-abort-test`, `cgroup-lookup-netipc-test`; `go test ./pkg/netipc/...`, `go vet`; `cargo test -p netipc` 401; `cargo fmt --check`, `cargo doc` 0 warnings.
- Netdata PR CI: 124 checks pass, Windows build and installer included.
- Re-vendored `d3c94a5` (Netdata `d2343836cc`): Linux build without NetIPC warnings under Netdata's `-O2 -D_FORTIFY_SOURCE=3`; the five lookup tests pass and leave no temporary directory; `go test`, `go vet`, `cargo test -p netipc` 413, `cargo fmt --check`, `cargo doc` 0 warnings. Netdata PR CI on `d2343836cc`: 124 pass, 20 skipped, 1 failure (SonarCloud, below); "Build Windows" passes. Eight jobs first failed on HTTP 504 downloads from GitHub (eBPF release artifacts, the golangci-lint action) and passed when rerun.
- Source suites per SOW (SOW-0031 to SOW-0067) on Linux, FreeBSD 14.3 and 15.1, macOS and Windows.

Real-use evidence:

- Netdata's apps and cgroups lookup tests start the real plugin-side NetIPC servers and clients from the vendored tree.

Reviewer findings:

- SonarCloud and CodeQL findings on the PRs are triaged above, with evidence; no code change was needed.
- SonarCloud on `d2343836cc`: the gate fails on the same two conditions as on `42313ad410`: the c:S3519 false positive (`netipc_named_pipe.c:1921` now; the copy is preceded by the `assembled + chunk_data > payload_len` check) and duplication 3.2% against 3%. The other open issues are code smells outside the gate.
- PR review replies: 46 on plugin-ipc#18 and 50 on netdata#24633, posted and resolved; 0 unresolved threads on either PR. Review round of 2026-11-12: 29 more on plugin-ipc#18 and 15 on netdata#24633 (SOW-0072); 0 unresolved afterwards.
- SonarCloud on `6aaaeea23a`: the gate fails only on duplication on new code, 3.3% against 3% (vendored transport and test code); the reliability condition (the c:S3519 false positive) now passes. CodeQL alerts 7768 to 7771 dismissed (user, "1b").
- `GOOS=windows go vet` flags `possible misuse of unsafe.Pointer` on the `MapViewOfFile` address conversions in `transport/windows/shm.go`. Rejected with evidence: the address is a mapped view outside the Go heap, so the garbage collector never moves or frees it; the same conversions exist in `63919ec`.

Same-failure scan:

- Vendored `.c` files against Netdata's build lists: `netipc_win_security.c` was the only one missing. System libraries called by new code: only `advapi32` (security descriptors), now linked.
- Re-vendoring: no vendored `.c` file missing from the lists; fortified library build: no other warning in library sources; every Netdata test that creates a NetIPC run directory (the three lookup tests) now removes the lock file.

Sensitive data gate:

- This SOW cites Netdata evidence as `netdata/netdata @ commit` with repo-relative paths, test hosts by role, and no tokens, private endpoints or workstation paths; the Codacy token is named only by its secret name. The worktree path is under the user's home and recorded with `~`.

Artifact maintenance gate:

- AGENTS.md: no update; no workflow change.
- Runtime project skills: `project-netdata-vendoring` step 10 now requires checking Netdata's build lists and link libraries, and records that the Windows build is proven by the Netdata PR CI (lesson below). Step 9 now gives the Go import-path rewrite to run by hand; step 10 the fortified-build warning check (SOW-0070 regression 2026-11-09).
- Specs: no update; vendoring changes no protocol or API.
- End-user/operator docs: no update; the vendor scripts and their usage did not change.
- End-user/operator skills: `docs/netipc-integrator-skill.md` unaffected; no API or workflow change in this SOW.
- SOW lifecycle: follow-ups tracked as SOW-0068 and SOW-0069. Correction (2026-11-08, SOW-0070): this SOW is `paused` in `current/`; it moves to `done/` after the re-vendoring and the Netdata PR CI. Done 2026-11-09: `completed`, moved to `done/`. Reopened 2026-11-12 for the SOW-0072 re-vendoring; `completed` again and moved to `done/` in the same commit.

Performance assessment:

- Same-host A/B against `63919ec` on both hosts (Implications And Decisions); possible costs of about 5% (Windows snapshot-baseline) and about 2% (POSIX Go-client SHM batch) accepted by the user (decision 1A).

## Outcome

Netdata PR netdata/netdata#24633 vendors plugin-ipc `792eb3c` (first `4261919`, re-vendored after SOW-0070 as `d3c94a5` and `ce6834e`, and after SOW-0072) with the consumer fixes for `OVERSIZED_ITEM` and `PAYLOAD_EXCEEDED`, the lookup-test cleanup, and the Windows build-list and `advapi32` link changes; its CI passes except the SonarCloud gate, whose findings are triaged as false positives on the PR. Source PR netdata/plugin-ipc#18 carries the SOWs since the previous vendoring. Merging both is the user's and the maintainers' decision.

## Lessons Extracted

- `vendor-to-netdata.sh` copies sources but not build integration: a new `.c` file and the system libraries it calls must be added to Netdata's CMake by hand, and only the Netdata Windows CI proves the Windows side. Recorded in the vendoring skill.
- The vendor script leaves the Go import rewrite to the operator, and Netdata compiles with fortify flags plugin-ipc does not use; both now sit in the vendoring skill.
- NetIPC keeps the endpoint lock file on disk by design, so every consumer test that removes its run directory must remove the lock file first.
- Two sequential five-hour benchmark runs cannot resolve differences under about 20% on these hosts; row-interleaved runs with CPU pinning can. Host-wide drift since June is tracked by SOW-0069.

## Followup

- SOW-0068 (pending): restore Go coverage to its 90% threshold.
- SOW-0069 (pending): benchmark host drift and floor re-baseline.
- Codacy coverage upload: Codacy revoked every token without an expiry date. User action, accepted in decision 5: create a token that expires within one year and store it in `CODACY_API_TOKEN`. Not code.

## Regression Log

None yet.
