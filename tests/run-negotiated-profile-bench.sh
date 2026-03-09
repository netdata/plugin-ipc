#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"

configure_build() {
  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi
  run cmake -S . -B "${CMAKE_BUILD_DIR}"
}

build_targets() {
  if [[ "${NETIPC_SKIP_BUILD:-0}" == "1" ]]; then
    return 0
  fi
  configure_build
  run cmake --build "${CMAKE_BUILD_DIR}" --target "$@"
}

run() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  "$@"
  local exit_code=$?
  if [[ $exit_code -ne 0 ]]; then
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
    echo -e >&2 "${RED}        Full command:${NC} $*"
    echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    return $exit_code
  fi
}

write_csv_results() {
  local output_path=$1
  local output_dir tmp_path

  output_dir=$(dirname "${output_path}")
  mkdir -p "${output_dir}"
  tmp_path="${output_dir}/.$(basename "${output_path}").tmp.$$"

  printf "scenario,profile,throughput_rps,p50_us,client_cpu_cores,server_cpu_cores,total_cpu_cores\n" > "${tmp_path}"
  while IFS='|' read -r scenario profile throughput p50 ccpu scpu total; do
    printf "%s,%s,%s,%s,%s,%s,%s\n" \
      "${scenario}" "${profile}" "${throughput}" "${p50}" "${ccpu}" "${scpu}" "${total}" >> "${tmp_path}"
  done < "${results_file}"

  mv "${tmp_path}" "${output_path}"
}

bench_rust_profile() {
  local scenario=$1
  local target_rps=$2
  local label=$3
  local supported=$4
  local preferred=$5

  local service="netipc-neg-rs-${label}-${target_rps}-${RANDOM}"
  local sock="/tmp/${service}.sock"
  local shm="/tmp/${service}.ipcshm"
  local server_log="/tmp/${service}.log"
  local server_time="/tmp/${service}.time"

  run rm -f "${sock}" "${shm}" "${server_log}" "${server_time}"

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " /usr/bin/time -f "%U %S" -o "${server_time}" timeout 12s env NETIPC_SUPPORTED_PROFILES="${supported}" NETIPC_PREFERRED_PROFILES="${preferred}" "${C_BIN_DIR}/netipc_live_uds_rs" server-loop /tmp "${service}" 0
  printf >&2 "${NC}\n"

  /usr/bin/time -f "%U %S" -o "${server_time}" timeout 12s env NETIPC_SUPPORTED_PROFILES="${supported}" NETIPC_PREFERRED_PROFILES="${preferred}" "${C_BIN_DIR}/netipc_live_uds_rs" server-loop /tmp "${service}" 0 >"${server_log}" 2>&1 &
  local server_pid=$!
  sleep 0.2
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "rust uds server failed to start" >&2
    cat "${server_log}" >&2 || true
    return 1
  fi

  local start_ns end_ns
  start_ns=$(date +%s%N)

  local client_row
  client_row=$(NETIPC_SUPPORTED_PROFILES="${supported}" NETIPC_PREFERRED_PROFILES="${preferred}" "${C_BIN_DIR}/netipc_live_uds_rs" client-bench /tmp "${service}" 5 "${target_rps}" | awk -F, '/^rust-uds,|^rust-shm-hybrid,/ {print $0}')

  end_ns=$(date +%s%N)
  wait "${server_pid}" || true

  local mode duration target req resp mismatches throughput p50 p95 p99 c_cpu ignored_server_cpu ignored_total_cpu
  IFS=',' read -r mode duration target req resp mismatches throughput p50 p95 p99 c_cpu ignored_server_cpu ignored_total_cpu <<<"${client_row}"

  local elapsed_sec server_cpu_sec s_cpu total_cpu
  elapsed_sec=$(awk -v s="${start_ns}" -v e="${end_ns}" 'BEGIN { printf "%.6f", (e-s)/1e9 }')
  server_cpu_sec=$(awk '/^[0-9.]+ [0-9.]+$/ {print $1 + $2; found=1} END {if (!found) print 0}' "${server_time}" 2>/dev/null || echo "0")
  s_cpu=$(awk -v s="${server_cpu_sec}" -v e="${elapsed_sec}" 'BEGIN { if (e <= 0) e = 1e-9; printf "%.3f", s / e }')
  total_cpu=$(awk -v c="${c_cpu}" -v s="${s_cpu}" 'BEGIN { printf "%.3f", c + s }')

  printf "%s|%s|%s|%s|%s|%s|%s\n" "${scenario}" "${label}" "${throughput}" "${p50}" "${c_cpu}" "${s_cpu}" "${total_cpu}"
}

build_targets netipc_live_uds_rs

results_file=$(mktemp)
trap 'rm -f "${results_file}"' EXIT

bench_rust_profile "max" 0 "profile1-uds" 1 1 >> "${results_file}"
bench_rust_profile "max" 0 "profile2-shm" 3 2 >> "${results_file}"

bench_rust_profile "100k/s" 100000 "profile1-uds" 1 1 >> "${results_file}"
bench_rust_profile "100k/s" 100000 "profile2-shm" 3 2 >> "${results_file}"

bench_rust_profile "10k/s" 10000 "profile1-uds" 1 1 >> "${results_file}"
bench_rust_profile "10k/s" 10000 "profile2-shm" 3 2 >> "${results_file}"

printf "\n"
printf "Scenario | Profile       | Throughput (req/s) | p50 (us) | Client CPU (cores) | Server CPU (cores) | Total CPU (cores)\n"
printf -- "---------+---------------+--------------------+----------+--------------------+--------------------+------------------\n"
while IFS='|' read -r scenario profile throughput p50 ccpu scpu total; do
  printf "%-8s | %-13s | %18s | %8s | %18s | %18s | %16s\n" "${scenario}" "${profile}" "${throughput}" "${p50}" "${ccpu}" "${scpu}" "${total}"
done < "${results_file}"

if [[ -n "${NETIPC_RESULTS_FILE:-}" ]]; then
  write_csv_results "${NETIPC_RESULTS_FILE}"
fi

echo -e "\n${GREEN}Negotiated profile benchmark complete (Rust server <-> Rust client).${NC}"
