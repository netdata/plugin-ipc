#!/usr/bin/env bash
set -euo pipefail

DEFAULT_BUILD_DIR="build-mingw"
if [[ "${MSYSTEM:-}" == "MSYS" ]]; then
  DEFAULT_BUILD_DIR="build-msys-ninja"
fi
BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-${BUILD_DIR:-${DEFAULT_BUILD_DIR}}}"
BIN_DIR="${C_BIN_DIR:-${BUILD_DIR}/bin}"
SERVER_PID=""
SERVER_LOG=""

configure_build() {
  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi

  run cmake -S . -B "${BUILD_DIR}" -G Ninja
}

build_targets() {
  if [[ "${NETIPC_SKIP_BUILD:-0}" == "1" ]]; then
    return 0
  fi

  configure_build
  run cmake --build "${BUILD_DIR}" --target netipc-live-c
}

run() {
  printf >&2 '%s\n' "+ $*"
  "$@"
}

start_server() {
  SERVER_LOG=$1
  shift
  printf >&2 '%s\n' "+ $*"
  MSYS2_ARG_CONV_EXCL='*' "$@" >"${SERVER_LOG}" 2>&1 &
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

build_targets

SERVICE="netipc-win-smoke-$$"
SERVER_LOG_PATH="${BUILD_DIR}/netipc-live-c-server.log"
CLIENT_LOG_PATH="${BUILD_DIR}/netipc-live-c-client.log"
rm -f "${SERVER_LOG_PATH}" "${CLIENT_LOG_PATH}"

start_server "${SERVER_LOG_PATH}" "${BIN_DIR}/netipc-live-c.exe" server-once /tmp "${SERVICE}"
sleep 0.2
MSYS2_ARG_CONV_EXCL='*' "${BIN_DIR}/netipc-live-c.exe" client-once /tmp "${SERVICE}" 41 >"${CLIENT_LOG_PATH}" 2>&1
wait_server

cat "${CLIENT_LOG_PATH}" >&2

grep -q 'C-WIN-SERVER' "${SERVER_LOG_PATH}"
grep -q 'response=42' "${SERVER_LOG_PATH}"
grep -q 'C-WIN-CLIENT' "${CLIENT_LOG_PATH}"
grep -q 'response=42' "${CLIENT_LOG_PATH}"

echo "Windows Named Pipe smoke test passed."
