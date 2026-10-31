# SOW-0050 - Netdata Vendor Robustness Fixes

## Status

Status: open

Sub-state: runs after all code SOWs (0045, 0058, 0057, 0049, 0059, 0056), together with the full benchmark run (user ordering 2026-10-31).

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

Inferences:

- The SOW-0044 performance assessment found no established-session per-request change beyond one `Option` read in Rust, so its part of the vendored diff needs no benchmark run; SOW-0043's assessment decides the rest. SOW-0051 adds two monotonic clock reads per call (call start and remaining-time computation), so the vendored diff includes an established-session per-request change and the pre-vendoring benchmark run covers it.

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

- Both fix SOWs are on branch `sow-0040-robustness-repro-tests` in `plugin-ipc`, not yet pushed; source CI evidence therefore does not exist yet.

Risks:

- Downstream drift in Netdata's vendored copy since the last vendoring may conflict with these fixes; the preflight's two-way analysis covers it.

## Pre-Implementation Gate

Status: blocked

Problem / root-cause model:

- Blocked on SOW-0043 completion and on pushing the source branch (user approval) so GitHub CI and scanners can run on the candidate commit. The gate is filled at activation from the vendoring preflight.

Evidence reviewed:

- To be produced by the preflight at activation; this SOW cannot start before SOW-0043 completes, so the evidence cannot exist yet.

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

- Push and PR approval (user).

## Implications And Decisions

Ordering (2026-10-31, user): the full POSIX and Windows benchmark run happens after SOW-0045 and SOW-0058 land, and before this vendoring. SOW-0057 waits until after this vendoring.
- Ordering (2026-10-31, user, supersedes earlier ordering): finish all code changes first — SOW-0045, SOW-0058, SOW-0057, SOW-0049, SOW-0059, and SOW-0056 when the FreeBSD 15.1 VM arrives — then run the full benchmarks and the SOW-0050 vendoring together; benchmark regressions are fixed and re-vendored.

## Plan

See the implementation plan above.

## Execution Log

None yet.

## Validation

Pending activation.

## Outcome

Pending activation.

## Lessons Extracted

Pending activation.

## Followup

None yet.

## Regression Log

None yet.
