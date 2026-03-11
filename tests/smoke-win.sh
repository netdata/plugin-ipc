#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-${BUILD_DIR:-build-mingw}}"
BIN_DIR="${NETIPC_WINDOWS_BIN_DIR:-${BUILD_DIR}/bin}"
C_BIN="${NETIPC_WINDOWS_C_BIN:-${BIN_DIR}/netipc-live-c.exe}"
RS_BIN="${NETIPC_WINDOWS_RS_BIN:-${BIN_DIR}/netipc_live_win_rs.exe}"
GO_BIN="${NETIPC_WINDOWS_GO_BIN:-${BIN_DIR}/netipc-live-go-win.exe}"
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/smoke_win}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
SLOG="${TMPDIR:-/tmp}/smoke_server_$$.txt"
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

passed=0
failed=0

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
    echo "skip: Windows smoke test requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

if [[ "${MSYSTEM:-}" == "MSYS" ]]; then
  echo "error: run this smoke test from mingw64.exe or ucrt64.exe, not the plain msys shell" >&2
  exit 1
fi

cd "${ROOT_DIR}"
build_targets

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
