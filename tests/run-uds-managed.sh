#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SERVER_PID=""
PIPE_PID=""
ONCE_PID=""
STARTED_PID=""
CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"

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
  run cmake --build "${CMAKE_BUILD_DIR}" --target "$@"
}

start_background() {
  local log_file=$1
  shift

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  "$@" >"${log_file}" 2>&1 &
  STARTED_PID=$!
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

wait_pid_or_dump() {
  local pid=$1
  local label=$2
  local log_file=$3

  if ! wait "${pid}"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] ${label} failed (pid=${pid}, rc=${rc}). Log:${NC}"
    cat "${log_file}" >&2 || true
    return $rc
  fi

  cat "${log_file}" >&2 || true
}

cleanup() {
  if [[ -n "${PIPE_PID}" ]] && kill -0 "${PIPE_PID}" 2>/dev/null; then
    kill "${PIPE_PID}" || true
    wait "${PIPE_PID}" || true
  fi
  if [[ -n "${ONCE_PID}" ]] && kill -0 "${ONCE_PID}" 2>/dev/null; then
    kill "${ONCE_PID}" || true
    wait "${ONCE_PID}" || true
  fi
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
}
trap cleanup EXIT

build_targets netipc-live-c

SERVICE="netipc-uds-managed-test"
SOCK="/tmp/${SERVICE}.sock"
SERVER_LOG="/tmp/netipc-uds-managed-server.log"
PIPE_LOG="/tmp/netipc-uds-managed-pipeline.log"
ONCE_LOG="/tmp/netipc-uds-managed-once.log"

run rm -f "${SOCK}" "${SERVER_LOG}" "${PIPE_LOG}" "${ONCE_LOG}"

start_background "${SERVER_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-server-managed /tmp "${SERVICE}"
SERVER_PID="${STARTED_PID}"
wait_for_socket "${SOCK}" 200

start_background "${PIPE_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-client-pipeline /tmp "${SERVICE}" 41 42
PIPE_PID="${STARTED_PID}"
start_background "${ONCE_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-client-once /tmp "${SERVICE}" 99
ONCE_PID="${STARTED_PID}"

wait_pid_or_dump "${PIPE_PID}" "UDS managed pipeline client" "${PIPE_LOG}"
PIPE_PID=""
wait_pid_or_dump "${ONCE_PID}" "UDS managed once client" "${ONCE_LOG}"
ONCE_PID=""
wait_pid_or_dump "${SERVER_PID}" "UDS managed server" "${SERVER_LOG}"
SERVER_PID=""

pipe_out=$(<"${PIPE_LOG}")
once_out=$(<"${ONCE_LOG}")
server_out=$(<"${SERVER_LOG}")

if ! grep -q '^C-UDS-PIPE-CLIENT response0_id=1002 response0_value=43 profile=1$' <<<"${pipe_out}"; then
  echo -e >&2 "${RED}[ERROR] managed server did not produce the expected first pipelined response.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-PIPE-CLIENT response1_id=1001 response1_value=42 profile=1$' <<<"${pipe_out}"; then
  echo -e >&2 "${RED}[ERROR] managed server did not produce the expected second pipelined response.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-CLIENT request=99 response=100 profile=1$' <<<"${once_out}"; then
  echo -e >&2 "${RED}[ERROR] managed server did not serve the second client correctly.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-MANAGED-SERVER sessions=2 requests=3 ' <<<"${server_out}"; then
  echo -e >&2 "${RED}[ERROR] managed server summary is missing or incorrect.${NC}"
  exit 1
fi

echo -e "${GREEN}UDS managed multi-client worker-queue test passed.${NC}"
