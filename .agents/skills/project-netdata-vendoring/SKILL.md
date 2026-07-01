---
name: project-netdata-vendoring
description: "Mandatory workflow for copying, syncing, vendoring, or merging plugin-ipc NetIPC source changes into a Netdata checkout. Use whenever work mentions Netdata vendoring, vendor drift, vendor-to-netdata, diff-netdata-vendor.sh, vendor-to-netdata.sh, or updating src/libnetdata/netipc, src/crates/netipc, or src/go/pkg/netipc in Netdata."
---

# Netdata Vendoring Preflight

## Purpose

Netdata consumes NetIPC from this repository as vendored source. Before touching
a Netdata checkout, prove that the source `plugin-ipc` commit is safe enough to
copy downstream.

## Mandatory Rule

Do this preflight before any Netdata vendoring work, even if the user only asks
to copy, sync, vendor, merge, push, or continue.

Do not modify the Netdata vendored copy until:

- the candidate `plugin-ipc` commit is identified;
- source CI and GitHub code/security scanner status are checked;
- the last vendored baseline is identified or explicitly reconstructed;
- two-way drift is understood:
  - what changed in `plugin-ipc` since the last vendoring;
  - what changed in Netdata's vendored NetIPC copy since the last vendoring;
- a migration plan exists for every drift class;
- failures and open alerts are fixed, documented as evidence-backed false
  positives, or explicitly risk-accepted by the user in the active SOW.

## Scope Trigger

Load this skill for any task involving:

- `~/src/netdata-ktsaou.git` or another Netdata checkout receiving NetIPC files;
- `vendor-to-netdata.sh`;
- `diff-netdata-vendor.sh`;
- Netdata paths such as:
  - `src/libnetdata/netipc/`
  - `src/crates/netipc/`
  - `src/go/pkg/netipc/`
- statements like "vendor", "vendored", "copy to Netdata", "sync to Netdata",
  "merge to Netdata", or "update Netdata's NetIPC copy".

## Preflight Steps

1. Resolve the source commit.

   ```bash
   git rev-parse HEAD
   git status --short --branch
   git remote -v
   ```

   The source commit must be pushed or otherwise available to GitHub checks.
   If local uncommitted source changes are intended for vendoring, stop and
   make the source repository state explicit before touching Netdata.

2. Check GitHub Actions and commit checks for the source commit.

   ```bash
   gh run list --repo netdata/plugin-ipc --commit "$COMMIT" \
     --limit 50 \
     --json databaseId,name,status,conclusion,createdAt,updatedAt,url,headBranch,headSha

   gh api "repos/netdata/plugin-ipc/commits/$COMMIT/check-runs" \
     --jq '.check_runs[] | {name, status, conclusion, html_url}'

   gh api "repos/netdata/plugin-ipc/commits/$COMMIT/status" \
     --jq '{state, total_count, statuses}'
   ```

   Required result: no failing, cancelled, timed-out, or pending required checks
   unless the active SOW records why the result does not apply.

3. Check GitHub code/security scanners.

   ```bash
   gh api 'repos/netdata/plugin-ipc/code-scanning/alerts?state=open&per_page=100' \
     --jq '[.[] | {number, tool: .tool.name, rule: .rule.id, severity: .rule.severity, path: .most_recent_instance.location.path, line: .most_recent_instance.location.start_line, message: .most_recent_instance.message.text, url: .html_url}]'

   gh api 'repos/netdata/plugin-ipc/dependabot/alerts?state=open&per_page=100' \
     --jq '[.[] | {number, package: .dependency.package.name, ecosystem: .dependency.package.ecosystem, severity: .security_advisory.severity, manifest: .dependency.manifest_path, url: .html_url}]'

   gh api 'repos/netdata/plugin-ipc/secret-scanning/alerts?state=open&per_page=100' \
     --jq '[.[] | {number, secret_type, state, resolution, url: .html_url}]'
   ```

   Required result: no untriaged open code-scanning, dependency, or secret
   alerts for the candidate source branch/commit. If alerts exist, record exact
   files/rules and stop unless they are fixed, evidence-backed false positives,
   or explicitly accepted by the user.

4. Establish the last vendored baseline.

   Find the last Netdata commit or PR that updated the vendored NetIPC trees:

   ```bash
   git -C "$NETDATA_ROOT" log --oneline --decorate -- \
     src/libnetdata/netipc \
     src/crates/netipc \
     src/go/pkg/netipc
   ```

   Then identify the matching `plugin-ipc` source commit from one of:

   - Netdata commit message or PR description;
   - prior SOW evidence;
   - previous vendor-sync commit in `plugin-ipc`;
   - a reconstructed normalized match using `diff-netdata-vendor.sh` against
     candidate source commits.

   If the matching source commit cannot be identified with enough confidence,
   stop before vendoring. Record the uncertainty and propose a baseline
   reconstruction plan.

5. Perform two-way gap analysis.

   Direction 1: source changes since the last vendoring:

   ```bash
   git diff --stat "$BASE_PLUGIN_IPC_COMMIT"..HEAD -- \
     src/libnetdata/netipc \
     src/crates/netipc/src \
     src/go/pkg/netipc

   git diff "$BASE_PLUGIN_IPC_COMMIT"..HEAD -- \
     src/libnetdata/netipc \
     src/crates/netipc/src \
     src/go/pkg/netipc
   ```

   Direction 2: Netdata vendored-copy changes since the last vendoring:

   ```bash
   git -C "$NETDATA_ROOT" diff --stat "$BASE_NETDATA_VENDOR_COMMIT"..HEAD -- \
     src/libnetdata/netipc \
     src/crates/netipc \
     src/go/pkg/netipc

   git -C "$NETDATA_ROOT" diff "$BASE_NETDATA_VENDOR_COMMIT"..HEAD -- \
     src/libnetdata/netipc \
     src/crates/netipc \
     src/go/pkg/netipc
   ```

   Classify every change as one of:

   - upstream source change to vendor into Netdata;
   - Netdata-only wrapper/build/package/import-path difference to preserve;
   - Netdata vendored-source fix that must first be backported to `plugin-ipc`;
   - real conflict requiring a migration decision before copying;
   - obsolete downstream drift that can be replaced by upstream source.

6. Write the migration plan in the active SOW before vendoring.

   The plan must say:

   - exact baseline source commit and Netdata vendor commit;
   - files changed upstream since baseline;
   - files changed downstream since baseline;
   - for each file/class, whether upstream wins, Netdata-local changes are
     preserved, downstream fixes are backported first, or user decision is
     needed;
   - expected post-vendor diff after normal exclusions and Go import-path
     normalization.

   If any downstream vendored-source change is not understood, do not run
   `vendor-to-netdata.sh`.

7. Record the CI/scanner preflight in the active SOW before vendoring.

   Include:

   - source repository and commit;
   - GitHub Actions/check-run summary;
   - code-scanning summary by tool and severity;
   - Dependabot and secret-scanning summary;
   - decision: proceed, block, or proceed with explicit risk acceptance.

8. Only after the CI/scanner preflight and two-way migration plan pass, update
   the Netdata vendored copy using the project-local vendor workflow.

   ```bash
   bash ./diff-netdata-vendor.sh /path/to/netdata
   bash ./vendor-to-netdata.sh /path/to/netdata
   bash ./diff-netdata-vendor.sh /path/to/netdata
   ```

9. Validate in the Netdata checkout.

   Run the targeted Netdata build/tests required by the active SOW. At minimum,
   prove:

   - the vendor diff is expected after copying;
   - Netdata-only wrapper files were not overwritten;
   - unrelated Netdata files were not staged or modified by the vendoring step.

## Blockers

Stop and report if any of these are true:

- source commit has no GitHub CI/check-run evidence;
- source CI is failing, pending, cancelled, or timed out;
- GitHub code scanning has untriaged open alerts;
- Dependabot has untriaged open alerts;
- secret scanning has open alerts;
- the last source-to-Netdata vendoring baseline cannot be identified or
  reconstructed with evidence;
- either side has NetIPC changes since the last vendoring that are not
  understood;
- the active SOW does not contain a migration plan for both directions;
- source tree has uncommitted changes that are intended for vendoring;
- Netdata checkout has unrelated changes that would be hard to isolate.

## Reporting Shape

Use this concise report before vendoring:

```text
Netdata vendoring preflight:
- Source commit: <sha>
- Baseline: plugin-ipc <sha>, Netdata <sha>
- CI/checks: <pass/fail/pending summary>
- Code scanning: <count by tool/severity, exact blockers>
- Dependabot: <open count>
- Secret scanning: <open count>
- Upstream gap: <summary of plugin-ipc changes since baseline>
- Downstream gap: <summary of Netdata vendored changes since baseline>
- Migration plan: <preserve/backport/overwrite/decision summary>
- Decision: proceed | blocked | proceed with user-accepted risk
```
