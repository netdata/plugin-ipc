#!/usr/bin/env bash
#
# run-posix-bench.sh - Run the full POSIX benchmark matrix.
#
# Runs all C/Rust/Go client-server pairs for:
#   1. UDS ping-pong (9 pairs x 4 rates)
#   2. SHM ping-pong (9 pairs x 4 rates)
#   3. Snapshot baseline refresh (9 pairs x 2 rates)
#   4. Snapshot SHM refresh (9 pairs x 2 rates)
#   5. Local cache lookup (3 languages x 1 rate)
#   6. Negotiated profile comparison (Rust->Rust x 4 rates)
#
# Output: CSV file + human-readable summary.
#
# Usage:
#   ./tests/run-posix-bench.sh [output_csv] [duration_sec]

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

OUTPUT_CSV="${1:-${ROOT_DIR}/benchmarks-posix.csv}"
DURATION="${2:-5}"
RUN_DIR="/tmp/netipc-bench-$$"

# Binary locations
BENCH_C="${BUILD_DIR}/bin/bench_posix_c"
BENCH_RS="${ROOT_DIR}/src/crates/netipc/target/release/bench_posix"
BENCH_GO="${BUILD_DIR}/bin/bench_posix_go"

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------

cleanup() {
    rm -rf "$RUN_DIR"
    # Kill any leftover server processes we started
    for pid in "${SERVER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

SERVER_PIDS=()

log() {
    printf "${CYAN}[bench]${NC} %s\n" "$*" >&2
}

warn() {
    printf "${YELLOW}[warn]${NC} %s\n" "$*" >&2
}

err() {
    printf "${RED}[error]${NC} %s\n" "$*" >&2
}

# Get the binary for a language
bench_bin() {
    local lang="$1"
    case "$lang" in
        c)    echo "$BENCH_C" ;;
        rust) echo "$BENCH_RS" ;;
        go)   echo "$BENCH_GO" ;;
        *)    err "unknown lang: $lang"; return 1 ;;
    esac
}

# Start a server, wait for READY, return PID
start_server() {
    local lang="$1"
    local subcmd="$2"
    local svc="$3"
    local duration_arg="$4"

    local bin
    bin="$(bench_bin "$lang")"

    local server_out="${RUN_DIR}/server-${lang}-${svc}.out"

    "$bin" "$subcmd" "$RUN_DIR" "$svc" "$duration_arg" > "$server_out" 2>&1 &
    local pid=$!
    SERVER_PIDS+=("$pid")

    # Wait for READY (up to 5s)
    local waited=0
    while [ $waited -lt 50 ]; do
        if grep -q "^READY$" "$server_out" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
        # Check server is still alive
        if ! kill -0 "$pid" 2>/dev/null; then
            err "Server $lang ($subcmd) died before READY"
            cat "$server_out" >&2
            return 1
        fi
    done

    err "Server $lang ($subcmd) did not print READY within 5s"
    cat "$server_out" >&2
    kill "$pid" 2>/dev/null || true
    return 1
}

# Stop a server, extract server CPU
stop_server() {
    local pid="$1"
    local lang="$2"
    local svc="$3"

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    local server_out="${RUN_DIR}/server-${lang}-${svc}.out"
    local server_cpu="0.0"

    if [ -f "$server_out" ]; then
        local cpu_line
        cpu_line=$(grep "^SERVER_CPU_SEC=" "$server_out" 2>/dev/null || true)
        if [ -n "$cpu_line" ]; then
            server_cpu="${cpu_line#SERVER_CPU_SEC=}"
        fi
    fi

    echo "$server_cpu"
}

# Run a single benchmark pair
run_pair() {
    local scenario="$1"       # e.g., uds-ping-pong, shm-ping-pong, snapshot-baseline, snapshot-shm
    local server_lang="$2"    # c, rust, go
    local client_lang="$3"    # c, rust, go
    local target_rps="$4"     # 0 = max
    local duration="$5"

    local server_subcmd client_subcmd svc_name

    case "$scenario" in
        uds-ping-pong)
            server_subcmd="uds-ping-pong-server"
            client_subcmd="uds-ping-pong-client"
            ;;
        shm-ping-pong)
            server_subcmd="shm-ping-pong-server"
            client_subcmd="shm-ping-pong-client"
            ;;
        snapshot-baseline)
            server_subcmd="snapshot-server"
            client_subcmd="snapshot-client"
            ;;
        snapshot-shm)
            server_subcmd="snapshot-shm-server"
            client_subcmd="snapshot-shm-client"
            ;;
        *)
            err "unknown scenario: $scenario"
            return 1
            ;;
    esac

    # Unique service name per pair
    svc_name="${scenario}-${server_lang}-${client_lang}-${target_rps}"

    local rps_label
    if [ "$target_rps" = "0" ]; then
        rps_label="max"
    else
        rps_label="${target_rps}/s"
    fi

    log "  ${scenario}: ${client_lang}->${server_lang} @ ${rps_label}"

    # Server gets extra time beyond the client duration
    local server_duration=$((duration + 5))

    local server_pid
    server_pid=$(start_server "$server_lang" "$server_subcmd" "$svc_name" "$server_duration") || return 1

    # Delay to ensure the server has called accept() and created SHM
    # (if SHM profile). Without this, the client may attach before the
    # server creates the SHM region, causing a UDS/SHM mismatch deadlock.
    sleep 0.5

    # Run client with a safety timeout (duration + 15s for SHM attach retries)
    local client_bin
    client_bin="$(bench_bin "$client_lang")"

    local client_timeout=$((duration + 15))
    local client_output
    client_output=$(timeout "$client_timeout" "$client_bin" "$client_subcmd" "$RUN_DIR" "$svc_name" "$duration" "$target_rps" 2>/dev/null) || true

    # Stop server and get CPU
    local server_cpu_sec
    server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")

    # Remove the PID from our tracking array
    local new_pids=()
    for p in "${SERVER_PIDS[@]:-}"; do
        [ "$p" != "$server_pid" ] && new_pids+=("$p")
    done
    SERVER_PIDS=("${new_pids[@]:-}")

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} client for ${scenario}"
        echo "${scenario},${client_lang},${server_lang},0,0,0,0,0.0,0.0,0.0" >> "$OUTPUT_CSV"
        return 0
    fi

    # Parse client output and patch in server CPU
    # Client outputs: scenario,client,server,throughput,p50,p95,p99,client_cpu,0.0,total_cpu
    # We replace server=lang with actual server_lang, and fill in server_cpu
    local line
    line=$(echo "$client_output" | grep "^${scenario}," | head -1)

    if [ -z "$line" ]; then
        warn "  Could not parse output from ${client_lang} client"
        echo "${scenario},${client_lang},${server_lang},0,0,0,0,0.0,0.0,0.0" >> "$OUTPUT_CSV"
        return 0
    fi

    # Reconstruct with correct server_lang and server CPU
    local throughput p50 p95 p99 client_cpu
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)

    # Compute server CPU % and total CPU %
    local server_cpu_pct total_cpu_pct
    if command -v bc >/dev/null 2>&1; then
        server_cpu_pct=$(echo "scale=1; ${server_cpu_sec} / ${duration} * 100" | bc 2>/dev/null || echo "0.0")
        total_cpu_pct=$(echo "scale=1; ${client_cpu} + ${server_cpu_pct}" | bc 2>/dev/null || echo "0.0")
    else
        server_cpu_pct="0.0"
        total_cpu_pct="$client_cpu"
    fi

    echo "${scenario},${client_lang},${server_lang},${throughput},${p50},${p95},${p99},${client_cpu},${server_cpu_pct},${total_cpu_pct}" >> "$OUTPUT_CSV"

    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
}

# ---------------------------------------------------------------------------
#  Check prerequisites
# ---------------------------------------------------------------------------

check_binaries() {
    local ok=0

    if [ ! -x "$BENCH_C" ]; then
        err "C benchmark binary not found: $BENCH_C"
        err "Build with: cmake --build build --target bench_posix_c"
        ok=1
    fi

    if [ ! -x "$BENCH_RS" ]; then
        err "Rust benchmark binary not found: $BENCH_RS"
        err "Build with: cd src/crates/netipc && cargo build --release --bin bench_posix"
        ok=1
    fi

    if [ ! -x "$BENCH_GO" ]; then
        err "Go benchmark binary not found: $BENCH_GO"
        err "Build with: cmake --build build --target bench_posix_go"
        ok=1
    fi

    return $ok
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

main() {
    log "POSIX Benchmark Suite"
    log "Duration per run: ${DURATION}s"
    log "Output: ${OUTPUT_CSV}"

    if ! check_binaries; then
        err "Missing benchmark binaries. Build first."
        exit 1
    fi

    mkdir -p "$RUN_DIR"

    # CSV header
    echo "scenario,client,server,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$OUTPUT_CSV"

    local LANGS=(c rust go)
    local RATES_PING_PONG=(0 100000 10000 1000)
    local RATES_SNAPSHOT=(0 1000)

    # 1. UDS ping-pong: 9 pairs x 4 rates
    log "=== UDS Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_pair "uds-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION" || true
                sleep 0.5
            done
        done
    done

    # 2. SHM ping-pong: 9 pairs x 4 rates
    log "=== SHM Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_pair "shm-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION" || true
                sleep 0.5
            done
        done
    done

    # 3. Snapshot baseline refresh: 9 pairs x 2 rates
    log "=== Snapshot Baseline ==="
    for rate in "${RATES_SNAPSHOT[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_pair "snapshot-baseline" "$server_lang" "$client_lang" "$rate" "$DURATION" || true
                sleep 0.5
            done
        done
    done

    # 4. Snapshot SHM refresh: 9 pairs x 2 rates
    log "=== Snapshot SHM ==="
    for rate in "${RATES_SNAPSHOT[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_pair "snapshot-shm" "$server_lang" "$client_lang" "$rate" "$DURATION" || true
                sleep 0.5
            done
        done
    done

    # 5. Local cache lookup: 3 languages
    log "=== Local Cache Lookup ==="
    for lang in "${LANGS[@]}"; do
        local bin
        bin="$(bench_bin "$lang")"
        log "  lookup: ${lang}"
        "$bin" lookup-bench "$DURATION" >> "$OUTPUT_CSV" 2>/dev/null || true
    done

    # 6. UDS pipeline benchmarks (C only, various depths)
    log "=== UDS Pipeline (C client, C server, max rate) ==="
    local PIPELINE_DEPTHS=(1 4 8 16 32)
    for depth in "${PIPELINE_DEPTHS[@]}"; do
        local pipe_svc="pipeline-d${depth}"

        log "  uds-pipeline: depth=${depth}"

        # Use the same ping-pong server (it already echoes)
        local server_duration=$((DURATION + 5))
        local server_pid
        server_pid=$(start_server "c" "uds-ping-pong-server" "$pipe_svc" "$server_duration") || {
            warn "  Failed to start server for pipeline depth=$depth"
            continue
        }

        sleep 0.5

        local client_timeout=$((DURATION + 15))
        local client_output
        client_output=$(timeout "$client_timeout" "$BENCH_C" "uds-pipeline-client" "$RUN_DIR" "$pipe_svc" "$DURATION" "0" "$depth" 2>/dev/null) || true

        local server_cpu_sec
        server_cpu_sec=$(stop_server "$server_pid" "c" "$pipe_svc")

        # Remove PID from tracking
        local new_pids=()
        for p in "${SERVER_PIDS[@]:-}"; do
            [ "$p" != "$server_pid" ] && new_pids+=("$p")
        done
        SERVER_PIDS=("${new_pids[@]:-}")

        if [ -n "$client_output" ]; then
            local line
            line=$(echo "$client_output" | grep "^uds-pipeline" | head -1)
            if [ -n "$line" ]; then
                # Extract and patch server CPU
                local throughput p50 p95 p99 client_cpu
                throughput=$(echo "$line" | cut -d',' -f4)
                p50=$(echo "$line" | cut -d',' -f5)
                p95=$(echo "$line" | cut -d',' -f6)
                p99=$(echo "$line" | cut -d',' -f7)
                client_cpu=$(echo "$line" | cut -d',' -f8)

                local server_cpu_pct total_cpu_pct
                if command -v bc >/dev/null 2>&1; then
                    server_cpu_pct=$(echo "scale=1; ${server_cpu_sec} / ${DURATION} * 100" | bc 2>/dev/null || echo "0.0")
                    total_cpu_pct=$(echo "scale=1; ${client_cpu} + ${server_cpu_pct}" | bc 2>/dev/null || echo "0.0")
                else
                    server_cpu_pct="0.0"
                    total_cpu_pct="$client_cpu"
                fi

                echo "uds-pipeline-d${depth},c,c,${throughput},${p50},${p95},${p99},${client_cpu},${server_cpu_pct},${total_cpu_pct}" >> "$OUTPUT_CSV"
                log "    depth=${depth} throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
            else
                echo "uds-pipeline-d${depth},c,c,0,0,0,0,0.0,0.0,0.0" >> "$OUTPUT_CSV"
            fi
        else
            echo "uds-pipeline-d${depth},c,c,0,0,0,0,0.0,0.0,0.0" >> "$OUTPUT_CSV"
        fi

        sleep 0.5
    done

    # 7. Negotiated profile comparison (Rust->Rust)
    log "=== Negotiated Profile (Rust->Rust: UDS vs SHM) ==="
    # UDS already measured above; add labeled entries for clarity
    for rate in "${RATES_PING_PONG[@]}"; do
        run_pair "uds-ping-pong" "rust" "rust" "$rate" "$DURATION" || true
        sleep 0.5
        run_pair "shm-ping-pong" "rust" "rust" "$rate" "$DURATION" || true
        sleep 0.5
    done

    # Summary
    log ""
    log "=== Results ==="
    log "CSV: ${OUTPUT_CSV}"

    local total_lines
    total_lines=$(wc -l < "$OUTPUT_CSV")
    log "Total measurements: $((total_lines - 1))"

    # Print summary table
    printf "\n"
    printf "${CYAN}%-25s %-8s %-8s %12s %8s %8s %8s${NC}\n" \
        "Scenario" "Client" "Server" "Throughput" "p50(us)" "p95(us)" "p99(us)"
    printf -- "-------- -------- -------- ------------ -------- -------- --------\n"

    tail -n +2 "$OUTPUT_CSV" | while IFS=',' read -r scenario client server throughput p50 p95 p99 ccpu scpu tcpu; do
        # Only show max throughput (rate=0) results in summary
        printf "%-25s %-8s %-8s %12s %8s %8s %8s\n" \
            "$scenario" "$client" "$server" "$throughput" "$p50" "$p95" "$p99"
    done

    printf "\n"
    log "Done. Run tests/generate-benchmarks-posix.sh to generate the markdown report."
}

main "$@"
