#!/bin/bash
# Run all Go tests with the race detector enabled
# Detects: data races in concurrent Go code

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GO_DIR="$ROOT_DIR/src/go"

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

echo -e "${CYAN}=== Go Race Detector ===${NC}"
echo

# Go's race detector requires cgo (it uses a C runtime for instrumentation).
# CGO_ENABLED=1 is the default when the host C compiler is available.
# We explicitly set it to ensure the race detector works.

echo -e "${YELLOW}Running Go tests with -race...${NC}"
echo

# Test packages individually for clear per-package reporting
PACKAGES=(
    "./pkg/netipc/protocol/"
    "./pkg/netipc/transport/posix/"
    "./pkg/netipc/service/cgroups/"
)

total=0
passed=0
failed=0
failed_pkgs=()

for pkg in "${PACKAGES[@]}"; do
    total=$((total + 1))
    echo -e "${YELLOW}--- $pkg ---${NC}"

    log_file="/tmp/go-race-$(echo "$pkg" | tr '/' '-').log"

    set +e
    (cd "$GO_DIR" && CGO_ENABLED=1 go test -race -count=1 -timeout=120s -v "$pkg") > "$log_file" 2>&1
    exit_code=$?
    set -e

    has_race=false
    if grep -qE "(WARNING: DATA RACE|SUMMARY:.*data race)" "$log_file" 2>/dev/null; then
        has_race=true
    fi

    if [[ $exit_code -ne 0 ]] || $has_race; then
        echo -e "  ${RED}$pkg FAILED${NC} (exit=$exit_code, race=$has_race)"
        failed=$((failed + 1))
        failed_pkgs+=("$pkg")
        echo -e "${RED}--- Race detector output for $pkg ---${NC}"
        cat "$log_file"
        echo -e "${RED}--- end $pkg ---${NC}"
    else
        echo -e "  ${GREEN}$pkg PASSED${NC} (clean)"
        passed=$((passed + 1))
        # Show test summary
        grep -E "^(ok|FAIL|---)" "$log_file" || true
    fi
    echo
done

echo -e "${CYAN}=== Go Race Detector Summary ===${NC}"
echo -e "  Total:  $total"
echo -e "  Passed: ${GREEN}$passed${NC}"
echo -e "  Failed: ${RED}$failed${NC}"
if [[ ${#failed_pkgs[@]} -gt 0 ]]; then
    echo -e "  Failed packages: ${RED}${failed_pkgs[*]}${NC}"
fi
echo

if [[ $failed -gt 0 ]]; then
    echo -e "${RED}Go race detector FAILED. See errors above.${NC}"
    exit 1
else
    echo -e "${GREEN}All Go tests pass with race detector. Zero data races.${NC}"
    exit 0
fi
