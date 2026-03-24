#!/bin/bash
# Windows C coverage measurement for the native win11 environment.
# Uses the same MinGW64 + Ninja toolchain flow that is already validated for
# normal Windows builds in this repository.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-windows-coverage-c"

LIB_SOURCES=(
    "src/libnetdata/netipc/src/service/netipc_service_win.c"
    "src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c"
    "src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c"
)

# C coverage is about the C transport/service stack. Running the full Windows
# suite here adds unrelated Go fuzz tests that do not contribute gcov data for
# these files and can starve long-running C tests under coverage builds.
COVERAGE_TEST_REGEX='^(test_protocol|interop_codec|fuzz_protocol_30s|test_named_pipe|test_named_pipe_interop|test_win_shm|test_win_service_extra|test_win_stress|test_win_shm_interop|test_service_win_interop|test_service_win_shm_interop|test_cache_win_interop|test_cache_win_shm_interop)$'

THRESHOLD=${1:-85}

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

echo -e "${CYAN}=== Windows C Coverage ===${NC}"
echo

if [[ "$(uname -s)" != *MINGW* ]] && [[ "$(uname -s)" != *MSYS* ]] && [[ "${OS:-}" != "Windows_NT" ]]; then
    echo -e "${RED}ERROR:${NC} This script must run on Windows."
    exit 1
fi

export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:$PATH"
export MSYSTEM=MINGW64
for tool in cmake ninja gcc g++ gcov cygpath; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e "${RED}ERROR:${NC} Required tool not found: $tool"
        exit 1
    fi
done

CC_BIN=$(cygpath -m "$(command -v gcc)")
CXX_BIN=$(cygpath -m "$(command -v g++)")

run rm -rf "$BUILD_DIR"
run mkdir -p "$BUILD_DIR"

echo -e "${YELLOW}Configuring Windows coverage build...${NC}"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNETIPC_COVERAGE=ON \
    -DCMAKE_C_COMPILER="$CC_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN"

echo -e "${YELLOW}Building Windows binaries...${NC}"
run cmake --build "$BUILD_DIR" -j4

echo -e "${YELLOW}Running Windows test suite...${NC}"
run ctest --test-dir "$BUILD_DIR" --output-on-failure -j1 -R "$COVERAGE_TEST_REGEX"
echo -e "${YELLOW}Running Windows C coverage-only guard executable...${NC}"
run "$BUILD_DIR/bin/test_win_service_guards.exe"

echo
echo -e "${YELLOW}Collecting gcov data for Windows C sources...${NC}"
echo

GCOV_OUT="$BUILD_DIR/gcov-output"
run rm -rf "$GCOV_OUT"
run mkdir -p "$GCOV_OUT"

total_covered=0
total_lines=0
declare -A file_covered
declare -A file_total
all_pass=true

for src in "${LIB_SOURCES[@]}"; do
    basename_c=$(basename "$src")
    gcno_file=$(find "$BUILD_DIR" -name "${basename_c}.gcno" 2>/dev/null | head -1)

    if [[ -z "$gcno_file" ]]; then
        echo -e "${RED}No coverage data for $basename_c (no .gcno file)${NC}"
        all_pass=false
        continue
    fi

    gcno_dir=$(dirname "$gcno_file")
    gcov_output=$(cd "$gcno_dir" && gcov "${basename_c}.gcno" 2>&1)

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

    gcov_result_file="$gcno_dir/${basename_c}.gcov"
    if [[ -f "$gcov_result_file" ]]; then
        cp "$gcov_result_file" "$GCOV_OUT/"
    fi
done

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

        pct_int=$(echo "$pct" | cut -d. -f1)
        if [[ $pct_int -ge $THRESHOLD ]]; then
            color=$GREEN
        elif [[ $pct_int -ge 70 ]]; then
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
    exit 1
fi

echo
echo -e "${CYAN}=== Uncovered Lines ===${NC}"
for src in "${LIB_SOURCES[@]}"; do
    basename_c=$(basename "$src")
    gcov_file="$GCOV_OUT/${basename_c}.gcov"
    if [[ -f "$gcov_file" ]]; then
        uncovered_count=$(grep -c "^    #####:" "$gcov_file" 2>/dev/null || true)
        if [[ $uncovered_count -gt 0 ]]; then
            echo -e "\n${YELLOW}$basename_c${NC} ($uncovered_count uncovered lines):"
            grep "^    #####:" "$gcov_file" | sed -n '1,40p' | while IFS= read -r line; do
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
if $all_pass; then
    echo -e "${GREEN}All Windows C files meet the ${THRESHOLD}% coverage threshold.${NC}"
    exit 0
fi

echo -e "${RED}Some Windows C files are below the ${THRESHOLD}% coverage threshold.${NC}"
exit 1
