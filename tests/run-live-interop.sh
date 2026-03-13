#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SERVER_PID=""
SERVER_LOG=""
CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"
RUST_BIN="${CMAKE_BUILD_DIR}/bin/netipc_live_rs"

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

start_server() {
  SERVER_LOG=$1
  shift

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  "$@" >"$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
}

start_server_env() {
  SERVER_LOG=$1
  shift

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " env "$@"
  printf >&2 "${NC}\n"

  env "$@" >"$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
}

wait_server() {
  if [[ -z "$SERVER_PID" ]]; then
    return 0
  fi

  if ! wait "$SERVER_PID"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] Server process failed (pid=${SERVER_PID}, rc=${rc}). Log:${NC}"
    cat "$SERVER_LOG" >&2 || true
    SERVER_PID=""
    SERVER_LOG=""
    return $rc
  fi

  cat "$SERVER_LOG" >&2 || true
  SERVER_PID=""
  SERVER_LOG=""
}

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" || true
    wait "$SERVER_PID" || true
  fi
}
trap cleanup EXIT

build_targets netipc-live-c

direct_shm_binary_for() {
  case "$1" in
    c) printf '%s/netipc-live-c\n' "${C_BIN_DIR}" ;;
    rust) printf '%s\n' "${RUST_BIN}" ;;
    *) echo "unknown direct SHM language: $1" >&2; return 1 ;;
  esac
}

run_direct_shm_case() {
  local client_lang=$1
  local server_lang=$2
  local value=$3
  local case_dir service server_bin client_bin client_out

  case_dir=$(mktemp -d "/tmp/netipc-live-shm-${client_lang}-to-${server_lang}.XXXXXX")
  service="netipc-live-shm-${client_lang}-to-${server_lang}-${RANDOM}"
  server_bin=$(direct_shm_binary_for "${server_lang}")
  client_bin=$(direct_shm_binary_for "${client_lang}")

  if [[ "${server_lang}" == "rust" ]]; then
    start_server_env "${case_dir}/server.log" "${server_bin}" server-once "${case_dir}" "${service}"
  else
    start_server "${case_dir}/server.log" "${server_bin}" shm-server-once "${case_dir}" "${service}"
  fi
  sleep 0.2

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  if [[ "${client_lang}" == "rust" ]]; then
    printf >&2 "%q " "${client_bin}" client-once "${case_dir}" "${service}" "${value}"
    printf >&2 "${NC}\n"
    client_out=$("${client_bin}" client-once "${case_dir}" "${service}" "${value}" 2>&1)
  else
    printf >&2 "%q " "${client_bin}" shm-client-once "${case_dir}" "${service}" "${value}"
    printf >&2 "${NC}\n"
    client_out=$("${client_bin}" shm-client-once "${case_dir}" "${service}" "${value}" 2>&1)
  fi

  printf '%s\n' "${client_out}"
  wait_server

  rm -rf "${case_dir}"
}

build_targets netipc_live_rs

for client_lang in c rust; do
  for server_lang in c rust; do
    run_direct_shm_case "${client_lang}" "${server_lang}" 41
  done
done

echo -e "${GREEN}Live shm-hybrid tests passed (full C/Rust direct matrix).${NC}"
