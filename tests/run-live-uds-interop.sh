#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SERVER_PID=""
SERVER_LOG=""
TEMP_ROOT=""
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

binary_for() {
  case "$1" in
    c) printf '%s/netipc-live-c\n' "${C_BIN_DIR}" ;;
    rust) printf '%s/netipc_live_uds_rs\n' "${C_BIN_DIR}" ;;
    go) printf '%s/netipc-live-go\n' "${C_BIN_DIR}" ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

server_once_cmd_for() {
  case "$1" in
    c|go) printf 'uds-server-once\n' ;;
    rust) printf 'server-once\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

client_once_cmd_for() {
  case "$1" in
    c|go) printf 'uds-client-once\n' ;;
    rust) printf 'client-once\n' ;;
    *) echo "unknown language: $1" >&2; return 1 ;;
  esac
}

start_server() {
  SERVER_LOG=$1
  shift

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  "$@" >"${SERVER_LOG}" 2>&1 &
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

  cat "${SERVER_LOG}" >&2 || true
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

run_baseline_case() {
  local client_lang=$1
  local server_lang=$2
  local value=$3
  local case_dir service client_out server_bin server_cmd client_bin client_cmd

  case_dir=$(mktemp -d "${TEMP_ROOT}/baseline-${client_lang}-to-${server_lang}.XXXXXX")
  service="netipc-live-uds-${client_lang}-to-${server_lang}-${RANDOM}"
  server_bin=$(binary_for "${server_lang}")
  server_cmd=$(server_once_cmd_for "${server_lang}")
  client_bin=$(binary_for "${client_lang}")
  client_cmd=$(client_once_cmd_for "${client_lang}")

  start_server "${case_dir}/server.log" "${server_bin}" "${server_cmd}" "${case_dir}" "${service}"
  if ! wait_for_socket "${case_dir}/${service}.sock" 500; then
    return 1
  fi

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${client_bin}" "${client_cmd}" "${case_dir}" "${service}" "${value}"
  printf >&2 "${NC}\n"

  client_out=$("${client_bin}" "${client_cmd}" "${case_dir}" "${service}" "${value}" 2>&1)
  printf '%s\n' "${client_out}"
  if ! grep -q 'profile=1' <<<"${client_out}"; then
    echo -e >&2 "${RED}[ERROR] expected profile=1 in ${client_lang}->${server_lang} baseline case${NC}"
    return 1
  fi

  wait_server
  if ! grep -q 'profile=1' "${case_dir}/server.log"; then
    echo -e >&2 "${RED}[ERROR] expected server profile=1 in ${client_lang}->${server_lang} baseline case${NC}"
    return 1
  fi
}

run_c_to_rust_shm_case() {
  local case_dir service client_out

  case_dir=$(mktemp -d "${TEMP_ROOT}/shm-c-to-rust.XXXXXX")
  service="netipc-live-uds-c-to-rust-shm-${RANDOM}"

  start_server "${case_dir}/server.log" env NETIPC_SUPPORTED_PROFILES=3 NETIPC_PREFERRED_PROFILES=2 "${C_BIN_DIR}/netipc_live_uds_rs" server-once "${case_dir}" "${service}"
  if ! wait_for_socket "${case_dir}/${service}.sock" 500; then
    return 1
  fi

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "${C_BIN_DIR}/netipc-live-c" "uds-client-once" "${case_dir}" "${service}" "41" "1" "3" "2" "0"
  printf >&2 "${NC}\n"

  client_out=$("${C_BIN_DIR}/netipc-live-c" uds-client-once "${case_dir}" "${service}" 41 1 3 2 0 2>&1)
  printf '%s\n' "${client_out}"
  if ! grep -q 'profile=2' <<<"${client_out}"; then
    echo -e >&2 "${RED}[ERROR] expected profile=2 in C->Rust SHM case${NC}"
    return 1
  fi

  wait_server
  if ! grep -q 'profile=2' "${case_dir}/server.log"; then
    echo -e >&2 "${RED}[ERROR] Rust server did not report profile=2 for C->Rust SHM case${NC}"
    return 1
  fi
}

run_rust_to_c_shm_case() {
  local case_dir service client_out

  case_dir=$(mktemp -d "${TEMP_ROOT}/shm-rust-to-c.XXXXXX")
  service="netipc-live-uds-rust-to-c-shm-${RANDOM}"

  start_server "${case_dir}/server.log" "${C_BIN_DIR}/netipc-live-c" uds-server-once "${case_dir}" "${service}" 1 3 2 0
  if ! wait_for_socket "${case_dir}/${service}.sock" 500; then
    return 1
  fi

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " env "NETIPC_SUPPORTED_PROFILES=3" "NETIPC_PREFERRED_PROFILES=2" "${C_BIN_DIR}/netipc_live_uds_rs" client-once "${case_dir}" "${service}" "99"
  printf >&2 "${NC}\n"

  client_out=$(NETIPC_SUPPORTED_PROFILES=3 NETIPC_PREFERRED_PROFILES=2 "${C_BIN_DIR}/netipc_live_uds_rs" client-once "${case_dir}" "${service}" 99 2>&1)
  printf '%s\n' "${client_out}"
  if ! grep -q 'profile=2' <<<"${client_out}"; then
    echo -e >&2 "${RED}[ERROR] expected Rust->C SHM case profile=2${NC}"
    return 1
  fi

  wait_server
  if ! grep -q 'profile=2' "${case_dir}/server.log"; then
    echo -e >&2 "${RED}[ERROR] C server did not report profile=2 for Rust->C SHM case${NC}"
    return 1
  fi
}

build_targets netipc-live-c netipc-live-go netipc_live_uds_rs

TEMP_ROOT=$(mktemp -d)

for client_lang in c rust go; do
  for server_lang in c rust go; do
    run_baseline_case "${client_lang}" "${server_lang}" 41
  done
done

run_c_to_rust_shm_case
run_rust_to_c_shm_case

echo -e "${GREEN}Live UDS seqpacket interop tests passed (full C/Rust/Go matrix plus C<->Rust SHM negotiation).${NC}"
