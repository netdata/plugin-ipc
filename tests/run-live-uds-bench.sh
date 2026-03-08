#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

CMAKE_BUILD_DIR="build"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"

configure_build() {
  run cmake -S . -B "${CMAKE_BUILD_DIR}"
}

build_targets() {
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

read_proc_ticks() {
  local pid=$1
  awk '{print $14 + $15}' "/proc/${pid}/stat"
}

bench_c() {
  local scenario=$1
  local target_rps=$2

  local row
  row=$("${C_BIN_DIR}/ipc-bench" --transport seqpacket --mode pingpong --clients 1 --payloads 32 --duration 5 --target-rps "${target_rps}" | awk -F, '/^uds-seqpacket,/ {print $0}')

  local transport mode clients payload duration target failed req resp mismatches last throughput p50 p95 p99 c_in s_in c_cpu s_cpu sreq sresp
  IFS=',' read -r transport mode clients payload duration target failed req resp mismatches last throughput p50 p95 p99 c_in s_in c_cpu s_cpu sreq sresp <<<"${row}"

  local total_cpu
  total_cpu=$(awk -v c="${c_cpu}" -v s="${s_cpu}" 'BEGIN { printf "%.3f", c + s }')
  printf "%s|c-uds|%s|%s|%s|%s|%s\n" "${scenario}" "${throughput}" "${p50}" "${c_cpu}" "${s_cpu}" "${total_cpu}"
}

bench_go() {
  local scenario=$1
  local target_rps=$2

  local service="netipc-uds-go-${target_rps}-${RANDOM}"
  local endpoint="/tmp/${service}.sock"
  local server_log="/tmp/${service}.log"
  local server_time="/tmp/${service}.time"

  run rm -f "${endpoint}" "${server_log}" "${server_time}"
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${C_BIN_DIR}/netipc-live-go" uds-server-loop /tmp "${service}" 0
  printf >&2 "${NC}\n"

  /usr/bin/time -f "%U %S" -o "${server_time}" "${C_BIN_DIR}/netipc-live-go" uds-server-loop /tmp "${service}" 0 >"${server_log}" 2>&1 &
  local server_pid=$!
  sleep 0.2
  if ! kill -0 "${server_pid}" 2>/dev/null; then
    echo "go uds server failed to start" >&2
    cat "${server_log}" >&2 || true
    return 1
  fi

  local start_ns end_ns
  start_ns=$(date +%s%N)

  local client_row
  client_row=$("${C_BIN_DIR}/netipc-live-go" uds-client-bench /tmp "${service}" 5 "${target_rps}" | awk -F, '/^go-uds,/ {print $0}')

  end_ns=$(date +%s%N)
  if kill -0 "${server_pid}" 2>/dev/null; then
    kill "${server_pid}" || true
  fi
  wait "${server_pid}" || true

  local mode duration target req resp mismatches throughput p50 p95 p99 c_cpu
  IFS=',' read -r mode duration target req resp mismatches throughput p50 p95 p99 c_cpu <<<"${client_row}"

  local elapsed_sec s_cpu total_cpu server_cpu_sec
  elapsed_sec=$(awk -v s="${start_ns}" -v e="${end_ns}" 'BEGIN { printf "%.6f", (e-s)/1e9 }')
  server_cpu_sec=$(awk '/^[0-9.]+ [0-9.]+$/ {print $1 + $2; found=1} END {if (!found) print 0}' "${server_time}" 2>/dev/null || echo "0")
  s_cpu=$(awk -v s="${server_cpu_sec}" -v e="${elapsed_sec}" 'BEGIN { if (e <= 0) e = 1e-9; printf "%.3f", s / e }')
  total_cpu=$(awk -v c="${c_cpu}" -v s="${s_cpu}" 'BEGIN { printf "%.3f", c + s }')
  printf "%s|go-uds|%s|%s|%s|%s|%s\n" "${scenario}" "${throughput}" "${p50}" "${c_cpu}" "${s_cpu}" "${total_cpu}"
}

bench_rust() {
  local scenario=$1
  local target_rps=$2

  local service="netipc-uds-rs-${target_rps}-${RANDOM}"
  local endpoint="/tmp/${service}.sock"
  local server_log="/tmp/${service}.log"
  local server_time="/tmp/${service}.time"

  run rm -f "${endpoint}" "${server_log}" "${server_time}"
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${C_BIN_DIR}/netipc_live_uds_rs" server-loop /tmp "${service}" 0
  printf >&2 "${NC}\n"

  /usr/bin/time -f "%U %S" -o "${server_time}" "${C_BIN_DIR}/netipc_live_uds_rs" server-loop /tmp "${service}" 0 >"${server_log}" 2>&1 &
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
  client_row=$("${C_BIN_DIR}/netipc_live_uds_rs" client-bench /tmp "${service}" 5 "${target_rps}" | awk -F, '/^rust-uds,/ {print $0}')

  end_ns=$(date +%s%N)
  if kill -0 "${server_pid}" 2>/dev/null; then
    kill "${server_pid}" || true
  fi
  wait "${server_pid}" || true

  local mode duration target req resp mismatches throughput p50 p95 p99 c_cpu
  IFS=',' read -r mode duration target req resp mismatches throughput p50 p95 p99 c_cpu <<<"${client_row}"

  local elapsed_sec s_cpu total_cpu server_cpu_sec
  elapsed_sec=$(awk -v s="${start_ns}" -v e="${end_ns}" 'BEGIN { printf "%.6f", (e-s)/1e9 }')
  server_cpu_sec=$(awk '/^[0-9.]+ [0-9.]+$/ {print $1 + $2; found=1} END {if (!found) print 0}' "${server_time}" 2>/dev/null || echo "0")
  s_cpu=$(awk -v s="${server_cpu_sec}" -v e="${elapsed_sec}" 'BEGIN { if (e <= 0) e = 1e-9; printf "%.3f", s / e }')
  total_cpu=$(awk -v c="${c_cpu}" -v s="${s_cpu}" 'BEGIN { printf "%.3f", c + s }')
  printf "%s|rust-uds|%s|%s|%s|%s|%s\n" "${scenario}" "${throughput}" "${p50}" "${c_cpu}" "${s_cpu}" "${total_cpu}"
}

build_targets ipc-bench netipc-uds-server-demo netipc-uds-client-demo netipc_live_uds_rs netipc-live-go

results_file=$(mktemp)
trap 'rm -f "${results_file}"' EXIT

bench_c "max" 0 >> "${results_file}"
bench_rust "max" 0 >> "${results_file}"
bench_go "max" 0 >> "${results_file}"

bench_c "100k/s" 100000 >> "${results_file}"
bench_rust "100k/s" 100000 >> "${results_file}"
bench_go "100k/s" 100000 >> "${results_file}"

bench_c "10k/s" 10000 >> "${results_file}"
bench_rust "10k/s" 10000 >> "${results_file}"
bench_go "10k/s" 10000 >> "${results_file}"

printf "\n"
printf "Scenario | Method   | Throughput (req/s) | p50 (us) | Client CPU (cores) | Server CPU (cores) | Total CPU (cores)\n"
printf -- "---------+----------+--------------------+----------+--------------------+--------------------+------------------\n"
while IFS='|' read -r scenario method throughput p50 ccpu scpu total; do
  printf "%-8s | %-8s | %18s | %8s | %18s | %18s | %16s\n" "${scenario}" "${method}" "${throughput}" "${p50}" "${ccpu}" "${scpu}" "${total}"
done < "${results_file}"

echo -e "\n${GREEN}Live UDS benchmark complete.${NC}"
