#!/usr/bin/env bash
#
# run-extended-stress.sh - Stress, chaos, scale and benchmark-policy suites.
#
# These suites are not correctness tests. Default ctest, cargo and go test
# runs exclude them; run this script before Netdata vendoring when a change
# can affect what they exercise (see AGENTS.md Test Cadence).
#
# Usage:
#   ./tests/run-extended-stress.sh [build_dir]
#
# Default build dir: build-extended (configured with NETIPC_EXTENDED_TESTS=ON).

set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"

RED='\033[0;31m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build-extended}"

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"

    local exit_code=0
    "$@" || exit_code=$?
    if [ "$exit_code" -ne 0 ]; then
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
        echo -e >&2 "${RED}        Full command:${NC} $*"
        echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
        return $exit_code
    fi
}

cd "$ROOT_DIR"
run cmake -S . -B "$BUILD_DIR" -DNETIPC_EXTENDED_TESTS=ON
run cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"
run ctest --test-dir "$BUILD_DIR" --output-on-failure \
    -R '^(test_stress.*|test_win_stress|test_chaos_extended|test_lookup_scale.*|test_windows_bench_stability_policy)$'
