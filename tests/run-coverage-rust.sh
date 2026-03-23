#!/bin/bash
# Rust library coverage measurement using cargo-llvm-cov.
# Enforces a total line-coverage threshold for Linux-relevant library source files.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CRATE_DIR="$ROOT_DIR/src/crates/netipc"
THRESHOLD=${1:-80}
IGNORE_REGEX='(src[\\/]+service[\\/]+cgroups_windows_tests\.rs|src[\\/]+transport[\\/]+windows\.rs|src[\\/]+transport[\\/]+win_shm\.rs)'

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"
    if "$@"; then
        return 0
    else
        local exit_code=$?
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: $*"
        return $exit_code
    fi
}

echo -e "${CYAN}=== Rust Library Coverage ===${NC}"
echo

if ! command -v cargo-llvm-cov &>/dev/null && ! cargo llvm-cov --version &>/dev/null 2>&1; then
    echo -e "${YELLOW}Installing cargo-llvm-cov...${NC}"
    run cargo install cargo-llvm-cov --locked
fi

echo -e "${YELLOW}Ensuring llvm-tools-preview is installed...${NC}"
run rustup component add llvm-tools-preview

cd "$CRATE_DIR"

echo -e "${YELLOW}Cleaning previous llvm-cov artifacts...${NC}"
run cargo llvm-cov clean --workspace

echo -e "${YELLOW}Running tests with llvm-cov coverage...${NC}"
run cargo llvm-cov --lib --no-report -- --test-threads=1

echo
echo -e "${CYAN}=== Coverage Report ===${NC}"
run cargo llvm-cov report --ignore-filename-regex "$IGNORE_REGEX"
summary_log=$(mktemp)
trap 'rm -f "$summary_log"' EXIT
run cargo llvm-cov report --summary-only --ignore-filename-regex "$IGNORE_REGEX" > "$summary_log"
cat "$summary_log"
total_pct=$(awk '/^TOTAL[[:space:]]/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+%$/) last = $i; print last }' "$summary_log" | tail -n 1)

echo

if [[ -z "$total_pct" ]]; then
    echo -e "${RED}ERROR:${NC} Failed to parse total Rust coverage."
    exit 1
fi

total_pct_num=$(echo "$total_pct" | tr -d '%')
total_pct_int=${total_pct_num%.*}

if [[ $total_pct_int -ge $THRESHOLD ]]; then
    echo -e "${GREEN}Rust coverage ${total_pct} meets threshold ${THRESHOLD}%.${NC}"
    echo -e "${GRAY}Note: Rust testable ceiling is still expected to stay below 100% without fault injection and OS failure simulation.${NC}"
    exit 0
fi

echo -e "${RED}Rust coverage ${total_pct} is below threshold ${THRESHOLD}%.${NC}"
exit 1
