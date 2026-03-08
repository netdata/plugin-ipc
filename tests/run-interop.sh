#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

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

run_capture() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  local out
  out=$("$@")
  local exit_code=$?
  if [[ $exit_code -ne 0 ]]; then
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
    echo -e >&2 "${RED}        Full command:${NC} $*"
    echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    return $exit_code
  fi

  printf '%s\n' "$out"
}

expect_resp() {
  local label=$1
  local out=$2
  local expect_id=$3
  local expect_status=$4
  local expect_value=$5

  local kind
  local got_id
  local got_status
  local got_value
  read -r kind got_id got_status got_value <<<"$out"

  if [[ "$kind" != "RESP" || "$got_id" != "$expect_id" || "$got_status" != "$expect_status" || "$got_value" != "$expect_value" ]]; then
    echo "${label}: expected RESP ${expect_id} ${expect_status} ${expect_value}, got: ${out}" >&2
    return 1
  fi
}

build_targets netipc-codec-c netipc-codec-rs netipc-codec-go

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# C client frame -> Rust server frame -> C client decode
run "${C_BIN_DIR}/netipc-codec-c" encode-req 101 41 "$TMPDIR/c_r.req"
run "${C_BIN_DIR}/netipc-codec-rs" serve-once "$TMPDIR/c_r.req" "$TMPDIR/c_r.resp"
out=$(run_capture "${C_BIN_DIR}/netipc-codec-c" decode-resp "$TMPDIR/c_r.resp")
expect_resp "c->rust->c" "$out" 101 0 42

# Rust client frame -> Go server frame -> Rust client decode
run "${C_BIN_DIR}/netipc-codec-rs" encode-req 202 99 "$TMPDIR/r_g.req"
run "${C_BIN_DIR}/netipc-codec-go" serve-once "$TMPDIR/r_g.req" "$TMPDIR/r_g.resp"
out=$(run_capture "${C_BIN_DIR}/netipc-codec-rs" decode-resp "$TMPDIR/r_g.resp")
expect_resp "rust->go->rust" "$out" 202 0 100

# Go client frame -> C server frame -> Go client decode
run "${C_BIN_DIR}/netipc-codec-go" encode-req 303 7 "$TMPDIR/g_c.req"
run "${C_BIN_DIR}/netipc-codec-c" serve-once "$TMPDIR/g_c.req" "$TMPDIR/g_c.resp"
out=$(run_capture "${C_BIN_DIR}/netipc-codec-go" decode-resp "$TMPDIR/g_c.resp")
expect_resp "go->c->go" "$out" 303 0 8

echo -e "${GREEN}Interop schema tests passed (C <-> Rust <-> Go).${NC}"
