#!/usr/bin/env bash
#
# run-extended-fuzz.sh - Extended fuzz testing for netipc protocol.
#
# Runs all fuzz targets with longer durations than CTest:
#   - Go: 8 fuzz targets x 60s each
#   - C:  standalone fuzz harness with 1MB random data
#   - Rust: proptest with 100000 cases (via env var)
#
# Usage:
#   ./tests/run-extended-fuzz.sh [fuzztime_seconds]
#
# Default fuzz time: 60 seconds per Go target.

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

FUZZTIME="${1:-60}"

PASS=0
FAIL=0

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"

    if ! "$@"; then
        local exit_code=$?
        echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
        echo -e >&2 "${RED}        Full command:${NC} $*"
        echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
        echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        return $exit_code
    fi
}

log() {
    printf "${CYAN}[fuzz]${NC} %s\n" "$*" >&2
}

pass() {
    printf "${GREEN}  PASS:${NC} %s\n" "$*"
    PASS=$((PASS + 1))
}

fail() {
    printf "${RED}  FAIL:${NC} %s\n" "$*"
    FAIL=$((FAIL + 1))
}

# ---------------------------------------------------------------------------
#  Go fuzz targets (8 targets x FUZZTIME seconds each)
# ---------------------------------------------------------------------------

run_go_fuzz() {
    log "=== Go Fuzz Targets (${FUZZTIME}s each) ==="

    local GO_PKG_DIR="${ROOT_DIR}/src/go"
    local targets=(
        FuzzDecodeHeader
        FuzzDecodeChunkHeader
        FuzzDecodeHello
        FuzzDecodeHelloAck
        FuzzDecodeCgroupsRequest
        FuzzDecodeCgroupsResponse
        FuzzBatchDirDecode
        FuzzBatchItemGet
    )

    for target in "${targets[@]}"; do
        log "  ${target} (${FUZZTIME}s)..."
        local output
        if output=$(cd "${GO_PKG_DIR}" && CGO_ENABLED=0 go test \
            -fuzz="^${target}$" -fuzztime="${FUZZTIME}s" \
            ./pkg/netipc/protocol/ 2>&1); then
            pass "Go ${target}"
        else
            echo "$output" >&2
            fail "Go ${target}"
        fi
    done
}

# ---------------------------------------------------------------------------
#  C fuzz harness (standalone mode with 1MB random data)
# ---------------------------------------------------------------------------

run_c_fuzz() {
    log "=== C Fuzz Harness (1MB random data) ==="

    local FUZZ_BIN="${BUILD_DIR}/bin/fuzz_protocol"

    if [ ! -x "$FUZZ_BIN" ]; then
        log "  C fuzz binary not found: ${FUZZ_BIN}"
        log "  Build with: cmake --build build"
        fail "C fuzz binary missing"
        return
    fi

    local tmpdata="/tmp/netipc_fuzz_$$.bin"

    log "  Feeding 1MB random data to standalone fuzz harness..."
    dd if=/dev/urandom bs=1048576 count=1 of="$tmpdata" 2>/dev/null
    if run "$FUZZ_BIN" "$tmpdata"; then
        pass "C fuzz harness (1MB random)"
    else
        fail "C fuzz harness (1MB random)"
    fi
    rm -f "$tmpdata"

    # Also run multiple smaller chunks to increase coverage of edge cases
    log "  Feeding 1000 x 1KB random chunks..."
    local c_ok=0
    for i in $(seq 1 1000); do
        dd if=/dev/urandom bs=1024 count=1 of="$tmpdata" 2>/dev/null
        if "$FUZZ_BIN" "$tmpdata" >/dev/null 2>&1; then
            c_ok=$((c_ok + 1))
        fi
    done
    rm -f "$tmpdata"
    log "    ${c_ok}/1000 chunks processed without crash"
    if [ "$c_ok" -eq 1000 ]; then
        pass "C fuzz harness (1000 x 1KB)"
    else
        fail "C fuzz harness (1000 x 1KB): ${c_ok}/1000"
    fi
}

# ---------------------------------------------------------------------------
#  Rust proptest (100000 cases via env var)
# ---------------------------------------------------------------------------

run_rust_fuzz() {
    log "=== Rust Proptest (100000 cases) ==="

    local RUST_CRATE_DIR="${ROOT_DIR}/src/crates/netipc"

    if ! command -v cargo &>/dev/null; then
        log "  cargo not found, skipping Rust proptest"
        fail "Rust proptest (cargo not found)"
        return
    fi

    log "  Running proptest with PROPTEST_CASES=100000..."
    if (cd "${RUST_CRATE_DIR}" && \
        PROPTEST_CASES=100000 run cargo test protocol -- --test-threads=1 2>&1); then
        pass "Rust proptest (100000 cases)"
    else
        fail "Rust proptest (100000 cases)"
    fi
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

main() {
    log "Extended Fuzz Testing Suite"
    log "Go fuzz time: ${FUZZTIME}s per target"
    log ""

    run_go_fuzz
    echo ""
    run_c_fuzz
    echo ""
    run_rust_fuzz
    echo ""

    log "=== Results: ${PASS} passed, ${FAIL} failed ==="

    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
}

main "$@"
