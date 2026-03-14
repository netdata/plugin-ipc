#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SERVER_PID=""
CLIENT_A_PID=""
CLIENT_B_PID=""
STARTED_PID=""
SERVER_LOG=""
CLIENT_A_LOG=""
CLIENT_B_LOG=""
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
  if [[ -n "${CLIENT_A_PID}" ]] && kill -0 "${CLIENT_A_PID}" 2>/dev/null; then
    kill "${CLIENT_A_PID}" || true
    wait "${CLIENT_A_PID}" || true
  fi
  if [[ -n "${CLIENT_B_PID}" ]] && kill -0 "${CLIENT_B_PID}" 2>/dev/null; then
    kill "${CLIENT_B_PID}" || true
    wait "${CLIENT_B_PID}" || true
  fi
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
}
trap cleanup EXIT

build_targets netipc-live-c

SERVICE="netipc-uds-multi-client-test"
SOCK="/tmp/${SERVICE}.sock"
SERVER_LOG="/tmp/netipc-uds-multi-server.log"
CLIENT_A_LOG="/tmp/netipc-uds-multi-client-a.log"
CLIENT_B_LOG="/tmp/netipc-uds-multi-client-b.log"

run rm -f "${SOCK}" "${SERVER_LOG}" "${CLIENT_A_LOG}" "${CLIENT_B_LOG}"

start_background "${SERVER_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-server-multi-client /tmp "${SERVICE}"
SERVER_PID="${STARTED_PID}"
wait_for_socket "${SOCK}" 200

start_background "${CLIENT_A_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-client-once /tmp "${SERVICE}" 41
CLIENT_A_PID="${STARTED_PID}"
start_background "${CLIENT_B_LOG}" "${C_BIN_DIR}/netipc-live-c" uds-client-once /tmp "${SERVICE}" 99
CLIENT_B_PID="${STARTED_PID}"

wait_pid_or_dump "${CLIENT_A_PID}" "UDS multi-client client A" "${CLIENT_A_LOG}"
CLIENT_A_PID=""
wait_pid_or_dump "${CLIENT_B_PID}" "UDS multi-client client B" "${CLIENT_B_LOG}"
CLIENT_B_PID=""
wait_pid_or_dump "${SERVER_PID}" "UDS multi-client server" "${SERVER_LOG}"
SERVER_PID=""

client_a_out=$(<"${CLIENT_A_LOG}")
client_b_out=$(<"${CLIENT_B_LOG}")
server_out=$(<"${SERVER_LOG}")

if ! grep -q '^C-UDS-CLIENT request=41 response=42 profile=1$' <<<"${client_a_out}"; then
  echo -e >&2 "${RED}[ERROR] client A did not receive the expected response.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-CLIENT request=99 response=100 profile=1$' <<<"${client_b_out}"; then
  echo -e >&2 "${RED}[ERROR] client B did not receive the expected response.${NC}"
  exit 1
fi

if ! grep -q '^C-UDS-MULTI-SERVER sessions=2 ' <<<"${server_out}"; then
  echo -e >&2 "${RED}[ERROR] server did not report two accepted sessions.${NC}"
  exit 1
fi

if ! grep -q 'value_a=41\|value_b=41' <<<"${server_out}"; then
  echo -e >&2 "${RED}[ERROR] server did not record the first client payload.${NC}"
  exit 1
fi

if ! grep -q 'value_a=99\|value_b=99' <<<"${server_out}"; then
  echo -e >&2 "${RED}[ERROR] server did not record the second client payload.${NC}"
  exit 1
fi

echo -e "${GREEN}UDS multi-client listener/session test passed.${NC}"
