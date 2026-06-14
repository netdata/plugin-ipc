#!/usr/bin/env bash
#
# generate-benchmarks-posix.sh - Generate benchmarks-posix.md from CSV data.
#
# Reads the benchmark CSV output, validates completeness, checks
# performance floors, and generates a markdown document with the full
# POSIX benchmark matrix.
#
# Usage:
#   ./tests/generate-benchmarks-posix.sh [input_csv] [output_md]

set -euo pipefail

RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

INPUT_CSV="${1:-${ROOT_DIR}/benchmarks-posix.csv}"
OUTPUT_MD="${2:-${ROOT_DIR}/benchmarks-posix.md}"
EXPECTED_HEADER="scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct"
EXPECTED_TOTAL_ROWS=345
RUNNER_DEFAULT_FIXED_DURATION=5
RUNNER_DEFAULT_MAX_DURATION=10
RUNNER_DEFAULT_FLOOR_RETRY_SAMPLES=7
RUNNER_DEFAULT_FLOOR_RETRY_DURATION=20
RUNNER_DEFAULT_FLOOR_RETRY_MAX_RATIO=1.35
VALIDATION_FAILED=0
FLOOR_FAILED=0
CSV_FILE=""
RETRY_CSV="${INPUT_CSV%.csv}.floor-retries.csv"
LOOKUP_METHOD_SCENARIOS=(
    cgroups-lookup-known-16
    cgroups-lookup-unknown-16
    cgroups-lookup-mixed-16
    cgroups-lookup-mixed-256
    cgroups-lookup-known-8192
    cgroups-lookup-known-32768
    apps-lookup-known-16
    apps-lookup-unknown-16
    apps-lookup-mixed-16
    apps-lookup-mixed-256
    apps-lookup-known-8192
    apps-lookup-known-32768
)

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

    for scenario in uds-ping-pong shm-ping-pong uds-batch-ping-pong shm-batch-ping-pong; do
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
    for scenario in "${LOOKUP_METHOD_SCENARIOS[@]}"; do
        require_rows "$scenario" "0" 3
        require_rows "$scenario" "100000" 3
        require_rows "$scenario" "10000" 3
        require_rows "$scenario" "1000" 3
    done
    require_rows "uds-pipeline-d16" "0" 9
    require_rows "uds-pipeline-batch-d16" "0" 9
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

emit_lookup_method_section() {
    echo "## Lookup Method Codec And Dispatch"
    echo ""
    echo "| Scenario | Target RPS | Language | Throughput | p50 (us) | p95 (us) | p99 (us) | CPU |"
    echo "|----------|-----------:|----------|-----------:|---------:|---------:|---------:|----:|"

    for scenario in "${LOOKUP_METHOD_SCENARIOS[@]}"; do
        for target_rps in 0 100000 10000 1000; do
            awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" '
                NR > 1 && $1 == scenario && $4 == target_rps { print }
            ' "$CSV_FILE" | sort -t',' -k2,2 | while IFS=',' read -r row_scenario client server row_target_rps throughput p50 p95 p99 ccpu scpu tcpu; do
                local tp_fmt
                tp_fmt=$(format_throughput "$throughput")
                printf "| %s | %s | %-8s | %10s | %8s | %8s | %8s | %s%% |\n" \
                    "$row_scenario" "$row_target_rps" "$client" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu"
            done
        done
    done

    echo ""
}

emit_validation_summary() {
    echo "## Validation Summary"
    echo ""
    echo "| Scenario | Target RPS | Expected Rows | Actual Rows |"
    echo "|----------|-----------:|--------------:|------------:|"

    for scenario in uds-ping-pong shm-ping-pong uds-batch-ping-pong shm-batch-ping-pong; do
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
    for scenario in "${LOOKUP_METHOD_SCENARIOS[@]}"; do
        for target_rps in 0 100000 10000 1000; do
            printf "| %s | %s | %d | %d |\n" \
                "$scenario" "$target_rps" 3 "$(count_rows "$scenario" "$target_rps")"
        done
    done
    printf "| %s | %s | %d | %d |\n" "uds-pipeline-d16" "0" 9 "$(count_rows "uds-pipeline-d16" "0")"
    printf "| %s | %s | %d | %d |\n" "uds-pipeline-batch-d16" "0" 9 "$(count_rows "uds-pipeline-batch-d16" "0")"

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

has_snapshot_shm_violation() {
    awk -F',' '
        NR > 1 && $1 == "snapshot-shm" && $4 == "0" {
            throughput = $5 + 0
            min = ($2 == "go" || $3 == "go") ? 800000 : 1000000
            if (throughput < min) {
                found = 1
            }
        }
        END { exit(found ? 0 : 1) }
    ' "$CSV_FILE"
}

retry_recovered() {
    local scenario="$1"
    local client="$2"
    local server="$3"
    local target_rps="$4"
    local min_throughput="$5"
    local original_throughput="$6"

    [ -f "$RETRY_CSV" ] || return 1

    awk -F',' \
        -v scenario="$scenario" \
        -v client="$client" \
        -v server="$server" \
        -v target_rps="$target_rps" \
        -v min_throughput="$min_throughput" \
        -v original_throughput="$original_throughput" '
        NR > 1 &&
        $1 == scenario &&
        $2 == client &&
        $3 == server &&
        $4 == target_rps &&
        ($5 + 0) == (min_throughput + 0) &&
        ($6 + 0) == (original_throughput + 0) &&
        $12 == "recovered" &&
        ($8 + 0) >= (min_throughput + 0) {
            found = 1
        }
        END { exit(found ? 0 : 1) }
    ' "$RETRY_CSV"
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
                printf "%s,%s,%.0f\n", $2, $3, throughput
            }
        }
    ' "$CSV_FILE")

    if [ -n "$violations" ]; then
        while IFS= read -r violation; do
            IFS=',' read -r client server throughput <<< "$violation"
            if retry_recovered "$scenario" "$client" "$server" "$target_rps" "$min_throughput" "$throughput"; then
                log "FLOOR RECOVERED: ${label} ${client}->${server}: ${throughput} (min ${min_throughput})"
            else
                warn "FLOOR VIOLATION: ${label} ${client}->${server}: ${throughput} (min ${min_throughput})"
                FLOOR_FAILED=1
            fi
        done <<< "$violations"
    fi
}

log_floor_violations_snapshot_shm() {
    local violations
    violations=$(awk -F',' '
        NR > 1 && $1 == "snapshot-shm" && $4 == "0" {
            throughput = $5 + 0
            min = ($2 == "go" || $3 == "go") ? 800000 : 1000000
            if (throughput < min) {
                printf "%s,%s,%.0f,%d\n", $2, $3, throughput, min
            }
        }
    ' "$CSV_FILE")

    if [ -n "$violations" ]; then
        while IFS= read -r violation; do
            IFS=',' read -r client server throughput min_throughput <<< "$violation"
            if retry_recovered "snapshot-shm" "$client" "$server" "0" "$min_throughput" "$throughput"; then
                log "FLOOR RECOVERED: snapshot-shm ${client}->${server}: ${throughput} (min ${min_throughput})"
            else
                warn "FLOOR VIOLATION: snapshot-shm ${client}->${server}: ${throughput} (min ${min_throughput})"
                FLOOR_FAILED=1
            fi
        done <<< "$violations"
    fi
}

floor_status_min() {
    local scenario="$1"
    local target_rps="$2"
    local min_throughput="$3"
    local violations
    violations=$(awk -F',' -v scenario="$scenario" -v target_rps="$target_rps" -v min_throughput="$min_throughput" '
        NR > 1 && $1 == scenario && $4 == target_rps && ($5 + 0) < (min_throughput + 0) {
            printf "%s,%s,%.0f\n", $2, $3, $5
        }
    ' "$CSV_FILE")
    if [ -z "$violations" ]; then
        printf "PASS"
        return
    fi

    local recovered=0
    while IFS= read -r violation; do
        IFS=',' read -r client server throughput <<< "$violation"
        if retry_recovered "$scenario" "$client" "$server" "$target_rps" "$min_throughput" "$throughput"; then
            recovered=1
        else
            printf "FAIL"
            return
        fi
    done <<< "$violations"

    if [ "$recovered" -eq 1 ]; then
        printf "PASS (retry)"
    else
        printf "PASS"
    fi
}

floor_status_snapshot_shm() {
    local violations
    violations=$(awk -F',' '
        NR > 1 && $1 == "snapshot-shm" && $4 == "0" {
            throughput = $5 + 0
            min = ($2 == "go" || $3 == "go") ? 800000 : 1000000
            if (throughput < min) {
                printf "%s,%s,%.0f,%d\n", $2, $3, throughput, min
            }
        }
    ' "$CSV_FILE")
    if [ -z "$violations" ]; then
        printf "PASS"
        return
    fi

    local recovered=0
    while IFS= read -r violation; do
        IFS=',' read -r client server throughput min_throughput <<< "$violation"
        if retry_recovered "snapshot-shm" "$client" "$server" "0" "$min_throughput" "$throughput"; then
            recovered=1
        else
            printf "FAIL"
            return
        fi
    done <<< "$violations"

    if [ "$recovered" -eq 1 ]; then
        printf "PASS (retry)"
    else
        printf "PASS"
    fi
}

check_floors() {
    log "Checking performance floors..."

    log_floor_violations_min "shm-ping-pong" "0" 1000000 "shm-ping-pong"
    log_floor_violations_min "uds-ping-pong" "0" 120000 "uds-ping-pong"
    log_floor_violations_min "snapshot-baseline" "0" 100000 "snapshot-baseline"
    log_floor_violations_min "lookup" "0" 10000000 "lookup"
    log_floor_violations_min "cgroups-lookup-known-16" "0" 250000 "cgroups-lookup-known-16"
    log_floor_violations_min "cgroups-lookup-unknown-16" "0" 500000 "cgroups-lookup-unknown-16"
    log_floor_violations_min "cgroups-lookup-mixed-16" "0" 350000 "cgroups-lookup-mixed-16"
    log_floor_violations_min "cgroups-lookup-mixed-256" "0" 25000 "cgroups-lookup-mixed-256"
    log_floor_violations_min "apps-lookup-known-16" "0" 300000 "apps-lookup-known-16"
    log_floor_violations_min "apps-lookup-unknown-16" "0" 500000 "apps-lookup-unknown-16"
    log_floor_violations_min "apps-lookup-mixed-16" "0" 350000 "apps-lookup-mixed-16"
    log_floor_violations_min "apps-lookup-mixed-256" "0" 25000 "apps-lookup-mixed-256"
    log_floor_violations_snapshot_shm

    if [ "$FLOOR_FAILED" -eq 0 ]; then
        log "All performance floors met"
    fi
}

emit_floor_summary() {
    echo "## Performance Floors"
    echo ""
    echo "| Metric | Floor | Status |"
    echo "|--------|-------|--------|"
    echo "| SHM ping-pong max | >= 1M req/s | $(floor_status_min "shm-ping-pong" "0" 1000000) |"
    echo "| SHM snapshot refresh max | >= 1M req/s for C/Rust pairs, >= 800k req/s for Go pairs | $(floor_status_snapshot_shm) |"
    echo "| UDS ping-pong max | >= 120k req/s | $(floor_status_min "uds-ping-pong" "0" 120000) |"
    echo "| UDS snapshot refresh max | >= 100k req/s | $(floor_status_min "snapshot-baseline" "0" 100000) |"
    echo "| Local cache lookup | >= 10M lookups/s | $(floor_status_min "lookup" "0" 10000000) |"
    echo "| cgroups-lookup known-16 max | >= 250k req/s | $(floor_status_min "cgroups-lookup-known-16" "0" 250000) |"
    echo "| cgroups-lookup unknown-16 max | >= 500k req/s | $(floor_status_min "cgroups-lookup-unknown-16" "0" 500000) |"
    echo "| cgroups-lookup mixed-16 max | >= 350k req/s | $(floor_status_min "cgroups-lookup-mixed-16" "0" 350000) |"
    echo "| cgroups-lookup mixed-256 max | >= 25k req/s | $(floor_status_min "cgroups-lookup-mixed-256" "0" 25000) |"
    echo "| apps-lookup known-16 max | >= 300k req/s | $(floor_status_min "apps-lookup-known-16" "0" 300000) |"
    echo "| apps-lookup unknown-16 max | >= 500k req/s | $(floor_status_min "apps-lookup-unknown-16" "0" 500000) |"
    echo "| apps-lookup mixed-16 max | >= 350k req/s | $(floor_status_min "apps-lookup-mixed-16" "0" 350000) |"
    echo "| apps-lookup mixed-256 max | >= 25k req/s | $(floor_status_min "apps-lookup-mixed-256" "0" 25000) |"
    echo ""
}

emit_methodology() {
    echo "## Methodology"
    echo ""
    echo "- Fixed-rate rows use ${RUNNER_DEFAULT_FIXED_DURATION}s samples by default."
    echo "- Max-throughput rows use ${RUNNER_DEFAULT_MAX_DURATION}s samples by default."
    echo "- The script CLI duration controls fixed-rate rows; \`NIPC_BENCH_MAX_DURATION\` controls max-throughput rows."
    echo "- Rows that miss an enforced max-throughput floor are retried before publication: ${RUNNER_DEFAULT_FLOOR_RETRY_SAMPLES} samples x ${RUNNER_DEFAULT_FLOOR_RETRY_DURATION}s by default, published only when the retry median meets the same floor and retry max/min throughput ratio is <= ${RUNNER_DEFAULT_FLOOR_RETRY_MAX_RATIO}."
    echo "- Retry diagnostics, when used, are written next to the CSV as \`*.floor-retries.csv\`."
    echo "- Lookup method scale rows at 8192 and 32768 items are codec+dispatch stress gates: the generator requires complete positive-throughput rows, while stable floors are introduced only after baseline data is collected."
    echo ""
}

generate_md() {
    local tmp_file="${OUTPUT_MD}.tmp.$$"

    {
        echo "# POSIX Benchmark Results"
        echo ""
        echo "Generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
        echo ""
        echo "Machine: local benchmark host ($(uname -m), $(nproc) cores)"
        echo ""
        echo "CSV: $(basename "$INPUT_CSV")"
        echo ""
        echo "Complete matrix rows expected: ${EXPECTED_TOTAL_ROWS}"
        echo ""

        emit_validation_summary
        emit_scenario_section "UDS Ping-Pong" "uds-ping-pong" 0 100000 10000 1000
        emit_scenario_section "SHM Ping-Pong" "shm-ping-pong" 0 100000 10000 1000
        emit_scenario_section "UDS Batch Ping-Pong" "uds-batch-ping-pong" 0 100000 10000 1000
        emit_scenario_section "SHM Batch Ping-Pong" "shm-batch-ping-pong" 0 100000 10000 1000
        emit_scenario_section "Snapshot Baseline Refresh" "snapshot-baseline" 0 1000
        emit_scenario_section "Snapshot SHM Refresh" "snapshot-shm" 0 1000
        emit_scenario_section "UDS Pipeline" "uds-pipeline-d16" 0
        emit_scenario_section "UDS Pipeline+Batch" "uds-pipeline-batch-d16" 0
        emit_lookup_section
        emit_lookup_method_section
        emit_methodology
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
