#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-mingw}"
BIN_DIR="${C_BIN_DIR:-${BUILD_DIR}/bin}"
SERVER_PID=""
SERVER_LOG=""
RESULTS_TMP=""

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

  SERVER_PID=""
  SERVER_LOG=""
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
  if [[ -n "${RESULTS_TMP}" ]] && [[ -f "${RESULTS_TMP}" ]]; then
    rm -f "${RESULTS_TMP}"
  fi
}
trap cleanup EXIT

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows profile bench requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

if [[ "${MSYSTEM:-}" == "MSYS" ]]; then
  echo "error: run this benchmark from mingw64.exe or ucrt64.exe, not the plain msys shell" >&2
  exit 1
fi

run cmake -S . -B "${BUILD_DIR}" -G Ninja
run cmake --build "${BUILD_DIR}" --target netipc-live-c

printf '%s\n' 'mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores'

if [[ -n "${NETIPC_RESULTS_FILE:-}" ]]; then
  mkdir -p "$(dirname "${NETIPC_RESULTS_FILE}")"
  RESULTS_TMP="$(dirname "${NETIPC_RESULTS_FILE}")/.$(basename "${NETIPC_RESULTS_FILE}").tmp.$$"
  printf '%s\n' 'mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores' > "${RESULTS_TMP}"
fi

run_case() {
  local supported=$1
  local preferred=$2
  local target_rps=$3
  local service=$4
  local server_log="${BUILD_DIR}/${service}.log"

  start_server "${server_log}" env \
    NETIPC_SUPPORTED_PROFILES="${supported}" \
    NETIPC_PREFERRED_PROFILES="${preferred}" \
    "${BIN_DIR}/netipc-live-c.exe" server-loop /tmp "${service}" 0
  sleep 0.2

  local row
  row=$(env \
    NETIPC_SUPPORTED_PROFILES="${supported}" \
    NETIPC_PREFERRED_PROFILES="${preferred}" \
    "${BIN_DIR}/netipc-live-c.exe" client-bench /tmp "${service}" 5 "${target_rps}" | awk -F, '/^c-npipe,|^c-shm-hybrid,|^c-shm-busywait,/ {print $0}')

  printf '%s\n' "${row}"
  if [[ -n "${RESULTS_TMP}" ]]; then
    printf '%s\n' "${row}" >> "${RESULTS_TMP}"
  fi

  wait_server
}

for target_rps in 0 100000 10000; do
  run_case 1 1 "${target_rps}" "netipc-win-npipe-${target_rps}-$$"
  run_case 3 2 "${target_rps}" "netipc-win-shm-${target_rps}-$$"
  run_case 7 4 "${target_rps}" "netipc-win-spin-${target_rps}-$$"
done

if [[ -n "${RESULTS_TMP}" ]]; then
  mv "${RESULTS_TMP}" "${NETIPC_RESULTS_FILE}"
  RESULTS_TMP=""
fi
