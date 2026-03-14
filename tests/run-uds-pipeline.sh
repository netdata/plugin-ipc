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

  if ! wait "${SERVER_PID}"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] UDS pipeline server failed (pid=${SERVER_PID}, rc=${rc}). Log:${NC}"
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
}
trap cleanup EXIT

build_targets netipc-live-c

SERVICE="netipc-uds-pipeline-test"
SOCK="/tmp/${SERVICE}.sock"
run rm -f "${SOCK}"

start_server /tmp/netipc-uds-pipeline-server.log "${C_BIN_DIR}/netipc-live-c" uds-server-reorder /tmp "${SERVICE}"
wait_for_socket "${SOCK}" 200

printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
printf >&2 "${YELLOW}"
printf >&2 "%q " "${C_BIN_DIR}/netipc-live-c" uds-client-pipeline /tmp "${SERVICE}" 41 42
printf >&2 "${NC}\n"
client_out=$("${C_BIN_DIR}/netipc-live-c" uds-client-pipeline /tmp "${SERVICE}" 41 42 2>&1)
printf '%s\n' "${client_out}" >&2

if ! grep -q '^C-UDS-PIPE-CLIENT response0_id=1002 response0_value=43 profile=1$' <<<"${client_out}"; then
  echo -e >&2 "${RED}[ERROR] missing first out-of-order pipelined response.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-PIPE-CLIENT response1_id=1001 response1_value=42 profile=1$' <<<"${client_out}"; then
  echo -e >&2 "${RED}[ERROR] missing second out-of-order pipelined response.${NC}"
  exit 1
fi

wait_server

echo -e "${GREEN}UDS pipelined out-of-order response test passed.${NC}"
