#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SERVER_PID=""
SERVER_LOG=""
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

wait_server() {
  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  if ! wait "${SERVER_PID}"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] UDS server failed (pid=${SERVER_PID}, rc=${rc}). Log:${NC}"
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

build_targets netipc-uds-server-demo netipc-uds-client-demo

SERVICE="netipc-uds-test"
SOCK="/tmp/${SERVICE}.sock"
run rm -f "${SOCK}"

start_server /tmp/netipc-uds-server.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp "${SERVICE}" 2
sleep 0.2

client_out=$("${C_BIN_DIR}/netipc-uds-client-demo" /tmp "${SERVICE}" 41 2)
printf '%s\n' "${client_out}" >&2

if ! grep -q '^negotiated_profile=1$' <<<"${client_out}"; then
  echo -e >&2 "${RED}[ERROR] client did not negotiate UDS_SEQPACKET profile.${NC}"
  exit 1
fi

if ! grep -q '^request=41 response=42$' <<<"${client_out}"; then
  echo -e >&2 "${RED}[ERROR] missing first increment response in client output.${NC}"
  exit 1
fi

if ! grep -q '^request=42 response=43$' <<<"${client_out}"; then
  echo -e >&2 "${RED}[ERROR] missing second increment response in client output.${NC}"
  exit 1
fi

wait_server

echo -e "${GREEN}UDS seqpacket negotiation + increment test passed.${NC}"
