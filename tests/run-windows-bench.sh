#!/usr/bin/env bash
#
# run-windows-bench.sh - Run the full Windows benchmark matrix.
#
# Runs C/Rust/Go client-server pairs for (Rust optional):
#   1. Named Pipe ping-pong (N pairs x 4 rates)
#   2. Win SHM ping-pong (N pairs x 4 rates)
#   3. Snapshot Named Pipe refresh (N pairs x 2 rates)
#   4. Snapshot Win SHM refresh (N pairs x 2 rates)
#   5. NP batch ping-pong (N pairs x 4 rates, random 2-1000 items)
#   6. Win SHM batch ping-pong (N pairs x 4 rates, random 2-1000 items)
#   7. Local cache lookup (C, Rust, Go)
#   8. NP pipeline (N pairs x 1 rate, depth=16)
#   9. NP pipeline+batch (N pairs x 1 rate, depth=16)
#
# N = 9 pairs (3x3) if Rust bench binary available, else 4 pairs (2x2)
#
# Output: CSV file + human-readable summary.
# CSV columns:
#   scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,
#   client_cpu_pct,server_cpu_pct,total_cpu_pct
#
# Usage:
#   ./tests/run-windows-bench.sh [output_csv] [duration_sec]
#
# Must be run from MSYS2/Git Bash or PowerShell on Windows.

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

OUTPUT_CSV="${1:-${ROOT_DIR}/benchmarks-windows.csv}"
DURATION="${2:-5}"
RUN_DIR="${TEMP:-/tmp}/netipc-bench-$$"
POWERSHELL_EXE="/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
BLOCK_FIRST="${NIPC_BENCH_FIRST_BLOCK:-1}"
BLOCK_LAST="${NIPC_BENCH_LAST_BLOCK:-9}"

# Binary locations
BENCH_C="${BUILD_DIR}/bin/bench_windows_c.exe"
BENCH_RS="${ROOT_DIR}/src/crates/netipc/target/release/bench_windows.exe"
BENCH_GO="${BUILD_DIR}/bin/bench_windows_go.exe"

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------

cleanup() {
    if [ "${NIPC_KEEP_RUN_DIR:-0}" = "1" ]; then
        warn "Preserving RUN_DIR: $RUN_DIR"
    else
        rm -rf "$RUN_DIR"
    fi
    for pid in "${SERVER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

SERVER_PIDS=()
RUN_FAILED=0

log() {
    printf "${CYAN}[bench]${NC} %s\n" "$*" >&2
}

warn() {
    printf "${YELLOW}[warn]${NC} %s\n" "$*" >&2
}

err() {
    printf "${RED}[error]${NC} %s\n" "$*" >&2
}

bench_bin() {
    local lang="$1"
    case "$lang" in
        c)    echo "$BENCH_C" ;;
        rust) echo "$BENCH_RS" ;;
        go)   echo "$BENCH_GO" ;;
        *)    err "unknown lang: $lang"; return 1 ;;
    esac
}

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

    # Wait for READY (up to 10s - Windows pipes are slower to initialize)
    local waited=0
    while [ $waited -lt 100 ]; do
        if grep -q "^READY$" "$server_out" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
        if ! kill -0 "$pid" 2>/dev/null; then
            err "Server $lang ($subcmd) died before READY"
            cat "$server_out" >&2
            return 1
        fi
    done

    err "Server $lang ($subcmd) did not print READY within 10s"
    cat "$server_out" >&2
    kill "$pid" 2>/dev/null || true
    return 1
}

server_cpu_seconds() {
    local pid="$1"
    local winpid=""
    local cpu=""

    winpid=$(ps -W | awk -v p="$pid" '$1 == p { print $4; exit }' 2>/dev/null || true)

    if [ -x "$POWERSHELL_EXE" ]; then
        # The shell job PID is an MSYS PID. Resolve it once to a real Windows
        # PID and query that directly instead of scanning all processes by
        # command line via WMI.
        cpu=$("$POWERSHELL_EXE" -NoProfile -Command \
            "\$p = Get-Process -Id ${winpid:-0} -ErrorAction SilentlyContinue; \
             if (\$p) { [Console]::Out.Write('{0:F6}' -f \$p.TotalProcessorTime.TotalSeconds) }" \
            2>/dev/null | tr -d '\r')
    fi

    if [ -n "$cpu" ]; then
        echo "$cpu"
    else
        echo "0.0"
    fi
}

stop_server() {
    local pid="$1"
    local lang="$2"
    local svc="$3"
    local server_out="${RUN_DIR}/server-${lang}-${svc}.out"
    local server_cpu
    server_cpu=$(server_cpu_from_output "$server_out")

    if [ -z "$server_cpu" ]; then
        server_cpu=$(server_cpu_seconds "$pid")
    fi

    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true

    if [ -f "$server_out" ]; then
        local output_cpu
        output_cpu=$(server_cpu_from_output "$server_out")
        if [ -n "$output_cpu" ]; then
            server_cpu="$output_cpu"
        fi
    fi

    echo "${server_cpu:-0.0}"
}

write_csv_row() {
    local scenario="$1"
    local client="$2"
    local server="$3"
    local target_rps="$4"
    local throughput="$5"
    local p50="$6"
    local p95="$7"
    local p99="$8"
    local client_cpu="$9"
    local server_cpu_pct="${10}"
    local total_cpu_pct="${11}"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$scenario" "$client" "$server" "$target_rps" "$throughput" "$p50" "$p95" "$p99" \
        "$client_cpu" "$server_cpu_pct" "$total_cpu_pct" >> "$OUTPUT_CSV"
}

throughput_is_positive() {
    awk -v value="$1" 'BEGIN { exit ((value + 0) > 0 ? 0 : 1) }'
}

run_block() {
    local idx="$1"
    [ "$idx" -ge "$BLOCK_FIRST" ] && [ "$idx" -le "$BLOCK_LAST" ]
}

server_cpu_from_output() {
    local server_out="$1"
    local cpu_line
    cpu_line=$(grep "^SERVER_CPU_SEC=" "$server_out" 2>/dev/null | tail -1 || true)
    if [ -n "$cpu_line" ]; then
        echo "${cpu_line#SERVER_CPU_SEC=}"
    fi
}

cpu_pct_for_duration() {
    awk -v cpu_sec="$1" -v duration_sec="$2" 'BEGIN {
        if ((duration_sec + 0) <= 0) {
            print "0.000"
        } else {
            printf "%.3f", ((cpu_sec + 0) / (duration_sec + 0)) * 100.0
        }
    }'
}

sum_cpu_pct() {
    awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", (a + 0) + (b + 0) }'
}

dump_client_error() {
    local err_file="$1"
    if [ -s "$err_file" ]; then
        cat "$err_file" >&2
    fi
}

dump_client_output() {
    local output="$1"
    if [ -n "$output" ]; then
        warn "  Raw client output follows:"
        while IFS= read -r line; do
            warn "    ${line}"
        done <<< "$output"
    fi
}

dump_server_output() {
    local server_out="$1"
    if [ -f "$server_out" ]; then
        warn "  Server output follows:"
        while IFS= read -r line; do
            warn "    ${line}"
        done < "$server_out"
    fi
}

dump_bench_processes() {
    if [ -x "$POWERSHELL_EXE" ]; then
        warn "  Live bench_windows processes:"
        "$POWERSHELL_EXE" -NoProfile -Command \
            "Get-CimInstance Win32_Process | Where-Object { \$_.Name -like 'bench_windows*' } | Select-Object ProcessId,Name,CommandLine | Format-Table -AutoSize" \
            2>/dev/null | tr -d '\r' >&2 || true
    fi
}

run_pair() {
    local scenario="$1"
    local server_lang="$2"
    local client_lang="$3"
    local target_rps="$4"
    local duration="$5"

    local server_subcmd client_subcmd

    case "$scenario" in
        np-ping-pong)
            server_subcmd="np-ping-pong-server"
            client_subcmd="np-ping-pong-client"
            ;;
        shm-ping-pong)
            server_subcmd="shm-ping-pong-server"
            client_subcmd="shm-ping-pong-client"
            ;;
        np-batch-ping-pong)
            server_subcmd="np-batch-ping-pong-server"
            client_subcmd="np-batch-ping-pong-client"
            ;;
        shm-batch-ping-pong)
            server_subcmd="shm-batch-ping-pong-server"
            client_subcmd="shm-batch-ping-pong-client"
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

    local svc_name="${scenario}-${server_lang}-${client_lang}-${target_rps}"

    local rps_label
    if [ "$target_rps" = "0" ]; then
        rps_label="max"
    else
        rps_label="${target_rps}/s"
    fi

    log "  ${scenario}: ${client_lang}->${server_lang} @ ${rps_label}"

    local server_duration="$duration"
    local server_out="${RUN_DIR}/server-${server_lang}-${svc_name}.out"

    local server_pid
    server_pid=$(start_server "$server_lang" "$server_subcmd" "$svc_name" "$server_duration") || return 1

    sleep 0.2

    local client_bin
    client_bin="$(bench_bin "$client_lang")"

    local client_timeout=$((duration + 15))
    local client_output
    local client_status
    local client_err="${RUN_DIR}/client-${scenario}-${server_lang}-${client_lang}-${target_rps}.err"
    set +e
    client_output=$(timeout "$client_timeout" "$client_bin" "$client_subcmd" "$RUN_DIR" "$svc_name" "$duration" "$target_rps" 2>"$client_err")
    client_status=$?
    set -e

    if [ "$client_status" -ne 0 ]; then
        warn "  ${client_lang} client failed for ${scenario} (exit ${client_status})"
        dump_client_error "$client_err"
        dump_client_output "$client_output"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        local server_cpu_sec
        server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")
        local new_pids=()
        for p in "${SERVER_PIDS[@]:-}"; do
            [ "$p" != "$server_pid" ] && new_pids+=("$p")
        done
        SERVER_PIDS=("${new_pids[@]:-}")
        return 1
    fi

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} client for ${scenario}"
        dump_client_error "$client_err"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        local server_cpu_sec
        server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")
        local new_pids=()
        for p in "${SERVER_PIDS[@]:-}"; do
            [ "$p" != "$server_pid" ] && new_pids+=("$p")
        done
        SERVER_PIDS=("${new_pids[@]:-}")
        return 1
    fi

    local line
    line=$(echo "$client_output" | grep "^${scenario}," | head -1)

    if [ -z "$line" ]; then
        warn "  Could not parse output from ${client_lang} client"
        dump_client_error "$client_err"
        dump_client_output "$client_output"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        local server_cpu_sec
        server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")
        local new_pids=()
        for p in "${SERVER_PIDS[@]:-}"; do
            [ "$p" != "$server_pid" ] && new_pids+=("$p")
        done
        SERVER_PIDS=("${new_pids[@]:-}")
        return 1
    fi

    local throughput p50 p95 p99 client_cpu
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)

    if ! throughput_is_positive "$throughput"; then
        warn "  Invalid zero throughput from ${client_lang} client for ${scenario}"
        dump_client_error "$client_err"
        dump_client_output "$client_output"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        local server_cpu_sec
        server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")
        local new_pids=()
        for p in "${SERVER_PIDS[@]:-}"; do
            [ "$p" != "$server_pid" ] && new_pids+=("$p")
        done
        SERVER_PIDS=("${new_pids[@]:-}")
        return 1
    fi

    local server_cpu_sec
    server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")

    local new_pids=()
    for p in "${SERVER_PIDS[@]:-}"; do
        [ "$p" != "$server_pid" ] && new_pids+=("$p")
    done
    SERVER_PIDS=("${new_pids[@]:-}")

    local server_cpu_pct total_cpu_pct
    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$duration")
    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

    write_csv_row "$scenario" "$client_lang" "$server_lang" "$target_rps" \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"

    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
}

# ---------------------------------------------------------------------------
#  Check prerequisites
# ---------------------------------------------------------------------------

HAS_RUST=0
if [ -x "$BENCH_RS" ]; then
    HAS_RUST=1
fi

check_binaries() {
    local ok=0

    if [ ! -x "$BENCH_C" ]; then
        err "C benchmark binary not found: $BENCH_C"
        err "Build with: cmake --build build --target bench_windows_c"
        ok=1
    fi

    if [ ! -x "$BENCH_GO" ]; then
        err "Go benchmark binary not found: $BENCH_GO"
        err "Build with: cmake --build build --target bench_windows_go"
        ok=1
    fi

    if [ $HAS_RUST -eq 0 ]; then
        warn "Rust benchmark binary not found: $BENCH_RS (Rust tests will be skipped)"
    fi

    return $ok
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

main() {
    log "Windows Benchmark Suite"
    log "Duration per run: ${DURATION}s"
    log "Output: ${OUTPUT_CSV}"

    if ! check_binaries; then
        err "Missing benchmark binaries. Build first."
        exit 1
    fi

    mkdir -p "$RUN_DIR"

    # CSV header
    echo "scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$OUTPUT_CSV"

    local LANGS=(c go)
    if [ $HAS_RUST -eq 1 ]; then
        LANGS=(c rust go)
    fi
    local RATES_PING_PONG=(0 100000 10000 1000)
    local RATES_SNAPSHOT=(0 1000)
    local PIPELINE_DEPTH=16

    # 1. Named Pipe ping-pong: N pairs x 4 rates
    if run_block 1; then
        log "=== Named Pipe Ping-Pong ==="
        for rate in "${RATES_PING_PONG[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "np-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 2. Win SHM ping-pong: 4 pairs x 4 rates
    if run_block 2; then
        log "=== Win SHM Ping-Pong ==="
        for rate in "${RATES_PING_PONG[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "shm-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 3. Snapshot Named Pipe refresh: 4 pairs x 2 rates
    if run_block 3; then
        log "=== Snapshot Named Pipe ==="
        for rate in "${RATES_SNAPSHOT[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "snapshot-baseline" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 4. Snapshot Win SHM refresh: 4 pairs x 2 rates
    if run_block 4; then
        log "=== Snapshot Win SHM ==="
        for rate in "${RATES_SNAPSHOT[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "snapshot-shm" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 5. NP batch ping-pong: N pairs x 4 rates
    if run_block 5; then
        log "=== NP Batch Ping-Pong ==="
        for rate in "${RATES_PING_PONG[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "np-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 6. Win SHM batch ping-pong: N pairs x 4 rates
    if run_block 6; then
        log "=== Win SHM Batch Ping-Pong ==="
        for rate in "${RATES_PING_PONG[@]}"; do
            for server_lang in "${LANGS[@]}"; do
                for client_lang in "${LANGS[@]}"; do
                    if ! run_pair "shm-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                        RUN_FAILED=1
                    fi
                    sleep 0.5
                done
            done
        done
    fi

    # 7. Local cache lookup
    if run_block 7; then
        log "=== Local Cache Lookup ==="
        for lang in "${LANGS[@]}"; do
            local bin
            bin="$(bench_bin "$lang")"
            log "  lookup: ${lang}"
            local line
            local lookup_status
            local lookup_err="${RUN_DIR}/lookup-${lang}.err"
            set +e
            line=$("$bin" lookup-bench "$DURATION" 2>"$lookup_err" | grep "^lookup," | head -1)
            lookup_status=$?
            set -e
            if [ "$lookup_status" -ne 0 ]; then
                warn "  ${lang} lookup benchmark failed"
                dump_client_error "$lookup_err"
                RUN_FAILED=1
                continue
            fi
            if [ -n "$line" ]; then
                local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
                throughput=$(echo "$line" | cut -d',' -f4)
                p50=$(echo "$line" | cut -d',' -f5)
                p95=$(echo "$line" | cut -d',' -f6)
                p99=$(echo "$line" | cut -d',' -f7)
                client_cpu=$(echo "$line" | cut -d',' -f8)
                server_cpu_pct=$(echo "$line" | cut -d',' -f9)
                total_cpu_pct=$(echo "$line" | cut -d',' -f10)
                if ! throughput_is_positive "$throughput"; then
                    warn "  Invalid zero throughput from ${lang} lookup benchmark"
                    dump_client_error "$lookup_err"
                    RUN_FAILED=1
                    continue
                fi
                write_csv_row "lookup" "$lang" "$lang" "0" \
                    "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
            else
                warn "  No output from ${lang} lookup benchmark"
                dump_client_error "$lookup_err"
                RUN_FAILED=1
            fi
        done
    fi

    # 8. NP pipeline: N pairs x 1 rate (max), depth=16
    if run_block 8; then
        log "=== NP Pipeline (depth=${PIPELINE_DEPTH}) ==="
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
            local pipe_svc="pipeline-${server_lang}-${client_lang}"

            log "  np-pipeline: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration="$DURATION"
            local server_pid
            server_pid=$(start_server "$server_lang" "np-ping-pong-server" "$pipe_svc" "$server_duration") || {
                warn "  Failed to start pipeline server"
                continue
            }

            sleep 0.2

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((DURATION + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-np-pipeline-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "np-pipeline-client" "$RUN_DIR" "$pipe_svc" "$DURATION" "0" "$PIPELINE_DEPTH" 2>"$client_err")
            client_status=$?
            set -e

            local server_cpu_sec
            server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pipe_svc")

            local new_pids=()
            for p in "${SERVER_PIDS[@]:-}"; do
                [ "$p" != "$server_pid" ] && new_pids+=("$p")
            done
            SERVER_PIDS=("${new_pids[@]:-}")

            if [ "$client_status" -ne 0 ]; then
                warn "  ${client_lang} pipeline client failed for ${server_lang} server (exit ${client_status})"
                dump_client_error "$client_err"
                RUN_FAILED=1
            elif [ -n "$client_output" ]; then
                local line
                line=$(echo "$client_output" | grep "^np-pipeline" | head -1)
                if [ -n "$line" ]; then
                    local throughput p50 p95 p99 client_cpu
                    throughput=$(echo "$line" | cut -d',' -f4)
                    p50=$(echo "$line" | cut -d',' -f5)
                    p95=$(echo "$line" | cut -d',' -f6)
                    p99=$(echo "$line" | cut -d',' -f7)
                    client_cpu=$(echo "$line" | cut -d',' -f8)

                    if ! throughput_is_positive "$throughput"; then
                        warn "  Invalid zero throughput from ${client_lang} pipeline client for ${server_lang} server"
                        warn "  Raw pipeline line: ${line:-<none>}"
                        warn "  Raw client output follows:"
                        printf '%s\n' "$client_output" >&2
                        warn "  Preserving RUN_DIR for investigation: ${RUN_DIR}"
                        export NIPC_KEEP_RUN_DIR=1
                        dump_client_error "$client_err"
                        RUN_FAILED=1
                        sleep 0.5
                        continue
                    fi

                    local server_cpu_pct total_cpu_pct
                    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$DURATION")
                    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

                    write_csv_row "np-pipeline-d${PIPELINE_DEPTH}" "$client_lang" "$server_lang" "0" \
                        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
                    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
                else
                    warn "  Could not parse pipeline output from ${client_lang} client for ${server_lang} server"
                    warn "  Raw client output follows:"
                    printf '%s\n' "$client_output" >&2
                    warn "  Preserving RUN_DIR for investigation: ${RUN_DIR}"
                    export NIPC_KEEP_RUN_DIR=1
                    dump_client_error "$client_err"
                    RUN_FAILED=1
                fi
            else
                warn "  No output from ${client_lang} pipeline client for ${server_lang} server"
                warn "  Preserving RUN_DIR for investigation: ${RUN_DIR}"
                export NIPC_KEEP_RUN_DIR=1
                dump_client_error "$client_err"
                RUN_FAILED=1
            fi

            sleep 0.5
            done
        done
    fi

    # 9. NP pipeline+batch: N pairs x 1 rate (max), depth=16
    if run_block 9; then
        log "=== NP Pipeline+Batch (depth=${PIPELINE_DEPTH}) ==="
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
            local pb_svc="pipe-batch-${server_lang}-${client_lang}"

            log "  np-pipeline-batch: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration="$DURATION"
            local server_pid
            server_pid=$(start_server "$server_lang" "np-batch-ping-pong-server" "$pb_svc" "$server_duration") || {
                warn "  Failed to start pipeline-batch server"
                continue
            }

            sleep 0.2

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((DURATION + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-np-pipeline-batch-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "np-pipeline-batch-client" "$RUN_DIR" "$pb_svc" "$DURATION" "0" "$PIPELINE_DEPTH" 2>"$client_err")
            client_status=$?
            set -e

            local server_cpu_sec
            server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pb_svc")

            local new_pids=()
            for p in "${SERVER_PIDS[@]:-}"; do
                [ "$p" != "$server_pid" ] && new_pids+=("$p")
            done
            SERVER_PIDS=("${new_pids[@]:-}")

            if [ "$client_status" -ne 0 ]; then
                warn "  ${client_lang} pipeline-batch client failed for ${server_lang} server (exit ${client_status})"
                dump_client_error "$client_err"
                RUN_FAILED=1
            elif [ -n "$client_output" ]; then
                local line
                line=$(echo "$client_output" | grep "^np-pipeline-batch" | head -1)
                if [ -n "$line" ]; then
                    local throughput p50 p95 p99 client_cpu
                    throughput=$(echo "$line" | cut -d',' -f4)
                    p50=$(echo "$line" | cut -d',' -f5)
                    p95=$(echo "$line" | cut -d',' -f6)
                    p99=$(echo "$line" | cut -d',' -f7)
                    client_cpu=$(echo "$line" | cut -d',' -f8)

                    if ! throughput_is_positive "$throughput"; then
                        warn "  Invalid zero throughput from ${client_lang} pipeline-batch client for ${server_lang} server"
                        dump_client_error "$client_err"
                        RUN_FAILED=1
                        sleep 0.5
                        continue
                    fi

                    local server_cpu_pct total_cpu_pct
                    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$DURATION")
                    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

                    write_csv_row "np-pipeline-batch-d${PIPELINE_DEPTH}" "$client_lang" "$server_lang" "0" \
                        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
                    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
                else
                    warn "  Could not parse pipeline-batch output from ${client_lang} client for ${server_lang} server"
                    dump_client_error "$client_err"
                    RUN_FAILED=1
                fi
            else
                warn "  No output from ${client_lang} pipeline-batch client for ${server_lang} server"
                dump_client_error "$client_err"
                RUN_FAILED=1
            fi

            sleep 0.5
            done
        done
    fi

    if [ "$RUN_FAILED" -ne 0 ]; then
        err "One or more Windows benchmark scenarios failed; CSV is incomplete or invalid"
        return 1
    fi

    # Summary
    log ""
    log "=== Results ==="
    log "CSV: ${OUTPUT_CSV}"

    local total_lines
    total_lines=$(wc -l < "$OUTPUT_CSV")
    log "Total measurements: $((total_lines - 1))"

    printf "\n"
    printf "${CYAN}%-25s %-8s %-8s %-10s %12s %8s %8s %8s${NC}\n" \
        "Scenario" "Client" "Server" "Target RPS" "Throughput" "p50(us)" "p95(us)" "p99(us)"
    printf -- "-------- -------- -------- ---------- ------------ -------- -------- --------\n"

    tail -n +2 "$OUTPUT_CSV" | while IFS=',' read -r scenario client server target_rps throughput p50 p95 p99 ccpu scpu tcpu; do
        printf "%-25s %-8s %-8s %-10s %12s %8s %8s %8s\n" \
            "$scenario" "$client" "$server" "$target_rps" "$throughput" "$p50" "$p95" "$p99"
    done

    printf "\n"
    log "Done. Run tests/generate-benchmarks-windows.sh to generate the markdown report."
}

main "$@"
