#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNNER="${ROOT_DIR}/tests/run-windows-bench.sh"
OUT_DIR="${1:-${TEMP:-/tmp}/netipc-bench-canary}"
DURATION="${2:-5}"
ROW_SETTLE_SEC="${NIPC_BENCH_ROW_SETTLE_SEC:-2}"

run() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"
  "$@"
}

log() {
  printf "${CYAN}[canary]${NC} %s\n" "$*" >&2
}

mkdir -p "$OUT_DIR"

CANARY_CASES=(
  "1 np-ping-pong go rust 0 np-ping-pong-go-rust-max"
  "2 shm-ping-pong go go 0 shm-ping-pong-go-go-max"
  "2 shm-ping-pong c go 100000 shm-ping-pong-c-go-100000"
  "2 shm-ping-pong rust go 100000 shm-ping-pong-rust-go-100000"
  "3 snapshot-baseline c rust 0 snapshot-baseline-c-rust-max"
  "3 snapshot-baseline rust rust 0 snapshot-baseline-rust-rust-max"
  "3 snapshot-baseline rust go 0 snapshot-baseline-rust-go-max"
  "4 snapshot-shm c rust 0 snapshot-shm-c-rust-max"
  "7 lookup rust rust 0 lookup-rust-rust-max"
  "8 np-pipeline-d16 rust go 0 np-pipeline-d16-rust-go-max"
  "8 np-pipeline-d16 go go 0 np-pipeline-d16-go-go-max"
  "9 np-pipeline-batch-d16 c rust 0 np-pipeline-batch-d16-c-rust-max"
  "9 np-pipeline-batch-d16 c go 0 np-pipeline-batch-d16-c-go-max"
  "9 np-pipeline-batch-d16 go go 0 np-pipeline-batch-d16-go-go-max"
)

failures=0

for case_def in "${CANARY_CASES[@]}"; do
  read -r block scenario client server target label <<< "$case_def"
  csv="${OUT_DIR}/${label}.csv"
  log "Running ${label}"
  if ! run env \
      NIPC_BENCH_FIRST_BLOCK="${block}" \
      NIPC_BENCH_LAST_BLOCK="${block}" \
      NIPC_BENCH_SCENARIOS="${scenario}" \
      NIPC_BENCH_CLIENTS="${client}" \
      NIPC_BENCH_SERVERS="${server}" \
      NIPC_BENCH_TARGETS="${target}" \
      bash "$RUNNER" "$csv" "$DURATION"; then
    failures=$((failures + 1))
  fi
  sleep "$ROW_SETTLE_SEC"
done

if [ "$failures" -ne 0 ]; then
  printf "${RED}[canary]${NC} %s canary case(s) failed\n" "$failures" >&2
  exit 1
fi

printf "${GREEN}[canary]${NC} all canary cases passed\n" >&2
