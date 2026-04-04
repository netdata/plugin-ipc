#!/usr/bin/env bash
#
# generate-benchmarks-windows.sh - Generate benchmarks-windows.md from CSV data.
#
# Reads the benchmark CSV output, validates completeness, checks
# performance floors, and generates a markdown document with the full
# Windows benchmark matrix.
#
# Usage:
#   ./tests/generate-benchmarks-windows.sh [input_csv] [output_md]

set -euo pipefail

RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

INPUT_CSV="${1:-${ROOT_DIR}/benchmarks-windows.csv}"
OUTPUT_MD="${2:-${ROOT_DIR}/benchmarks-windows.md}"
EXPECTED_HEADER="scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct"
EXPECTED_TOTAL_ROWS=201
RUNNER_DEFAULT_REPETITIONS=5
RUNNER_DEFAULT_FIXED_DURATION=5
RUNNER_DEFAULT_MAX_DURATION=10
RUNNER_DEFAULT_PIPELINE_BATCH_MAX_DURATION=20
RUNNER_DEFAULT_MAX_THROUGHPUT_RATIO=1.35
RUNNER_DEFAULT_MIN_STABLE_SAMPLES=3
VALIDATION_FAILED=0
FLOOR_FAILED=0
CSV_FILE=""

log() {
    printf "${CYAN}[gen]${NC} %s\n" "$*" >&2
}

warn() {
    printf "${YELLOW}[warn]${NC} %s\n" "$*" >&2
}

err() {
    printf "${RED}[error]${NC} %s\n" "$*" >&2
}

cleanup() {
    if [ -n "$CSV_FILE" ] && [ -f "$CSV_FILE" ]; then
        rm -f "$CSV_FILE"
    fi
}
trap cleanup EXIT

count_rows() {
    local scenario="$1"
    local target_rps="$2"
    awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" '
        NR > 1 && $1 == scenario && $4 == target_rps { count++ }
        END { print count + 0 }
    ' "$CSV_FILE"
}

format_throughput() {
    local val="$1"
    local num
    num=$(printf "%.0f" "$val" 2>/dev/null || echo "0")

    if [ "$num" -ge 1000000 ]; then
        printf "%.2fM" "$(echo "$num / 1000000" | bc -l)"
    elif [ "$num" -ge 1000 ]; then
        printf "%.1fk" "$(echo "$num / 1000" | bc -l)"
    else
        printf "%d" "$num"
    fi
}

rate_label() {
    local target_rps="$1"
    if [ "$target_rps" = "0" ]; then
        printf "Max throughput"
    else
        printf "%s req/s target" "$target_rps"
    fi
}

require_header() {
    local actual_header
    actual_header=$(head -n 1 "$CSV_FILE")
    if [ "$actual_header" != "$EXPECTED_HEADER" ]; then
        err "Unexpected CSV header."
        err "Expected: $EXPECTED_HEADER"
        err "Actual:   $actual_header"
        err "Re-run the benchmark scripts with the updated runner to regenerate the CSV."
        VALIDATION_FAILED=1
    fi
}

require_total_rows() {
    local total_rows
    total_rows=$(( $(wc -l < "$CSV_FILE") - 1 ))
    if [ "$total_rows" -ne "$EXPECTED_TOTAL_ROWS" ]; then
        err "Expected ${EXPECTED_TOTAL_ROWS} data rows, found ${total_rows}."
        VALIDATION_FAILED=1
    fi
}

require_rows() {
    local scenario="$1"
    local target_rps="$2"
    local expected="$3"
    local actual
    actual=$(count_rows "$scenario" "$target_rps")
    if [ "$actual" -ne "$expected" ]; then
        err "Expected ${expected} rows for ${scenario} at target_rps=${target_rps}, found ${actual}."
        VALIDATION_FAILED=1
    fi
}

require_positive_throughput() {
    awk -F',' '
        NR > 1 && ($5 + 0) <= 0 {
            printf "INVALID throughput<=0 for %s %s->%s @ %s\n", $1, $2, $3, $4
            bad = 1
        }
        END { exit bad }
    ' "$CSV_FILE" | while IFS= read -r line; do
        err "$line"
    done

    if [ "${PIPESTATUS[0]}" -ne 0 ]; then
        VALIDATION_FAILED=1
    fi
}

validate_csv() {
    if [ ! -f "$INPUT_CSV" ]; then
        err "Input CSV not found: $INPUT_CSV"
        exit 1
    fi

    if [ "$(wc -l < "$INPUT_CSV")" -lt 2 ]; then
        err "CSV has no data rows"
        exit 1
    fi

    log "Input CSV: $INPUT_CSV"

    CSV_FILE=$(mktemp)
    tr -d '\r' < "$INPUT_CSV" > "$CSV_FILE"

    require_header
    require_total_rows

    for scenario in np-ping-pong shm-ping-pong np-batch-ping-pong shm-batch-ping-pong; do
        require_rows "$scenario" "0" 9
        require_rows "$scenario" "100000" 9
        require_rows "$scenario" "10000" 9
        require_rows "$scenario" "1000" 9
    done

    for scenario in snapshot-baseline snapshot-shm; do
        require_rows "$scenario" "0" 9
        require_rows "$scenario" "1000" 9
    done

    require_rows "lookup" "0" 3
    require_rows "np-pipeline-d16" "0" 9
    require_rows "np-pipeline-batch-d16" "0" 9
    require_positive_throughput

    if [ "$VALIDATION_FAILED" -ne 0 ]; then
        exit 1
    fi
}

emit_matrix_table() {
    local scenario="$1"
    local target_rps="$2"

    echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |"
    echo "|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|"

    awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" '
        NR > 1 && $1 == scenario && $4 == target_rps { print }
    ' "$CSV_FILE" | sort -t',' -k2,2 -k3,3 | while IFS=',' read -r row_scenario client server row_target_rps throughput p50 p95 p99 ccpu scpu tcpu; do
        local tp_fmt
        tp_fmt=$(format_throughput "$throughput")
        printf "| %-6s | %-6s | %9s | %8s | %8s | %8s | %10s%% | %10s%% | %9s%% |\n" \
            "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu" "$scpu" "$tcpu"
    done

    echo ""
}

emit_scenario_section() {
    local title="$1"
    local scenario="$2"
    shift 2

    echo "## ${title}"
    echo ""
    for target_rps in "$@"; do
        echo "### $(rate_label "$target_rps")"
        echo ""
        emit_matrix_table "$scenario" "$target_rps"
    done
}

emit_lookup_section() {
    echo "## Local Cache Lookup"
    echo ""
    echo "| Language | Throughput | Client CPU | Total CPU |"
    echo "|----------|-----------|------------|-----------|"

    awk -F',' '
        NR > 1 && $1 == "lookup" && $4 == "0" { print }
    ' "$CSV_FILE" | sort -t',' -k2,2 | while IFS=',' read -r scenario client server target_rps throughput p50 p95 p99 ccpu scpu tcpu; do
        local tp_fmt
        tp_fmt=$(format_throughput "$throughput")
        printf "| %-8s | %9s | %10s%% | %9s%% |\n" "$client" "$tp_fmt" "$ccpu" "$tcpu"
    done

    echo ""
}

emit_validation_summary() {
    echo "## Validation Summary"
    echo ""
    echo "| Scenario | Target RPS | Expected Rows | Actual Rows |"
    echo "|----------|-----------:|--------------:|------------:|"

    for scenario in np-ping-pong shm-ping-pong np-batch-ping-pong shm-batch-ping-pong; do
        for target_rps in 0 100000 10000 1000; do
            printf "| %s | %s | %d | %d |\n" \
                "$scenario" "$target_rps" 9 "$(count_rows "$scenario" "$target_rps")"
        done
    done

    for scenario in snapshot-baseline snapshot-shm; do
        for target_rps in 0 1000; do
            printf "| %s | %s | %d | %d |\n" \
                "$scenario" "$target_rps" 9 "$(count_rows "$scenario" "$target_rps")"
        done
    done

    printf "| %s | %s | %d | %d |\n" "lookup" "0" 3 "$(count_rows "lookup" "0")"
    printf "| %s | %s | %d | %d |\n" "np-pipeline-d16" "0" 9 "$(count_rows "np-pipeline-d16" "0")"
    printf "| %s | %s | %d | %d |\n" "np-pipeline-batch-d16" "0" 9 "$(count_rows "np-pipeline-batch-d16" "0")"

    echo ""
}

emit_methodology() {
    echo "## Methodology"
    echo ""
    echo "- The current Windows runner publishes one CSV row per matrix cell after aggregating repeated measurements."
    echo "- Current runner defaults: ${RUNNER_DEFAULT_REPETITIONS} samples per cell."
    echo "- Fixed-rate rows use ${RUNNER_DEFAULT_FIXED_DURATION}s samples by default."
    echo "- Most max-tier rows use ${RUNNER_DEFAULT_MAX_DURATION}s samples by default."
    echo '- `np-pipeline-batch-d16 @ max` uses '"${RUNNER_DEFAULT_PIPELINE_BATCH_MAX_DURATION}"'s samples by default because the 10s window was not stable enough on `win11`.'
    echo "- Throughput publication is strict on the full raw repeated sample set for max-tier rows and attainable fixed-rate rows."
    echo "- Every repeated row must keep raw max/min <= ${RUNNER_DEFAULT_MAX_THROUGHPUT_RATIO}; one lucky or unlucky extreme is no longer publishable."
    echo "- With 5 samples, the runner still computes the trimmed stable core for diagnostics."
    echo "- Oversubscribed fixed-rate rows are the one exception: when the requested target is above the same pair's already-measured @ max capacity, the row may still publish if the trimmed stable core contains at least ${RUNNER_DEFAULT_MIN_STABLE_SAMPLES} samples and stays within max/min <= ${RUNNER_DEFAULT_MAX_THROUGHPUT_RATIO}."
    echo '- This keeps the Windows `100000/s` saturation-style rows visible without pretending they are attainable fixed-rate commitments.'
    echo '- The script CLI duration still controls the fixed-rate rows; `NIPC_BENCH_MAX_DURATION` controls most max-tier rows; `NIPC_BENCH_PIPELINE_BATCH_MAX_DURATION` controls `np-pipeline-batch-d16 @ max`.'
    echo ""
}

has_min_violation() {
    local scenario="$1"
    local target_rps="$2"
    local min_throughput="$3"
    awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" -v min_throughput="$min_throughput" '
        NR > 1 && $1 == scenario && $4 == target_rps {
            throughput = $5 + 0
            if (throughput < min_throughput) {
                found = 1
            }
        }
        END { exit(found ? 0 : 1) }
    ' "$CSV_FILE"
}

has_windows_shm_cr_violation() {
    awk -F',' '
        NR > 1 && $1 == "shm-ping-pong" && $4 == "0" && $2 != "go" && $3 != "go" {
            throughput = $5 + 0
            if (throughput < 1000000) {
                found = 1
            }
        }
        END { exit(found ? 0 : 1) }
    ' "$CSV_FILE"
}

log_floor_violations_min() {
    local scenario="$1"
    local target_rps="$2"
    local min_throughput="$3"
    local label="$4"

    local violations
    violations=$(awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" -v min_throughput="$min_throughput" '
        NR > 1 && $1 == scenario && $4 == target_rps {
            throughput = $5 + 0
            if (throughput < min_throughput) {
                printf "%s->%s: %.0f\n", $2, $3, throughput
            }
        }
    ' "$CSV_FILE")

    if [ -n "$violations" ]; then
        while IFS= read -r violation; do
            warn "FLOOR VIOLATION: ${label} ${violation} (min ${min_throughput})"
        done <<< "$violations"
        FLOOR_FAILED=1
    fi
}

log_floor_violations_windows_shm_cr() {
    local violations
    violations=$(awk -F',' '
        NR > 1 && $1 == "shm-ping-pong" && $4 == "0" && $2 != "go" && $3 != "go" {
            throughput = $5 + 0
            if (throughput < 1000000) {
                printf "%s->%s: %.0f\n", $2, $3, throughput
            }
        }
    ' "$CSV_FILE")

    if [ -n "$violations" ]; then
        while IFS= read -r violation; do
            warn "FLOOR VIOLATION: shm-ping-pong (C/Rust pairs) ${violation} (min 1000000)"
        done <<< "$violations"
        FLOOR_FAILED=1
    fi
}

floor_status_min() {
    local scenario="$1"
    local target_rps="$2"
    local min_throughput="$3"
    if has_min_violation "$scenario" "$target_rps" "$min_throughput"; then
        printf "FAIL"
    else
        printf "PASS"
    fi
}

floor_status_windows_shm_cr() {
    if has_windows_shm_cr_violation; then
        printf "FAIL"
    else
        printf "PASS"
    fi
}

check_floors() {
    log "Checking performance floors..."

    log_floor_violations_windows_shm_cr
    log_floor_violations_min "lookup" "0" 10000000 "lookup"

    if has_min_violation "np-ping-pong" "0" 1; then
        warn "FLOOR VIOLATION: np-ping-pong has zero-throughput max-tier rows"
        FLOOR_FAILED=1
    fi

    if [ "$FLOOR_FAILED" -eq 0 ]; then
        log "All performance floors met"
    fi
}

emit_floor_summary() {
    echo "## Performance Floors"
    echo ""
    echo "| Metric | Floor | Status |"
    echo "|--------|-------|--------|"
    echo "| Win SHM ping-pong max (C/Rust pairs) | >= 1M req/s | $(floor_status_windows_shm_cr) |"
    echo "| Named Pipe ping-pong max | > 0 req/s | $(floor_status_min "np-ping-pong" "0" 1) |"
    echo "| Local cache lookup | >= 10M lookups/s | $(floor_status_min "lookup" "0" 10000000) |"
    echo ""
}

generate_md() {
    local tmp_file="${OUTPUT_MD}.tmp.$$"

    {
        echo "# Windows Benchmark Results"
        echo ""
        echo "Generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
        echo ""
        echo "Machine: $(hostname) ($(uname -m))"
        echo ""
        echo "CSV: $(basename "$INPUT_CSV")"
        echo ""
        echo "Complete matrix rows expected: ${EXPECTED_TOTAL_ROWS}"
        echo ""

        emit_methodology
        emit_validation_summary
        emit_scenario_section "Named Pipe Ping-Pong" "np-ping-pong" 0 100000 10000 1000
        emit_scenario_section "Win SHM Ping-Pong" "shm-ping-pong" 0 100000 10000 1000
        emit_scenario_section "Named Pipe Batch Ping-Pong" "np-batch-ping-pong" 0 100000 10000 1000
        emit_scenario_section "Win SHM Batch Ping-Pong" "shm-batch-ping-pong" 0 100000 10000 1000
        emit_scenario_section "Snapshot Named Pipe Refresh" "snapshot-baseline" 0 1000
        emit_scenario_section "Snapshot Win SHM Refresh" "snapshot-shm" 0 1000
        emit_scenario_section "Named Pipe Pipeline" "np-pipeline-d16" 0
        emit_scenario_section "Named Pipe Pipeline+Batch" "np-pipeline-batch-d16" 0
        emit_lookup_section
        emit_floor_summary
    } > "$tmp_file"

    mv "$tmp_file" "$OUTPUT_MD"
    log "Generated: $OUTPUT_MD"
}

validate_csv
check_floors
generate_md

if [ "$FLOOR_FAILED" -ne 0 ]; then
    exit 1
fi
