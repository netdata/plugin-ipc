#!/bin/bash
# Windows Rust coverage measurement using cargo-llvm-cov.
# Enforces the same total line-coverage threshold policy as the Linux Rust
# coverage script, while still collecting Windows-native Rust code paths.

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
BUILD_DIR="$ROOT_DIR/build-windows-coverage-rust"
REPORT_LOG="$BUILD_DIR/rust-coverage-summary.txt"
INTEROP_REGEX='^(test_named_pipe_interop|test_win_shm_interop|test_service_win_interop|test_service_win_shm_interop|test_cache_win_interop|test_cache_win_shm_interop)$'
IGNORE_REGEX='(src[\\/]+bin[\\/]|tests[\\/]+fixtures[\\/]+rust[\\/]+src[\\/]+bin[\\/]|bench[\\/]+drivers[\\/]+rust[\\/]+src[\\/])'
THRESHOLD=${1:-80}

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

echo -e "${CYAN}=== Windows Rust Coverage ===${NC}"
echo

if [[ "$(uname -s)" != *MINGW* ]] && [[ "$(uname -s)" != *MSYS* ]] && [[ "${OS:-}" != "Windows_NT" ]]; then
    echo -e "${RED}ERROR:${NC} This script must run on Windows."
    exit 1
fi

export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:$PATH"
export MSYSTEM=MINGW64

for tool in cargo rustup cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e "${RED}ERROR:${NC} Missing required tool: $tool"
        exit 1
    fi
done

if ! command -v cargo-llvm-cov >/dev/null 2>&1; then
    echo -e "${YELLOW}Installing cargo-llvm-cov...${NC}"
    run cargo install cargo-llvm-cov --locked
fi

echo -e "${YELLOW}Ensuring llvm-tools-preview is installed...${NC}"
run rustup component add llvm-tools-preview

echo -e "${YELLOW}Checking for stale Rust interop binaries...${NC}"
stale_found=0
for image in interop_named_pipe.exe interop_win_shm.exe interop_service_win.exe interop_cache_win.exe; do
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        echo -e "${RED}ERROR:${NC} Stale process is holding a Rust interop binary:"
        echo "$line"
        stale_found=1
    done < <(
        tasklist //FO CSV //NH //FI "IMAGENAME eq $image" 2>/dev/null | grep -v "No tasks are running"
    )
done
if [[ $stale_found -ne 0 ]]; then
    echo -e "${YELLOW}Stop the exact PIDs above and rerun this script.${NC}"
    exit 1
fi

cd "$ROOT_DIR"

echo -e "${YELLOW}Setting cargo-llvm-cov environment...${NC}"
# shellcheck disable=SC1090
source <(cd "$CRATE_DIR" && cargo llvm-cov show-env --sh)

echo -e "${YELLOW}Cleaning previous Rust coverage artifacts...${NC}"
cd "$CRATE_DIR"
run cargo llvm-cov clean --workspace

echo -e "${YELLOW}Running Rust unit tests with coverage...${NC}"
cd "$ROOT_DIR"
run cargo test --manifest-path "$CRATE_DIR/Cargo.toml" --lib -- --test-threads=1

echo -e "${YELLOW}Preparing dedicated Windows Rust coverage build...${NC}"
run rm -rf "$BUILD_DIR"
run mkdir -p "$BUILD_DIR"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug

echo -e "${YELLOW}Building Windows interop binaries under coverage...${NC}"
run cmake --build "$BUILD_DIR" -j4

echo -e "${YELLOW}Running Rust-relevant Windows interop tests...${NC}"
run ctest --test-dir "$BUILD_DIR" --output-on-failure -j1 -R "$INTEROP_REGEX"

echo -e "${YELLOW}Generating Rust coverage summary...${NC}"
printf >&2 "${GRAY}$(pwd) >${NC} ${YELLOW}"
printf >&2 "%q " cargo llvm-cov report --manifest-path "$CRATE_DIR/Cargo.toml" --summary-only --ignore-filename-regex "$IGNORE_REGEX"
printf >&2 "> %q" "$REPORT_LOG"
printf >&2 "${NC}\n"
(
    cd "$CRATE_DIR"
    cargo llvm-cov report \
        --manifest-path "$CRATE_DIR/Cargo.toml" \
        --summary-only \
        --ignore-filename-regex "$IGNORE_REGEX" \
        > "$REPORT_LOG"
)

echo
echo -e "${CYAN}=== Key Rust Windows Coverage ===${NC}"
grep -E 'service\\cgroups.rs|transport\\windows.rs|transport\\win_shm.rs|^TOTAL' "$REPORT_LOG" || true

echo
echo -e "${CYAN}=== Full Summary Path ===${NC}"
echo "$REPORT_LOG"

total_pct=$(awk '/^TOTAL[[:space:]]/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+%$/) last = $i; print last }' "$REPORT_LOG" | tail -n 1)
if [[ -z "$total_pct" ]]; then
    echo -e "${RED}ERROR:${NC} Failed to parse TOTAL coverage from $REPORT_LOG"
    exit 1
fi

echo
total_pct_num=$(echo "$total_pct" | tr -d '%')
total_pct_int=${total_pct_num%.*}

if [[ $total_pct_int -ge $THRESHOLD ]]; then
    echo -e "${GREEN}Windows Rust coverage ${total_pct} meets threshold ${THRESHOLD}%.${NC}"
    echo -e "${GRAY}Note: one retry/shutdown test is still intentionally ignored and tracked separately from this normal coverage gate.${NC}"
    exit 0
fi

echo -e "${RED}Windows Rust coverage ${total_pct} is below threshold ${THRESHOLD}%.${NC}"
exit 1
