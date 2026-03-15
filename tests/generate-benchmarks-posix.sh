#!/usr/bin/env bash
#
# generate-benchmarks-posix.sh - Generate benchmarks-posix.md from CSV data.
#
# Reads the benchmark CSV output, validates completeness, and generates
# a markdown document with tables and analysis.
#
# Atomic write: writes to a temp file, then renames on success.
#
# Usage:
#   ./tests/generate-benchmarks-posix.sh [input_csv] [output_md]

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

INPUT_CSV="${1:-${ROOT_DIR}/benchmarks-posix.csv}"
OUTPUT_MD="${2:-${ROOT_DIR}/benchmarks-posix.md}"

log() {
    printf "${CYAN}[gen]${NC} %s\n" "$*" >&2
}

warn() {
    printf "${YELLOW}[warn]${NC} %s\n" "$*" >&2
}

err() {
    printf "${RED}[error]${NC} %s\n" "$*" >&2
}

# ---------------------------------------------------------------------------
#  Validation
# ---------------------------------------------------------------------------

validate_csv() {
    if [ ! -f "$INPUT_CSV" ]; then
        err "Input CSV not found: $INPUT_CSV"
        exit 1
    fi

    local line_count
    line_count=$(wc -l < "$INPUT_CSV")

    if [ "$line_count" -lt 2 ]; then
        err "CSV has no data rows"
        exit 1
    fi

    log "Input CSV: $INPUT_CSV ($((line_count - 1)) data rows)"
}

# ---------------------------------------------------------------------------
#  Performance floor checks
# ---------------------------------------------------------------------------

check_floors() {
    local violations=0

    log "Checking performance floors..."

    # SHM ping-pong max: >= 1M req/s (first 9 rows = max rate)
    while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
        if [ "$scenario" = "shm-ping-pong" ]; then
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 1000000 ]; then
                warn "FLOOR VIOLATION: shm-ping-pong ${client}->${server}: ${tp_int} req/s (min 1M)"
                violations=$((violations + 1))
            fi
        fi
    done < <(grep "^shm-ping-pong," "$INPUT_CSV" | head -9)

    # UDS ping-pong max: >= 150k req/s for all pairs
    while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
        if [ "$scenario" = "uds-ping-pong" ]; then
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 150000 ]; then
                warn "FLOOR VIOLATION: uds-ping-pong ${client}->${server}: ${tp_int} req/s (min 150k)"
                violations=$((violations + 1))
            fi
        fi
    done < <(grep "^uds-ping-pong," "$INPUT_CSV" | head -9)

    # Local cache lookup: >= 10M lookups/s
    while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
        if [ "$scenario" = "lookup" ]; then
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 10000000 ]; then
                warn "FLOOR VIOLATION: lookup ${client}: ${tp_int} lookups/s (min 10M)"
                violations=$((violations + 1))
            fi
        fi
    done < <(grep "^lookup," "$INPUT_CSV")

    if [ "$violations" -gt 0 ]; then
        warn "$violations performance floor violation(s) detected"
    else
        log "All performance floors met"
    fi

    return $violations
}

# ---------------------------------------------------------------------------
#  Markdown generation
# ---------------------------------------------------------------------------

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

generate_md() {
    local tmp_file="${OUTPUT_MD}.tmp.$$"

    {
        echo "# POSIX Benchmark Results"
        echo ""
        echo "Generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
        echo ""
        echo "Machine: $(uname -n) ($(uname -m), $(nproc) cores)"
        echo ""
        echo "Duration per run: extracted from CSV data."
        echo ""

        # --- UDS Ping-Pong ---
        echo "## UDS Ping-Pong (max throughput)"
        echo ""
        echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |"
        echo "|--------|--------|-----------|----------|----------|----------|------------|------------|"

        grep "^uds-ping-pong," "$INPUT_CSV" | head -9 | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_fmt
            tp_fmt=$(format_throughput "$throughput")
            printf "| %-6s | %-6s | %9s | %8s | %8s | %8s | %10s%% | %10s%% |\n" \
                "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu" "$scpu"
        done

        echo ""

        # --- SHM Ping-Pong ---
        echo "## SHM Ping-Pong (max throughput)"
        echo ""
        echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |"
        echo "|--------|--------|-----------|----------|----------|----------|------------|------------|"

        grep "^shm-ping-pong," "$INPUT_CSV" | head -9 | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_fmt
            tp_fmt=$(format_throughput "$throughput")
            printf "| %-6s | %-6s | %9s | %8s | %8s | %8s | %10s%% | %10s%% |\n" \
                "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu" "$scpu"
        done

        echo ""

        # --- Snapshot Baseline ---
        echo "## Snapshot Baseline Refresh (max throughput)"
        echo ""
        echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |"
        echo "|--------|--------|-----------|----------|----------|----------|------------|------------|"

        grep "^snapshot-baseline," "$INPUT_CSV" | head -9 | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_fmt
            tp_fmt=$(format_throughput "$throughput")
            printf "| %-6s | %-6s | %9s | %8s | %8s | %8s | %10s%% | %10s%% |\n" \
                "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu" "$scpu"
        done

        echo ""

        # --- Snapshot SHM ---
        echo "## Snapshot SHM Refresh (max throughput)"
        echo ""
        echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |"
        echo "|--------|--------|-----------|----------|----------|----------|------------|------------|"

        grep "^snapshot-shm," "$INPUT_CSV" | head -9 | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_fmt
            tp_fmt=$(format_throughput "$throughput")
            printf "| %-6s | %-6s | %9s | %8s | %8s | %8s | %10s%% | %10s%% |\n" \
                "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu" "$scpu"
        done

        echo ""

        # --- Local Cache Lookup ---
        echo "## Local Cache Lookup"
        echo ""
        echo "| Language | Throughput | CPU |"
        echo "|----------|-----------|-----|"

        grep "^lookup," "$INPUT_CSV" | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_fmt
            tp_fmt=$(format_throughput "$throughput")
            printf "| %-8s | %9s | %s%% |\n" "$client" "$tp_fmt" "$ccpu"
        done

        echo ""

        # --- Rate-limited results ---
        echo "## Rate-Limited Results"
        echo ""

        # UDS at 100k/s
        local has_rate_limited
        has_rate_limited=$(grep -c "uds-ping-pong," "$INPUT_CSV" 2>/dev/null || echo "0")
        if [ "$has_rate_limited" -gt 9 ]; then
            echo "### UDS Ping-Pong at 100k/s"
            echo ""
            echo "| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) |"
            echo "|--------|--------|-----------|----------|----------|----------|"

            grep "^uds-ping-pong," "$INPUT_CSV" | sed -n '10,18p' | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
                local tp_fmt
                tp_fmt=$(format_throughput "$throughput")
                printf "| %-6s | %-6s | %9s | %8s | %8s | %8s |\n" \
                    "$client" "$server" "$tp_fmt" "$p50" "$p95" "$p99"
            done

            echo ""
        fi

        # --- UDS Pipeline ---
        local has_pipeline
        has_pipeline=$(grep -c "^uds-pipeline-" "$INPUT_CSV" 2>/dev/null || echo "0")
        if [ "$has_pipeline" -gt 0 ]; then
            echo "## UDS Pipeline (C client, C server, max rate)"
            echo ""
            echo "| Depth | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU |"
            echo "|-------|-----------|----------|----------|----------|------------|"

            grep "^uds-pipeline-" "$INPUT_CSV" | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
                local depth="${scenario#uds-pipeline-d}"
                local tp_fmt
                tp_fmt=$(format_throughput "$throughput")
                printf "| %5s | %9s | %8s | %8s | %8s | %10s%% |\n" \
                    "$depth" "$tp_fmt" "$p50" "$p95" "$p99" "$ccpu"
            done

            echo ""
        fi

        # --- Performance Floor Summary ---
        echo "## Performance Floors"
        echo ""
        echo "| Metric | Floor | Status |"
        echo "|--------|-------|--------|"

        # Check each floor
        local shm_ok="PASS" uds_ok="PASS" lookup_ok="PASS"

        while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 1000000 ]; then
                shm_ok="FAIL"
            fi
        done < <(grep "^shm-ping-pong," "$INPUT_CSV" | head -9)

        while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 150000 ]; then
                uds_ok="FAIL"
            fi
        done < <(grep "^uds-ping-pong," "$INPUT_CSV" | head -9)

        while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
            local tp_int
            tp_int=$(printf "%.0f" "$throughput" 2>/dev/null || echo "0")
            if [ "$tp_int" -gt 0 ] && [ "$tp_int" -lt 10000000 ]; then
                lookup_ok="FAIL"
            fi
        done < <(grep "^lookup," "$INPUT_CSV")

        echo "| SHM ping-pong max | >= 1M req/s | $shm_ok |"
        echo "| UDS ping-pong max | >= 150k req/s | $uds_ok |"
        echo "| Local cache lookup | >= 10M lookups/s | $lookup_ok |"

        echo ""

    } > "$tmp_file"

    # Atomic rename
    mv "$tmp_file" "$OUTPUT_MD"

    log "Generated: $OUTPUT_MD"
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

validate_csv
check_floors || true
generate_md
