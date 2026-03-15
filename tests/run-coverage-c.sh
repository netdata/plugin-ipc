#!/bin/bash
# C library coverage measurement using gcov
# Builds with coverage flags, runs all C tests, reports per-file line coverage.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-coverage"

# Library source files to measure (relative to ROOT_DIR)
LIB_SOURCES=(
    "src/libnetdata/netipc/src/protocol/netipc_protocol.c"
    "src/libnetdata/netipc/src/transport/posix/netipc_uds.c"
    "src/libnetdata/netipc/src/transport/posix/netipc_shm.c"
    "src/libnetdata/netipc/src/service/netipc_service.c"
)

THRESHOLD=${1:-90}

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

echo -e "${CYAN}=== C Library Coverage ===${NC}"
echo

# Clean previous coverage build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Configure with coverage enabled
echo -e "${YELLOW}Configuring with coverage...${NC}"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNETIPC_COVERAGE=ON \
    -DCMAKE_C_COMPILER=gcc 2>&1

# Build all test binaries
echo -e "${YELLOW}Building test binaries...${NC}"
run cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1

# Run all C tests
echo -e "${YELLOW}Running tests...${NC}"
TESTS=(test_protocol test_uds test_shm test_service test_cache test_multi_server)
for t in "${TESTS[@]}"; do
    binary="$BUILD_DIR/bin/$t"
    if [[ -x "$binary" ]]; then
        echo -e "  ${GRAY}Running $t...${NC}"
        if ! "$binary" >/dev/null 2>&1; then
            echo -e "  ${RED}$t FAILED${NC}"
        else
            echo -e "  ${GREEN}$t PASSED${NC}"
        fi
    else
        echo -e "  ${YELLOW}$t not found, skipping${NC}"
    fi
done
echo

# Collect coverage data using gcov
echo -e "${YELLOW}Collecting coverage...${NC}"
echo

total_covered=0
total_lines=0
declare -A file_covered
declare -A file_total
all_pass=true

# gcov output directory for all results
GCOV_OUT="$BUILD_DIR/gcov-output"
mkdir -p "$GCOV_OUT"

for src in "${LIB_SOURCES[@]}"; do
    basename_c=$(basename "$src")

    # Find the .gcno file for this source
    # GCC 15 names them: netipc_protocol.c.gcno (with the .c extension)
    gcno_file=$(find "$BUILD_DIR" -name "${basename_c}.gcno" 2>/dev/null | head -1)

    if [[ -z "$gcno_file" ]]; then
        echo -e "${RED}No coverage data for $basename_c (no .gcno file)${NC}"
        all_pass=false
        continue
    fi

    gcno_dir=$(dirname "$gcno_file")

    # Run gcov from the .gcno/.gcda directory, passing the .gcno file
    gcov_output=$(cd "$gcno_dir" && gcov "${basename_c}.gcno" 2>&1)

    # Parse gcov output for this specific source file
    # Format: "Lines executed:XX.XX% of YYY"
    # gcov outputs multiple sections if headers are included; match our source file
    pct_line=$(echo "$gcov_output" | grep -A1 "File '.*${basename_c}'" | grep "Lines executed:" | head -1 | sed 's/.*Lines executed:\([0-9.]*\)% of \([0-9]*\)/\1 \2/')
    if [[ -n "$pct_line" ]]; then
        pct=$(echo "$pct_line" | awk '{print $1}')
        total_l=$(echo "$pct_line" | awk '{print $2}')
        covered=$(awk -v p="$pct" -v t="$total_l" 'BEGIN { printf "%d", (p/100)*t + 0.5 }')

        file_covered[$basename_c]=$covered
        file_total[$basename_c]=$total_l
        total_covered=$((total_covered + covered))
        total_lines=$((total_lines + total_l))
    fi

    # Move gcov output file to our output dir for later analysis
    gcov_result_file="$gcno_dir/${basename_c}.gcov"
    if [[ -f "$gcov_result_file" ]]; then
        cp "$gcov_result_file" "$GCOV_OUT/"
    fi
done

# Print results table
echo -e "${CYAN}=== Coverage Results ===${NC}"
echo
printf "%-30s %8s %12s\n" "File" "Coverage" "Lines"
printf "%-30s %8s %12s\n" "------------------------------" "--------" "------------"

for src in "${LIB_SOURCES[@]}"; do
    basename_c=$(basename "$src")
    if [[ -n "${file_covered[$basename_c]:-}" ]]; then
        cov=${file_covered[$basename_c]}
        tot=${file_total[$basename_c]}
        if [[ $tot -gt 0 ]]; then
            pct=$(awk -v c="$cov" -v t="$tot" 'BEGIN { printf "%.1f", (c/t)*100 }')
        else
            pct="0.0"
        fi

        # Color based on threshold
        pct_int=$(echo "$pct" | cut -d. -f1)
        if [[ $pct_int -ge $THRESHOLD ]]; then
            color=$GREEN
        elif [[ $pct_int -ge 75 ]]; then
            color=$YELLOW
        else
            color=$RED
        fi

        printf "%-30s ${color}%6s%%${NC} %6d/%-6d\n" "$basename_c" "$pct" "$cov" "$tot"

        if [[ $pct_int -lt $THRESHOLD ]]; then
            all_pass=false
        fi
    else
        printf "%-30s ${RED}%8s${NC} %12s\n" "$basename_c" "NO DATA" "N/A"
        all_pass=false
    fi
done

echo
if [[ $total_lines -gt 0 ]]; then
    total_pct=$(awk -v c="$total_covered" -v t="$total_lines" 'BEGIN { printf "%.1f", (c/t)*100 }')
    printf "%-30s %6s%% %6d/%-6d\n" "TOTAL" "$total_pct" "$total_covered" "$total_lines"
else
    echo "TOTAL: No coverage data collected"
fi
echo

# Show uncovered lines per file
echo -e "${CYAN}=== Uncovered Lines ===${NC}"
for src in "${LIB_SOURCES[@]}"; do
    basename_c=$(basename "$src")
    gcov_file="$GCOV_OUT/${basename_c}.gcov"
    if [[ -f "$gcov_file" ]]; then
        uncovered_count=$(grep -c "^    #####:" "$gcov_file" 2>/dev/null || true)
        if [[ $uncovered_count -gt 0 ]]; then
            echo -e "\n${YELLOW}$basename_c${NC} ($uncovered_count uncovered lines):"
            # gcov format: "    #####:  123:    code here"
            grep "^    #####:" "$gcov_file" | head -40 | while IFS= read -r line; do
                lineno=$(echo "$line" | awk -F: '{gsub(/^[ \t]+/,"",$2); print $2}')
                code=$(echo "$line" | cut -d: -f3-)
                echo -e "  ${RED}line $lineno:${NC}$code"
            done
            if [[ $uncovered_count -gt 40 ]]; then
                echo -e "  ${GRAY}... and $((uncovered_count - 40)) more${NC}"
            fi
        fi
    fi
done
echo

# Exit status
if $all_pass; then
    echo -e "${GREEN}All files meet the ${THRESHOLD}% coverage threshold.${NC}"
    exit 0
else
    echo -e "${RED}Some files are below the ${THRESHOLD}% coverage threshold.${NC}"
    exit 1
fi
