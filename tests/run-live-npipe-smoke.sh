#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-mingw}"
BIN_DIR="${C_BIN_DIR:-${BUILD_DIR}/bin}"
SERVER_PID=""
SERVER_LOG=""

run() {
  printf >&2 '%s\n' "+ $*"
  "$@"
}

start_server() {
  SERVER_LOG=$1
  shift
  printf >&2 '%s\n' "+ $*"
  "$@" >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
}

wait_server() {
  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  if ! wait "${SERVER_PID}"; then
    local rc=$?
    cat "${SERVER_LOG}" >&2 || true
    SERVER_PID=""
    SERVER_LOG=""
    return "${rc}"
  fi

  cat "${SERVER_LOG}" >&2 || true
  SERVER_PID=""
  SERVER_LOG=""
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
}
trap cleanup EXIT

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows Named Pipe smoke test requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

if [[ "${MSYSTEM:-}" == "MSYS" ]]; then
  echo "error: run this test from mingw64.exe or ucrt64.exe, not the plain msys shell" >&2
  exit 1
fi

run cmake -S . -B "${BUILD_DIR}" -G Ninja
run cmake --build "${BUILD_DIR}" --target netipc-live-c

SERVICE="netipc-win-smoke-$$"
SERVER_LOG_PATH="${BUILD_DIR}/netipc-live-c-server.log"
CLIENT_LOG_PATH="${BUILD_DIR}/netipc-live-c-client.log"
rm -f "${SERVER_LOG_PATH}" "${CLIENT_LOG_PATH}"

start_server "${SERVER_LOG_PATH}" "${BIN_DIR}/netipc-live-c.exe" server-once /tmp "${SERVICE}"
sleep 0.2
"${BIN_DIR}/netipc-live-c.exe" client-once /tmp "${SERVICE}" 41 >"${CLIENT_LOG_PATH}" 2>&1
wait_server

cat "${CLIENT_LOG_PATH}" >&2

grep -q 'C-WIN-SERVER' "${SERVER_LOG_PATH}"
grep -q 'response=42' "${SERVER_LOG_PATH}"
grep -q 'C-WIN-CLIENT' "${CLIENT_LOG_PATH}"
grep -q 'response=42' "${CLIENT_LOG_PATH}"

echo "Windows Named Pipe smoke test passed."
