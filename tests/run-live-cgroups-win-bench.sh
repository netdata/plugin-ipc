#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NATIVE_BUILD_DIR="${NETIPC_WINDOWS_NATIVE_BUILD_DIR:-${NETIPC_CMAKE_BUILD_DIR:-build-mingw}}"
MSYS_BUILD_DIR="${NETIPC_WINDOWS_MSYS_BUILD_DIR:-build-msys-ninja}"
NATIVE_BIN_DIR="${NETIPC_WINDOWS_NATIVE_BIN_DIR:-${NATIVE_BUILD_DIR}/bin}"
MSYS_BIN_DIR="${NETIPC_WINDOWS_MSYS_BIN_DIR:-${MSYS_BUILD_DIR}/bin}"
C_NATIVE_BIN="${NETIPC_WINDOWS_CGROUPS_C_NATIVE_BIN:-${NATIVE_BIN_DIR}/netipc-cgroups-live-c.exe}"
C_MSYS_BIN="${NETIPC_WINDOWS_CGROUPS_C_MSYS_BIN:-${MSYS_BIN_DIR}/netipc-cgroups-live-c.exe}"
GO_CODEC_BIN="${NETIPC_WINDOWS_CGROUPS_GO_BIN:-${ROOT_DIR}/${NATIVE_BUILD_DIR}/bin/netipc-codec-go.exe}"
RUST_CODEC_BIN="${NETIPC_WINDOWS_CGROUPS_RUST_BIN:-${ROOT_DIR}/${NATIVE_BUILD_DIR}/bin/netipc-codec-rs.exe}"
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/cgroups_win_bench}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
REFRESH_TARGETS_STR="${NETIPC_CGROUPS_REFRESH_TARGETS:-0 1000}"
LOOKUP_TARGETS_STR="${NETIPC_CGROUPS_LOOKUP_TARGETS:-0 1000}"
BENCH_DURATION_SEC="${NETIPC_BENCH_DURATION_SEC:-5}"
PROFILE_LABELS=("named-pipe" "shm-hybrid")
PROFILE_VALUES=(1 2)
GO_EXECUTABLE="${NETIPC_GO_EXECUTABLE:-}"
NATIVE_CMAKE="${NETIPC_WINDOWS_NATIVE_CMAKE:-}"
MSYS_CMAKE="${NETIPC_WINDOWS_MSYS_CMAKE:-}"
NATIVE_PATH="${NETIPC_WINDOWS_NATIVE_PATH:-/c/Users/costa/.cargo/bin:/mingw64/bin:/usr/bin}"
MSYS_PATH="${NETIPC_WINDOWS_MSYS_PATH:-/c/Users/costa/.cargo/bin:/usr/bin}"
SERVER_PID=""
SERVER_LOG=""
RESULTS_FILE="${NETIPC_RESULTS_FILE:-/tmp/cgroups_win_bench_results_$$.txt}"

if [[ -x /c/Program\ Files/Go/bin/go.exe ]]; then
  NATIVE_PATH="${NATIVE_PATH}:/c/Program Files/Go/bin"
  MSYS_PATH="${MSYS_PATH}:/c/Program Files/Go/bin"
  if [[ -z "${GO_EXECUTABLE}" ]]; then
    GO_EXECUTABLE="/c/Program Files/Go/bin/go.exe"
  fi
fi

if [[ -z "${NATIVE_CMAKE}" && -x /mingw64/bin/cmake ]]; then
  NATIVE_CMAKE="/mingw64/bin/cmake"
fi
if [[ -z "${MSYS_CMAKE}" && -x /usr/bin/cmake ]]; then
  MSYS_CMAKE="/usr/bin/cmake"
fi
if [[ -z "${NATIVE_CMAKE}" ]]; then
  NATIVE_CMAKE="cmake"
fi
if [[ -z "${MSYS_CMAKE}" ]]; then
  MSYS_CMAKE="cmake"
fi

run() {
  printf >&2 '%s\n' "+ $*"
  "$@"
}

configure_build() {
  local build_dir=$1
  local cmake_bin=$2
  local runtime_path=$3
  local msystem=$4
  local helper_setting=$5

  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi

  local cmake_args=(
    -S "${ROOT_DIR}"
    -B "${build_dir}"
    -G Ninja
    "-DNETIPC_BUILD_HELPERS=${helper_setting}"
  )
  if [[ -n "${GO_EXECUTABLE}" && "${helper_setting}" == "ON" ]]; then
    cmake_args+=("-DGO_EXECUTABLE=${GO_EXECUTABLE}")
  fi

  run env \
    "PATH=${runtime_path}:${PATH}" \
    "MSYSTEM=${msystem}" \
    "CHERE_INVOKING=1" \
    "${cmake_bin}" "${cmake_args[@]}"
}

build_targets() {
  if [[ "${NETIPC_SKIP_BUILD:-0}" == "1" ]]; then
    return 0
  fi

  configure_build "${NATIVE_BUILD_DIR}" "${NATIVE_CMAKE}" "${NATIVE_PATH}" "MINGW64" "ON"
  run env \
    "PATH=${NATIVE_PATH}:${PATH}" \
    "MSYSTEM=MINGW64" \
    "CHERE_INVOKING=1" \
    "${NATIVE_CMAKE}" --build "${NATIVE_BUILD_DIR}" --target netipc-cgroups-live-c netipc-codec-go netipc-codec-rs

  configure_build "${MSYS_BUILD_DIR}" "${MSYS_CMAKE}" "${MSYS_PATH}" "MSYS" "OFF"
  run env \
    "PATH=${MSYS_PATH}:${PATH}" \
    "MSYSTEM=MSYS" \
    "CHERE_INVOKING=1" \
    "${MSYS_CMAKE}" --build "${MSYS_BUILD_DIR}" --target netipc-cgroups-live-c
}

start_server() {
  SERVER_LOG=$1
  local supported_profiles=$2
  local preferred_profiles=$3
  shift
  shift
  shift
  printf >&2 '%s\n' "+ $*"
  MSYS2_ARG_CONV_EXCL='*' \
    NETIPC_SUPPORTED_PROFILES="${supported_profiles}" \
    NETIPC_PREFERRED_PROFILES="${preferred_profiles}" \
    "$@" >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
}

wait_server() {
  if [[ -z "${SERVER_PID}" ]]; then
    return 0
  fi

  wait "${SERVER_PID}"
  local rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "[SERVER LOG]" >&2
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
}
trap cleanup EXIT

scenario_label() {
  local target=$1
  if [[ "${target}" == "0" ]]; then
    printf 'max'
  else
    printf '%s/s' "${target}"
  fi
}

append_result() {
  local bench_type=$1
  local scenario=$2
  local client=$3
  local server=$4
  local row=$5
  local server_cpu=$6
  local mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu total_cpu

  IFS=, read -r mode duration target requests responses mismatches throughput p50 p95 p99 client_cpu <<<"${row}"

  total_cpu="N/A"
  if [[ -n "${client_cpu}" && -n "${server_cpu}" && "${server_cpu}" != "N/A" ]]; then
    total_cpu=$(awk -v c="${client_cpu}" -v s="${server_cpu}" 'BEGIN { printf "%.3f", c + s }')
  fi

  printf "%-7s | %-8s | %-10s | %-10s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
    "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}"

  printf "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n" \
    "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}" >> "${RESULTS_FILE}"
}

show_logs_and_fail() {
  local message=$1
  local client_log=$2
  local server_log=$3

  echo "[ERROR] ${message}" >&2
  if [[ -f "${client_log}" ]]; then
    echo "[CLIENT LOG]" >&2
    cat "${client_log}" >&2 || true
  fi
  if [[ -f "${server_log}" ]]; then
    echo "[SERVER LOG]" >&2
    cat "${server_log}" >&2 || true
  fi
  exit 1
}

run_refresh_case() {
  local server_label=$1
  local server_bin=$2
  local client_label=$3
  local client_bin=$4
  local target=$5
  local profile_label=$6
  local supported_profiles=$7
  local preferred_profiles=$8
  local case_dir service client_log server_log client_row server_cpu

  case_dir=$(mktemp -d "${DIR}/refresh.${server_label}.${client_label}.XXXXXX")
  service="netipc-cgroups-refresh-${server_label}-to-${client_label}-${RANDOM}"
  client_log="${case_dir}/client.log"
  server_log="${case_dir}/server.log"

  start_server "${server_log}" "${supported_profiles}" "${preferred_profiles}" \
    "${server_bin}" server-bench "${case_dir}" "${service}" 0 "${TOK}"
  sleep 2

  if ! MSYS2_ARG_CONV_EXCL='*' \
    NETIPC_SUPPORTED_PROFILES="${supported_profiles}" \
    NETIPC_PREFERRED_PROFILES="${preferred_profiles}" \
    "${client_bin}" client-refresh-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
      SERVER_PID=""
    fi
    show_logs_and_fail "refresh benchmark failed for ${server_label}->${client_label}" "${client_log}" "${server_log}"
  fi

  wait_server || show_logs_and_fail "refresh server failed for ${server_label}->${client_label}" "${client_log}" "${server_log}"
  client_row=$(tail -1 "${client_log}")
  server_cpu=$(grep 'SERVER_CPU_CORES=' "${server_log}" | tail -1 | cut -d= -f2)
  append_result "refresh" "$(scenario_label "${target}")" "${client_label}/${profile_label}" "${server_label}/${profile_label}" "${client_row}" "${server_cpu:-N/A}"
}

run_lookup_case() {
  local label=$1
  local bin=$2
  local target=$3
  local profile_label=$4
  local supported_profiles=$5
  local preferred_profiles=$6
  local case_dir service client_log server_log client_row server_cpu

  case_dir=$(mktemp -d "${DIR}/lookup.${label}.XXXXXX")
  service="netipc-cgroups-lookup-${label}-${RANDOM}"
  client_log="${case_dir}/client.log"
  server_log="${case_dir}/server.log"

  start_server "${server_log}" "${supported_profiles}" "${preferred_profiles}" \
    "${bin}" server-bench "${case_dir}" "${service}" 1 "${TOK}"
  sleep 2

  if ! MSYS2_ARG_CONV_EXCL='*' \
    NETIPC_SUPPORTED_PROFILES="${supported_profiles}" \
    NETIPC_PREFERRED_PROFILES="${preferred_profiles}" \
    "${bin}" client-lookup-bench "${case_dir}" "${service}" "${BENCH_DURATION_SEC}" "${target}" "123" "system.slice-nginx" "${TOK}" >"${client_log}" 2>&1; then
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill "${SERVER_PID}" || true
      wait "${SERVER_PID}" || true
      SERVER_PID=""
    fi
    show_logs_and_fail "lookup benchmark failed for ${label}" "${client_log}" "${server_log}"
  fi

  wait_server || show_logs_and_fail "lookup server failed for ${label}" "${client_log}" "${server_log}"
  client_row=$(tail -1 "${client_log}")
  server_cpu=$(grep 'SERVER_CPU_CORES=' "${server_log}" | tail -1 | cut -d= -f2)
  append_result "lookup" "$(scenario_label "${target}")" "${label}/${profile_label}" "${label}/${profile_label}" "${client_row}" "${server_cpu:-N/A}"
}

write_csv_results() {
  local output_path=$1
  local output_dir tmp_path

  output_dir=$(dirname "${output_path}")
  mkdir -p "${output_dir}"
  tmp_path="${output_dir}/.$(basename "${output_path}").tmp.$$"

  printf "bench_type,scenario,client,server,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores,server_cpu_cores,total_cpu_cores\n" > "${tmp_path}"
  while IFS='|' read -r bench_type scenario client server throughput p50 p95 p99 client_cpu server_cpu total_cpu; do
    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
      "${bench_type}" "${scenario}" "${client}" "${server}" "${throughput}" "${p50}" "${p95}" "${p99}" "${client_cpu}" "${server_cpu}" "${total_cpu}" >> "${tmp_path}"
  done < "${RESULTS_FILE}"

  mv "${tmp_path}" "${output_path}"
}

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows cgroups benchmark requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

cd "${ROOT_DIR}"
mkdir -p "${DIR}"
build_targets

for bin in "${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}"; do
  if [[ ! -f "${bin}" ]]; then
    echo "Missing binary: ${bin}" >&2
    exit 1
  fi
done

: > "${RESULTS_FILE}"
read -r -a refresh_targets <<<"${REFRESH_TARGETS_STR}"
read -r -a lookup_targets <<<"${LOOKUP_TARGETS_STR}"

labels=("c-native" "c-msys" "go-native" "rust-native")
bins=("${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}")

printf "%-7s | %-8s | %-10s | %-10s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
  "Type" "Scenario" "Client" "Server" "Throughput (rps)" "p50 (us)" "p95 (us)" "p99 (us)" "Client CPU" "Server CPU" "Total"
printf -- "--------+----------+------------+------------+--------------------+----------+----------+----------+------------+------------+---------\n"

for ((profile_idx = 0; profile_idx < ${#PROFILE_VALUES[@]}; profile_idx++)); do
  profile_label="${PROFILE_LABELS[profile_idx]}"
  profile_value="${PROFILE_VALUES[profile_idx]}"

  for target in "${refresh_targets[@]}"; do
    for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
      for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
        run_refresh_case \
          "${labels[server_idx]}" \
          "${bins[server_idx]}" \
          "${labels[client_idx]}" \
          "${bins[client_idx]}" \
          "${target}" \
          "${profile_label}" \
          "${profile_value}" \
          "${profile_value}"
      done
    done
  done

  for target in "${lookup_targets[@]}"; do
    for ((idx = 0; idx < ${#labels[@]}; idx++)); do
      run_lookup_case \
        "${labels[idx]}" \
        "${bins[idx]}" \
        "${target}" \
        "${profile_label}" \
        "${profile_value}" \
        "${profile_value}"
    done
  done
done

write_csv_results "${RESULTS_FILE}.csv"

echo "Windows live cgroups benchmark matrix passed."
echo "Results saved to ${RESULTS_FILE}"
echo "CSV results saved to ${RESULTS_FILE}.csv"
