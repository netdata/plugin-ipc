#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGETED_RUNNER="${ROOT_DIR}/tests/run-windows-bench-targeted.sh"
OUT_DIR="${1:-${TEMP:-/tmp}/netipc-windows-toolchain-compare}"
DURATION="${2:-3}"
REPETITIONS="${NIPC_BENCH_COMPARE_REPETITIONS:-5}"
ROW_SETTLE_SEC="${NIPC_BENCH_COMPARE_ROW_SETTLE_SEC:-1}"
MAX_DURATION="${NIPC_BENCH_COMPARE_MAX_DURATION:-${DURATION}}"
PIPELINE_BATCH_MAX_DURATION="${NIPC_BENCH_COMPARE_PIPELINE_BATCH_MAX_DURATION:-${DURATION}}"
MAX_THROUGHPUT_RATIO="${NIPC_BENCH_COMPARE_MAX_THROUGHPUT_RATIO:-2.00}"
ENFORCE_POLICY="${NIPC_BENCH_COMPARE_ENFORCE_POLICY:-1}"
SUMMARY_CSV="${OUT_DIR}/summary.csv"
JOINED_CSV="${OUT_DIR}/joined.csv"
POLICY_CSV="${OUT_DIR}/policy.csv"

COMPARE_CASES=(
  "np-max|np-ping-pong|c|c|0"
  "np-100k|np-ping-pong|c|c|100000"
  "shm-max|shm-ping-pong|c|c|0"
  "shm-100k|shm-ping-pong|c|c|100000"
  "snapshot-np|snapshot-baseline|c|c|0"
  "snapshot-shm|snapshot-shm|c|c|0"
  "lookup|lookup|c|c|0"
  "shm-max-c-client-vs-rust-server|shm-ping-pong|c|rust|0"
  "shm-max-rust-client-vs-c-server|shm-ping-pong|rust|c|0"
  "snapshot-shm-c-client-vs-rust-server|snapshot-shm|c|rust|0"
  "snapshot-shm-rust-client-vs-c-server|snapshot-shm|rust|c|0"
)

run() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"
  "$@"
}

log() {
  printf "${CYAN}[compare]${NC} %s\n" "$*" >&2
}

err() {
  printf "${RED}[compare]${NC} %s\n" "$*" >&2
}

sanitize_label() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

c_role_for_case() {
  local client="$1"
  local server="$2"
  if [ "$client" = "c" ] && [ "$server" = "c" ]; then
    printf 'both'
  elif [ "$client" = "c" ]; then
    printf 'client'
  elif [ "$server" = "c" ]; then
    printf 'server'
  else
    printf 'none'
  fi
}

extract_row_from_csv() {
  local csv="$1"
  if [ ! -f "$csv" ]; then
    err "expected CSV not found: $csv"
    exit 1
  fi

  local row
  row=$(awk 'NR == 2 { print; exit }' "$csv")
  if [ -z "$row" ]; then
    err "CSV did not contain a measurement row: $csv"
    exit 1
  fi

  printf '%s\n' "$row"
}

float_pct() {
  local numerator="$1"
  local denominator="$2"
  awk -v n="$numerator" -v d="$denominator" 'BEGIN {
    if ((d + 0) == 0) {
      printf "0.0"
    } else {
      printf "%.1f", (100.0 * (n + 0)) / (d + 0)
    }
  }'
}

float_delta_pct() {
  local base="$1"
  local value="$2"
  awk -v b="$base" -v v="$value" 'BEGIN {
    if ((b + 0) == 0) {
      printf "0.0"
    } else {
      printf "%.1f", (100.0 * ((v + 0) - (b + 0))) / (b + 0)
    }
  }'
}

float_slowdown_pct() {
  local native="$1"
  local msys="$2"
  awk -v n="$native" -v m="$msys" 'BEGIN {
    if ((n + 0) == 0) {
      printf "0.0"
    } else {
      printf "%.1f", (100.0 * ((n + 0) - (m + 0))) / (n + 0)
    }
  }'
}

pct_meets_floor() {
  local actual_pct="$1"
  local min_pct="$2"
  awk -v actual="$actual_pct" -v min="$min_pct" 'BEGIN {
    exit ((actual + 0) >= (min + 0) ? 0 : 1)
  }'
}

min_msys_pct_for_label() {
  local label="$1"

  case "$label" in
    np-max)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_NP_MAX:-70.0}"
      ;;
    np-100k)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_NP_100K:-70.0}"
      ;;
    shm-max)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SHM_MAX:-85.0}"
      ;;
    shm-100k)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SHM_100K:-95.0}"
      ;;
    snapshot-np)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SNAPSHOT_NP:-80.0}"
      ;;
    snapshot-shm)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SNAPSHOT_SHM_BOTH:-50.0}"
      ;;
    lookup)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_LOOKUP:-60.0}"
      ;;
    shm-max-c-client-vs-rust-server)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SHM_MAX_C_CLIENT:-85.0}"
      ;;
    shm-max-rust-client-vs-c-server)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SHM_MAX_C_SERVER:-85.0}"
      ;;
    snapshot-shm-c-client-vs-rust-server)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SNAPSHOT_SHM_C_CLIENT:-80.0}"
      ;;
    snapshot-shm-rust-client-vs-c-server)
      printf '%s\n' "${NIPC_BENCH_COMPARE_MIN_PCT_SNAPSHOT_SHM_C_SERVER:-50.0}"
      ;;
    *)
      printf '0.0\n'
      ;;
  esac
}

run_toolchain_case() {
  local toolchain="$1"
  local label="$2"
  local scenario="$3"
  local client="$4"
  local server="$5"
  local target="$6"
  local toolchain_out="${OUT_DIR}/${toolchain}"
  local row_arg="${scenario},${client},${server},${target}"

  mkdir -p "$toolchain_out"

  log "Running ${label} with toolchain=${toolchain}"
  run env \
    NIPC_WINDOWS_TOOLCHAIN="$toolchain" \
    NIPC_BENCH_REPETITIONS="$REPETITIONS" \
    NIPC_BENCH_ROW_SETTLE_SEC="$ROW_SETTLE_SEC" \
    NIPC_BENCH_MAX_DURATION="$MAX_DURATION" \
    NIPC_BENCH_PIPELINE_BATCH_MAX_DURATION="$PIPELINE_BATCH_MAX_DURATION" \
    NIPC_BENCH_MAX_THROUGHPUT_RATIO="$MAX_THROUGHPUT_RATIO" \
    NIPC_BENCH_ALLOW_TRIMMED_UNSTABLE_RAW=1 \
    NIPC_BENCH_TARGETED_ATTEMPTS="${NIPC_BENCH_COMPARE_TARGETED_ATTEMPTS:-3}" \
    bash "$TARGETED_RUNNER" --out-dir "$toolchain_out" --duration "$DURATION" --row "$row_arg"
}

run_paired_cases() {
  local case_def label scenario client server target

  for case_def in "${COMPARE_CASES[@]}"; do
    IFS='|' read -r label scenario client server target <<< "$case_def"
    log "Running paired comparison row: ${label}"
    run_toolchain_case mingw64 "$label" "$scenario" "$client" "$server" "$target"
    run_toolchain_case msys "$label" "$scenario" "$client" "$server" "$target"
  done
}

write_summary_csv() {
  local case_def label scenario client server target toolchain csv row c_role
  local row_scenario row_client row_server row_target throughput p50 p95 p99 client_cpu server_cpu total_cpu

  printf 'toolchain,label,scenario,client,server,target_rps,c_role,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct\n' > "$SUMMARY_CSV"

  for toolchain in mingw64 msys; do
    for case_def in "${COMPARE_CASES[@]}"; do
      IFS='|' read -r label scenario client server target <<< "$case_def"
      csv="${OUT_DIR}/${toolchain}/$(sanitize_label "${scenario}-${client}-${server}-${target}").csv"
      row="$(extract_row_from_csv "$csv")"
      IFS=',' read -r row_scenario row_client row_server row_target throughput p50 p95 p99 client_cpu server_cpu total_cpu <<< "$row"
      c_role="$(c_role_for_case "$client" "$server")"

      printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$toolchain" "$label" "$row_scenario" "$row_client" "$row_server" "$row_target" "$c_role" \
        "$throughput" "$p50" "$p95" "$p99" "$client_cpu" "$server_cpu" "$total_cpu" >> "$SUMMARY_CSV"
    done
  done
}

write_joined_csv() {
  local case_def label scenario client server target c_role
  local native_csv msys_csv native_row msys_row
  local n_scenario n_client n_server n_target n_throughput n_p50 n_p95 n_p99 n_client_cpu n_server_cpu n_total_cpu
  local m_scenario m_client m_server m_target m_throughput m_p50 m_p95 m_p99 m_client_cpu m_server_cpu m_total_cpu
  local throughput_pct slowdown_pct p95_delta_pct

  printf 'label,scenario,client,server,target_rps,c_role,mingw64_throughput,msys_throughput,msys_pct_of_mingw64,throughput_slowdown_pct,mingw64_p95_us,msys_p95_us,p95_delta_pct,mingw64_client_cpu_pct,msys_client_cpu_pct\n' > "$JOINED_CSV"

  for case_def in "${COMPARE_CASES[@]}"; do
    IFS='|' read -r label scenario client server target <<< "$case_def"
    c_role="$(c_role_for_case "$client" "$server")"

    native_csv="${OUT_DIR}/mingw64/$(sanitize_label "${scenario}-${client}-${server}-${target}").csv"
    msys_csv="${OUT_DIR}/msys/$(sanitize_label "${scenario}-${client}-${server}-${target}").csv"
    native_row="$(extract_row_from_csv "$native_csv")"
    msys_row="$(extract_row_from_csv "$msys_csv")"

    IFS=',' read -r n_scenario n_client n_server n_target n_throughput n_p50 n_p95 n_p99 n_client_cpu n_server_cpu n_total_cpu <<< "$native_row"
    IFS=',' read -r m_scenario m_client m_server m_target m_throughput m_p50 m_p95 m_p99 m_client_cpu m_server_cpu m_total_cpu <<< "$msys_row"

    throughput_pct="$(float_pct "$m_throughput" "$n_throughput")"
    slowdown_pct="$(float_slowdown_pct "$n_throughput" "$m_throughput")"
    p95_delta_pct="$(float_delta_pct "$n_p95" "$m_p95")"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$label" "$n_scenario" "$n_client" "$n_server" "$n_target" "$c_role" \
      "$n_throughput" "$m_throughput" "$throughput_pct" "$slowdown_pct" \
      "$n_p95" "$m_p95" "$p95_delta_pct" "$n_client_cpu" "$m_client_cpu" >> "$JOINED_CSV"
  done
}

print_joined_table() {
  awk -F',' '
    NR == 1 { next }
    {
      printf "%-38s %-17s %-8s %12s %12s %8s %8s %9s\n",
             $1, $2, $6, $7, $8, $9 "%", $10 "%", $13 "%"
    }
  ' "$JOINED_CSV"
}

write_policy_csv() {
  local label scenario client server target_rps c_role
  local mingw64_throughput msys_throughput msys_pct throughput_slowdown mingw64_p95 msys_p95 p95_delta
  local mingw64_client_cpu msys_client_cpu min_pct status
  local failures=0

  printf 'label,scenario,client,server,target_rps,c_role,min_msys_pct_of_mingw64,actual_msys_pct_of_mingw64,status\n' > "$POLICY_CSV"

  while IFS=',' read -r label scenario client server target_rps c_role \
    mingw64_throughput msys_throughput msys_pct throughput_slowdown \
    mingw64_p95 msys_p95 p95_delta mingw64_client_cpu msys_client_cpu; do

    [ "$label" = "label" ] && continue

    min_pct="$(min_msys_pct_for_label "$label")"
    status="pass"
    if ! pct_meets_floor "$msys_pct" "$min_pct"; then
      status="fail"
      failures=$((failures + 1))
      err "Policy failed for ${label}: msys=${msys_pct}% of mingw64, required >= ${min_pct}%"
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$label" "$scenario" "$client" "$server" "$target_rps" "$c_role" \
      "$min_pct" "$msys_pct" "$status" >> "$POLICY_CSV"
  done < "$JOINED_CSV"

  if [ "$ENFORCE_POLICY" != "1" ]; then
    log "Benchmark comparison policy not enforced (NIPC_BENCH_COMPARE_ENFORCE_POLICY=${ENFORCE_POLICY})"
    return 0
  fi

  if [ "$failures" -ne 0 ]; then
    err "MSYS comparison policy failed (${failures} row(s)); see ${POLICY_CSV}"
    return 1
  fi

  log "MSYS comparison policy passed"
}

main() {
  mkdir -p "$OUT_DIR"

  run_paired_cases

  write_summary_csv
  write_joined_csv
  write_policy_csv

  log "Joined comparison:"
  printf "${CYAN}%-38s %-17s %-8s %12s %12s %8s %8s %9s${NC}\n" \
    "Label" "Scenario" "C role" "mingw64" "msys" "msys%" "slow" "p95 d%"
  print_joined_table

  log "Summary CSV: ${SUMMARY_CSV}"
  log "Joined CSV: ${JOINED_CSV}"
  log "Policy CSV: ${POLICY_CSV}"
}

main "$@"
