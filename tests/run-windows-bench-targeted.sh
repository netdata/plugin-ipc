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
OUT_DIR="${TEMP:-/tmp}/netipc-bench-targeted"
DURATION=5
ROW_SETTLE_SEC="${NIPC_BENCH_ROW_SETTLE_SEC:-2}"
ATTEMPTS="${NIPC_BENCH_TARGETED_ATTEMPTS:-1}"
DIAGNOSTICS_SUMMARY=""

usage() {
  cat <<EOF
Usage:
  bash tests/run-windows-bench-targeted.sh [options] [row_spec ...]

Options:
  --out-dir DIR
      Directory for per-row CSV outputs.
  --duration SEC
      Sample duration to pass through to tests/run-windows-bench.sh.
  --diagnostics-summary PATH
      Read failed-row tuples from a diagnostics-summary.txt file emitted by
      tests/run-windows-bench.sh when NIPC_BENCH_DIAGNOSE_FAILURES=1.
  --attempts COUNT
      Maximum attempts per row. Defaults to NIPC_BENCH_TARGETED_ATTEMPTS or 1.
  --row SPEC
      Explicit row tuple in scenario,client,server,target form.
      ':' separators are also accepted.
  -h, --help
      Show this help.

Examples:
  bash tests/run-windows-bench-targeted.sh \
    --row np-ping-pong,c,rust,0 \
    --row np-ping-pong,c,rust,100000

  bash tests/run-windows-bench-targeted.sh \
    --diagnostics-summary /tmp/netipc-bench-52658/diagnostics-summary.txt
EOF
}

run() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"
  "$@"
}

log() {
  printf "${CYAN}[targeted]${NC} %s\n" "$*" >&2
}

err() {
  printf "${RED}[targeted]${NC} %s\n" "$*" >&2
}

sanitize_label() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

extract_rows_from_diagnostics() {
  local file="$1"
  awk '
    /^scenario=/ {
      scenario = ""
      client = ""
      server = ""
      target = ""
      for (i = 1; i <= NF; i++) {
        split($i, kv, "=")
        if (kv[1] == "scenario") {
          scenario = kv[2]
        } else if (kv[1] == "client") {
          client = kv[2]
        } else if (kv[1] == "server") {
          server = kv[2]
        } else if (kv[1] == "target_rps") {
          target = kv[2]
        }
      }
      if (scenario != "" && client != "" && server != "" && target != "") {
        printf "%s,%s,%s,%s\n", scenario, client, server, target
      }
    }
  ' "$file"
}

declare -a ROW_SPECS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --duration)
      DURATION="$2"
      shift 2
      ;;
    --diagnostics-summary)
      DIAGNOSTICS_SUMMARY="$2"
      shift 2
      ;;
    --attempts)
      ATTEMPTS="$2"
      shift 2
      ;;
    --row)
      ROW_SPECS+=("$2")
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      ROW_SPECS+=("$1")
      shift
      ;;
  esac
done

if [ -n "$DIAGNOSTICS_SUMMARY" ]; then
  if [ ! -f "$DIAGNOSTICS_SUMMARY" ]; then
    err "diagnostics summary not found: $DIAGNOSTICS_SUMMARY"
    exit 1
  fi
  while IFS= read -r row; do
    [ -n "$row" ] || continue
    ROW_SPECS+=("$row")
  done < <(extract_rows_from_diagnostics "$DIAGNOSTICS_SUMMARY")
fi

if [ "${#ROW_SPECS[@]}" -eq 0 ]; then
  usage
  exit 1
fi

case "$ATTEMPTS" in
  ''|*[!0-9]*)
    err "invalid attempts value: ${ATTEMPTS}"
    exit 1
    ;;
esac
if [ "$ATTEMPTS" -lt 1 ]; then
  err "attempts must be >= 1"
  exit 1
fi

mkdir -p "$OUT_DIR"

declare -A SEEN_ROWS=()
failures=0
ran=0

for row_spec in "${ROW_SPECS[@]}"; do
  IFS=',:' read -r scenario client server target extra <<< "$row_spec"
  if [ -n "${extra:-}" ] || [ -z "${scenario:-}" ] || [ -z "${client:-}" ] || \
     [ -z "${server:-}" ] || [ -z "${target:-}" ]; then
    err "invalid row spec: $row_spec"
    failures=$((failures + 1))
    continue
  fi

  normalized="${scenario},${client},${server},${target}"
  if [ -n "${SEEN_ROWS[$normalized]+x}" ]; then
    continue
  fi
  SEEN_ROWS[$normalized]=1

  label="$(sanitize_label "${scenario}-${client}-${server}-${target}")"
  csv="${OUT_DIR}/${label}.csv"

  row_ok=0
  for attempt in $(seq 1 "$ATTEMPTS"); do
    if [ "$ATTEMPTS" -gt 1 ]; then
      log "Running ${scenario} ${client}->${server} @ ${target} (attempt ${attempt}/${ATTEMPTS})"
    else
      log "Running ${scenario} ${client}->${server} @ ${target}"
    fi

    if run env \
        NIPC_BENCH_SCENARIOS="${scenario}" \
        NIPC_BENCH_CLIENTS="${client}" \
        NIPC_BENCH_SERVERS="${server}" \
        NIPC_BENCH_TARGETS="${target}" \
        bash "$RUNNER" "$csv" "$DURATION"; then
      row_ok=1
      break
    fi

    if [ "$attempt" -lt "$ATTEMPTS" ]; then
      log "Retrying ${scenario} ${client}->${server} @ ${target} after failed attempt ${attempt}/${ATTEMPTS}"
      sleep "$ROW_SETTLE_SEC"
    fi
  done

  if [ "$row_ok" -ne 1 ]; then
    failures=$((failures + 1))
  fi

  ran=$((ran + 1))
  sleep "$ROW_SETTLE_SEC"
done

if [ "$ran" -eq 0 ]; then
  err "no runnable row specs were found"
  exit 1
fi

if [ "$failures" -ne 0 ]; then
  err "${failures} targeted row(s) failed"
  exit 1
fi

printf "${GREEN}[targeted]${NC} reran %s targeted row(s) successfully\n" "$ran" >&2
