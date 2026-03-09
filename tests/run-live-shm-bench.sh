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
  run cmake --build "${CMAKE_BUILD_DIR}" --target netipc-live-c
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

bench_c_shm() {
  local scenario=$1
  local target_rps=$2

  local service="netipc-shm-c-${target_rps}-${RANDOM}"
  local client_row
  client_row=$("${C_BIN_DIR}/netipc-live-c" shm-bench /tmp "${service}" 5 "${target_rps}" | awk -F, '/^c-shm-hybrid,/ {print $0}')

  local mode duration target req resp mismatches throughput p50 p95 p99 c_cpu s_cpu total_cpu
  IFS=',' read -r mode duration target req resp mismatches throughput p50 p95 p99 c_cpu s_cpu total_cpu <<<"${client_row}"
  printf "%s|c-shm-hybrid|%s|%s|%s|%s|%s\n" "${scenario}" "${throughput}" "${p50}" "${c_cpu}" "${s_cpu}" "${total_cpu}"
}

build_targets

results_file=$(mktemp)
trap 'rm -f "${results_file}"' EXIT

bench_c_shm "max" 0 >> "${results_file}"
bench_c_shm "100k/s" 100000 >> "${results_file}"
bench_c_shm "10k/s" 10000 >> "${results_file}"

printf "\n"
printf "Scenario | Method       | Throughput (req/s) | p50 (us) | Client CPU (cores) | Server CPU (cores) | Total CPU (cores)\n"
printf -- "---------+--------------+--------------------+----------+--------------------+--------------------+------------------\n"
while IFS='|' read -r scenario method throughput p50 ccpu scpu total; do
  printf "%-8s | %-12s | %18s | %8s | %18s | %18s | %16s\n" "${scenario}" "${method}" "${throughput}" "${p50}" "${ccpu}" "${scpu}" "${total}"
done < "${results_file}"

echo -e "\n${GREEN}Live shm-hybrid benchmark complete.${NC}"
