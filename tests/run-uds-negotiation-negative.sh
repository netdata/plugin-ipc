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

  "$@" >"$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
}

wait_server_expect_fail() {
  if [[ -z "${SERVER_PID}" ]]; then
    echo "server pid missing" >&2
    return 1
  fi

  set +e
  wait "${SERVER_PID}"
  local rc=$?
  set -e

  local log="${SERVER_LOG}"
  SERVER_PID=""
  SERVER_LOG=""

  cat "${log}" >&2 || true
  if [[ $rc -eq 0 ]]; then
    echo -e >&2 "${RED}[ERROR] server unexpectedly succeeded in negative test${NC}"
    return 1
  fi
  return 0
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
}
trap cleanup EXIT

build_targets netipc-uds-server-demo netipc-uds-client-demo netipc_live_uds_rs netipc-live-go

# 1) Profile mismatch: raw hello advertises only profile bit 2, server supports only bit 1.
run rm -f /tmp/netipc-neg-profile.sock
start_server /tmp/netipc-neg-profile.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp netipc-neg-profile 1 1 1 0
sleep 0.2
set +e
client_out=$("${C_BIN_DIR}/netipc-live-go" uds-client-rawhello /tmp netipc-neg-profile 2 2 0 2>&1)
client_rc=$?
set -e
printf '%s\n' "${client_out}" >&2
if [[ $client_rc -eq 0 ]]; then
  echo -e >&2 "${RED}[ERROR] profile mismatch client unexpectedly succeeded${NC}"
  exit 1
fi
if ! grep -Ei 'status=|not supported|Operation not supported' <<<"${client_out}" >/dev/null; then
  echo -e >&2 "${RED}[ERROR] profile mismatch client output did not indicate ENOTSUP${NC}"
  exit 1
fi
wait_server_expect_fail

# 2) Auth mismatch: same profiles, different auth tokens.
run rm -f /tmp/netipc-neg-auth.sock
start_server /tmp/netipc-neg-auth.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp netipc-neg-auth 1 1 1 111
sleep 0.2
set +e
client_out=$("${C_BIN_DIR}/netipc-live-go" uds-client-rawhello /tmp netipc-neg-auth 1 1 222 2>&1)
client_rc=$?
set -e
printf '%s\n' "${client_out}" >&2
if [[ $client_rc -eq 0 ]]; then
  echo -e >&2 "${RED}[ERROR] auth mismatch client unexpectedly succeeded${NC}"
  exit 1
fi
if ! grep -Ei 'Permission denied|denied' <<<"${client_out}" >/dev/null; then
  echo -e >&2 "${RED}[ERROR] auth mismatch client output did not indicate EACCES${NC}"
  exit 1
fi
wait_server_expect_fail

# 3) Malformed hello: send invalid negotiation frame magic.
run rm -f /tmp/netipc-neg-malformed.sock
start_server /tmp/netipc-neg-malformed.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp netipc-neg-malformed 1
sleep 0.2
run "${C_BIN_DIR}/netipc-live-go" uds-client-badhello /tmp netipc-neg-malformed
wait_server_expect_fail

echo -e "${GREEN}UDS negotiation negative tests passed.${NC}"
