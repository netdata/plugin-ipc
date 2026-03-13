#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

CMAKE_BUILD_DIR="${NETIPC_CMAKE_BUILD_DIR:-build}"
C_BIN_DIR="${CMAKE_BUILD_DIR}/bin"
C_LIVE_BIN="${NETIPC_CGROUPS_LIVE_C_BIN:-${C_BIN_DIR}/netipc-cgroups-live-c}"
GO_CODEC_BIN="${NETIPC_CGROUPS_GO_BIN:-${C_BIN_DIR}/netipc-codec-go}"
RUST_CODEC_BIN="${NETIPC_CGROUPS_RUST_BIN:-${C_BIN_DIR}/netipc-codec-rs}"
SHM_PROFILE=2
SERVER_PID=""
SERVER_LOG=""
TEMP_ROOT=""

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

configure_build() {
  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi
  run cmake -S . -B "${CMAKE_BUILD_DIR}"
}

build_targets() {
  if [[ "${NETIPC_SKIP_BUILD:-0}" == "1" ]]; then
    return 0
  fi
  configure_build
  run cmake --build "${CMAKE_BUILD_DIR}" --target netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
}

start_server() {
  SERVER_LOG=$1
  shift

  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " env NETIPC_SUPPORTED_PROFILES="${SHM_PROFILE}" NETIPC_PREFERRED_PROFILES="${SHM_PROFILE}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  env NETIPC_SUPPORTED_PROFILES="${SHM_PROFILE}" NETIPC_PREFERRED_PROFILES="${SHM_PROFILE}" \
    "$@" >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
}

wait_for_socket() {
  local path=$1
  local attempts=$2

  for ((i = 0; i < attempts; i++)); do
    if [[ -S "${path}" ]]; then
      return 0
    fi
    sleep 0.01
  done

  echo -e >&2 "${RED}[ERROR] endpoint ${path} was not created in time${NC}"
  return 1
}

wait_server() {
  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  if ! timeout 10 tail --pid="${SERVER_PID}" -f /dev/null; then
    echo -e >&2 "${RED}[ERROR] Server process did not exit in time (pid=${SERVER_PID}). Log:${NC}"
    cat "${SERVER_LOG}" >&2 || true
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
    fi
    SERVER_PID=""
    SERVER_LOG=""
    return 1
  fi

  if ! wait "${SERVER_PID}"; then
    local rc=$?
    echo -e >&2 "${RED}[ERROR] Server process failed (pid=${SERVER_PID}, rc=${rc}). Log:${NC}"
    cat "${SERVER_LOG}" >&2 || true
    SERVER_PID=""
    SERVER_LOG=""
    return $rc
  fi

  SERVER_PID=""
  SERVER_LOG=""
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" || true
    wait "${SERVER_PID}" || true
  fi
  if [[ -n "${TEMP_ROOT}" ]] && [[ -d "${TEMP_ROOT}" ]]; then
    rm -rf "${TEMP_ROOT}"
  fi
}
trap cleanup EXIT

run_client() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " env NETIPC_SUPPORTED_PROFILES="${SHM_PROFILE}" NETIPC_PREFERRED_PROFILES="${SHM_PROFILE}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"

  env NETIPC_SUPPORTED_PROFILES="${SHM_PROFILE}" NETIPC_PREFERRED_PROFILES="${SHM_PROFILE}" "$@"
}

run_case() {
  local label=$1
  local server_bin=$2
  local client_bin=$3
  local mode=$4
  local lookup_hash=$5
  local lookup_name=$6
  local expected_lookup=$7
  local case_dir service client_out client_log

  case_dir=$(mktemp -d "${TEMP_ROOT}/${mode}.XXXXXX")
  service="netipc-cgroups-shm-${mode}-${RANDOM}"

  if [[ "${mode}" == "once" ]]; then
    start_server "${case_dir}/server.log" "${server_bin}" server-once "${case_dir}" "${service}"
  else
    start_server "${case_dir}/server.log" "${server_bin}" server-loop "${case_dir}" "${service}" 2
  fi
  wait_for_socket "${case_dir}/${service}.sock" 500

  client_log="${case_dir}/client.out"
  if [[ "${mode}" == "once" ]]; then
    run_client "${client_bin}" client-refresh-once "${case_dir}" "${service}" "${lookup_hash}" "${lookup_name}" \
      >"${client_log}" 2>&1
  else
    run_client "${client_bin}" client-refresh-loop "${case_dir}" "${service}" 2 "${lookup_hash}" "${lookup_name}" \
      >"${client_log}" 2>&1
  fi

  client_out=$(cat "${client_log}")

  printf '%s\n' "${client_out}"
  wait_server

  grep -q $'^CGROUPS_CACHE\t42\t1\t2$' <<<"${client_out}" || {
    echo -e >&2 "${RED}[ERROR] ${label}: unexpected cache header${NC}"
    exit 1
  }

  if [[ "${mode}" == "loop" ]]; then
    grep -q $'^REFRESHES\t2$' <<<"${client_out}" || {
      echo -e >&2 "${RED}[ERROR] ${label}: expected two refreshes${NC}"
      exit 1
    }
  fi

  grep -q $'^LOOKUP\t'"${expected_lookup}"'$' <<<"${client_out}" || {
    echo -e >&2 "${RED}[ERROR] ${label}: expected lookup hit${NC}"
    exit 1
  }
}

build_targets
TEMP_ROOT=$(mktemp -d)

for bin in "${C_LIVE_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}"; do
  if [[ ! -x "${bin}" ]]; then
    echo -e >&2 "${RED}[ERROR] missing executable ${bin}${NC}"
    exit 1
  fi
done

labels=("c" "go" "rust")
bins=("${C_LIVE_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}")

for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
  for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
    run_case \
      "${labels[server_idx]} server + ${labels[client_idx]} client (refresh once)" \
      "${bins[server_idx]}" \
      "${bins[client_idx]}" \
      "once" \
      123 \
      "system.slice-nginx" \
      $'123\t2\t1\tsystem.slice-nginx\t/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs'

    run_case \
      "${labels[server_idx]} server + ${labels[client_idx]} client (refresh loop)" \
      "${bins[server_idx]}" \
      "${bins[client_idx]}" \
      "loop" \
      456 \
      "docker-1234" \
      $'456\t4\t0\tdocker-1234\t'
  done
done

echo -e "${GREEN}Live SHM cgroups snapshot tests passed (C/Rust/Go producers and clients).${NC}"
