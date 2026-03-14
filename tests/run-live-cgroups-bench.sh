#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"
C_LIVE_BIN="${NETIPC_CGROUPS_LIVE_C_BIN:-${C_BIN_DIR}/netipc-cgroups-live-c}"
GO_CODEC_BIN="${NETIPC_CGROUPS_GO_BIN:-${C_BIN_DIR}/netipc-codec-go}"
RUST_CODEC_BIN="${NETIPC_CGROUPS_RUST_BIN:-${C_BIN_DIR}/netipc-codec-rs}"
SERVER_PID=""
SERVER_LOG=""
TEMP_ROOT=""
RESULTS_FILE="${NETIPC_RESULTS_FILE:-/tmp/cgroups_bench_results_$$.txt}"
REFRESH_TARGETS_STR="${NETIPC_CGROUPS_REFRESH_TARGETS:-0 1000}"
LOOKUP_TARGETS_STR="${NETIPC_CGROUPS_LOOKUP_TARGETS:-0 1000}"
BENCH_DURATION_SEC="${NETIPC_BENCH_DURATION_SEC:-5}"
SERVER_IDLE_TIMEOUT_MS="${NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS:-1000}"
SUPPORTED_PROFILES="${NETIPC_SUPPORTED_PROFILES:-}"
PREFERRED_PROFILES="${NETIPC_PREFERRED_PROFILES:-}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
MAX_RESPONSE_PAYLOAD_BYTES="${NETIPC_MAX_RESPONSE_PAYLOAD_BYTES:-4358}"
MAX_RESPONSE_BATCH_ITEMS="${NETIPC_MAX_RESPONSE_BATCH_ITEMS:-1000}"

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
  run cmake --build "${CMAKE_BUILD_DIR}" --target netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
}

start_server() {
  local server_bin=$1
  local service_namespace=$2
  local service=$3
  local max_requests=$4
  local log_file=$5

  SERVER_LOG=${log_file}

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  if [[ -n "${SUPPORTED_PROFILES}" ]]; then
    printf >&2 "%q " env "NETIPC_SUPPORTED_PROFILES=${SUPPORTED_PROFILES}"
    printf >&2 "%q " "NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS=${SERVER_IDLE_TIMEOUT_MS}"
    if [[ -n "${PREFERRED_PROFILES}" ]]; then
      printf >&2 "%q " "NETIPC_PREFERRED_PROFILES=${PREFERRED_PROFILES}"
    fi
    printf >&2 "%q " "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}"
    printf >&2 "%q " "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}"
  else
    printf >&2 "%q " env \
      "NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS=${SERVER_IDLE_TIMEOUT_MS}" \
      "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
      "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}"
  fi
  printf >&2 "%q " "${server_bin}" "server-bench" "${service_namespace}" "${service}" "${max_requests}" "${TOK}"
  printf >&2 "${NC}\n"

  if [[ -n "${SUPPORTED_PROFILES}" ]]; then
    local -a env_args=(
      "NETIPC_SUPPORTED_PROFILES=${SUPPORTED_PROFILES}"
      "NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS=${SERVER_IDLE_TIMEOUT_MS}"
      "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}"
      "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}"
    )
    if [[ -n "${PREFERRED_PROFILES}" ]]; then
      env_args+=("NETIPC_PREFERRED_PROFILES=${PREFERRED_PROFILES}")
    fi
    env "${env_args[@]}" \
      "${server_bin}" server-bench "${service_namespace}" "${service}" "${max_requests}" "${TOK}" >"${SERVER_LOG}" 2>&1 &
  else
    env \
      "NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS=${SERVER_IDLE_TIMEOUT_MS}" \
      "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
      "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}" \
      "${server_bin}" server-bench "${service_namespace}" "${service}" "${max_requests}" "${TOK}" >"${SERVER_LOG}" 2>&1 &
  fi
  SERVER_PID=$!
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

wait_server() {
  local rc watchdog_pid timeout_flag

  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  timeout_flag=$(mktemp)
  (
    sleep 10
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
      printf 'timeout\n' > "${timeout_flag}"
      kill "${SERVER_PID}" || true
    fi
  ) &
  watchdog_pid=$!

  wait "${SERVER_PID}"
  rc=$?

  if kill -0 "${watchdog_pid}" 2>/dev/null; then
    kill "${watchdog_pid}" || true
  fi
  wait "${watchdog_pid}" 2>/dev/null || true

  if [[ -s "${timeout_flag}" ]]; then
    echo -e >&2 "${RED}[ERROR] Server process did not exit in time (pid=${SERVER_PID}). Log:${NC}"
    cat "${SERVER_LOG}" >&2 || true
    rm -f "${timeout_flag}"
    SERVER_PID=""
    SERVER_LOG=""
    return 1
  fi
  rm -f "${timeout_flag}"

  if [[ $rc -ne 0 ]]; then
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
    wait "${SERVER_PID}" 2>/dev/null || true
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

scenario_label() {
  local target=$1
  if [[ "${target}" == "0" ]]; then
    printf 'max'
  else
    printf '%s/s' "${target}"
  fi
}

append_result() {
  local bench_type=$1
  local scenario=$2
  local client=$3
  local server=$4
  local row=$5
  local server_cpu=$6
  local mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu total_cpu

  IFS=, read -r mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu <<<"${row}"

  total_cpu="N/A"
  if [[ -n "${client_cpu}" && -n "${server_cpu}" && "${server_cpu}" != "N/A" ]]; then
    total_cpu=$(awk -v c="${client_cpu}" -v s="${server_cpu}" 'BEGIN { printf "%.3f", c + s }')
  fi

  printf "%-7s | %-8s | %-10s | %-10s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
    "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}"

  printf "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n" \
    "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}" >> "${RESULTS_FILE}"
}

run_refresh_case() {
  local server_label=$1
  local server_bin=$2
  local client_label=$3
  local client_bin=$4
  local target=$5
  local case_dir service client_log server_log client_row server_cpu

  case_dir=$(mktemp -d "${TEMP_ROOT}/refresh.${server_label}.${client_label}.XXXXXX")
  service="netipc-cgroups-refresh-${server_label}-to-${client_label}-${RANDOM}"
  client_log="${case_dir}/client.log"
  server_log="${case_dir}/server.log"

  start_server "${server_bin}" "${case_dir}" "${service}" 0 "${server_log}"
  wait_for_socket "${case_dir}/${service}.sock" 500 || show_logs_and_fail "refresh server endpoint did not appear for ${server_label}->${client_label}" "${client_log}" "${server_log}"

  if [[ -n "${SUPPORTED_PROFILES}" ]]; then
    local -a env_args=(
      "NETIPC_SUPPORTED_PROFILES=${SUPPORTED_PROFILES}"
    )
    if [[ -n "${PREFERRED_PROFILES}" ]]; then
      env_args+=("NETIPC_PREFERRED_PROFILES=${PREFERRED_PROFILES}")
    fi
    if ! env "${env_args[@]}" \
      "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
      "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}" \
      "${client_bin}" client-refresh-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
      if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" || true
        wait "${SERVER_PID}" || true
        SERVER_PID=""
      fi
      show_logs_and_fail "refresh benchmark failed for ${server_label}->${client_label}" "${client_log}" "${server_log}"
    fi
  elif ! env \
    "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
    "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}" \
    "${client_bin}" client-refresh-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
      SERVER_PID=""
    fi
    show_logs_and_fail "refresh benchmark failed for ${server_label}->${client_label}" "${client_log}" "${server_log}"
  fi

  wait_server || show_logs_and_fail "refresh server failed for ${server_label}->${client_label}" "${client_log}" "${server_log}"
  client_row=$(tail -1 "${client_log}")
  server_cpu=$(grep 'SERVER_CPU_CORES=' "${server_log}" | tail -1 | cut -d= -f2)
  append_result "refresh" "$(scenario_label "${target}")" "${client_label}" "${server_label}" "${client_row}" "${server_cpu:-N/A}"
}

run_lookup_case() {
  local label=$1
  local bin=$2
  local target=$3
  local case_dir service client_log server_log client_row server_cpu

  case_dir=$(mktemp -d "${TEMP_ROOT}/lookup.${label}.XXXXXX")
  service="netipc-cgroups-lookup-${label}-${RANDOM}"
  client_log="${case_dir}/client.log"
  server_log="${case_dir}/server.log"

  start_server "${bin}" "${case_dir}" "${service}" 1 "${server_log}"
  wait_for_socket "${case_dir}/${service}.sock" 500 || show_logs_and_fail "lookup server endpoint did not appear for ${label}" "${client_log}" "${server_log}"

  if [[ -n "${SUPPORTED_PROFILES}" ]]; then
    local -a env_args=(
      "NETIPC_SUPPORTED_PROFILES=${SUPPORTED_PROFILES}"
    )
    if [[ -n "${PREFERRED_PROFILES}" ]]; then
      env_args+=("NETIPC_PREFERRED_PROFILES=${PREFERRED_PROFILES}")
    fi
    if ! env "${env_args[@]}" \
      "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
      "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}" \
      "${bin}" client-lookup-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
      if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" || true
        wait "${SERVER_PID}" || true
        SERVER_PID=""
      fi
      show_logs_and_fail "lookup benchmark failed for ${label}" "${client_log}" "${server_log}"
    fi
  elif ! env \
    "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES=${MAX_RESPONSE_PAYLOAD_BYTES}" \
    "NETIPC_MAX_RESPONSE_BATCH_ITEMS=${MAX_RESPONSE_BATCH_ITEMS}" \
    "${bin}" client-lookup-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
      SERVER_PID=""
    fi
    show_logs_and_fail "lookup benchmark failed for ${label}" "${client_log}" "${server_log}"
  fi

  wait_server || show_logs_and_fail "lookup server failed for ${label}" "${client_log}" "${server_log}"
  client_row=$(tail -1 "${client_log}")
  server_cpu=$(grep 'SERVER_CPU_CORES=' "${server_log}" | tail -1 | cut -d= -f2)
  append_result "lookup" "$(scenario_label "${target}")" "${label}" "${label}" "${client_row}" "${server_cpu:-N/A}"
}

write_csv_results() {
  local output_path=$1
  local output_dir tmp_path

  output_dir=$(dirname "${output_path}")
  mkdir -p "${output_dir}"
  tmp_path="${output_dir}/.$(basename "${output_path}").tmp.$$"

  printf "bench_type,scenario,client,server,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores,server_cpu_cores,total_cpu_cores\n" > "${tmp_path}"
  while IFS='|' read -r bench_type scenario client server throughput p50 p95 p99 client_cpu server_cpu total_cpu; do
    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
      "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}" >> "${tmp_path}"
  done < "${RESULTS_FILE}"

  mv "${tmp_path}" "${output_path}"
}

build_targets
TEMP_ROOT=$(mktemp -d)
: > "${RESULTS_FILE}"

for bin in "${C_LIVE_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}"; do
  if [[ ! -x "${bin}" ]]; then
    echo -e >&2 "${RED}[ERROR] missing executable ${bin}${NC}"
    exit 1
  fi
done

read -r -a refresh_targets <<<"${REFRESH_TARGETS_STR}"
read -r -a lookup_targets <<<"${LOOKUP_TARGETS_STR}"

labels=("c" "go" "rust")
bins=("${C_LIVE_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}")

printf "%-7s | %-8s | %-10s | %-10s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
  "Type" "Scenario" "Client" "Server" "Throughput (rps)" "p50 (us)" "p95 (us)" "p99 (us)" "Client CPU" "Server CPU" "Total"
printf -- "--------+----------+------------+------------+--------------------+----------+----------+----------+------------+------------+---------\n"

for target in "${refresh_targets[@]}"; do
  for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
    for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
      run_refresh_case "${labels[server_idx]}" "${bins[server_idx]}" "${labels[client_idx]}" "${bins[client_idx]}" "${target}"
    done
  done
done

for target in "${lookup_targets[@]}"; do
  for ((idx = 0; idx < ${#labels[@]}; idx++)); do
    run_lookup_case "${labels[idx]}" "${bins[idx]}" "${target}"
  done
done

write_csv_results "${RESULTS_FILE}.csv"

echo -e "${GREEN}Live cgroups benchmark matrix passed.${NC}"
echo "Results saved to ${RESULTS_FILE}"
echo "CSV results saved to ${RESULTS_FILE}.csv"
