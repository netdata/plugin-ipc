#!/bin/bash
# Run all C tests under Valgrind memcheck
# Detects: memory leaks, invalid reads/writes, use of uninitialized memory

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-valgrind"

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

echo -e "${CYAN}=== Valgrind Memcheck ===${NC}"
echo

# Build with debug symbols, no sanitizer (valgrind does its own instrumentation)
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo -e "${YELLOW}Configuring with debug symbols...${NC}"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_C_FLAGS="-O0 -g" 2>&1

echo -e "${YELLOW}Building test binaries...${NC}"
run cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1

# All C test binaries
TESTS=(test_protocol test_uds test_shm test_service test_service_extra test_cache test_multi_server)

total=0
passed=0
failed=0
failed_tests=()

echo
echo -e "${CYAN}=== Running tests under Valgrind ===${NC}"
echo

for t in "${TESTS[@]}"; do
    binary="$BUILD_DIR/bin/$t"
    if [[ ! -x "$binary" ]]; then
        echo -e "  ${YELLOW}$t not found, skipping${NC}"
        continue
    fi

    total=$((total + 1))
    echo -e "${YELLOW}--- $t ---${NC}"

    log_file="$BUILD_DIR/${t}-valgrind.log"

    set +e
    valgrind \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --error-exitcode=99 \
        --errors-for-leak-kinds=definite,possible \
        --log-file="$log_file" \
        "$binary" > "$BUILD_DIR/${t}-stdout.log" 2>&1
    exit_code=$?
    set -e

    # Check valgrind log for errors
    has_errors=false
    error_count=0
    if [[ -f "$log_file" ]]; then
        # Parse the ERROR SUMMARY line
        summary_line=$(grep "ERROR SUMMARY:" "$log_file" | tail -1 || true)
        if [[ -n "$summary_line" ]]; then
            error_count=$(echo "$summary_line" | sed 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/')
        fi
        if [[ $error_count -gt 0 ]]; then
            has_errors=true
        fi
        # Also check for "definitely lost" or "possibly lost" blocks
        if grep -qE "definitely lost: [1-9]|possibly lost: [1-9]|Invalid (read|write)" "$log_file" 2>/dev/null; then
            has_errors=true
        fi
    fi

    if [[ $exit_code -eq 99 ]] || $has_errors; then
        echo -e "  ${RED}$t FAILED${NC} (exit=$exit_code, errors=$error_count)"
        failed=$((failed + 1))
        failed_tests+=("$t")
        echo -e "${RED}--- Valgrind output for $t ---${NC}"
        cat "$log_file"
        echo -e "${RED}--- end $t ---${NC}"
        echo
    elif [[ $exit_code -ne 0 ]]; then
        # Test itself failed (not valgrind error)
        echo -e "  ${RED}$t TEST FAILED${NC} (exit=$exit_code, not a valgrind error)"
        failed=$((failed + 1))
        failed_tests+=("$t")
        echo -e "${RED}--- Test output for $t ---${NC}"
        cat "$BUILD_DIR/${t}-stdout.log"
        echo -e "${RED}--- Valgrind log ---${NC}"
        cat "$log_file"
        echo -e "${RED}--- end $t ---${NC}"
        echo
    else
        echo -e "  ${GREEN}$t PASSED${NC} (clean, errors=$error_count)"
        passed=$((passed + 1))
        # Show leak summary
        if [[ -f "$log_file" ]]; then
            grep -E "(HEAP SUMMARY|LEAK SUMMARY|ERROR SUMMARY)" "$log_file" | while IFS= read -r line; do
                echo -e "    ${GRAY}$line${NC}"
            done
        fi
    fi
    echo
done

echo -e "${CYAN}=== Valgrind Summary ===${NC}"
echo -e "  Total:  $total"
echo -e "  Passed: ${GREEN}$passed${NC}"
echo -e "  Failed: ${RED}$failed${NC}"
if [[ ${#failed_tests[@]} -gt 0 ]]; then
    echo -e "  Failed tests: ${RED}${failed_tests[*]}${NC}"
fi
echo

if [[ $failed -gt 0 ]]; then
    echo -e "${RED}Valgrind validation FAILED. See errors above.${NC}"
    exit 1
else
    echo -e "${GREEN}All C tests pass under Valgrind. Zero leaks, zero invalid accesses.${NC}"
    exit 0
fi
