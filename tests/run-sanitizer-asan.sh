#!/bin/bash
# Run all C tests under AddressSanitizer + UndefinedBehaviorSanitizer
# Detects: heap/stack buffer overflow, use-after-free, double-free,
#          memory leaks, signed integer overflow, null pointer deref, etc.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-asan"

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

echo -e "${CYAN}=== AddressSanitizer + UndefinedBehaviorSanitizer ===${NC}"
echo

# Clean and configure
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

ASAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g -fno-sanitize-recover=all"

echo -e "${YELLOW}Configuring with ASAN+UBSAN...${NC}"
run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_C_FLAGS="$ASAN_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" 2>&1

echo -e "${YELLOW}Building test binaries...${NC}"
run cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1

# All C test binaries to run
TESTS=(test_protocol test_uds test_shm test_service test_service_extra test_cache test_multi_server)

total=0
passed=0
failed=0
failed_tests=()

echo
echo -e "${CYAN}=== Running tests under ASAN ===${NC}"
echo

for t in "${TESTS[@]}"; do
    binary="$BUILD_DIR/bin/$t"
    if [[ ! -x "$binary" ]]; then
        echo -e "  ${YELLOW}$t not found, skipping${NC}"
        continue
    fi

    total=$((total + 1))
    echo -e "${YELLOW}--- $t ---${NC}"

    # Capture both stdout and stderr; ASAN reports to stderr
    log_file="$BUILD_DIR/${t}-asan.log"

    # ASAN options: halt_on_error=1 ensures we catch errors
    # detect_leaks=1 enables LeakSanitizer (on by default with ASAN on Linux)
    export ASAN_OPTIONS="halt_on_error=0:detect_leaks=1:print_stacktrace=1:color=always"
    export UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1:color=always"

    set +e
    "$binary" > "$log_file" 2>&1
    exit_code=$?
    set -e

    # Check for ASAN/UBSAN errors in output
    has_asan_error=false
    if grep -qE "(ERROR: AddressSanitizer|SUMMARY: AddressSanitizer|ERROR: LeakSanitizer|SUMMARY: UndefinedBehaviorSanitizer|runtime error:)" "$log_file" 2>/dev/null; then
        has_asan_error=true
    fi

    if [[ $exit_code -ne 0 ]] || $has_asan_error; then
        echo -e "  ${RED}$t FAILED${NC} (exit=$exit_code, asan_error=$has_asan_error)"
        failed=$((failed + 1))
        failed_tests+=("$t")
        # Show the sanitizer output
        echo -e "${RED}--- ASAN output for $t ---${NC}"
        cat "$log_file"
        echo -e "${RED}--- end $t ---${NC}"
        echo
    else
        echo -e "  ${GREEN}$t PASSED${NC} (clean)"
        passed=$((passed + 1))
        # Still show test output for transparency
        if [[ -s "$log_file" ]]; then
            # Show just the summary lines
            grep -E "(PASS|FAIL|test_|Test )" "$log_file" | tail -5 || true
        fi
    fi
    echo
done

echo -e "${CYAN}=== ASAN Summary ===${NC}"
echo -e "  Total:  $total"
echo -e "  Passed: ${GREEN}$passed${NC}"
echo -e "  Failed: ${RED}$failed${NC}"
if [[ ${#failed_tests[@]} -gt 0 ]]; then
    echo -e "  Failed tests: ${RED}${failed_tests[*]}${NC}"
fi
echo

if [[ $failed -gt 0 ]]; then
    echo -e "${RED}ASAN validation FAILED. See errors above.${NC}"
    exit 1
else
    echo -e "${GREEN}All C tests pass under AddressSanitizer. Zero findings.${NC}"
    exit 0
fi
