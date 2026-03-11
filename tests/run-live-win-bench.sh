#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-${BUILD_DIR:-build-mingw}}"
BIN_DIR="${NETIPC_WINDOWS_BIN_DIR:-${BUILD_DIR}/bin}"
C_BIN="${NETIPC_WINDOWS_C_BIN:-${BIN_DIR}/netipc-live-c.exe}"
RS_BIN="${NETIPC_WINDOWS_RS_BIN:-${BIN_DIR}/netipc_live_win_rs.exe}"
GO_BIN="${NETIPC_WINDOWS_GO_BIN:-${BIN_DIR}/netipc-live-go-win.exe}"
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/bench_win}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
DUR="${NETIPC_BENCH_DURATION_SEC:-5}"
GO_EXECUTABLE="${NETIPC_GO_EXECUTABLE:-}"

if [[ -z "${GO_EXECUTABLE}" && -x /c/Program\ Files/Go/bin/go.exe ]]; then
  GO_EXECUTABLE="/c/Program Files/Go/bin/go.exe"
fi

if [[ -n "${GO_EXECUTABLE}" ]]; then
  if [[ -n "${NETIPC_GOROOT:-}" ]]; then
    export GOROOT="${NETIPC_GOROOT}"
  elif [[ -z "${GOROOT:-}" && "${GO_EXECUTABLE}" == /* ]]; then
    export GOROOT
    GOROOT=$(cd "$(dirname "${GO_EXECUTABLE}")/.." && pwd)
  fi
fi

run() {
  printf >&2 '%s\n' "+ $*"
  "$@"
}

configure_build() {
  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi

  local cmake_args=(-S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja)
  if [[ -n "${GO_EXECUTABLE}" ]]; then
    cmake_args+=("-DGO_EXECUTABLE=${GO_EXECUTABLE}")
  fi

  run cmake "${cmake_args[@]}"
}

build_targets() {
  if [[ "${NETIPC_SKIP_BUILD:-0}" == "1" ]]; then
    return 0
  fi

  configure_build
  run cmake --build "${BUILD_DIR}" --target netipc-live-c netipc_live_win_rs netipc-live-go-win
}

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows benchmark requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

if [[ "${MSYSTEM:-}" == "MSYS" ]]; then
  echo "error: run this benchmark from mingw64.exe or ucrt64.exe, not the plain msys shell" >&2
  exit 1
fi

cd "${ROOT_DIR}"
build_targets

for bin in "$C_BIN" "$RS_BIN" "$GO_BIN"; do
  if [ ! -f "$bin" ]; then
    echo "Missing binary: $bin" >&2
    exit 1
  fi
done

results_file="/tmp/bench_results_$$.txt"
> "$results_file"

run_bench() {
  local label="$1" lang="$2" bin="$3" sprof="$4" pprof="$5" spin="$6" target_rps="$7"
  local svc="bench-${lang}-$$-${RANDOM}"
  local slog="/tmp/slog_${svc}.txt"

  export NETIPC_SUPPORTED_PROFILES="$sprof"
  export NETIPC_PREFERRED_PROFILES="$pprof"
  export NETIPC_AUTH_TOKEN="$TOK"
  if [ -n "$spin" ]; then
    export NETIPC_SHM_SPIN_TRIES="$spin"
  else
    unset NETIPC_SHM_SPIN_TRIES 2>/dev/null || true
  fi

  > "$slog"
  "$bin" server-loop "$DIR" "$svc" 0 2>"$slog" &
  sleep 2

  local client_out
  client_out=$("$bin" client-bench "$DIR" "$svc" "$DUR" "$target_rps" 2>/dev/null) || true
  wait 2>/dev/null || true

  local server_cpu
  server_cpu=$(grep 'SERVER_CPU_CORES=' "$slog" 2>/dev/null | head -1 | cut -d= -f2) || true
  [ -z "$server_cpu" ] && server_cpu="N/A"

  local data
  data=$(echo "$client_out" | tail -1)
  local mode tput p50 p95 p99 c_cpu
  mode=$(echo "$data" | cut -d, -f1)
  tput=$(echo "$data" | cut -d, -f7)
  p50=$(echo "$data" | cut -d, -f8)
  p95=$(echo "$data" | cut -d, -f9)
  p99=$(echo "$data" | cut -d, -f10)
  c_cpu=$(echo "$data" | cut -d, -f11)

  [ -z "$tput" ] && tput="FAIL"

  local total_cpu="N/A"
  if [ -n "$c_cpu" ] && [ "$server_cpu" != "N/A" ]; then
    total_cpu=$(awk -v c="$c_cpu" -v s="$server_cpu" 'BEGIN { printf "%.3f", c + s }')
  fi

  printf "%-8s | %-18s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
    "$label" "$mode" "$tput" "$p50" "$p95" "$p99" "$c_cpu" "$server_cpu" "$total_cpu" | tee -a "$results_file"

  rm -f "$slog"
}

echo ""
echo "Running Windows IPC benchmarks (${DUR}s each)..."
echo ""
printf "%-8s | %-18s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
  "Scenario" "Method" "Throughput (rps)" "p50 (us)" "p95 (us)" "p99 (us)" "Client CPU" "Server CPU" "Total"
printf -- "---------+--------------------+--------------------+----------+----------+----------+------------+------------+---------\n"

run_bench "max" "c" "$C_BIN" 2 2 "" 0
run_bench "max" "rs" "$RS_BIN" 2 2 "" 0
run_bench "max" "go" "$GO_BIN" 2 2 "" 0
run_bench "max" "c" "$C_BIN" 1 1 "" 0
run_bench "max" "rs" "$RS_BIN" 1 1 "" 0
run_bench "max" "go" "$GO_BIN" 1 1 "" 0
run_bench "100k/s" "c" "$C_BIN" 2 2 "" 100000
run_bench "100k/s" "rs" "$RS_BIN" 2 2 "" 100000
run_bench "100k/s" "go" "$GO_BIN" 2 2 "" 100000
run_bench "10k/s" "c" "$C_BIN" 1 1 "" 10000
run_bench "10k/s" "rs" "$RS_BIN" 1 1 "" 10000
run_bench "10k/s" "go" "$GO_BIN" 1 1 "" 10000

echo ""
echo "Windows IPC benchmark complete."
echo "Results saved to $results_file"
