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

build_targets netipc-shm-server-demo netipc-shm-client-demo netipc_live_rs

# 1) C client -> Rust server
run rm -f /tmp/netipc-live-c-rust.ipcshm
start_server /tmp/netipc-live-c-rust.log "${C_BIN_DIR}/netipc_live_rs" server-once /tmp netipc-live-c-rust
sleep 0.2
run "${C_BIN_DIR}/netipc-shm-client-demo" /tmp netipc-live-c-rust 41 1
wait_server

# 2) Rust client -> C server
run rm -f /tmp/netipc-live-rust-c.ipcshm
start_server /tmp/netipc-live-rust-c.log "${C_BIN_DIR}/netipc-shm-server-demo" /tmp netipc-live-rust-c 1
sleep 0.2
run "${C_BIN_DIR}/netipc_live_rs" client-once /tmp netipc-live-rust-c 99
wait_server

echo -e "${GREEN}Live shm-hybrid interop tests passed (C <-> Rust).${NC}"
