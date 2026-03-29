#!/bin/bash
# Run multi-threaded C tests under ThreadSanitizer (TSAN)
# Detects: data races, lock order inversions, deadlocks

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-tsan"

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

echo -e "${CYAN}=== ThreadSanitizer ===${NC}"
echo

# TSAN is incompatible with ASAN; separate build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

TSAN_FLAGS="-fsanitize=thread -O1 -g -fno-omit-frame-pointer"

echo -e "${YELLOW}Configuring with TSAN...${NC}"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_C_FLAGS="$TSAN_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" 2>&1

echo -e "${YELLOW}Building test binaries...${NC}"
run cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1

# Multi-threaded tests: these exercise the worker pool, multiple sessions, etc.
# Also include test_uds and test_shm since they use threads for server/client pairs.
TESTS=(test_multi_server test_service test_service_extra test_uds test_shm test_cache)

total=0
passed=0
failed=0
failed_tests=()

echo
echo -e "${CYAN}=== Running tests under TSAN ===${NC}"
echo

for t in "${TESTS[@]}"; do
    binary="$BUILD_DIR/bin/$t"
    if [[ ! -x "$binary" ]]; then
        echo -e "  ${YELLOW}$t not found, skipping${NC}"
        continue
    fi

    total=$((total + 1))
    echo -e "${YELLOW}--- $t ---${NC}"

    log_file="$BUILD_DIR/${t}-tsan.log"

    export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:print_stacktrace=1:color=always"

    set +e
    "$binary" > "$log_file" 2>&1
    exit_code=$?
    set -e

    has_tsan_error=false
    if grep -qE "(WARNING: ThreadSanitizer|SUMMARY: ThreadSanitizer)" "$log_file" 2>/dev/null; then
        has_tsan_error=true
    fi

    if [[ $exit_code -ne 0 ]] || $has_tsan_error; then
        echo -e "  ${RED}$t FAILED${NC} (exit=$exit_code, tsan_error=$has_tsan_error)"
        failed=$((failed + 1))
        failed_tests+=("$t")
        echo -e "${RED}--- TSAN output for $t ---${NC}"
        cat "$log_file"
        echo -e "${RED}--- end $t ---${NC}"
        echo
    else
        echo -e "  ${GREEN}$t PASSED${NC} (clean)"
        passed=$((passed + 1))
        if [[ -s "$log_file" ]]; then
            grep -E "(PASS|FAIL|test_|Test )" "$log_file" | tail -5 || true
        fi
    fi
    echo
done

echo -e "${CYAN}=== TSAN Summary ===${NC}"
echo -e "  Total:  $total"
echo -e "  Passed: ${GREEN}$passed${NC}"
echo -e "  Failed: ${RED}$failed${NC}"
if [[ ${#failed_tests[@]} -gt 0 ]]; then
    echo -e "  Failed tests: ${RED}${failed_tests[*]}${NC}"
fi
echo

if [[ $failed -gt 0 ]]; then
    echo -e "${RED}TSAN validation FAILED. See errors above.${NC}"
    exit 1
else
    echo -e "${GREEN}All multi-threaded tests pass under ThreadSanitizer. Zero data races.${NC}"
    exit 0
fi
