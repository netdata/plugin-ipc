#!/bin/bash
# Rust library coverage measurement using cargo-llvm-cov or cargo-tarpaulin
# Reports line coverage for library source files (excludes test/bin code).

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

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"
    if ! "$@"; then
        local exit_code=$?
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: $*"
        return $exit_code
    fi
}

echo -e "${CYAN}=== Rust Library Coverage ===${NC}"
echo

# Check for coverage tools
if command -v cargo-llvm-cov &>/dev/null; then
    TOOL="llvm-cov"
elif cargo llvm-cov --version &>/dev/null 2>&1; then
    TOOL="llvm-cov"
elif command -v cargo-tarpaulin &>/dev/null; then
    TOOL="tarpaulin"
elif cargo tarpaulin --version &>/dev/null 2>&1; then
    TOOL="tarpaulin"
else
    echo -e "${YELLOW}No coverage tool found. Installing cargo-tarpaulin...${NC}"
    run cargo install cargo-tarpaulin
    TOOL="tarpaulin"
fi

echo -e "${YELLOW}Using: $TOOL${NC}"
echo

cd "$CRATE_DIR"

if [[ "$TOOL" == "llvm-cov" ]]; then
    # cargo-llvm-cov approach
    echo -e "${YELLOW}Running tests with llvm-cov coverage...${NC}"
    cargo llvm-cov --lib --no-report -- --test-threads=1 2>&1
    echo
    echo -e "${CYAN}=== Coverage Report ===${NC}"
    cargo llvm-cov report 2>&1
    echo
    # Also show uncovered regions
    echo -e "${CYAN}=== Uncovered Regions ===${NC}"
    cargo llvm-cov report --show-missing-regions 2>&1 || true
else
    # cargo-tarpaulin approach
    echo -e "${YELLOW}Running tests with tarpaulin coverage...${NC}"
    # --lib: only measure library code (not bins)
    # --out: output format
    # --test-threads=1: serialize tests that use sockets
    cargo tarpaulin \
        --lib \
        --out Stdout \
        --skip-clean \
        --exclude-files "src/bin/*" \
        --exclude-files "tests/*" \
        -- --test-threads=1 2>&1
fi

echo

# Threshold check - Rust testable ceiling is ~85% due to transport error paths
# that require OS/kernel failures (socket, mmap, futex, etc.)
# See COVERAGE-EXCLUSIONS.md for detailed justifications
THRESHOLD=${1:-80}

# Parse coverage percentage from tarpaulin output
# Format: "XX.XX% coverage, NNNN/MMMM lines covered"
if [[ "$TOOL" == "tarpaulin" ]]; then
    coverage_line=$(cargo tarpaulin --lib --out Stdout --skip-clean --exclude-files "src/bin/*" --exclude-files "tests/*" -- --test-threads=1 2>&1 | grep "coverage," | tail -1)
    if [[ -n "$coverage_line" ]]; then
        total_pct=$(echo "$coverage_line" | grep -oE '^[0-9]+\.[0-9]+%')
        total_pct_num=$(echo "$total_pct" | tr -d '%')
        total_pct_int=${total_pct_num%.*}

        if [[ $total_pct_int -ge $THRESHOLD ]]; then
            echo -e "${GREEN}Rust coverage ${total_pct} meets threshold ${THRESHOLD}%.${NC}"
            echo -e "${GRAY}Note: Rust testable ceiling is ~85% due to transport error paths.${NC}"
            exit 0
        else
            echo -e "${RED}Rust coverage ${total_pct} is below threshold ${THRESHOLD}%.${NC}"
            exit 1
        fi
    fi
fi

echo -e "${GREEN}Rust coverage measurement complete.${NC}"
