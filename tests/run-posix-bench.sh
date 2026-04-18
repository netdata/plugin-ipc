#!/usr/bin/env bash
#
# run-posix-bench.sh - Run the full POSIX benchmark matrix.
#
# Runs all C/Rust/Go client-server pairs for:
#   1. UDS ping-pong (9 pairs x 4 rates)
#   2. SHM ping-pong (9 pairs x 4 rates)
#   3. Snapshot baseline refresh (9 pairs x 2 rates)
#   4. Snapshot SHM refresh (9 pairs x 2 rates)
#   5. UDS batch ping-pong (9 pairs x 4 rates, random 2-1000 items)
#   6. SHM batch ping-pong (9 pairs x 4 rates, random 2-1000 items)
#   7. Local cache lookup (3 languages x 1 rate)
#   8. UDS pipeline (9 pairs x 1 rate, depth=16)
#   9. UDS pipeline+batch (9 pairs x 1 rate, depth=16)
#
# Output: CSV file + human-readable summary.
# CSV columns:
#   scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,
#   client_cpu_pct,server_cpu_pct,total_cpu_pct
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
BUILD_DIR="${NIPC_BENCH_BUILD_DIR:-${ROOT_DIR}/build-bench-posix}"
BENCH_BUILD_TYPE="${NIPC_BENCH_BUILD_TYPE:-Release}"
SERVER_STOP_GRACE_SEC="${NIPC_BENCH_SERVER_STOP_GRACE_SEC:-10}"

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

run() {
    printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"
    "$@"
}

build_jobs() {
    if command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4
    elif command -v nproc >/dev/null 2>&1; then
        nproc 2>/dev/null || echo 4
    else
        echo 4
    fi
}

ensure_bench_build() {
    local cache="${BUILD_DIR}/CMakeCache.txt"
    local current_type=""

    if [ -f "$cache" ]; then
        current_type=$(awk -F= '/^CMAKE_BUILD_TYPE:STRING=/{print $2; exit}' "$cache")
    fi

    if [ ! -f "$cache" ] || [ "$current_type" != "$BENCH_BUILD_TYPE" ]; then
        log "Configuring benchmark build dir: ${BUILD_DIR} (${BENCH_BUILD_TYPE})"
        run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BENCH_BUILD_TYPE"
    fi

    log "Building benchmark binaries in ${BUILD_DIR}"
    run cmake --build "$BUILD_DIR" --target bench_posix_c bench_posix_go -j"$(build_jobs)"
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

    local server_out="${RUN_DIR}/server-${lang}-${svc}.out"
    local server_cpu=""
    local waited=0
    local wait_ticks=$((SERVER_STOP_GRACE_SEC * 10))

    # Bench servers are not child jobs of the calling shell, so wait(1) cannot
    # be used here. Poll for natural exit first: Go and Rust print
    # SERVER_CPU_SEC only after their own timer-driven shutdown path.
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$wait_ticks" ]; do
        sleep 0.1
        waited=$((waited + 1))
    done

    if kill -0 "$pid" 2>/dev/null; then
        warn "  Server ${lang} (${svc}) did not stop itself within ${SERVER_STOP_GRACE_SEC}s; requesting shutdown"
        kill "$pid" 2>/dev/null || true
        local term_waited=0
        while kill -0 "$pid" 2>/dev/null && [ "$term_waited" -lt 30 ]; do
            sleep 0.1
            term_waited=$((term_waited + 1))
        done
    fi

    if kill -0 "$pid" 2>/dev/null; then
        warn "  Server ${lang} (${svc}) did not exit after SIGTERM; forcing kill"
        kill -9 "$pid" 2>/dev/null || true
        local forced_waited=0
        while kill -0 "$pid" 2>/dev/null && [ "$forced_waited" -lt 20 ]; do
            sleep 0.1
            forced_waited=$((forced_waited + 1))
        done
    fi

    if [ -f "$server_out" ]; then
        local cpu_line
        cpu_line=$(grep "^SERVER_CPU_SEC=" "$server_out" 2>/dev/null | tail -1 || true)
        if [ -n "$cpu_line" ]; then
            server_cpu="${cpu_line#SERVER_CPU_SEC=}"
        fi
    fi

    if [ -z "$server_cpu" ]; then
        warn "  Missing SERVER_CPU_SEC for ${lang} (${svc}); refusing to publish a fake 0%"
        if [ -f "$server_out" ]; then
            cat "$server_out" >&2
        fi
        return 1
    fi

    echo "$server_cpu"
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

dump_client_error() {
    local err_file="$1"
    if [ -s "$err_file" ]; then
        cat "$err_file" >&2
    fi
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
        uds-batch-ping-pong)
            server_subcmd="uds-batch-ping-pong-server"
            client_subcmd="uds-batch-ping-pong-client"
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
    local client_status
    local client_err="${RUN_DIR}/client-${scenario}-${server_lang}-${client_lang}-${target_rps}.err"
    set +e
    client_output=$(timeout "$client_timeout" "$client_bin" "$client_subcmd" "$RUN_DIR" "$svc_name" "$duration" "$target_rps" 2>"$client_err")
    client_status=$?
    set -e

    # Stop server and get CPU
    local server_cpu_sec
    if ! server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name"); then
        return 1
    fi

    # Remove the PID from our tracking array
    local new_pids=()
    for p in "${SERVER_PIDS[@]:-}"; do
        [ "$p" != "$server_pid" ] && new_pids+=("$p")
    done
    SERVER_PIDS=("${new_pids[@]:-}")

    if [ "$client_status" -ne 0 ]; then
        warn "  ${client_lang} client failed for ${scenario} (exit ${client_status})"
        dump_client_error "$client_err"
        return 1
    fi

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} client for ${scenario}"
        dump_client_error "$client_err"
        return 1
    fi

    # Parse client output and patch in server CPU
    # Client outputs: scenario,client,server,throughput,p50,p95,p99,client_cpu,0.0,total_cpu
    # We replace server=lang with actual server_lang, add target_rps, and fill in server_cpu
    local line
    line=$(echo "$client_output" | grep "^${scenario}," | head -1)

    if [ -z "$line" ]; then
        warn "  Could not parse output from ${client_lang} client"
        dump_client_error "$client_err"
        return 1
    fi

    # Reconstruct with correct server_lang and server CPU
    local throughput p50 p95 p99 client_cpu
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)

    if ! throughput_is_positive "$throughput"; then
        warn "  Invalid zero throughput from ${client_lang} client for ${scenario}"
        dump_client_error "$client_err"
        return 1
    fi

    # Compute server CPU % and total CPU %
    local server_cpu_pct total_cpu_pct
    if command -v bc >/dev/null 2>&1; then
        server_cpu_pct=$(echo "scale=1; ${server_cpu_sec} / ${duration} * 100" | bc 2>/dev/null || echo "0.0")
        total_cpu_pct=$(echo "scale=1; ${client_cpu} + ${server_cpu_pct}" | bc 2>/dev/null || echo "0.0")
    else
        server_cpu_pct="0.0"
        total_cpu_pct="$client_cpu"
    fi

    write_csv_row "$scenario" "$client_lang" "$server_lang" "$target_rps" \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"

    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
}

# ---------------------------------------------------------------------------
#  Check prerequisites
# ---------------------------------------------------------------------------

check_binaries() {
    local ok=0

    ensure_bench_build

    if [ ! -x "$BENCH_RS" ]; then
        err "Rust benchmark binary not found: $BENCH_RS"
        err "Build with: cd src/crates/netipc && cargo build --release --bin bench_posix"
        ok=1
    fi

    if [ ! -x "$BENCH_C" ]; then
        err "C benchmark binary not found after build: $BENCH_C"
        ok=1
    fi

    if [ ! -x "$BENCH_GO" ]; then
        err "Go benchmark binary not found after build: $BENCH_GO"
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
    echo "scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$OUTPUT_CSV"

    local LANGS=(c rust go)
    local RATES_PING_PONG=(0 100000 10000 1000)
    local RATES_SNAPSHOT=(0 1000)

    # 1. UDS ping-pong: 9 pairs x 4 rates
    log "=== UDS Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "uds-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 2. SHM ping-pong: 9 pairs x 4 rates
    log "=== SHM Ping-Pong ==="
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

    # 3. Snapshot baseline refresh: 9 pairs x 2 rates
    log "=== Snapshot Baseline ==="
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

    # 4. Snapshot SHM refresh: 9 pairs x 2 rates
    log "=== Snapshot SHM ==="
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

    # 5. UDS batch ping-pong: 9 pairs x 4 rates
    log "=== UDS Batch Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "uds-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 6. SHM batch ping-pong: 9 pairs x 4 rates
    log "=== SHM Batch Ping-Pong ==="
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

    # 7. Local cache lookup: 3 languages
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

    # 8. UDS pipeline benchmarks (all 9 pairs, max rate, depth=16)
    log "=== UDS Pipeline (9 pairs, max rate, depth=16) ==="
    local PIPELINE_DEPTH=16
    for server_lang in "${LANGS[@]}"; do
        for client_lang in "${LANGS[@]}"; do
            local pipe_svc="pipeline-${server_lang}-${client_lang}"

            log "  uds-pipeline: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration=$((DURATION + 5))
            local server_pid
            server_pid=$(start_server "$server_lang" "uds-ping-pong-server" "$pipe_svc" "$server_duration") || {
                warn "  Failed to start pipeline server"
                continue
            }

            sleep 0.5

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((DURATION + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-uds-pipeline-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "uds-pipeline-client" "$RUN_DIR" "$pipe_svc" "$DURATION" "0" "$PIPELINE_DEPTH" 2>"$client_err")
            client_status=$?
            set -e

            local server_cpu_sec
            if ! server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pipe_svc"); then
                warn "  Missing server CPU for ${client_lang}->${server_lang} pipeline benchmark"
                RUN_FAILED=1
                sleep 0.5
                continue
            fi

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
                line=$(echo "$client_output" | grep "^uds-pipeline" | head -1)
                if [ -n "$line" ]; then
                    local throughput p50 p95 p99 client_cpu
                    throughput=$(echo "$line" | cut -d',' -f4)
                    p50=$(echo "$line" | cut -d',' -f5)
                    p95=$(echo "$line" | cut -d',' -f6)
                    p99=$(echo "$line" | cut -d',' -f7)
                    client_cpu=$(echo "$line" | cut -d',' -f8)

                    if ! throughput_is_positive "$throughput"; then
                        warn "  Invalid zero throughput from ${client_lang} pipeline client for ${server_lang} server"
                        dump_client_error "$client_err"
                        RUN_FAILED=1
                        sleep 0.5
                        continue
                    fi

                    local server_cpu_pct total_cpu_pct
                    if command -v bc >/dev/null 2>&1; then
                        server_cpu_pct=$(echo "scale=1; ${server_cpu_sec} / ${DURATION} * 100" | bc 2>/dev/null || echo "0.0")
                        total_cpu_pct=$(echo "scale=1; ${client_cpu} + ${server_cpu_pct}" | bc 2>/dev/null || echo "0.0")
                    else
                        server_cpu_pct="0.0"
                        total_cpu_pct="$client_cpu"
                    fi

                    write_csv_row "uds-pipeline-d${PIPELINE_DEPTH}" "$client_lang" "$server_lang" "0" \
                        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
                    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
                else
                    warn "  Could not parse pipeline output from ${client_lang} client for ${server_lang} server"
                    dump_client_error "$client_err"
                    RUN_FAILED=1
                fi
            else
                warn "  No output from ${client_lang} pipeline client for ${server_lang} server"
                dump_client_error "$client_err"
                RUN_FAILED=1
            fi

            sleep 0.5
        done
    done

    # 9. UDS pipeline+batch benchmarks (9 pairs, max rate, depth=16)
    log "=== UDS Pipeline+Batch (9 pairs, max rate, depth=16) ==="
    for server_lang in "${LANGS[@]}"; do
        for client_lang in "${LANGS[@]}"; do
            local pipe_batch_svc="pipe-batch-${server_lang}-${client_lang}"

            log "  uds-pipeline-batch: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration=$((DURATION + 5))
            local server_pid
            # Pipeline+batch needs the batch server (higher limits)
            server_pid=$(start_server "$server_lang" "uds-batch-ping-pong-server" "$pipe_batch_svc" "$server_duration") || {
                warn "  Failed to start pipeline-batch server"
                continue
            }

            sleep 0.5

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((DURATION + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-uds-pipeline-batch-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "uds-pipeline-batch-client" "$RUN_DIR" "$pipe_batch_svc" "$DURATION" "0" "$PIPELINE_DEPTH" 2>"$client_err")
            client_status=$?
            set -e

            local server_cpu_sec
            if ! server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pipe_batch_svc"); then
                warn "  Missing server CPU for ${client_lang}->${server_lang} pipeline-batch benchmark"
                RUN_FAILED=1
                sleep 0.5
                continue
            fi

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
                line=$(echo "$client_output" | grep "^uds-pipeline-batch" | head -1)
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
                    if command -v bc >/dev/null 2>&1; then
                        server_cpu_pct=$(echo "scale=1; ${server_cpu_sec} / ${DURATION} * 100" | bc 2>/dev/null || echo "0.0")
                        total_cpu_pct=$(echo "scale=1; ${client_cpu} + ${server_cpu_pct}" | bc 2>/dev/null || echo "0.0")
                    else
                        server_cpu_pct="0.0"
                        total_cpu_pct="$client_cpu"
                    fi

                    write_csv_row "uds-pipeline-batch-d${PIPELINE_DEPTH}" "$client_lang" "$server_lang" "0" \
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

    if [ "$RUN_FAILED" -ne 0 ]; then
        err "One or more POSIX benchmark scenarios failed; CSV is incomplete or invalid"
        return 1
    fi

    # Summary
    log ""
    log "=== Results ==="
    log "CSV: ${OUTPUT_CSV}"

    local total_lines
    total_lines=$(wc -l < "$OUTPUT_CSV")
    log "Total measurements: $((total_lines - 1))"

    # Print summary table
    printf "\n"
    printf "${CYAN}%-25s %-8s %-8s %-10s %12s %8s %8s %8s${NC}\n" \
        "Scenario" "Client" "Server" "Target RPS" "Throughput" "p50(us)" "p95(us)" "p99(us)"
    printf -- "-------- -------- -------- ---------- ------------ -------- -------- --------\n"

    tail -n +2 "$OUTPUT_CSV" | while IFS=',' read -r scenario client server target_rps throughput p50 p95 p99 ccpu scpu tcpu; do
        printf "%-25s %-8s %-8s %-10s %12s %8s %8s %8s\n" \
            "$scenario" "$client" "$server" "$target_rps" "$throughput" "$p50" "$p95" "$p99"
    done

    printf "\n"
    log "Done. Run tests/generate-benchmarks-posix.sh to generate the markdown report."
}

main "$@"
