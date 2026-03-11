#!/usr/bin/env bash
# Windows IPC benchmark for C, Rust, Go
export PATH="/c/Users/costa/.cargo/bin:/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Program Files/Go/bin:$PATH"
export GOROOT="/c/Program Files/Go"

BASE="/c/Users/costa/src/plugin-ipc-win.git"
C_BIN="$BASE/build/bin/netipc-live-c.exe"
RS_BIN="$BASE/build/bin/netipc_live_win_rs.exe"
GO_BIN="$BASE/build/bin/netipc-live-go-win.exe"
DIR="/tmp/bench_win"
TOK=12345
DUR=5

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
