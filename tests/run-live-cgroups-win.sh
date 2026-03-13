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
DIR="${NETIPC_WINDOWS_TMP_DIR:-/tmp/cgroups_win}"
TOK="${NETIPC_AUTH_TOKEN:-12345}"
PROFILE_LABELS=("named-pipe" "shm-hybrid")
PROFILE_VALUES=(1 2)
GO_EXECUTABLE="${NETIPC_GO_EXECUTABLE:-}"
NATIVE_CMAKE="${NETIPC_WINDOWS_NATIVE_CMAKE:-}"
MSYS_CMAKE="${NETIPC_WINDOWS_MSYS_CMAKE:-}"
NATIVE_PATH="${NETIPC_WINDOWS_NATIVE_PATH:-/c/Users/costa/.cargo/bin:/mingw64/bin:/usr/bin}"
MSYS_PATH="${NETIPC_WINDOWS_MSYS_PATH:-/c/Users/costa/.cargo/bin:/usr/bin}"
SERVER_PID=""
SERVER_LOG=""
passed=0
failed=0

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

  if [[ "${NETIPC_SKIP_CONFIGURE:-0}" == "1" ]]; then
    return 0
  fi

  local cmake_args=(
    -S "${ROOT_DIR}"
    -B "${build_dir}"
    -G Ninja
    -DNETIPC_BUILD_HELPERS=OFF
  )
  if [[ -n "${GO_EXECUTABLE}" ]]; then
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

  configure_build "${NATIVE_BUILD_DIR}" "${NATIVE_CMAKE}" "${NATIVE_PATH}" "MINGW64"
  run env \
    "PATH=${NATIVE_PATH}:${PATH}" \
    "MSYSTEM=MINGW64" \
    "CHERE_INVOKING=1" \
    "${NATIVE_CMAKE}" --build "${NATIVE_BUILD_DIR}" --target netipc-cgroups-live-c

  configure_build "${MSYS_BUILD_DIR}" "${MSYS_CMAKE}" "${MSYS_PATH}" "MSYS"
  run env \
    "PATH=${MSYS_PATH}:${PATH}" \
    "MSYSTEM=MSYS" \
    "CHERE_INVOKING=1" \
    "${MSYS_CMAKE}" --build "${MSYS_BUILD_DIR}" --target netipc-cgroups-live-c

  (
    cd "${ROOT_DIR}/tests/fixtures/go"
    run env \
      "PATH=${NATIVE_PATH}:${PATH}" \
      "MSYSTEM=MINGW64" \
      "CHERE_INVOKING=1" \
      "${GO_EXECUTABLE}" build -o "${GO_CODEC_BIN}" .
  )

  (
    cd "${ROOT_DIR}"
    run env \
      "PATH=${NATIVE_PATH}:${PATH}" \
      "MSYSTEM=MINGW64" \
      "CHERE_INVOKING=1" \
      cargo build --release --manifest-path tests/fixtures/rust/Cargo.toml --bin netipc-codec-rs
    run "${NATIVE_CMAKE}" -E copy_if_different \
      "${ROOT_DIR}/tests/fixtures/rust/target/release/netipc-codec-rs.exe" \
      "${RUST_CODEC_BIN}"
  )
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

  wait "${SERVER_PID}"
  local rc=$?
  if [[ $rc -ne 0 ]]; then
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

has_line() {
  local needle=$1
  local line

  for line in "${output_lines[@]}"; do
    if [[ "${line}" == "${needle}" ]]; then
      return 0
    fi
  done

  return 1
}

case "$(uname -s)" in
  MINGW*|MSYS*) ;;
  *)
    echo "skip: Windows cgroups smoke requires MSYS2 on Windows" >&2
    exit 0
    ;;
esac

cd "${ROOT_DIR}"
build_targets

for bin in "${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}"; do
  if [[ ! -f "${bin}" ]]; then
    echo "Missing binary: ${bin}" >&2
    exit 1
  fi
done

run_case() {
  local label="$1"
  local server_bin="$2"
  local client_bin="$3"
  local mode="$4"
  local lookup_hash="$5"
  local lookup_name="$6"
  local expected_lookup="$7"
  local supported_profiles="$8"
  local preferred_profiles="$9"
  local svc="cgroups-${mode}-$$-$RANDOM"
  local log="${TMPDIR:-/tmp}/cgroups_${mode}_$$.log"
  local -a output_lines=()

  export NETIPC_SUPPORTED_PROFILES="${supported_profiles}"
  export NETIPC_PREFERRED_PROFILES="${preferred_profiles}"
  export NETIPC_AUTH_TOKEN="${TOK}"

  > "${log}"
  if [[ "${mode}" == "once" ]]; then
    start_server "${log}" "${server_bin}" server-once "${DIR}" "${svc}" "${TOK}"
  else
    start_server "${log}" "${server_bin}" server-loop "${DIR}" "${svc}" 2 "${TOK}"
  fi
  sleep 2

  local client_out=""
  if [[ "${mode}" == "once" ]]; then
    if client_out=$(MSYS2_ARG_CONV_EXCL='*' "${client_bin}" client-refresh-once "${DIR}" "${svc}" "${lookup_hash}" "${lookup_name}" "${TOK}" 2>&1); then
      :
    else
      echo "  FAIL: ${label}"
      echo "    client: ${client_out}"
      cat "${log}" >&2 || true
      ((failed++)) || true
      wait_server || true
      rm -f "${log}"
      return 1
    fi
  else
    if client_out=$(MSYS2_ARG_CONV_EXCL='*' "${client_bin}" client-refresh-loop "${DIR}" "${svc}" 2 "${lookup_hash}" "${lookup_name}" "${TOK}" 2>&1); then
      :
    else
      echo "  FAIL: ${label}"
      echo "    client: ${client_out}"
      cat "${log}" >&2 || true
      ((failed++)) || true
      wait_server || true
      rm -f "${log}"
      return 1
    fi
  fi

  client_out=${client_out//$'\r'/}
  mapfile -t output_lines <<<"${client_out}"

  if ! wait_server; then
    echo "  FAIL: ${label}"
    ((failed++)) || true
    rm -f "${log}"
    return 1
  fi

  if ! has_line $'CGROUPS_CACHE\t42\t1\t2'; then
    echo "  FAIL: ${label}"
    echo "    client: ${client_out}"
    ((failed++)) || true
    rm -f "${log}"
    return 1
  fi

  if [[ "${mode}" == "loop" ]] && ! has_line $'REFRESHES\t2'; then
    echo "  FAIL: ${label}"
    echo "    client: ${client_out}"
    ((failed++)) || true
    rm -f "${log}"
    return 1
  fi

  if [[ "${mode}" == "once" ]]; then
    if ! has_line $'LOOKUP\t'"${expected_lookup}"; then
      echo "  FAIL: ${label}"
      echo "    client: ${client_out}"
      ((failed++)) || true
      rm -f "${log}"
      return 1
    fi
  elif ! has_line $'LOOKUP\t'"${expected_lookup}"; then
    echo "  FAIL: ${label}"
    echo "    client: ${client_out}"
    ((failed++)) || true
    rm -f "${log}"
    return 1
  fi

  echo "  PASS: ${label}"
  ((passed++)) || true
  rm -f "${log}"
  return 0
}

echo "=== Windows fake cgroups snapshot full producer/client matrix ==="
labels=("c-native" "c-msys" "go-native" "rust-native")
bins=("${C_NATIVE_BIN}" "${C_MSYS_BIN}" "${GO_CODEC_BIN}" "${RUST_CODEC_BIN}")
for ((profile_idx = 0; profile_idx < ${#PROFILE_VALUES[@]}; profile_idx++)); do
  profile_label="${PROFILE_LABELS[profile_idx]}"
  profile_value="${PROFILE_VALUES[profile_idx]}"
  for ((server_idx = 0; server_idx < ${#labels[@]}; server_idx++)); do
    for ((client_idx = 0; client_idx < ${#labels[@]}; client_idx++)); do
      run_case \
        "${labels[server_idx]} server + ${labels[client_idx]} client (${profile_label}, refresh once)" \
        "${bins[server_idx]}" \
        "${bins[client_idx]}" \
        "once" \
        123 \
        "system.slice-nginx" \
        $'123\t2\t1\tsystem.slice-nginx\t/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs' \
        "${profile_value}" \
        "${profile_value}"

      run_case \
        "${labels[server_idx]} server + ${labels[client_idx]} client (${profile_label}, refresh loop)" \
        "${bins[server_idx]}" \
        "${bins[client_idx]}" \
        "loop" \
        456 \
        "docker-1234" \
        $'456\t4\t0\tdocker-1234\t' \
        "${profile_value}" \
        "${profile_value}"
    done
  done
done

echo ""
echo "=== Results: ${passed} passed, ${failed} failed ==="
[ "${failed}" -eq 0 ]
