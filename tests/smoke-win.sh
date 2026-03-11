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
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/smoke_win}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
SLOG="${TMPDIR:-/tmp}/smoke_server_$$.txt"
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

passed=0
failed=0

run() {
  printf >&2 '%s\n' "+ $*"
  "$@"
}

configure_build() {
  local build_kind=$1
  local build_dir=$2
  local cmake_bin=$3
  local runtime_path=$4
  local msystem=$5
  local helper_setting=$6

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

  configure_build "native" "${NATIVE_BUILD_DIR}" "${NATIVE_CMAKE}" "${NATIVE_PATH}" "MINGW64" "ON"
  run env \
    "PATH=${NATIVE_PATH}:${PATH}" \
    "MSYSTEM=MINGW64" \
    "CHERE_INVOKING=1" \
    "${NATIVE_CMAKE}" --build "${NATIVE_BUILD_DIR}" --target netipc-live-c netipc_live_win_rs netipc-live-go-win

  configure_build "msys" "${MSYS_BUILD_DIR}" "${MSYS_CMAKE}" "${MSYS_PATH}" "MSYS" "OFF"
  run env \
    "PATH=${MSYS_PATH}:${PATH}" \
    "MSYSTEM=MSYS" \
    "CHERE_INVOKING=1" \
    "${MSYS_CMAKE}" --build "${MSYS_BUILD_DIR}" --target netipc-live-c
}

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows smoke test requires MSYS2 on Windows" >&2
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

  > "${SLOG}"
  MSYS2_ARG_CONV_EXCL='*' "${server_bin}" server-once "${DIR}" "${svc}" 2>"${SLOG}" &
  local sp=$!
  sleep 2

  local client_out
  if client_out=$(MSYS2_ARG_CONV_EXCL='*' timeout 10 "${client_bin}" client-once "${DIR}" "${svc}" 42 2>&1); then
    echo "  PASS: ${label}"
    echo "    server: $(cat "${SLOG}" 2>/dev/null)"
    echo "    client: ${client_out}"
    ((passed++)) || true
  else
    echo "  FAIL: ${label}"
    echo "    server: $(cat "${SLOG}" 2>/dev/null)"
    echo "    client: ${client_out}"
    ((failed++)) || true
  fi
  wait "${sp}" 2>/dev/null || true
}

labels=("c-native" "c-msys" "rust-native" "go-native")
bins=("${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${RS_BIN}" "${GO_BIN}")

echo "=== Named Pipe (profile=1) ==="
for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
  for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
    run_test \
      "${labels[server_idx]} server + ${labels[client_idx]} client" \
      "${bins[server_idx]}" \
      "${bins[client_idx]}" \
      1
  done
done

echo ""
echo "=== SHM HYBRID (profile=2) ==="
for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
  for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
    run_test \
      "${labels[server_idx]} server + ${labels[client_idx]} client" \
      "${bins[server_idx]}" \
      "${bins[client_idx]}" \
      2
  done
done

echo ""
echo "=== Results: ${passed} passed, ${failed} failed ==="

rm -f "${SLOG}"
[ "${failed}" -eq 0 ]
