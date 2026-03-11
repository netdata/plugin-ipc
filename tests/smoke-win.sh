#!/bin/bash
set -e
export PATH="/c/msys64/mingw64/bin:/c/Program Files/Go/bin:$PATH"
export GOROOT="/c/Program Files/Go"

BASE="/c/Users/costa/src/plugin-ipc-win.git"
C_BIN="$BASE/build/bin/netipc-live-c.exe"
RS_BIN="$BASE/build/bin/netipc_live_win_rs.exe"
GO_BIN="$BASE/build/bin/netipc-live-go-win.exe"
DIR="/tmp/smoke_win"
TOK=12345
SLOG="/tmp/smoke_server_$$.txt"

passed=0
failed=0

run_test() {
  local label="$1"
  local server_bin="$2"
  local client_bin="$3"
  local sprof="$4"

  local svc="smoke-${sprof}-$$-$RANDOM"

  export NETIPC_SUPPORTED_PROFILES="$sprof"
  export NETIPC_PREFERRED_PROFILES="$sprof"
  export NETIPC_AUTH_TOKEN="$TOK"
  unset NETIPC_SHM_SPIN_TRIES

  > "$SLOG"
  "$server_bin" server-once "$DIR" "$svc" 2>"$SLOG" &
  local sp=$!
  sleep 2

  local client_out
  if client_out=$(timeout 10 "$client_bin" client-once "$DIR" "$svc" 42 2>&1); then
    echo "  PASS: $label"
    echo "    server: $(cat "$SLOG" 2>/dev/null)"
    echo "    client: $client_out"
    ((passed++)) || true
  else
    echo "  FAIL: $label"
    echo "    server: $(cat "$SLOG" 2>/dev/null)"
    echo "    client: $client_out"
    ((failed++)) || true
  fi
  wait $sp 2>/dev/null || true
}

echo "=== Named Pipe (profile=1) ==="
run_test "C server + C client" "$C_BIN" "$C_BIN" 1
run_test "Rust server + Rust client" "$RS_BIN" "$RS_BIN" 1
run_test "Go server + Go client" "$GO_BIN" "$GO_BIN" 1
run_test "C server + Rust client" "$C_BIN" "$RS_BIN" 1
run_test "C server + Go client" "$C_BIN" "$GO_BIN" 1
run_test "Rust server + C client" "$RS_BIN" "$C_BIN" 1
run_test "Rust server + Go client" "$RS_BIN" "$GO_BIN" 1
run_test "Go server + C client" "$GO_BIN" "$C_BIN" 1
run_test "Go server + Rust client" "$GO_BIN" "$RS_BIN" 1

echo ""
echo "=== SHM HYBRID (profile=2) ==="
run_test "C server + C client" "$C_BIN" "$C_BIN" 2
run_test "Rust server + Rust client" "$RS_BIN" "$RS_BIN" 2
run_test "Go server + Go client" "$GO_BIN" "$GO_BIN" 2
run_test "C server + Rust client" "$C_BIN" "$RS_BIN" 2
run_test "C server + Go client" "$C_BIN" "$GO_BIN" 2
run_test "Rust server + C client" "$RS_BIN" "$C_BIN" 2
run_test "Rust server + Go client" "$RS_BIN" "$GO_BIN" 2
run_test "Go server + C client" "$GO_BIN" "$C_BIN" 2
run_test "Go server + Rust client" "$GO_BIN" "$RS_BIN" 2

echo ""
echo "=== Results: $passed passed, $failed failed ==="

rm -f "$SLOG"
[ "$failed" -eq 0 ]
