#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NATIVE_BUILD_DIR="${NETIPC_WINDOWS_NATIVE_BUILD_DIR:-${NETIPC_CMAKE_BUILD_DIR:-build-mingw}}"
MSYS_BUILD_DIR="${NETIPC_WINDOWS_MSYS_BUILD_DIR:-build-msys-ninja}"
NATIVE_BIN_DIR="${NETIPC_WINDOWS_NATIVE_BIN_DIR:-${NATIVE_BUILD_DIR}/bin}"
MSYS_BIN_DIR="${NETIPC_WINDOWS_MSYS_BIN_DIR:-${MSYS_BUILD_DIR}/bin}"
C_NATIVE_BIN="${NETIPC_WINDOWS_C_NATIVE_BIN:-${NATIVE_BIN_DIR}/netipc-live-c.exe}"
C_MSYS_BIN="${NETIPC_WINDOWS_C_MSYS_BIN:-${MSYS_BIN_DIR}/netipc-live-c.exe}"
RS_BIN="${NETIPC_WINDOWS_RS_BIN:-${NATIVE_BIN_DIR}/netipc_live_win_rs.exe}"
GO_BIN="${NETIPC_WINDOWS_GO_BIN:-${NATIVE_BIN_DIR}/netipc-live-go-win.exe}"
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/bench_win}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
DUR="${NETIPC_BENCH_DURATION_SEC:-5}"
GO_EXECUTABLE="${NETIPC_GO_EXECUTABLE:-}"
NATIVE_CMAKE="${NETIPC_WINDOWS_NATIVE_CMAKE:-}"
MSYS_CMAKE="${NETIPC_WINDOWS_MSYS_CMAKE:-}"
NATIVE_PATH="${NETIPC_WINDOWS_NATIVE_PATH:-/c/Users/costa/.cargo/bin:/mingw64/bin:/usr/bin}"
MSYS_PATH="${NETIPC_WINDOWS_MSYS_PATH:-/c/Users/costa/.cargo/bin:/usr/bin}"

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

if [[ -x /c/Program\ Files/Go/bin/go.exe ]]; then
  NATIVE_PATH="${NATIVE_PATH}:/c/Program Files/Go/bin"
  MSYS_PATH="${MSYS_PATH}:/c/Program Files/Go/bin"
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
    "${NATIVE_CMAKE}" --build "${NATIVE_BUILD_DIR}" --target netipc-live-c netipc_live_win_rs netipc-live-go-win

  configure_build "${MSYS_BUILD_DIR}" "${MSYS_CMAKE}" "${MSYS_PATH}" "MSYS" "OFF"
  run env \
    "PATH=${MSYS_PATH}:${PATH}" \
    "MSYSTEM=MSYS" \
    "CHERE_INVOKING=1" \
    "${MSYS_CMAKE}" --build "${MSYS_BUILD_DIR}" --target netipc-live-c
}

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows benchmark requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

cd "${ROOT_DIR}"
build_targets

for bin in "${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${RS_BIN}" "${GO_BIN}"; do
  if [[ ! -f "${bin}" ]]; then
    echo "Missing binary: ${bin}" >&2
    exit 1
  fi
done

RESULTS_FILE="${NETIPC_RESULTS_FILE:-/tmp/bench_results_$$.txt}"
> "${RESULTS_FILE}"

run_bench() {
  local scenario="$1"
  local pair_label="$2"
  local server_bin="$3"
  local client_bin="$4"
  local sprof="$5"
  local pprof="$6"
  local spin="$7"
  local target_rps="$8"

  local svc="bench-${pair_label//[^a-zA-Z0-9]/-}-$$-${RANDOM}"
  local slog="/tmp/slog_${svc}.txt"

  export NETIPC_SUPPORTED_PROFILES="${sprof}"
  export NETIPC_PREFERRED_PROFILES="${pprof}"
  export NETIPC_AUTH_TOKEN="${TOK}"
  if [[ -n "${spin}" ]]; then
    export NETIPC_SHM_SPIN_TRIES="${spin}"
  else
    unset NETIPC_SHM_SPIN_TRIES 2>/dev/null || true
  fi

  > "${slog}"
  "${server_bin}" server-loop "${DIR}" "${svc}" 0 2>"${slog}" &
  sleep 2

  local client_out
  client_out=$("${client_bin}" client-bench "${DIR}" "${svc}" "${DUR}" "${target_rps}" 2>/dev/null) || true
  wait 2>/dev/null || true

  local server_cpu
  server_cpu=$(grep 'SERVER_CPU_CORES=' "${slog}" 2>/dev/null | head -1 | cut -d= -f2) || true
  [[ -z "${server_cpu}" ]] && server_cpu="N/A"

  local data
  data=$(echo "${client_out}" | tail -1)
  local mode tput p50 p95 p99 c_cpu
  mode=$(echo "${data}" | cut -d, -f1)
  tput=$(echo "${data}" | cut -d, -f7)
  p50=$(echo "${data}" | cut -d, -f8)
  p95=$(echo "${data}" | cut -d, -f9)
  p99=$(echo "${data}" | cut -d, -f10)
  c_cpu=$(echo "${data}" | cut -d, -f11)

  [[ -z "${tput}" ]] && tput="FAIL"

  local total_cpu="N/A"
  if [[ -n "${c_cpu}" && "${server_cpu}" != "N/A" ]]; then
    total_cpu=$(awk -v c="${c_cpu}" -v s="${server_cpu}" 'BEGIN { printf "%.3f", c + s }')
  fi

  printf "%-8s | %-26s | %-18s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
    "${scenario}" "${pair_label}" "${mode}" "${tput}" "${p50}" "${p95}" "${p99}" "${c_cpu}" "${server_cpu}" "${total_cpu}" | tee -a "${RESULTS_FILE}"

  rm -f "${slog}"
}

labels=("c-native" "c-msys" "rust-native" "go-native")
bins=("${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${RS_BIN}" "${GO_BIN}")

echo ""
echo "Running Windows IPC benchmarks (${DUR}s each)..."
echo ""
printf "%-8s | %-26s | %-18s | %18s | %8s | %8s | %8s | %10s | %10s | %8s\n" \
  "Scenario" "Pair" "Method" "Throughput (rps)" "p50 (us)" "p95 (us)" "p99 (us)" "Client CPU" "Server CPU" "Total"
printf -- "---------+----------------------------+--------------------+--------------------+----------+----------+----------+------------+------------+---------\n"

for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
  for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
    pair_label="${labels[server_idx]}->${labels[client_idx]}"
    run_bench "max" "${pair_label}" "${bins[server_idx]}" "${bins[client_idx]}" 2 2 "" 0
    run_bench "max" "${pair_label}" "${bins[server_idx]}" "${bins[client_idx]}" 1 1 "" 0
    run_bench "100k/s" "${pair_label}" "${bins[server_idx]}" "${bins[client_idx]}" 2 2 "" 100000
    run_bench "10k/s" "${pair_label}" "${bins[server_idx]}" "${bins[client_idx]}" 1 1 "" 10000
  done
done

echo ""
echo "Windows IPC benchmark complete."
echo "Results saved to ${RESULTS_FILE}"
