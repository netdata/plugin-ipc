# SOW-0050 - Netdata Vendor Robustness Fixes

## Status

Status: open

Sub-state: waiting for SOW-0043 to complete; vendoring runs after both robustness fix SOWs land.

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

Inferences:

- The SOW-0044 performance assessment found no established-session per-request change beyond one `Option` read in Rust, so its part of the vendored diff needs no benchmark run; SOW-0043's assessment decides the rest.

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

None yet.

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
