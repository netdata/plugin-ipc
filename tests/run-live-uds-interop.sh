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

build_targets netipc-uds-server-demo netipc-uds-client-demo netipc_live_uds_rs netipc-live-go

# 1) C client -> Rust server
run rm -f /tmp/netipc-live-uds-c-rust.sock
start_server /tmp/netipc-live-uds-c-rust.log "${C_BIN_DIR}/netipc_live_uds_rs" server-once /tmp netipc-live-uds-c-rust
sleep 0.2
client_out=$("${C_BIN_DIR}/netipc-uds-client-demo" /tmp netipc-live-uds-c-rust 41 1 2>&1)
printf '%s\n' "$client_out"
if ! grep -q '^negotiated_profile=1$' <<<"$client_out"; then
  echo -e >&2 "${RED}[ERROR] expected negotiated_profile=1 in C->Rust baseline case${NC}"
  exit 1
fi
wait_server

# 2) Rust client -> Go server
run rm -f /tmp/netipc-live-uds-rust-go.sock
start_server /tmp/netipc-live-uds-rust-go.log "${C_BIN_DIR}/netipc-live-go" uds-server-once /tmp netipc-live-uds-rust-go
sleep 0.2
client_out=$("${C_BIN_DIR}/netipc_live_uds_rs" client-once /tmp netipc-live-uds-rust-go 99 2>&1)
printf '%s\n' "$client_out"
if ! grep -q 'profile=1' <<<"$client_out"; then
  echo -e >&2 "${RED}[ERROR] expected Rust->Go baseline profile=1${NC}"
  exit 1
fi
wait_server

# 3) Go client -> C server
run rm -f /tmp/netipc-live-uds-go-c.sock
start_server /tmp/netipc-live-uds-go-c.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp netipc-live-uds-go-c 1
sleep 0.2
client_out=$("${C_BIN_DIR}/netipc-live-go" uds-client-once /tmp netipc-live-uds-go-c 7 2>&1)
printf '%s\n' "$client_out"
if ! grep -q 'profile=1' <<<"$client_out"; then
  echo -e >&2 "${RED}[ERROR] expected Go->C baseline profile=1${NC}"
  exit 1
fi
wait_server

# 4) C client -> Rust server prefers SHM_HYBRID (profile 2)
run rm -f /tmp/netipc-live-uds-c-rust-shm.sock /tmp/netipc-live-uds-c-rust-shm.ipcshm
start_server /tmp/netipc-live-uds-c-rust-shm.log env NETIPC_SUPPORTED_PROFILES=3 NETIPC_PREFERRED_PROFILES=2 "${C_BIN_DIR}/netipc_live_uds_rs" server-once /tmp netipc-live-uds-c-rust-shm
sleep 0.2
client_out=$("${C_BIN_DIR}/netipc-uds-client-demo" /tmp netipc-live-uds-c-rust-shm 41 1 3 2 0 2>&1)
printf '%s\n' "$client_out"
if ! grep -q '^negotiated_profile=2$' <<<"$client_out"; then
  echo -e >&2 "${RED}[ERROR] expected negotiated_profile=2 in C->Rust SHM case${NC}"
  exit 1
fi
wait_server
if ! grep -q 'profile=2' /tmp/netipc-live-uds-c-rust-shm.log; then
  echo -e >&2 "${RED}[ERROR] Rust server did not report profile=2 for C->Rust SHM case${NC}"
  exit 1
fi

# 5) Rust client -> C server prefers SHM_HYBRID (profile 2)
run rm -f /tmp/netipc-live-uds-rust-c-shm.sock /tmp/netipc-live-uds-rust-c-shm.ipcshm
start_server /tmp/netipc-live-uds-rust-c-shm.log "${C_BIN_DIR}/netipc-uds-server-demo" /tmp netipc-live-uds-rust-c-shm 1 3 2 0
sleep 0.2
client_out=$(NETIPC_SUPPORTED_PROFILES=3 NETIPC_PREFERRED_PROFILES=2 "${C_BIN_DIR}/netipc_live_uds_rs" client-once /tmp netipc-live-uds-rust-c-shm 99 2>&1)
printf '%s\n' "$client_out"
if ! grep -q 'profile=2' <<<"$client_out"; then
  echo -e >&2 "${RED}[ERROR] expected Rust->C SHM case profile=2${NC}"
  exit 1
fi
wait_server

echo -e "${GREEN}Live UDS seqpacket interop tests passed (C <-> Rust <-> Go).${NC}"
