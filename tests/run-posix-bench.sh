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
#   8. Lookup method codec+dispatch (12 scenarios x 3 languages x 4 rates)
#   9. UDS pipeline (9 pairs x 1 rate, depth=16)
#   10. UDS pipeline+batch (9 pairs x 1 rate, depth=16)
#
# Output: CSV file + human-readable summary.
# CSV columns:
#   scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,
#   client_cpu_pct,server_cpu_pct,total_cpu_pct
#
# Usage:
#   ./tests/run-posix-bench.sh [output_csv] [duration_sec]
#
# Environment:
#   NIPC_BENCH_MAX_DURATION
#     Duration for max-throughput rows. Defaults to 10s to reduce scheduler
#     noise in floor-sensitive rows. The CLI duration still controls fixed-rate
#     rows.
#   NIPC_BENCH_FLOOR_RETRY_SAMPLES
#     Repeated diagnostic samples for max-throughput rows that miss a published
#     performance floor. Defaults to 7. Set to 0 to disable recovery.
#   NIPC_BENCH_FLOOR_RETRY_DURATION
#     Duration for each floor-retry sample. Defaults to 20s.
#   NIPC_BENCH_FLOOR_RETRY_MAX_RATIO
#     Maximum retry sample max/min throughput ratio accepted as stable.

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
MAX_DURATION="${NIPC_BENCH_MAX_DURATION:-10}"
FLOOR_RETRY_SAMPLES="${NIPC_BENCH_FLOOR_RETRY_SAMPLES:-7}"
FLOOR_RETRY_DURATION="${NIPC_BENCH_FLOOR_RETRY_DURATION:-20}"
FLOOR_RETRY_MAX_RATIO="${NIPC_BENCH_FLOOR_RETRY_MAX_RATIO:-1.35}"
FLOOR_RETRY_CSV="${OUTPUT_CSV%.csv}.floor-retries.csv"
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

    # Wait for READY and for the UDS listener socket to exist (up to 5s).
    # Some drivers print READY just before entering the managed server loop.
    local waited=0
    local socket_path="${RUN_DIR}/${svc}.sock"
    while [ $waited -lt 50 ]; do
        if grep -q "^READY$" "$server_out" 2>/dev/null && [ -S "$socket_path" ]; then
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
    awk -v first="$1" -v second="$2" 'BEGIN {
        printf "%.3f", (first + 0) + (second + 0)
    }'
}

duration_for_target() {
    local target_rps="$1"
    if [ "$target_rps" = "0" ]; then
        echo "$MAX_DURATION"
    else
        echo "$DURATION"
    fi
}

positive_integer_or_zero() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

positive_number() {
    awk -v value="$1" 'BEGIN { exit ((value + 0) > 0 ? 0 : 1) }'
}

float_lt() {
    awk -v left="$1" -v right="$2" 'BEGIN { exit ((left + 0) < (right + 0) ? 0 : 1) }'
}

floor_min_for_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"

    [ "$target_rps" = "0" ] || return 1

    case "$scenario" in
        shm-ping-pong)
            echo 1000000
            ;;
        uds-ping-pong)
            echo 120000
            ;;
        snapshot-baseline)
            echo 100000
            ;;
        snapshot-shm)
            if [ "$client_lang" = "go" ] || [ "$server_lang" = "go" ]; then
                echo 800000
            else
                echo 1000000
            fi
            ;;
        lookup)
            echo 10000000
            ;;
        cgroups-lookup-known-16)
            echo 250000
            ;;
        cgroups-lookup-unknown-16)
            echo 500000
            ;;
        cgroups-lookup-mixed-16)
            echo 350000
            ;;
        cgroups-lookup-mixed-256)
            echo 25000
            ;;
        apps-lookup-known-16)
            echo 300000
            ;;
        apps-lookup-unknown-16)
            echo 500000
            ;;
        apps-lookup-mixed-16)
            echo 350000
            ;;
        apps-lookup-mixed-256)
            echo 25000
            ;;
        *)
            return 1
            ;;
    esac
}

median_from_sample_file() {
    local sample_file="$1"
    local column="$2"

    awk -F',' -v column="$column" 'NR > 1 { print $column + 0 }' "$sample_file" | sort -g | awk '
        { values[NR] = $1 }
        END {
            if (NR == 0) {
                exit 1
            }
            if ((NR % 2) == 1) {
                printf "%.3f", values[(NR + 1) / 2]
            } else {
                printf "%.3f", (values[NR / 2] + values[(NR / 2) + 1]) / 2.0
            }
        }
    '
}

min_from_sample_file() {
    local sample_file="$1"
    local column="$2"

    awk -F',' -v column="$column" 'NR > 1 { print $column + 0 }' "$sample_file" | sort -g | head -1
}

max_from_sample_file() {
    local sample_file="$1"
    local column="$2"

    awk -F',' -v column="$column" 'NR > 1 { print $column + 0 }' "$sample_file" | sort -g | tail -1
}

throughput_ratio() {
    local min_value="$1"
    local max_value="$2"

    awk -v min_value="$min_value" -v max_value="$max_value" 'BEGIN {
        if ((min_value + 0) <= 0) {
            print "999999.000000"
        } else {
            printf "%.6f", (max_value + 0) / (min_value + 0)
        }
    }'
}

retry_ratio_is_stable() {
    local ratio="$1"

    awk -v ratio="$ratio" -v max_ratio="$FLOOR_RETRY_MAX_RATIO" '
        BEGIN { exit ((ratio + 0) <= (max_ratio + 0) ? 0 : 1) }
    '
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
    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$duration")
    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

    write_csv_row "$scenario" "$client_lang" "$server_lang" "$target_rps" \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"

    log "    throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us"
}

run_pair_once_to_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local duration="$5"
    local repeat_idx="$6"

    local saved_output_csv="$OUTPUT_CSV"
    local saved_run_dir="$RUN_DIR"
    local retry_run_dir="${saved_run_dir}/r-${RANDOM}-${repeat_idx}"
    local retry_csv="${retry_run_dir}/row.csv"
    local status=0

    mkdir -p "$retry_run_dir"
    echo "scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$retry_csv"

    OUTPUT_CSV="$retry_csv"
    RUN_DIR="$retry_run_dir"
    if ! run_pair "$scenario" "$server_lang" "$client_lang" "$target_rps" "$duration"; then
        status=1
    fi
    OUTPUT_CSV="$saved_output_csv"
    RUN_DIR="$saved_run_dir"

    [ "$status" -eq 0 ] || return "$status"
    tail -n 1 "$retry_csv"
}

lookup_once_to_row() {
    local lang="$1"
    local duration="$2"

    local bin
    bin="$(bench_bin "$lang")"

    local line
    line=$("$bin" lookup-bench "$duration" | grep "^lookup," | head -1)
    [ -n "$line" ] || return 1

    local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)
    server_cpu_pct=$(echo "$line" | cut -d',' -f9)
    total_cpu_pct=$(echo "$line" | cut -d',' -f10)

    printf 'lookup,%s,%s,0,%s,%s,%s,%s,%s,%s,%s\n' \
        "$lang" "$lang" "$throughput" "$p50" "$p95" "$p99" \
        "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

lookup_method_once_to_row() {
    local scenario="$1"
    local lang="$2"
    local target_rps="$3"
    local duration="$4"

    local bin
    bin="$(bench_bin "$lang")"

    local line
    line=$("$bin" lookup-method-bench "$duration" "$scenario" "$target_rps" | grep "^${scenario}," | head -1)
    [ -n "$line" ] || return 1

    local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)
    server_cpu_pct=$(echo "$line" | cut -d',' -f9)
    total_cpu_pct=$(echo "$line" | cut -d',' -f10)

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$scenario" "$lang" "$lang" "$target_rps" "$throughput" "$p50" "$p95" "$p99" \
        "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

floor_retry_once_to_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local duration="$5"
    local repeat_idx="$6"

    case "$scenario" in
        lookup)
            [ "$client_lang" = "$server_lang" ] || return 1
            lookup_once_to_row "$client_lang" "$duration"
            ;;
        cgroups-lookup-*|apps-lookup-*)
            [ "$client_lang" = "$server_lang" ] || return 1
            lookup_method_once_to_row "$scenario" "$client_lang" "$target_rps" "$duration"
            ;;
        *)
            run_pair_once_to_row "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$repeat_idx"
            ;;
    esac
}

replace_csv_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local replacement="$5"
    local tmp_file="${OUTPUT_CSV}.tmp.$$"

    awk -F',' -v OFS=',' \
        -v scenario="$scenario" \
        -v client="$client_lang" \
        -v server="$server_lang" \
        -v target="$target_rps" \
        -v replacement="$replacement" '
        NR == 1 {
            print
            next
        }
        $1 == scenario && $2 == client && $3 == server && $4 == target {
            if (!done) {
                print replacement
                done = 1
            }
            next
        }
        {
            print
        }
        END {
            if (!done) {
                exit 1
            }
        }
    ' "$OUTPUT_CSV" > "$tmp_file"

    mv "$tmp_file" "$OUTPUT_CSV"
}

append_floor_retry_log() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local min_throughput="$5"
    local original_throughput="$6"
    local retry_samples="$7"
    local median_throughput="$8"
    local min_retry="$9"
    local max_retry="${10}"
    local stable_ratio="${11}"
    local status="${12}"

    if [ ! -f "$FLOOR_RETRY_CSV" ]; then
        echo "scenario,client,server,target_rps,min_throughput,original_throughput,retry_samples,median_throughput,min_retry,max_retry,stable_ratio,status" > "$FLOOR_RETRY_CSV"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$scenario" "$client_lang" "$server_lang" "$target_rps" "$min_throughput" \
        "$original_throughput" "$retry_samples" "$median_throughput" "$min_retry" \
        "$max_retry" "$stable_ratio" "$status" >> "$FLOOR_RETRY_CSV"
}

retry_floor_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local min_throughput="$5"
    local original_throughput="$6"

    local sample_file="${RUN_DIR}/floor-retry-${scenario}-${client_lang}-${server_lang}-${target_rps}.samples.csv"
    echo "repeat,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$sample_file"

    log "  retry floor: ${scenario} ${client_lang}->${server_lang} @ max original=${original_throughput} min=${min_throughput}"

    local repeat_idx
    for repeat_idx in $(seq 1 "$FLOOR_RETRY_SAMPLES"); do
        local row
        if ! row=$(floor_retry_once_to_row "$scenario" "$client_lang" "$server_lang" "$target_rps" "$FLOOR_RETRY_DURATION" "$repeat_idx"); then
            warn "    retry sample ${repeat_idx}/${FLOOR_RETRY_SAMPLES} failed"
            append_floor_retry_log "$scenario" "$client_lang" "$server_lang" "$target_rps" \
                "$min_throughput" "$original_throughput" "$FLOOR_RETRY_SAMPLES" "" "" "" "" "failed"
            return 1
        fi

        local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
        throughput=$(echo "$row" | cut -d',' -f5)
        p50=$(echo "$row" | cut -d',' -f6)
        p95=$(echo "$row" | cut -d',' -f7)
        p99=$(echo "$row" | cut -d',' -f8)
        client_cpu=$(echo "$row" | cut -d',' -f9)
        server_cpu_pct=$(echo "$row" | cut -d',' -f10)
        total_cpu_pct=$(echo "$row" | cut -d',' -f11)

        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$repeat_idx" "$throughput" "$p50" "$p95" "$p99" \
            "$client_cpu" "$server_cpu_pct" "$total_cpu_pct" >> "$sample_file"
    done

    local median_throughput min_retry max_retry stable_ratio
    median_throughput=$(median_from_sample_file "$sample_file" 2) || return 1
    min_retry=$(min_from_sample_file "$sample_file" 2) || return 1
    max_retry=$(max_from_sample_file "$sample_file" 2) || return 1
    stable_ratio=$(throughput_ratio "$min_retry" "$max_retry")

    local p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
    p50=$(median_from_sample_file "$sample_file" 3) || return 1
    p95=$(median_from_sample_file "$sample_file" 4) || return 1
    p99=$(median_from_sample_file "$sample_file" 5) || return 1
    client_cpu=$(median_from_sample_file "$sample_file" 6) || return 1
    server_cpu_pct=$(median_from_sample_file "$sample_file" 7) || return 1
    total_cpu_pct=$(median_from_sample_file "$sample_file" 8) || return 1

    if float_lt "$median_throughput" "$min_throughput"; then
        warn "    retry median still below floor: median=${median_throughput} min=${min_throughput}"
        append_floor_retry_log "$scenario" "$client_lang" "$server_lang" "$target_rps" \
            "$min_throughput" "$original_throughput" "$FLOOR_RETRY_SAMPLES" "$median_throughput" \
            "$min_retry" "$max_retry" "$stable_ratio" "failed_below_floor"
        return 1
    fi

    if ! retry_ratio_is_stable "$stable_ratio"; then
        warn "    retry samples unstable: min=${min_retry} max=${max_retry} ratio=${stable_ratio} max_ratio=${FLOOR_RETRY_MAX_RATIO}"
        append_floor_retry_log "$scenario" "$client_lang" "$server_lang" "$target_rps" \
            "$min_throughput" "$original_throughput" "$FLOOR_RETRY_SAMPLES" "$median_throughput" \
            "$min_retry" "$max_retry" "$stable_ratio" "failed_unstable"
        return 1
    fi

    local replacement
    replacement=$(printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s' \
        "$scenario" "$client_lang" "$server_lang" "$target_rps" "$median_throughput" \
        "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct")

    replace_csv_row "$scenario" "$client_lang" "$server_lang" "$target_rps" "$replacement"
    append_floor_retry_log "$scenario" "$client_lang" "$server_lang" "$target_rps" \
        "$min_throughput" "$original_throughput" "$FLOOR_RETRY_SAMPLES" "$median_throughput" \
        "$min_retry" "$max_retry" "$stable_ratio" "recovered"
    log "    recovered median_throughput=${median_throughput} stable_ratio=${stable_ratio}"
}

retry_floor_violations() {
    if [ "$FLOOR_RETRY_SAMPLES" -eq 0 ]; then
        log "Floor retry disabled"
        return 0
    fi

    local violations_file="${RUN_DIR}/floor-violations.csv"
    : > "$violations_file"

    while IFS=',' read -r scenario client_lang server_lang target_rps throughput _rest; do
        local min_throughput
        if ! min_throughput=$(floor_min_for_row "$scenario" "$client_lang" "$server_lang" "$target_rps"); then
            continue
        fi
        if float_lt "$throughput" "$min_throughput"; then
            printf '%s,%s,%s,%s,%s,%s\n' \
                "$scenario" "$client_lang" "$server_lang" "$target_rps" "$min_throughput" "$throughput" >> "$violations_file"
        fi
    done < <(tail -n +2 "$OUTPUT_CSV")

    if [ ! -s "$violations_file" ]; then
        log "No floor retry needed"
        return 0
    fi

    log "=== Floor Retry Diagnostics ==="
    log "Retry samples per failed floor row: ${FLOOR_RETRY_SAMPLES}"
    log "Retry duration per sample: ${FLOOR_RETRY_DURATION}s"
    log "Retry stability max/min ratio limit: ${FLOOR_RETRY_MAX_RATIO}"
    log "Retry log: ${FLOOR_RETRY_CSV}"

    local failed=0
    while IFS=',' read -r scenario client_lang server_lang target_rps min_throughput original_throughput; do
        if ! retry_floor_row "$scenario" "$client_lang" "$server_lang" "$target_rps" "$min_throughput" "$original_throughput"; then
            failed=1
        fi
    done < "$violations_file"

    return "$failed"
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
    log "Fixed-rate duration per run: ${DURATION}s"
    log "Max-throughput duration per run: ${MAX_DURATION}s"
    log "Floor retry samples: ${FLOOR_RETRY_SAMPLES}"
    log "Output: ${OUTPUT_CSV}"

    if ! positive_integer_or_zero "$FLOOR_RETRY_SAMPLES"; then
        err "NIPC_BENCH_FLOOR_RETRY_SAMPLES must be a non-negative integer"
        exit 1
    fi
    if ! positive_number "$FLOOR_RETRY_DURATION"; then
        err "NIPC_BENCH_FLOOR_RETRY_DURATION must be greater than zero"
        exit 1
    fi
    if ! positive_number "$FLOOR_RETRY_MAX_RATIO"; then
        err "NIPC_BENCH_FLOOR_RETRY_MAX_RATIO must be greater than zero"
        exit 1
    fi

    if ! check_binaries; then
        err "Missing benchmark binaries. Build first."
        exit 1
    fi

    mkdir -p "$RUN_DIR"

    # CSV header
    echo "scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$OUTPUT_CSV"
    rm -f "$FLOOR_RETRY_CSV"

    local LANGS=(c rust go)
    local RATES_PING_PONG=(0 100000 10000 1000)
    local RATES_SNAPSHOT=(0 1000)
    local RATES_LOOKUP_METHOD=(0 100000 10000 1000)
    local LOOKUP_METHOD_SCENARIOS=(
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

    # 1. UDS ping-pong: 9 pairs x 4 rates
    log "=== UDS Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "uds-ping-pong" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 2. SHM ping-pong: 9 pairs x 4 rates
    log "=== SHM Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "shm-ping-pong" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 3. Snapshot baseline refresh: 9 pairs x 2 rates
    log "=== Snapshot Baseline ==="
    for rate in "${RATES_SNAPSHOT[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "snapshot-baseline" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 4. Snapshot SHM refresh: 9 pairs x 2 rates
    log "=== Snapshot SHM ==="
    for rate in "${RATES_SNAPSHOT[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "snapshot-shm" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 5. UDS batch ping-pong: 9 pairs x 4 rates
    log "=== UDS Batch Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "uds-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
                    RUN_FAILED=1
                fi
                sleep 0.5
            done
        done
    done

    # 6. SHM batch ping-pong: 9 pairs x 4 rates
    log "=== SHM Batch Ping-Pong ==="
    for rate in "${RATES_PING_PONG[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                if ! run_pair "shm-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$row_duration"; then
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
        line=$("$bin" lookup-bench "$MAX_DURATION" 2>"$lookup_err" | grep "^lookup," | head -1)
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

    # 8. Lookup method codec+dispatch: 12 scenarios x 3 languages x 4 rates
    log "=== Lookup Methods (codec + dispatch) ==="
    for rate in "${RATES_LOOKUP_METHOD[@]}"; do
        row_duration="$(duration_for_target "$rate")"
        for scenario in "${LOOKUP_METHOD_SCENARIOS[@]}"; do
            for lang in "${LANGS[@]}"; do
                local bin
                bin="$(bench_bin "$lang")"
                log "  ${scenario}: ${lang} @ ${rate}"
                local line
                local lookup_status
                local lookup_err="${RUN_DIR}/${scenario}-${lang}-${rate}.err"
                set +e
                line=$("$bin" lookup-method-bench "$row_duration" "$scenario" "$rate" 2>"$lookup_err" | grep "^${scenario}," | head -1)
                lookup_status=$?
                set -e
                if [ "$lookup_status" -ne 0 ]; then
                    warn "  ${lang} ${scenario} benchmark failed at target ${rate}"
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
                        warn "  Invalid zero throughput from ${lang} ${scenario} benchmark at target ${rate}"
                        dump_client_error "$lookup_err"
                        RUN_FAILED=1
                        continue
                    fi
                    write_csv_row "$scenario" "$lang" "$lang" "$rate" \
                        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
                else
                    warn "  No output from ${lang} ${scenario} benchmark at target ${rate}"
                    dump_client_error "$lookup_err"
                    RUN_FAILED=1
                fi
            done
        done
    done

    # 9. UDS pipeline benchmarks (all 9 pairs, max rate, depth=16)
    log "=== UDS Pipeline (9 pairs, max rate, depth=16) ==="
    local PIPELINE_DEPTH=16
    local pipeline_duration="$MAX_DURATION"
    for server_lang in "${LANGS[@]}"; do
        for client_lang in "${LANGS[@]}"; do
            local pipe_svc="pipeline-${server_lang}-${client_lang}"

            log "  uds-pipeline: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration=$((pipeline_duration + 5))
            local server_pid
            server_pid=$(start_server "$server_lang" "uds-ping-pong-server" "$pipe_svc" "$server_duration") || {
                warn "  Failed to start pipeline server"
                continue
            }

            sleep 0.5

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((pipeline_duration + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-uds-pipeline-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "uds-pipeline-client" "$RUN_DIR" "$pipe_svc" "$pipeline_duration" "0" "$PIPELINE_DEPTH" 2>"$client_err")
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
                    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$pipeline_duration")
                    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

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

    # 10. UDS pipeline+batch benchmarks (9 pairs, max rate, depth=16)
    log "=== UDS Pipeline+Batch (9 pairs, max rate, depth=16) ==="
    for server_lang in "${LANGS[@]}"; do
        for client_lang in "${LANGS[@]}"; do
            local pipe_batch_svc="pipe-batch-${server_lang}-${client_lang}"

            log "  uds-pipeline-batch: ${client_lang}->${server_lang} depth=${PIPELINE_DEPTH}"

            local server_duration=$((pipeline_duration + 5))
            local server_pid
            # Pipeline+batch needs the batch server (higher limits)
            server_pid=$(start_server "$server_lang" "uds-batch-ping-pong-server" "$pipe_batch_svc" "$server_duration") || {
                warn "  Failed to start pipeline-batch server"
                continue
            }

            sleep 0.5

            local client_bin
            client_bin="$(bench_bin "$client_lang")"
            local client_timeout=$((pipeline_duration + 15))
            local client_output
            local client_status
            local client_err="${RUN_DIR}/client-uds-pipeline-batch-${server_lang}-${client_lang}.err"
            set +e
            client_output=$(timeout "$client_timeout" "$client_bin" "uds-pipeline-batch-client" "$RUN_DIR" "$pipe_batch_svc" "$pipeline_duration" "0" "$PIPELINE_DEPTH" 2>"$client_err")
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
                    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$pipeline_duration")
                    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

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

    if ! retry_floor_violations; then
        err "One or more POSIX benchmark floor retries failed; generated CSV still violates a performance floor"
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
