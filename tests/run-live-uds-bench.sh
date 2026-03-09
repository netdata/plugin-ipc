#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"
SERVER_PID=""
SERVER_LOG=""
TEMP_ROOT=""

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

binary_for() {
  case "$1" in
    c) printf '%s/netipc-live-c\n' "${C_BIN_DIR}" ;;
    rust) printf '%s/netipc_live_uds_rs\n' "${C_BIN_DIR}" ;;
    go) printf '%s/netipc-live-go\n' "${C_BIN_DIR}" ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

server_cmd_for() {
  case "$1" in
    c|go) printf 'uds-server-bench\n' ;;
    rust) printf 'server-bench\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

client_cmd_for() {
  case "$1" in
    c|go) printf 'uds-client-bench\n' ;;
    rust) printf 'client-bench\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

client_label_for() {
  case "$1" in
    c) printf 'c-uds\n' ;;
    rust) printf 'rust-uds\n' ;;
    go) printf 'go-uds\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

server_label_for() {
  case "$1" in
    c) printf 'c-uds-server\n' ;;
    rust) printf 'rust-uds-server\n' ;;
    go) printf 'go-uds-server\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

start_server() {
  local server_lang=$1
  local run_dir=$2
  local service=$3
  local log_file=$4
  local bin
  local cmd

  bin=$(binary_for "${server_lang}")
  cmd=$(server_cmd_for "${server_lang}")
  SERVER_LOG=${log_file}

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${bin}" "${cmd}" "${run_dir}" "${service}" "0"
  printf >&2 "${NC}\n"

  "${bin}" "${cmd}" "${run_dir}" "${service}" "0" >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
  printf '%s\n' "${SERVER_PID}" > "${run_dir}/server.pid"
}

wait_for_socket() {
  local path=$1
  local attempts=$2

  for ((i = 0; i < attempts; i++)); do
    if [[ -S "${path}" ]]; then
      return 0
    fi
    sleep 0.01
  done

  echo -e >&2 "${RED}[ERROR] endpoint ${path} was not created in time${NC}"
  return 1
}

wait_for_server_exit() {
  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  if ! timeout 10 tail --pid="${SERVER_PID}" -f /dev/null; then
    echo -e >&2 "${RED}[ERROR] Server process did not exit in time (pid=${SERVER_PID}). Log:${NC}"
    cat "${SERVER_LOG}" >&2 || true
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
    fi
    SERVER_PID=""
    SERVER_LOG=""
    return 1
  fi

  if ! wait "${SERVER_PID}"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] Server process failed (pid=${SERVER_PID}, rc=${rc}). Log:${NC}"
    cat "${SERVER_LOG}" >&2 || true
    SERVER_PID=""
    SERVER_LOG=""
    return $rc
  fi

  SERVER_PID=""
  SERVER_LOG=""
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
  if [[ -n "${TEMP_ROOT}" ]] && [[ -d "${TEMP_ROOT}" ]]; then
    rm -rf "${TEMP_ROOT}"
  fi
}
trap cleanup EXIT

show_logs_and_fail() {
  local message=$1
  local client_log=$2
  local server_log=$3

  echo -e >&2 "${RED}[ERROR] ${message}${NC}"
  if [[ -f "${client_log}" ]]; then
    echo -e >&2 "${RED}[CLIENT LOG]${NC}"
    cat "${client_log}" >&2 || true
  fi
  if [[ -f "${server_log}" ]]; then
    echo -e >&2 "${RED}[SERVER LOG]${NC}"
    cat "${server_log}" >&2 || true
  fi
  exit 1
}

run_client_capture() {
  local client_lang=$1
  local run_dir=$2
  local service=$3
  local duration_sec=$4
  local target_rps=$5
  local log_file=$6
  local bin
  local cmd

  bin=$(binary_for "${client_lang}")
  cmd=$(client_cmd_for "${client_lang}")

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${bin}" "${cmd}" "${run_dir}" "${service}" "${duration_sec}" "${target_rps}"
  printf >&2 "${NC}\n"

  if ! "${bin}" "${cmd}" "${run_dir}" "${service}" "${duration_sec}" "${target_rps}" >"${log_file}" 2>&1; then
    return 1
  fi
}

extract_row() {
  local prefix=$1
  local log_file=$2

  awk -F, -v prefix="${prefix}" '$1 == prefix {print; exit}' "${log_file}"
}

run_matrix_case() {
  local scenario=$1
  local target_rps=$2
  local client_lang=$3
  local server_lang=$4
  local case_dir service client_log server_log client_row server_row scenario_tag
  local mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu ignored_server total_ignored
  local server_mode handled elapsed server_cpu total_cpu

  scenario_tag=${scenario//\//-}
  case_dir=$(mktemp -d "${TEMP_ROOT}/uds-${scenario_tag}-${client_lang}-to-${server_lang}.XXXXXX")
  service="netipc-uds-${scenario_tag}-${client_lang}-to-${server_lang}-${RANDOM}"
  client_log="${case_dir}/client.log"
  server_log="${case_dir}/server.log"

  start_server "${server_lang}" "${case_dir}" "${service}" "${server_log}"

  if ! wait_for_socket "${case_dir}/${service}.sock" 500; then
    show_logs_and_fail "server endpoint did not appear for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  if ! run_client_capture "${client_lang}" "${case_dir}" "${service}" 5 "${target_rps}" "${client_log}"; then
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
      SERVER_PID=""
      SERVER_LOG=""
    fi
    show_logs_and_fail "client benchmark failed for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  if ! wait_for_server_exit; then
    show_logs_and_fail "server benchmark failed for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  client_row=$(extract_row "$(client_label_for "${client_lang}")" "${client_log}")
  if [[ -z "${client_row}" ]]; then
    show_logs_and_fail "missing client result row for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  server_row=$(extract_row "$(server_label_for "${server_lang}")" "${server_log}")
  if [[ -z "${server_row}" ]]; then
    show_logs_and_fail "missing server result row for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  IFS=',' read -r mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu ignored_server total_ignored <<<"${client_row}"
  IFS=',' read -r server_mode handled elapsed server_cpu <<<"${server_row}"

  if [[ "${mismatches}" != "0" ]]; then
    show_logs_and_fail "benchmark reported mismatches=${mismatches} for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi
  if [[ "${requests}" != "${responses}" ]]; then
    show_logs_and_fail "requests=${requests} responses=${responses} for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi
  if [[ "${handled}" != "${responses}" ]]; then
    show_logs_and_fail "server handled=${handled} but client responses=${responses} for ${client_lang}->${server_lang} (${scenario})" "${client_log}" "${server_log}"
  fi

  total_cpu=$(awk -v client="${client_cpu}" -v server="${server_cpu}" 'BEGIN { printf "%.3f", client + server }')
  printf "%s|%s|%s|%s|%s|%s|%s|%s\n" \
    "${scenario}" \
    "${client_lang}" \
    "${server_lang}" \
    "${throughput}" \
    "${p50}" \
    "${client_cpu}" \
    "${server_cpu}" \
    "${total_cpu}"
}

build_targets netipc-live-c netipc_live_uds_rs netipc-live-go

TEMP_ROOT=$(mktemp -d)
results_file=$(mktemp "${TEMP_ROOT}/results.XXXXXX")

languages=(c rust go)
scenarios=("max:0" "100k/s:100000" "10k/s:10000")

for scenario_spec in "${scenarios[@]}"; do
  scenario=${scenario_spec%%:*}
  target_rps=${scenario_spec##*:}
  for client_lang in "${languages[@]}"; do
    for server_lang in "${languages[@]}"; do
      run_matrix_case "${scenario}" "${target_rps}" "${client_lang}" "${server_lang}" >> "${results_file}"
    done
  done
done

printf "\n"
printf "Scenario | Client | Server | Throughput (req/s) | p50 (us) | Client CPU (cores) | Server CPU (cores) | Total CPU (cores)\n"
printf -- "---------+--------+--------+--------------------+----------+--------------------+--------------------+------------------\n"
while IFS='|' read -r scenario client server throughput p50 client_cpu server_cpu total_cpu; do
  printf "%-8s | %-6s | %-6s | %18s | %8s | %18s | %18s | %16s\n" \
    "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${client_cpu}" "${server_cpu}" "${total_cpu}"
done < "${results_file}"

echo -e "\n${GREEN}Live UDS benchmark matrix complete.${NC}"
