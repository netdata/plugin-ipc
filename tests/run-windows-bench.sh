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
#
# Optional diagnostics:
#   NIPC_BENCH_DIAGNOSE_FAILURES=1
#     When a row fails, preserve the first-attempt evidence and rerun the
#     same row in isolation for debugging. Diagnostic reruns never write to
#     the publish CSV and never turn a failing publish run into a success.
#   NIPC_WINDOWS_TOOLCHAIN=mingw64|msys
#     Select the C toolchain lane for the benchmark build. `mingw64` is the
#     native Windows sign-off default. `msys` is intended for the separate
#     compatibility / comparison lane.

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
WINDOWS_TOOLCHAIN="${NIPC_WINDOWS_TOOLCHAIN:-mingw64}"
case "$WINDOWS_TOOLCHAIN" in
    mingw64)
        DEFAULT_BUILD_DIR="${ROOT_DIR}/build-bench-windows"
        ;;
    msys)
        DEFAULT_BUILD_DIR="${ROOT_DIR}/build-bench-windows-msys"
        ;;
    *)
        DEFAULT_BUILD_DIR="${ROOT_DIR}/build-bench-windows"
        ;;
esac
BUILD_DIR="${NIPC_BENCH_BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
BENCH_BUILD_TYPE="${NIPC_BENCH_BUILD_TYPE:-Release}"

OUTPUT_CSV="${1:-${ROOT_DIR}/benchmarks-windows.csv}"
DURATION="${2:-5}"
REPETITIONS="${NIPC_BENCH_REPETITIONS:-5}"
MAX_DURATION="${NIPC_BENCH_MAX_DURATION:-10}"
PIPELINE_BATCH_MAX_DURATION="${NIPC_BENCH_PIPELINE_BATCH_MAX_DURATION:-20}"
MAX_THROUGHPUT_RATIO="${NIPC_BENCH_MAX_THROUGHPUT_RATIO:-1.35}"
MIN_STABLE_SAMPLES="${NIPC_BENCH_MIN_STABLE_SAMPLES:-3}"
ALLOW_TRIMMED_UNSTABLE_RAW="${NIPC_BENCH_ALLOW_TRIMMED_UNSTABLE_RAW:-0}"
SERVER_STOP_GRACE_SEC="${NIPC_BENCH_SERVER_STOP_GRACE_SEC:-10}"
ROW_SETTLE_SEC="${NIPC_BENCH_ROW_SETTLE_SEC:-2}"
DIAGNOSE_FAILURES="${NIPC_BENCH_DIAGNOSE_FAILURES:-0}"
RUN_DIR="${TEMP:-/tmp}/netipc-bench-$$"
ROOT_RUN_DIR="$RUN_DIR"
MEASURE_RUN_DIR="$RUN_DIR"
DIAGNOSTIC_SUMMARY_FILE=""
DIAGNOSTIC_FAILURE_COUNT=0
ACTIVE_DIAGNOSTIC=0
EMIT_CSV_ROWS=1
POWERSHELL_EXE="/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
BLOCK_FIRST="${NIPC_BENCH_FIRST_BLOCK:-1}"
BLOCK_LAST="${NIPC_BENCH_LAST_BLOCK:-9}"
SCENARIO_FILTER="${NIPC_BENCH_SCENARIOS:-}"
CLIENT_FILTER="${NIPC_BENCH_CLIENTS:-}"
SERVER_FILTER="${NIPC_BENCH_SERVERS:-}"
TARGET_FILTER="${NIPC_BENCH_TARGETS:-}"
SERVER_WARMUP_DURATION_SEC="${NIPC_BENCH_SERVER_WARMUP_DURATION_SEC:-1}"

# Binary locations
BENCH_C="${BUILD_DIR}/bin/bench_windows_c.exe"
BENCH_RS="${ROOT_DIR}/src/crates/netipc/target/release/bench_windows.exe"
BENCH_GO="${BUILD_DIR}/bin/bench_windows_go.exe"

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------

cleanup() {
    if [ "${NIPC_KEEP_RUN_DIR:-0}" = "1" ]; then
        warn "Preserving RUN_DIR: $ROOT_RUN_DIR"
    else
        rm -rf "$ROOT_RUN_DIR"
    fi
    for pid in "${SERVER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

SERVER_PIDS=()
RUN_FAILED=0
LAST_MEASUREMENT_STATUS=""
LAST_MEASUREMENT_REASON=""
LAST_MEASUREMENT_SAMPLE_FILE=""
LAST_MEASUREMENT_THROUGHPUT=""
LAST_MEASUREMENT_P50=""
LAST_MEASUREMENT_P95=""
LAST_MEASUREMENT_P99=""
LAST_MEASUREMENT_CLIENT_CPU=""
LAST_MEASUREMENT_SERVER_CPU=""
LAST_MEASUREMENT_TOTAL_CPU=""
LAST_MEASUREMENT_STABLE_SAMPLES=""
LAST_MEASUREMENT_STABLE_MIN=""
LAST_MEASUREMENT_STABLE_MAX=""
LAST_MEASUREMENT_STABLE_RATIO=""
LAST_MEASUREMENT_RAW_MIN=""
LAST_MEASUREMENT_RAW_MAX=""
LAST_MEASUREMENT_RAW_RATIO=""
LAST_MEASUREMENT_ARTIFACT_DIR=""

log() {
    printf "${CYAN}[bench]${NC} %s\n" "$*" >&2
}

warn() {
    printf "${YELLOW}[warn]${NC} %s\n" "$*" >&2
}

setup_windows_toolchain() {
    case "$WINDOWS_TOOLCHAIN" in
        mingw64)
            export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:/usr/bin:$PATH"
            export MSYSTEM=MINGW64
            ;;
        msys)
            export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/usr/bin:$PATH"
            export MSYSTEM=MSYS
            ;;
        *)
            err "Unsupported NIPC_WINDOWS_TOOLCHAIN: ${WINDOWS_TOOLCHAIN} (expected mingw64 or msys)"
            exit 1
            ;;
    esac

    for tool in cmake gcc g++ cygpath timeout; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            err "Required tool not found: $tool"
            exit 1
        fi
    done

    CC_BIN="$(command -v gcc)"
    CXX_BIN="$(command -v g++)"
}

runtime_dir_for_windows_bin() {
    local path="$1"

    case "$path" in
        [A-Za-z]:[\\/]*|\\\\*)
            printf '%s\n' "$path"
            ;;
        *)
            cygpath -w "$path"
            ;;
    esac
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
    local rust_manifest="${ROOT_DIR}/src/crates/netipc/Cargo.toml"

    setup_windows_toolchain

    if [ -f "$cache" ]; then
        current_type=$(awk -F= '/^CMAKE_BUILD_TYPE:STRING=/{print $2; exit}' "$cache")
    fi

    if [ ! -f "$cache" ] || [ "$current_type" != "$BENCH_BUILD_TYPE" ]; then
        log "Configuring benchmark build dir: ${BUILD_DIR} (${BENCH_BUILD_TYPE})"
        run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE="$BENCH_BUILD_TYPE" \
            -DCMAKE_C_COMPILER="$CC_BIN" \
            -DCMAKE_CXX_COMPILER="$CXX_BIN"
    fi

    log "Building benchmark binaries in ${BUILD_DIR}"
    run cmake --build "$BUILD_DIR" --target bench_windows_c bench_windows_go -j"$(build_jobs)"

    log "Building Rust Windows benchmark binary"
    (
        cd "$ROOT_DIR"
        run cargo build --release --manifest-path "$rust_manifest" --bin bench_windows
    )
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
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"
    local runtime_dir_bin
    runtime_dir_bin="$(runtime_dir_for_windows_bin "$runtime_dir")"

    local bin
    bin="$(bench_bin "$lang")"

    mkdir -p "$runtime_dir"
    local server_out="${runtime_dir}/server-${lang}-${svc}.out"

    "$bin" "$subcmd" "$runtime_dir_bin" "$svc" "$duration_arg" > "$server_out" 2>&1 &
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
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"
    local server_out="${runtime_dir}/server-${lang}-${svc}.out"
    local server_cpu
    server_cpu=$(server_cpu_from_output "$server_out")
    local wait_status=0
    local forced_stop=0

    if [ -z "$server_cpu" ]; then
        server_cpu=$(server_cpu_seconds "$pid")
    fi

    if kill -0 "$pid" 2>/dev/null; then
        local waited=0
        local wait_ticks=$((SERVER_STOP_GRACE_SEC * 10))
        while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$wait_ticks" ]; do
            sleep 0.1
            waited=$((waited + 1))
        done
    fi

    if kill -0 "$pid" 2>/dev/null; then
        forced_stop=1
        warn "  Server ${lang} (${svc}) did not exit cleanly within ${SERVER_STOP_GRACE_SEC}s; forcing kill"
        kill "$pid" 2>/dev/null || true
    fi

    if ! wait "$pid" 2>/dev/null; then
        wait_status=$?
    fi

    if [ -f "$server_out" ]; then
        local output_cpu
        output_cpu=$(server_cpu_from_output "$server_out")
        if [ -n "$output_cpu" ]; then
            server_cpu="$output_cpu"
        fi
    fi

    echo "${server_cpu:-0.0}"

    if [ "$forced_stop" -eq 1 ]; then
        return 1
    fi

    if [ "$wait_status" -ne 0 ]; then
        warn "  Server ${lang} (${svc}) exited with status ${wait_status}"
        return 1
    fi

    return 0
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

require_positive_integer() {
    local name="$1"
    local value="$2"

    case "$value" in
        ''|*[!0-9]*)
            err "${name} must be a positive integer, got: ${value}"
            exit 1
            ;;
    esac

    if [ "$value" -lt 1 ]; then
        err "${name} must be >= 1, got: ${value}"
        exit 1
    fi
}

require_positive_number() {
    local name="$1"
    local value="$2"

    if ! awk -v value="$value" 'BEGIN { exit ((value + 0) > 0 ? 0 : 1) }'; then
        err "${name} must be > 0, got: ${value}"
        exit 1
    fi
}

require_zero_or_one() {
    local name="$1"
    local value="$2"

    case "$value" in
        0|1)
            ;;
        *)
            err "${name} must be 0 or 1, got: ${value}"
            exit 1
            ;;
    esac
}

filter_matches() {
    local value="$1"
    local filter="$2"
    local token

    if [ -z "$filter" ]; then
        return 0
    fi

    IFS='|' read -r -a __bench_filter_tokens <<< "$filter"
    for token in "${__bench_filter_tokens[@]}"; do
        [ -n "$token" ] || continue
        case "$value" in
            $token) return 0 ;;
        esac
    done

    return 1
}

should_run_row() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"

    filter_matches "$scenario" "$SCENARIO_FILTER" || return 1
    filter_matches "$client_lang" "$CLIENT_FILTER" || return 1
    filter_matches "$server_lang" "$SERVER_FILTER" || return 1
    filter_matches "$target_rps" "$TARGET_FILTER" || return 1
    return 0
}

start_server_with_warmup() {
    local warmup_bin="$1"
    local warmup_subcmd="$2"
    local warmup_target="$3"
    local warmup_depth="$4"
    shift 4

    local prev_bin="${NIPC_BENCH_SERVER_WARMUP_BIN-__UNSET__}"
    local prev_subcmd="${NIPC_BENCH_SERVER_WARMUP_SUBCMD-__UNSET__}"
    local prev_duration="${NIPC_BENCH_SERVER_WARMUP_DURATION_SEC-__UNSET__}"
    local prev_target="${NIPC_BENCH_SERVER_WARMUP_TARGET_RPS-__UNSET__}"
    local prev_depth="${NIPC_BENCH_SERVER_WARMUP_DEPTH-__UNSET__}"

    export NIPC_BENCH_SERVER_WARMUP_BIN="$warmup_bin"
    export NIPC_BENCH_SERVER_WARMUP_SUBCMD="$warmup_subcmd"
    export NIPC_BENCH_SERVER_WARMUP_DURATION_SEC="$SERVER_WARMUP_DURATION_SEC"
    if [ -n "$warmup_target" ]; then
        export NIPC_BENCH_SERVER_WARMUP_TARGET_RPS="$warmup_target"
    else
        unset NIPC_BENCH_SERVER_WARMUP_TARGET_RPS
    fi
    if [ -n "$warmup_depth" ]; then
        export NIPC_BENCH_SERVER_WARMUP_DEPTH="$warmup_depth"
    else
        unset NIPC_BENCH_SERVER_WARMUP_DEPTH
    fi

    local pid rc
    set +e
    pid=$(start_server "$@")
    rc=$?
    set -e

    if [ "$prev_bin" = "__UNSET__" ]; then unset NIPC_BENCH_SERVER_WARMUP_BIN; else export NIPC_BENCH_SERVER_WARMUP_BIN="$prev_bin"; fi
    if [ "$prev_subcmd" = "__UNSET__" ]; then unset NIPC_BENCH_SERVER_WARMUP_SUBCMD; else export NIPC_BENCH_SERVER_WARMUP_SUBCMD="$prev_subcmd"; fi
    if [ "$prev_duration" = "__UNSET__" ]; then unset NIPC_BENCH_SERVER_WARMUP_DURATION_SEC; else export NIPC_BENCH_SERVER_WARMUP_DURATION_SEC="$prev_duration"; fi
    if [ "$prev_target" = "__UNSET__" ]; then unset NIPC_BENCH_SERVER_WARMUP_TARGET_RPS; else export NIPC_BENCH_SERVER_WARMUP_TARGET_RPS="$prev_target"; fi
    if [ "$prev_depth" = "__UNSET__" ]; then unset NIPC_BENCH_SERVER_WARMUP_DEPTH; else export NIPC_BENCH_SERVER_WARMUP_DEPTH="$prev_depth"; fi

    [ "$rc" -eq 0 ] || return "$rc"
    printf '%s\n' "$pid"
}

run_selected_measurement() {
    set +e
    "$@"
    local rc=$?
    set -e

    if [ "$rc" -eq 1 ]; then
        RUN_FAILED=1
    fi
    if [ "$rc" -ne 2 ]; then
        sleep "$ROW_SETTLE_SEC"
    fi

    return 0
}

target_rps_label() {
    local target_rps="$1"
    if [ "$target_rps" = "0" ]; then
        printf "max"
    else
        printf "%s/s" "$target_rps"
    fi
}

sanitize_path_component() {
    printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

reset_last_measurement() {
    LAST_MEASUREMENT_STATUS=""
    LAST_MEASUREMENT_REASON=""
    LAST_MEASUREMENT_SAMPLE_FILE=""
    LAST_MEASUREMENT_THROUGHPUT=""
    LAST_MEASUREMENT_P50=""
    LAST_MEASUREMENT_P95=""
    LAST_MEASUREMENT_P99=""
    LAST_MEASUREMENT_CLIENT_CPU=""
    LAST_MEASUREMENT_SERVER_CPU=""
    LAST_MEASUREMENT_TOTAL_CPU=""
    LAST_MEASUREMENT_STABLE_SAMPLES=""
    LAST_MEASUREMENT_STABLE_MIN=""
    LAST_MEASUREMENT_STABLE_MAX=""
    LAST_MEASUREMENT_STABLE_RATIO=""
    LAST_MEASUREMENT_RAW_MIN=""
    LAST_MEASUREMENT_RAW_MAX=""
    LAST_MEASUREMENT_RAW_RATIO=""
    LAST_MEASUREMENT_ARTIFACT_DIR=""
}

sample_count_from_sample_file() {
    local sample_file="$1"
    if [ ! -f "$sample_file" ]; then
        echo "0"
        return
    fi
    awk 'END { print (NR > 1 ? NR - 1 : 0) }' "$sample_file"
}

ensure_diagnostics_summary_file() {
    if [ -n "$DIAGNOSTIC_SUMMARY_FILE" ]; then
        return
    fi

    DIAGNOSTIC_SUMMARY_FILE="${ROOT_RUN_DIR}/diagnostics-summary.txt"
    mkdir -p "$ROOT_RUN_DIR"
    cat > "$DIAGNOSTIC_SUMMARY_FILE" <<EOF
Windows benchmark diagnostic reruns
root_run_dir=${ROOT_RUN_DIR}

EOF
}

append_diagnostics_summary() {
    ensure_diagnostics_summary_file
    printf '%s\n' "$*" >> "$DIAGNOSTIC_SUMMARY_FILE"
}

diagnostic_run_dir() {
    local scenario="$1"
    local client="$2"
    local server="$3"
    local target_rps="$4"

    DIAGNOSTIC_FAILURE_COUNT=$((DIAGNOSTIC_FAILURE_COUNT + 1))
    DIAGNOSTIC_RUN_DIR="${ROOT_RUN_DIR}/diagnostics/$(printf '%03d' "$DIAGNOSTIC_FAILURE_COUNT")-$(sanitize_path_component "$scenario")-$(sanitize_path_component "$client")-$(sanitize_path_component "$server")-$(sanitize_path_component "$target_rps")"
}

sample_duration_for_target() {
    local scenario="$1"
    local target_rps="$2"
    local fixed_duration="$3"
    if [ "$target_rps" != "0" ]; then
        printf '%s' "$fixed_duration"
        return
    fi

    case "$scenario" in
        np-pipeline-batch-d*)
            printf '%s' "$PIPELINE_BATCH_MAX_DURATION"
            ;;
        *)
            printf '%s' "$MAX_DURATION"
            ;;
    esac
}

sample_file_path() {
    local scenario="$1"
    local client="$2"
    local server="$3"
    local target_rps="$4"
    printf '%s/samples-%s-%s-%s-%s.csv' "$RUN_DIR" "$scenario" "$client" "$server" "$target_rps"
}

measurement_artifact_dir() {
    local scenario="$1"
    local client="$2"
    local server="$3"
    local target_rps="$4"
    local repeat_idx="$5"
    printf '%s/artifacts/%s-%s-%s-%s/repeat-%03d' \
        "$RUN_DIR" \
        "$(sanitize_path_component "$scenario")" \
        "$(sanitize_path_component "$client")" \
        "$(sanitize_path_component "$server")" \
        "$(sanitize_path_component "$target_rps")" \
        "$repeat_idx"
}

init_sample_file() {
    local sample_file="$1"
    echo "repeat,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct" > "$sample_file"
}

append_sample_file() {
    local sample_file="$1"
    local repeat_idx="$2"
    local metrics="$3"
    printf '%s,%s\n' "$repeat_idx" "$metrics" >> "$sample_file"
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

aggregate_sample_file() {
    local sample_file="$1"
    local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct

    throughput=$(median_from_sample_file "$sample_file" 2) || return 1
    p50=$(median_from_sample_file "$sample_file" 3) || return 1
    p95=$(median_from_sample_file "$sample_file" 4) || return 1
    p99=$(median_from_sample_file "$sample_file" 5) || return 1
    client_cpu=$(median_from_sample_file "$sample_file" 6) || return 1
    server_cpu_pct=$(median_from_sample_file "$sample_file" 7) || return 1
    total_cpu_pct=$(median_from_sample_file "$sample_file" 8) || return 1

    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

throughput_ratio_is_acceptable() {
    awk -v ratio="$1" -v max_ratio="$2" 'BEGIN { exit ((ratio + 0) <= (max_ratio + 0) ? 0 : 1) }'
}

lookup_prior_max_throughput() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"

    awk -F',' -v scenario="$scenario" -v client="$client_lang" -v server="$server_lang" '
        NR > 1 && $1 == scenario && $2 == client && $3 == server && $4 == "0" {
            print $5
            exit
        }
    ' "$OUTPUT_CSV"
}

fixed_rate_target_is_oversubscribed() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"

    [ "$target_rps" != "0" ] || return 1

    local prior_max
    prior_max=$(lookup_prior_max_throughput "$scenario" "$client_lang" "$server_lang")
    [ -n "$prior_max" ] || return 1

    awk -v target_rps="$target_rps" -v prior_max="$prior_max" '
        BEGIN { exit(((target_rps + 0) > (prior_max + 0)) ? 0 : 1) }
    '
}

throughput_stability_from_sample_file() {
    local sample_file="$1"
    awk -F',' 'NR > 1 { print $2 + 0 }' "$sample_file" | sort -g | awk '
        {
            values[++n] = $1
        }
        END {
            if (n == 0) {
                exit 1
            }

            start = 1
            stop = n
            if (n >= 5) {
                start = 2
                stop = n - 1
            }

            stable_n = stop - start + 1
            if (stable_n <= 0) {
                exit 1
            }

            stable_min = values[start]
            stable_max = values[stop]
            raw_min = values[1]
            raw_max = values[n]

            if (stable_min <= 0) {
                stable_ratio = 999999.0
            } else {
                stable_ratio = stable_max / stable_min
            }

            if (raw_min <= 0) {
                raw_ratio = 999999.0
            } else {
                raw_ratio = raw_max / raw_min
            }

            printf "%d,%d,%.3f,%.3f,%.6f,%.3f,%.3f,%.6f\n", stable_n, start - 1, stable_min, stable_max, stable_ratio, raw_min, raw_max, raw_ratio
        }
    '
}

raw_ratio_outlier_is_publishable() {
    local trimmed_each_side="$1"

    [ "$ALLOW_TRIMMED_UNSTABLE_RAW" = "1" ] || return 1
    [ "$trimmed_each_side" -gt 0 ] || return 1
    return 0
}

maybe_diagnose_failed_measurement() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local duration="$5"
    local original_sample_file="$6"
    shift 6
    local measure_fn="$1"
    shift

    if [ "$DIAGNOSE_FAILURES" != "1" ] || [ "$ACTIVE_DIAGNOSTIC" = "1" ]; then
        return
    fi

    local original_run_dir="$RUN_DIR"
    local original_status="$LAST_MEASUREMENT_STATUS"
    local original_reason="$LAST_MEASUREMENT_REASON"
    local original_throughput="$LAST_MEASUREMENT_THROUGHPUT"
    local original_p50="$LAST_MEASUREMENT_P50"
    local original_p95="$LAST_MEASUREMENT_P95"
    local original_p99="$LAST_MEASUREMENT_P99"
    local original_client_cpu="$LAST_MEASUREMENT_CLIENT_CPU"
    local original_server_cpu="$LAST_MEASUREMENT_SERVER_CPU"
    local original_total_cpu="$LAST_MEASUREMENT_TOTAL_CPU"
    local original_stable_samples="$LAST_MEASUREMENT_STABLE_SAMPLES"
    local original_stable_min="$LAST_MEASUREMENT_STABLE_MIN"
    local original_stable_max="$LAST_MEASUREMENT_STABLE_MAX"
    local original_stable_ratio="$LAST_MEASUREMENT_STABLE_RATIO"
    local original_raw_min="$LAST_MEASUREMENT_RAW_MIN"
    local original_raw_max="$LAST_MEASUREMENT_RAW_MAX"
    local original_raw_ratio="$LAST_MEASUREMENT_RAW_RATIO"
    local original_artifact_dir="$LAST_MEASUREMENT_ARTIFACT_DIR"
    local original_sample_count
    original_sample_count=$(sample_count_from_sample_file "$original_sample_file")

    diagnostic_run_dir "$scenario" "$client_lang" "$server_lang" "$target_rps"
    local diag_run_dir="$DIAGNOSTIC_RUN_DIR"
    mkdir -p "$diag_run_dir"

    warn "  Diagnostic rerun enabled for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
    warn "  First-attempt failure remains authoritative"
    warn "  First-attempt evidence: run_dir=${original_run_dir}"
    if [ -n "$original_artifact_dir" ]; then
        warn "  First-attempt artifacts: ${original_artifact_dir}"
    fi
    warn "  First-attempt sample file: ${original_sample_file}"
    if [ -n "$original_reason" ]; then
        warn "  First-attempt reason: ${original_reason}"
    fi
    warn "  Diagnostic rerun directory: ${diag_run_dir}"

    append_diagnostics_summary "---"
    append_diagnostics_summary "scenario=${scenario} client=${client_lang} server=${server_lang} target_rps=${target_rps} duration=${duration}"
    append_diagnostics_summary "first_attempt.status=${original_status:-failed}"
    append_diagnostics_summary "first_attempt.reason=${original_reason:-unknown}"
    append_diagnostics_summary "first_attempt.run_dir=${original_run_dir}"
    append_diagnostics_summary "first_attempt.artifact_dir=${original_artifact_dir}"
    append_diagnostics_summary "first_attempt.sample_file=${original_sample_file}"
    append_diagnostics_summary "first_attempt.sample_count=${original_sample_count}"
    append_diagnostics_summary "first_attempt.stable_samples=${original_stable_samples:-}"
    append_diagnostics_summary "first_attempt.stable_min=${original_stable_min:-}"
    append_diagnostics_summary "first_attempt.stable_max=${original_stable_max:-}"
    append_diagnostics_summary "first_attempt.stable_ratio=${original_stable_ratio:-}"
    append_diagnostics_summary "first_attempt.raw_min=${original_raw_min:-}"
    append_diagnostics_summary "first_attempt.raw_max=${original_raw_max:-}"
    append_diagnostics_summary "first_attempt.raw_ratio=${original_raw_ratio:-}"

    local saved_run_dir="$RUN_DIR"
    local saved_measure_run_dir="$MEASURE_RUN_DIR"
    local saved_emit_csv_rows="$EMIT_CSV_ROWS"
    local saved_active_diagnostic="$ACTIVE_DIAGNOSTIC"

    RUN_DIR="$diag_run_dir"
    MEASURE_RUN_DIR="$RUN_DIR"
    EMIT_CSV_ROWS=0
    ACTIVE_DIAGNOSTIC=1
    mkdir -p "$RUN_DIR"

    if run_repeated_measurement "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$measure_fn" "$@"; then
        warn "  Diagnostic rerun succeeded"
    else
        warn "  Diagnostic rerun also failed"
    fi

    local diag_status="$LAST_MEASUREMENT_STATUS"
    local diag_reason="$LAST_MEASUREMENT_REASON"
    local diag_sample_file="$LAST_MEASUREMENT_SAMPLE_FILE"
    local diag_sample_count
    diag_sample_count=$(sample_count_from_sample_file "$diag_sample_file")

    warn "  Diagnostic sample file: ${diag_sample_file}"
    if [ -n "$LAST_MEASUREMENT_THROUGHPUT" ]; then
        warn "  Diagnostic median_throughput=${LAST_MEASUREMENT_THROUGHPUT} stable_ratio=${LAST_MEASUREMENT_STABLE_RATIO}"
    elif [ -n "$LAST_MEASUREMENT_STABLE_RATIO" ]; then
        warn "  Diagnostic stable_ratio=${LAST_MEASUREMENT_STABLE_RATIO}"
    fi

    append_diagnostics_summary "diagnostic.status=${diag_status:-failed}"
    append_diagnostics_summary "diagnostic.reason=${diag_reason:-unknown}"
    append_diagnostics_summary "diagnostic.run_dir=${diag_run_dir}"
    append_diagnostics_summary "diagnostic.sample_file=${diag_sample_file}"
    append_diagnostics_summary "diagnostic.sample_count=${diag_sample_count}"
    append_diagnostics_summary "diagnostic.throughput=${LAST_MEASUREMENT_THROUGHPUT:-}"
    append_diagnostics_summary "diagnostic.p50=${LAST_MEASUREMENT_P50:-}"
    append_diagnostics_summary "diagnostic.p95=${LAST_MEASUREMENT_P95:-}"
    append_diagnostics_summary "diagnostic.p99=${LAST_MEASUREMENT_P99:-}"
    append_diagnostics_summary "diagnostic.client_cpu=${LAST_MEASUREMENT_CLIENT_CPU:-}"
    append_diagnostics_summary "diagnostic.server_cpu=${LAST_MEASUREMENT_SERVER_CPU:-}"
    append_diagnostics_summary "diagnostic.total_cpu=${LAST_MEASUREMENT_TOTAL_CPU:-}"
    append_diagnostics_summary "diagnostic.stable_samples=${LAST_MEASUREMENT_STABLE_SAMPLES:-}"
    append_diagnostics_summary "diagnostic.stable_min=${LAST_MEASUREMENT_STABLE_MIN:-}"
    append_diagnostics_summary "diagnostic.stable_max=${LAST_MEASUREMENT_STABLE_MAX:-}"
    append_diagnostics_summary "diagnostic.stable_ratio=${LAST_MEASUREMENT_STABLE_RATIO:-}"
    append_diagnostics_summary "diagnostic.raw_min=${LAST_MEASUREMENT_RAW_MIN:-}"
    append_diagnostics_summary "diagnostic.raw_max=${LAST_MEASUREMENT_RAW_MAX:-}"
    append_diagnostics_summary "diagnostic.raw_ratio=${LAST_MEASUREMENT_RAW_RATIO:-}"
    append_diagnostics_summary ""

    LAST_MEASUREMENT_STATUS="$original_status"
    LAST_MEASUREMENT_REASON="$original_reason"
    LAST_MEASUREMENT_SAMPLE_FILE="$original_sample_file"
    LAST_MEASUREMENT_THROUGHPUT="$original_throughput"
    LAST_MEASUREMENT_P50="$original_p50"
    LAST_MEASUREMENT_P95="$original_p95"
    LAST_MEASUREMENT_P99="$original_p99"
    LAST_MEASUREMENT_CLIENT_CPU="$original_client_cpu"
    LAST_MEASUREMENT_SERVER_CPU="$original_server_cpu"
    LAST_MEASUREMENT_TOTAL_CPU="$original_total_cpu"
    LAST_MEASUREMENT_STABLE_SAMPLES="$original_stable_samples"
    LAST_MEASUREMENT_STABLE_MIN="$original_stable_min"
    LAST_MEASUREMENT_STABLE_MAX="$original_stable_max"
    LAST_MEASUREMENT_STABLE_RATIO="$original_stable_ratio"
    LAST_MEASUREMENT_RAW_MIN="$original_raw_min"
    LAST_MEASUREMENT_RAW_MAX="$original_raw_max"
    LAST_MEASUREMENT_RAW_RATIO="$original_raw_ratio"
    LAST_MEASUREMENT_ARTIFACT_DIR="$original_artifact_dir"

    RUN_DIR="$saved_run_dir"
    MEASURE_RUN_DIR="$saved_measure_run_dir"
    EMIT_CSV_ROWS="$saved_emit_csv_rows"
    ACTIVE_DIAGNOSTIC="$saved_active_diagnostic"
}

run_repeated_measurement() {
    local scenario="$1"
    local client_lang="$2"
    local server_lang="$3"
    local target_rps="$4"
    local duration="$5"
    local measure_fn="$6"
    shift 6

    reset_last_measurement

    local sample_file
    sample_file=$(sample_file_path "$scenario" "$client_lang" "$server_lang" "$target_rps")
    LAST_MEASUREMENT_SAMPLE_FILE="$sample_file"
    init_sample_file "$sample_file"

    local repeat_idx
    for repeat_idx in $(seq 1 "$REPETITIONS"); do
        MEASURE_RUN_DIR=$(measurement_artifact_dir "$scenario" "$client_lang" "$server_lang" "$target_rps" "$repeat_idx")
        LAST_MEASUREMENT_ARTIFACT_DIR="$MEASURE_RUN_DIR"
        mkdir -p "$MEASURE_RUN_DIR"
        log "    sample ${repeat_idx}/${REPETITIONS} (artifacts: ${MEASURE_RUN_DIR})"
        local metrics
        if ! metrics=$("$measure_fn" "$@"); then
            LAST_MEASUREMENT_STATUS="failed"
            LAST_MEASUREMENT_REASON="measurement_command_failed_repeat_${repeat_idx}"
            export NIPC_KEEP_RUN_DIR=1
            maybe_diagnose_failed_measurement \
                "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
                "$measure_fn" "$@"
            return 1
        fi
        append_sample_file "$sample_file" "$repeat_idx" "$metrics"
    done

    local aggregate
    if ! aggregate=$(aggregate_sample_file "$sample_file"); then
        LAST_MEASUREMENT_STATUS="failed"
        LAST_MEASUREMENT_REASON="aggregate_failed"
        maybe_diagnose_failed_measurement \
            "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
            "$measure_fn" "$@"
        return 1
    fi

    local throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct
    IFS=',' read -r throughput p50 p95 p99 client_cpu server_cpu_pct total_cpu_pct <<< "$aggregate"
    LAST_MEASUREMENT_THROUGHPUT="$throughput"
    LAST_MEASUREMENT_P50="$p50"
    LAST_MEASUREMENT_P95="$p95"
    LAST_MEASUREMENT_P99="$p99"
    LAST_MEASUREMENT_CLIENT_CPU="$client_cpu"
    LAST_MEASUREMENT_SERVER_CPU="$server_cpu_pct"
    LAST_MEASUREMENT_TOTAL_CPU="$total_cpu_pct"

    local stability
    if ! stability=$(throughput_stability_from_sample_file "$sample_file"); then
        LAST_MEASUREMENT_STATUS="failed"
        LAST_MEASUREMENT_REASON="stability_analysis_failed"
        maybe_diagnose_failed_measurement \
            "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
            "$measure_fn" "$@"
        return 1
    fi

    local stable_samples trimmed_each_side stable_min stable_max stable_ratio raw_min raw_max raw_ratio
    IFS=',' read -r stable_samples trimmed_each_side stable_min stable_max stable_ratio raw_min raw_max raw_ratio <<< "$stability"
    LAST_MEASUREMENT_STABLE_SAMPLES="$stable_samples"
    LAST_MEASUREMENT_STABLE_MIN="$stable_min"
    LAST_MEASUREMENT_STABLE_MAX="$stable_max"
    LAST_MEASUREMENT_STABLE_RATIO="$stable_ratio"
    LAST_MEASUREMENT_RAW_MIN="$raw_min"
    LAST_MEASUREMENT_RAW_MAX="$raw_max"
    LAST_MEASUREMENT_RAW_RATIO="$raw_ratio"

    if [ "$stable_samples" -lt "$MIN_STABLE_SAMPLES" ]; then
        LAST_MEASUREMENT_STATUS="failed"
        LAST_MEASUREMENT_REASON="insufficient_stable_samples"
        warn "  Unstable repeated throughput for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
        warn "  stable_samples=${stable_samples} stable_min=${stable_min} stable_max=${stable_max} stable_ratio=${stable_ratio}"
        warn "  Required: stable_samples >= ${MIN_STABLE_SAMPLES}"
        warn "  Per-repeat samples: ${sample_file}"
        export NIPC_KEEP_RUN_DIR=1
        maybe_diagnose_failed_measurement \
            "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
            "$measure_fn" "$@"
        return 1
    fi

    if ! throughput_ratio_is_acceptable "$stable_ratio" "$MAX_THROUGHPUT_RATIO"; then
        LAST_MEASUREMENT_STATUS="failed"
        LAST_MEASUREMENT_REASON="stable_ratio_exceeded"
        warn "  Unstable repeated throughput for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
        warn "  stable_min=${stable_min} stable_max=${stable_max} stable_ratio=${stable_ratio} (max ${MAX_THROUGHPUT_RATIO})"
        warn "  Per-repeat samples: ${sample_file}"
        export NIPC_KEEP_RUN_DIR=1
        maybe_diagnose_failed_measurement \
            "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
            "$measure_fn" "$@"
        return 1
    fi

    if ! throughput_ratio_is_acceptable "$raw_ratio" "$MAX_THROUGHPUT_RATIO"; then
        if fixed_rate_target_is_oversubscribed "$scenario" "$client_lang" "$server_lang" "$target_rps"; then
            local prior_max
            prior_max=$(lookup_prior_max_throughput "$scenario" "$client_lang" "$server_lang")
            warn "  Oversubscribed fixed-rate row for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
            warn "  prior_max=${prior_max} target_rps=${target_rps} raw_min=${raw_min} raw_max=${raw_max} raw_ratio=${raw_ratio}"
            warn "  Keeping the row because the target exceeds measured max capacity and the trimmed stable core remains publishable"
        elif raw_ratio_outlier_is_publishable "$trimmed_each_side"; then
            warn "  Raw repeated throughput outlier for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
            warn "  raw_min=${raw_min} raw_max=${raw_max} raw_ratio=${raw_ratio} (max ${MAX_THROUGHPUT_RATIO})"
            warn "  stable_min=${stable_min} stable_max=${stable_max} stable_ratio=${stable_ratio}"
            warn "  Keeping the row because NIPC_BENCH_ALLOW_TRIMMED_UNSTABLE_RAW=1 and the trimmed stable core remains publishable"
            warn "  Per-repeat samples: ${sample_file}"
        else
            LAST_MEASUREMENT_STATUS="failed"
            LAST_MEASUREMENT_REASON="raw_ratio_exceeded"
            warn "  Unstable repeated throughput for ${scenario} ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps")"
            warn "  raw_min=${raw_min} raw_max=${raw_max} raw_ratio=${raw_ratio} (max ${MAX_THROUGHPUT_RATIO})"
            if [ "$trimmed_each_side" -gt 0 ]; then
                warn "  stable_min=${stable_min} stable_max=${stable_max} stable_ratio=${stable_ratio}"
                warn "  Diagnostic note: the trimmed stable core is no longer publishable when raw throughput is unstable"
            fi
            warn "  Per-repeat samples: ${sample_file}"
            export NIPC_KEEP_RUN_DIR=1
            maybe_diagnose_failed_measurement \
                "$scenario" "$client_lang" "$server_lang" "$target_rps" "$duration" "$sample_file" \
                "$measure_fn" "$@"
            return 1
        fi
    fi

    LAST_MEASUREMENT_STATUS="success"
    LAST_MEASUREMENT_REASON=""
    if [ "$EMIT_CSV_ROWS" -eq 1 ]; then
        write_csv_row "$scenario" "$client_lang" "$server_lang" "$target_rps" \
            "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
    fi

    log "    median_throughput=${throughput} p50=${p50}us p95=${p95}us p99=${p99}us stable_ratio=${stable_ratio}"
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

remove_server_pid() {
    local target_pid="$1"
    local new_pids=()
    local p
    for p in "${SERVER_PIDS[@]:-}"; do
        [ "$p" != "$target_pid" ] && new_pids+=("$p")
    done
    SERVER_PIDS=("${new_pids[@]:-}")
}

measure_pair_once() {
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

    local server_duration="$duration"
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"
    local runtime_dir_bin
    runtime_dir_bin="$(runtime_dir_for_windows_bin "$runtime_dir")"
    local repeat_tag
    repeat_tag=$(basename "$runtime_dir")
    local svc_name="${scenario}-${server_lang}-${client_lang}-${target_rps}-${repeat_tag}"
    local server_out="${runtime_dir}/server-${server_lang}-${svc_name}.out"
    local client_bin
    client_bin="$(bench_bin "$client_lang")"

    local server_pid
    if [ "$scenario" = "np-ping-pong" ] || \
       [ "$scenario" = "shm-ping-pong" ] || \
       [ "$scenario" = "np-batch-ping-pong" ] || \
       [ "$scenario" = "snapshot-baseline" ] || \
       [ "$scenario" = "snapshot-shm" ]; then
        server_pid=$(
            start_server_with_warmup \
                "$client_bin" \
                "$client_subcmd" \
                "$target_rps" \
                "" \
                "$server_lang" \
                "$server_subcmd" \
                "$svc_name" \
                "$server_duration"
        ) || return 1
    else
        server_pid=$(start_server "$server_lang" "$server_subcmd" "$svc_name" "$server_duration") || return 1
    fi

    sleep 0.2

    local client_timeout=$((duration + 15))
    local client_output
    local client_status
    local client_err="${runtime_dir}/client-${scenario}-${server_lang}-${client_lang}-${target_rps}.err"
    set +e
    client_output=$(timeout "$client_timeout" "$client_bin" "$client_subcmd" "$runtime_dir_bin" "$svc_name" "$duration" "$target_rps" 2>"$client_err")
    client_status=$?
    set -e

    if [ "$client_status" -ne 0 ]; then
        warn "  ${client_lang} client failed for ${scenario} (exit ${client_status})"
        dump_client_error "$client_err"
        dump_client_output "$client_output"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        stop_server "$server_pid" "$server_lang" "$svc_name" >/dev/null
        remove_server_pid "$server_pid"
        return 1
    fi

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} client for ${scenario}"
        dump_client_error "$client_err"
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        stop_server "$server_pid" "$server_lang" "$svc_name" >/dev/null
        remove_server_pid "$server_pid"
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
        stop_server "$server_pid" "$server_lang" "$svc_name" >/dev/null
        remove_server_pid "$server_pid"
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
        stop_server "$server_pid" "$server_lang" "$svc_name" >/dev/null
        remove_server_pid "$server_pid"
        return 1
    fi

    local server_cpu_sec
    local stop_status
    set +e
    server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$svc_name")
    stop_status=$?
    set -e
    remove_server_pid "$server_pid"
    if [ "$stop_status" -ne 0 ]; then
        dump_server_output "$server_out"
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    local server_cpu_pct total_cpu_pct
    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$duration")
    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

run_pair() {
    local scenario="$1"
    local server_lang="$2"
    local client_lang="$3"
    local target_rps="$4"
    local duration="$5"

    if ! should_run_row "$scenario" "$client_lang" "$server_lang" "$target_rps"; then
        return 2
    fi

    local effective_duration
    effective_duration=$(sample_duration_for_target "$scenario" "$target_rps" "$duration")

    log "  ${scenario}: ${client_lang}->${server_lang} @ $(target_rps_label "$target_rps") (${REPETITIONS} samples x ${effective_duration}s)"
    run_repeated_measurement "$scenario" "$client_lang" "$server_lang" "$target_rps" "$effective_duration" \
        measure_pair_once "$scenario" "$server_lang" "$client_lang" "$target_rps" "$effective_duration"
}

measure_lookup_once() {
    local lang="$1"
    local duration="$2"
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"

    local bin
    bin="$(bench_bin "$lang")"

    local line
    local lookup_status
    mkdir -p "$runtime_dir"
    local lookup_err="${runtime_dir}/lookup-${lang}.err"
    set +e
    line=$("$bin" lookup-bench "$duration" 2>"$lookup_err" | grep "^lookup," | head -1)
    lookup_status=$?
    set -e

    if [ "$lookup_status" -ne 0 ]; then
        warn "  ${lang} lookup benchmark failed"
        dump_client_error "$lookup_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    if [ -z "$line" ]; then
        warn "  No output from ${lang} lookup benchmark"
        dump_client_error "$lookup_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

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
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

run_lookup_bench() {
    local lang="$1"
    local duration="$2"

    if ! should_run_row "lookup" "$lang" "$lang" "0"; then
        return 2
    fi

    local effective_duration
    effective_duration=$(sample_duration_for_target "lookup" "0" "$duration")

    log "  lookup: ${lang} (${REPETITIONS} samples x ${effective_duration}s)"
    run_repeated_measurement "lookup" "$lang" "$lang" "0" "$effective_duration" \
        measure_lookup_once "$lang" "$effective_duration"
}

measure_np_pipeline_once() {
    local server_lang="$1"
    local client_lang="$2"
    local duration="$3"
    local depth="$4"

    local server_duration="$duration"
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"
    local runtime_dir_bin
    runtime_dir_bin="$(runtime_dir_for_windows_bin "$runtime_dir")"
    local repeat_tag
    repeat_tag=$(basename "$runtime_dir")
    local pipe_svc="pipeline-${server_lang}-${client_lang}-${repeat_tag}"
    local client_bin
    client_bin="$(bench_bin "$client_lang")"
    local server_pid
    server_pid=$(
        start_server_with_warmup \
            "$client_bin" \
            "np-pipeline-client" \
            "0" \
            "$depth" \
            "$server_lang" \
            "np-ping-pong-server" \
            "$pipe_svc" \
            "$server_duration"
    ) || {
        warn "  Failed to start pipeline server"
        return 1
    }

    sleep 0.2

    local client_timeout=$((duration + 15))
    local client_output
    local client_status
    local client_err="${runtime_dir}/client-np-pipeline-${server_lang}-${client_lang}.err"
    set +e
    client_output=$(timeout "$client_timeout" "$client_bin" "np-pipeline-client" "$runtime_dir_bin" "$pipe_svc" "$duration" "0" "$depth" 2>"$client_err")
    client_status=$?
    set -e

    local server_cpu_sec
    local stop_status
    set +e
    server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pipe_svc")
    stop_status=$?
    set -e
    remove_server_pid "$server_pid"
    if [ "$stop_status" -ne 0 ]; then
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    if [ "$client_status" -ne 0 ]; then
        warn "  ${client_lang} pipeline client failed for ${server_lang} server (exit ${client_status})"
        dump_client_error "$client_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} pipeline client for ${server_lang} server"
        warn "  Preserving RUN_DIR for investigation: ${RUN_DIR}"
        export NIPC_KEEP_RUN_DIR=1
        dump_client_error "$client_err"
        return 1
    fi

    local line
    line=$(echo "$client_output" | grep "^np-pipeline" | head -1)
    if [ -z "$line" ]; then
        warn "  Could not parse pipeline output from ${client_lang} client for ${server_lang} server"
        warn "  Raw client output follows:"
        printf '%s\n' "$client_output" >&2
        warn "  Preserving RUN_DIR for investigation: ${RUN_DIR}"
        export NIPC_KEEP_RUN_DIR=1
        dump_client_error "$client_err"
        return 1
    fi

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
        return 1
    fi

    local server_cpu_pct total_cpu_pct
    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$duration")
    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

run_np_pipeline() {
    local server_lang="$1"
    local client_lang="$2"
    local duration="$3"
    local depth="$4"
    local scenario="np-pipeline-d${depth}"

    if ! should_run_row "$scenario" "$client_lang" "$server_lang" "0"; then
        return 2
    fi

    local effective_duration
    effective_duration=$(sample_duration_for_target "$scenario" "0" "$duration")

    log "  np-pipeline: ${client_lang}->${server_lang} depth=${depth} (${REPETITIONS} samples x ${effective_duration}s)"
    run_repeated_measurement "$scenario" "$client_lang" "$server_lang" "0" "$effective_duration" \
        measure_np_pipeline_once "$server_lang" "$client_lang" "$effective_duration" "$depth"
}

measure_np_pipeline_batch_once() {
    local server_lang="$1"
    local client_lang="$2"
    local duration="$3"
    local depth="$4"

    local server_duration="$duration"
    local runtime_dir="${MEASURE_RUN_DIR:-$RUN_DIR}"
    local runtime_dir_bin
    runtime_dir_bin="$(runtime_dir_for_windows_bin "$runtime_dir")"
    local repeat_tag
    repeat_tag=$(basename "$runtime_dir")
    local pb_svc="pipe-batch-${server_lang}-${client_lang}-${repeat_tag}"
    local server_pid
    local client_bin
    client_bin="$(bench_bin "$client_lang")"
    server_pid=$(
        start_server_with_warmup \
            "$client_bin" \
            "np-pipeline-batch-client" \
            "0" \
            "$depth" \
            "$server_lang" \
            "np-batch-ping-pong-server" \
            "$pb_svc" \
            "$server_duration"
    ) || {
        warn "  Failed to start pipeline-batch server"
        return 1
    }

    sleep 0.2

    local client_timeout=$((duration + 15))
    local client_output
    local client_status
    local client_err="${runtime_dir}/client-np-pipeline-batch-${server_lang}-${client_lang}.err"
    set +e
    client_output=$(timeout "$client_timeout" "$client_bin" "np-pipeline-batch-client" "$runtime_dir_bin" "$pb_svc" "$duration" "0" "$depth" 2>"$client_err")
    client_status=$?
    set -e

    local server_cpu_sec
    local stop_status
    set +e
    server_cpu_sec=$(stop_server "$server_pid" "$server_lang" "$pb_svc")
    stop_status=$?
    set -e
    remove_server_pid "$server_pid"
    if [ "$stop_status" -ne 0 ]; then
        dump_bench_processes
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    if [ "$client_status" -ne 0 ]; then
        warn "  ${client_lang} pipeline-batch client failed for ${server_lang} server (exit ${client_status})"
        dump_client_error "$client_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    if [ -z "$client_output" ]; then
        warn "  No output from ${client_lang} pipeline-batch client for ${server_lang} server"
        dump_client_error "$client_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    local line
    line=$(echo "$client_output" | grep "^np-pipeline-batch" | head -1)
    if [ -z "$line" ]; then
        warn "  Could not parse pipeline-batch output from ${client_lang} client for ${server_lang} server"
        dump_client_error "$client_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    local throughput p50 p95 p99 client_cpu
    throughput=$(echo "$line" | cut -d',' -f4)
    p50=$(echo "$line" | cut -d',' -f5)
    p95=$(echo "$line" | cut -d',' -f6)
    p99=$(echo "$line" | cut -d',' -f7)
    client_cpu=$(echo "$line" | cut -d',' -f8)

    if ! throughput_is_positive "$throughput"; then
        warn "  Invalid zero throughput from ${client_lang} pipeline-batch client for ${server_lang} server"
        dump_client_error "$client_err"
        export NIPC_KEEP_RUN_DIR=1
        return 1
    fi

    local server_cpu_pct total_cpu_pct
    server_cpu_pct=$(cpu_pct_for_duration "$server_cpu_sec" "$duration")
    total_cpu_pct=$(sum_cpu_pct "$client_cpu" "$server_cpu_pct")

    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu_pct" "$total_cpu_pct"
}

run_np_pipeline_batch() {
    local server_lang="$1"
    local client_lang="$2"
    local duration="$3"
    local depth="$4"
    local scenario="np-pipeline-batch-d${depth}"

    if ! should_run_row "$scenario" "$client_lang" "$server_lang" "0"; then
        return 2
    fi

    local effective_duration
    effective_duration=$(sample_duration_for_target "$scenario" "0" "$duration")

    log "  np-pipeline-batch: ${client_lang}->${server_lang} depth=${depth} (${REPETITIONS} samples x ${effective_duration}s)"
    run_repeated_measurement "$scenario" "$client_lang" "$server_lang" "0" "$effective_duration" \
        measure_np_pipeline_batch_once "$server_lang" "$client_lang" "$effective_duration" "$depth"
}

# ---------------------------------------------------------------------------
#  Check prerequisites
# ---------------------------------------------------------------------------

HAS_RUST=0

refresh_rust_bench_availability() {
    if [ -x "$BENCH_RS" ]; then
        HAS_RUST=1
    else
        HAS_RUST=0
    fi
}

check_binaries() {
    local ok=0

    ensure_bench_build
    refresh_rust_bench_availability

    if [ ! -x "$BENCH_C" ]; then
        err "C benchmark binary not found after build: $BENCH_C"
        ok=1
    fi

    if [ ! -x "$BENCH_GO" ]; then
        err "Go benchmark binary not found after build: $BENCH_GO"
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
    require_positive_integer "DURATION" "$DURATION"
    require_positive_integer "NIPC_BENCH_REPETITIONS" "$REPETITIONS"
    require_positive_integer "NIPC_BENCH_MAX_DURATION" "$MAX_DURATION"
    require_positive_integer "NIPC_BENCH_PIPELINE_BATCH_MAX_DURATION" "$PIPELINE_BATCH_MAX_DURATION"
    require_positive_integer "NIPC_BENCH_MIN_STABLE_SAMPLES" "$MIN_STABLE_SAMPLES"
    require_positive_integer "NIPC_BENCH_SERVER_STOP_GRACE_SEC" "$SERVER_STOP_GRACE_SEC"
    require_positive_number "NIPC_BENCH_MAX_THROUGHPUT_RATIO" "$MAX_THROUGHPUT_RATIO"
    require_positive_number "NIPC_BENCH_ROW_SETTLE_SEC" "$ROW_SETTLE_SEC"
    require_zero_or_one "NIPC_BENCH_ALLOW_TRIMMED_UNSTABLE_RAW" "$ALLOW_TRIMMED_UNSTABLE_RAW"
    require_zero_or_one "NIPC_BENCH_DIAGNOSE_FAILURES" "$DIAGNOSE_FAILURES"
    log "Windows Benchmark Suite"
    log "Windows C toolchain mode: ${WINDOWS_TOOLCHAIN}"
    log "Duration per fixed-rate sample: ${DURATION}s"
    log "Duration per max-tier sample: ${MAX_DURATION}s"
    log "Duration per pipeline-batch max-tier sample: ${PIPELINE_BATCH_MAX_DURATION}s"
    log "Samples per published row: ${REPETITIONS}"
    log "Server stop grace before forced kill: ${SERVER_STOP_GRACE_SEC}s"
    log "Row settle barrier: ${ROW_SETTLE_SEC}s"
    log "Max allowed stable throughput ratio: ${MAX_THROUGHPUT_RATIO}"
    log "Minimum stable samples required: ${MIN_STABLE_SAMPLES}"
    log "Allow trimmed raw-outlier publication: $( [ "$ALLOW_TRIMMED_UNSTABLE_RAW" = "1" ] && printf enabled || printf disabled )"
    log "Diagnostic reruns for failed rows: $( [ "$DIAGNOSE_FAILURES" = "1" ] && printf enabled || printf disabled )"
    log "Scenario filter: ${SCENARIO_FILTER:-<all>}"
    log "Client filter: ${CLIENT_FILTER:-<all>}"
    log "Server filter: ${SERVER_FILTER:-<all>}"
    log "Target filter: ${TARGET_FILTER:-<all>}"
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
                    run_selected_measurement run_pair "np-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"
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
                    run_selected_measurement run_pair "shm-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"
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
                    run_selected_measurement run_pair "snapshot-baseline" "$server_lang" "$client_lang" "$rate" "$DURATION"
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
                    run_selected_measurement run_pair "snapshot-shm" "$server_lang" "$client_lang" "$rate" "$DURATION"
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
                    run_selected_measurement run_pair "np-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"
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
                    run_selected_measurement run_pair "shm-batch-ping-pong" "$server_lang" "$client_lang" "$rate" "$DURATION"
                done
            done
        done
    fi

    # 7. Local cache lookup
    if run_block 7; then
        log "=== Local Cache Lookup ==="
        for lang in "${LANGS[@]}"; do
            run_selected_measurement run_lookup_bench "$lang" "$DURATION"
        done
    fi

    # 8. NP pipeline: N pairs x 1 rate (max), depth=16
    if run_block 8; then
        log "=== NP Pipeline (depth=${PIPELINE_DEPTH}) ==="
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_selected_measurement run_np_pipeline "$server_lang" "$client_lang" "$DURATION" "$PIPELINE_DEPTH"
            done
        done
    fi

    # 9. NP pipeline+batch: N pairs x 1 rate (max), depth=16
    if run_block 9; then
        log "=== NP Pipeline+Batch (depth=${PIPELINE_DEPTH}) ==="
        for server_lang in "${LANGS[@]}"; do
            for client_lang in "${LANGS[@]}"; do
                run_selected_measurement run_np_pipeline_batch "$server_lang" "$client_lang" "$DURATION" "$PIPELINE_DEPTH"
            done
        done
    fi

    if [ "$RUN_FAILED" -ne 0 ]; then
        if [ -n "$DIAGNOSTIC_SUMMARY_FILE" ] && [ -f "$DIAGNOSTIC_SUMMARY_FILE" ]; then
            warn "Diagnostics summary: ${DIAGNOSTIC_SUMMARY_FILE}"
        fi
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

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
