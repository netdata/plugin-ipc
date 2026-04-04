#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPARE_SCRIPT="${ROOT_DIR}/tests/compare-windows-bench-toolchains.sh"
BUILD_DIR="${NIPC_MSYS_VALIDATION_BUILD_DIR:-${ROOT_DIR}/build-msys-validation}"
OUT_DIR="${1:-${TEMP:-/tmp}/netipc-msys-validation}"
BENCH_DURATION="${2:-3}"
BENCH_REPETITIONS="${NIPC_BENCH_COMPARE_REPETITIONS:-3}"
COMPARE_OUT_DIR="${OUT_DIR}/bench-compare"
SUMMARY_FILE="${OUT_DIR}/summary.txt"

FUNCTIONAL_TESTS=(
  test_named_pipe
  test_win_shm
  test_win_service
  test_win_service_extra
  test_named_pipe_interop
  test_win_shm_interop
  test_service_win_interop
  test_service_win_shm_interop
  test_cache_win_interop
  test_cache_win_shm_interop
  test_win_stress
)

BUILD_TARGETS=(
  test_named_pipe
  test_win_shm
  test_win_service
  test_win_service_extra
  test_win_stress
  interop_named_pipe_c
  interop_named_pipe_rs
  interop_named_pipe_go
  interop_win_shm_c
  interop_win_shm_rs
  interop_win_shm_go
  interop_service_win_c
  interop_service_win_rs
  interop_service_win_go
  interop_cache_win_c
  interop_cache_win_rs
  interop_cache_win_go
)

repeat_count_for_test() {
  local test_name="$1"

  case "$test_name" in
    test_win_shm)
      printf '%s\n' "${NIPC_MSYS_VALIDATION_WIN_SHM_REPEATS:-10}"
      ;;
    *)
      printf '1\n'
      ;;
  esac
}

run() {
  printf >&2 "${GRAY}%s >${NC} " "$(pwd)"
  printf >&2 "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 "${NC}\n"
  "$@"
}

log() {
  printf "${CYAN}[msys-validate]${NC} %s\n" "$*" >&2
}

build_jobs() {
  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4
  elif command -v nproc >/dev/null 2>&1; then
    nproc 2>/dev/null || echo 4
  else
    echo 4
  fi
}

setup_msys_toolchain() {
  export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/usr/bin:/mingw64/bin:$PATH"
  export MSYSTEM=MSYS
  CC_BIN="/usr/bin/gcc"
  CXX_BIN="/usr/bin/g++"
}

configure_and_build() {
  setup_msys_toolchain
  mkdir -p "$OUT_DIR"

  log "Configuring MSYS validation build: ${BUILD_DIR}"
  run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN"

  log "Building targeted MSYS validation targets"
  run cmake --build "$BUILD_DIR" --target "${BUILD_TARGETS[@]}" -j"$(build_jobs)"
}

run_ctest_case() {
  local test_name="$1"
  local repeat_count
  repeat_count="$(repeat_count_for_test "$test_name")"

  local repeat_idx
  for repeat_idx in $(seq 1 "$repeat_count"); do
    if [ "$repeat_count" -gt 1 ]; then
      log "Running ${test_name} (${repeat_idx}/${repeat_count})"
    else
      log "Running ${test_name}"
    fi
    (
      cd "$BUILD_DIR"
      run /usr/bin/ctest --output-on-failure -R "^${test_name}\$"
    )
  done
}

run_functional_slice() {
  local test_name
  for test_name in "${FUNCTIONAL_TESTS[@]}"; do
    run_ctest_case "$test_name"
  done
}

run_benchmark_compare() {
  log "Running bounded MSYS vs mingw64 benchmark comparison"
  run env \
    NIPC_BENCH_COMPARE_REPETITIONS="$BENCH_REPETITIONS" \
    bash "$COMPARE_SCRIPT" "$COMPARE_OUT_DIR" "$BENCH_DURATION"
}

write_summary() {
  local win_shm_repeats
  win_shm_repeats="$(repeat_count_for_test test_win_shm)"

  {
    printf 'msys_validation=passed\n'
    printf 'build_dir=%s\n' "$BUILD_DIR"
    printf 'test_win_shm_repeats=%s\n' "$win_shm_repeats"
    printf 'functional_tests='
    printf '%s ' "${FUNCTIONAL_TESTS[@]}"
    printf '\n'
    printf 'benchmark_compare_summary=%s\n' "${COMPARE_OUT_DIR}/summary.csv"
    printf 'benchmark_compare_joined=%s\n' "${COMPARE_OUT_DIR}/joined.csv"
  } > "$SUMMARY_FILE"

  log "Validation summary: ${SUMMARY_FILE}"
}

main() {
  configure_and_build
  run_functional_slice
  run_benchmark_compare
  write_summary
  printf "${GREEN}[msys-validate]${NC} MSYS validation passed\n" >&2
}

main "$@"
