#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck disable=SC1091
source "${ROOT_DIR}/tests/run-windows-bench.sh"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_true() {
    "$@" || fail "expected success: $*"
}

assert_false() {
    if "$@"; then
        fail "expected failure: $*"
    fi
}

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

SAMPLES="${TMP_DIR}/samples.csv"
cat > "$SAMPLES" <<'CSV'
repeat,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct
1,5730,40,80,120,0,0,0
2,21620,40,80,120,0,0,0
3,22100,40,80,120,0,0,0
4,22637,40,80,120,0,0,0
5,22687,40,80,120,0,0,0
CSV

stability="$(throughput_stability_from_sample_file "$SAMPLES")"
IFS=',' read -r stable_samples trimmed_each_side stable_min stable_max stable_ratio raw_min raw_max raw_ratio <<< "$stability"

[ "$stable_samples" = "3" ] || fail "stable_samples: got ${stable_samples}"
[ "$trimmed_each_side" = "1" ] || fail "trimmed_each_side: got ${trimmed_each_side}"
assert_true throughput_ratio_is_acceptable "$stable_ratio" "2.00"
assert_false throughput_ratio_is_acceptable "$raw_ratio" "2.00"

ALLOW_TRIMMED_UNSTABLE_RAW=0
assert_false raw_ratio_outlier_is_publishable "$trimmed_each_side"

ALLOW_TRIMMED_UNSTABLE_RAW=1
assert_true raw_ratio_outlier_is_publishable "$trimmed_each_side"

ALLOW_TRIMMED_UNSTABLE_RAW=1
assert_false raw_ratio_outlier_is_publishable 0

assert_true diagnostic_can_recover_reason stable_ratio_exceeded
assert_true diagnostic_can_recover_reason raw_ratio_exceeded
assert_false diagnostic_can_recover_reason measurement_command_failed_repeat_1
assert_false diagnostic_can_recover_reason aggregate_failed
assert_false diagnostic_can_recover_reason insufficient_stable_samples

DIAGNOSTIC_RECOVERED=0
assert_false diagnostic_recovered
DIAGNOSTIC_RECOVERED=1
assert_true diagnostic_recovered

printf 'PASS: Windows benchmark stability policy\n'
