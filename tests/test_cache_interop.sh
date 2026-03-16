#!/usr/bin/env bash
#
# test_cache_interop.sh - Cross-language L3 cache interop tests.
#
# Tests L3 cache clients against L2 servers across all language pairs.
# The server is pure L2 (no L3 component). The client uses L3 cache
# to refresh, populate, and verify lookups.
#
# This exercises all 9 directed pairs: C, Rust, and Go servers and
# L3 cache clients.
#
# Expects:
#   $INTEROP_CACHE_C    path to the C interop_cache binary
#   $INTEROP_CACHE_RS   path to the Rust interop_cache binary
#   $INTEROP_CACHE_GO   path to the Go interop_cache binary (optional)
#
# Returns 0 on all-pass, 1 on any failure.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0
RUN_DIR="/tmp/nipc_cache_interop_test"
TIMEOUT=10

# Resolve binary paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_CACHE_C:=${ROOT_DIR}/build/bin/interop_cache_c}"
: "${INTEROP_CACHE_RS:=${ROOT_DIR}/build/bin/interop_cache_rs}"
: "${INTEROP_CACHE_GO:=${ROOT_DIR}/build/bin/interop_cache_go}"

# Auto-discover: try cargo target directory for Rust binary
if [[ ! -x "$INTEROP_CACHE_RS" ]]; then
    CARGO_BIN="${ROOT_DIR}/src/crates/netipc/target/debug/interop_cache"
    if [[ -x "$CARGO_BIN" ]]; then
        INTEROP_CACHE_RS="$CARGO_BIN"
    fi
fi

HAS_GO=0
if [[ -x "$INTEROP_CACHE_GO" ]]; then
    HAS_GO=1
fi

cleanup() {
    local pids
    pids=$(jobs -p 2>/dev/null) || true
    if [[ -n "$pids" ]]; then
        kill $pids 2>/dev/null || true
        wait $pids 2>/dev/null || true
    fi
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

check_binaries() {
    local missing=0
    if [[ ! -x "$INTEROP_CACHE_C" ]]; then
        echo -e "${RED}Missing C binary: $INTEROP_CACHE_C${NC}" >&2
        missing=1
    fi
    if [[ ! -x "$INTEROP_CACHE_RS" ]]; then
        echo -e "${RED}Missing Rust binary: $INTEROP_CACHE_RS${NC}" >&2
        missing=1
    fi
    if [[ $missing -ne 0 ]]; then
        echo "Build both C and Rust binaries first." >&2
        exit 1
    fi
    if [[ $HAS_GO -eq 0 ]]; then
        echo -e "${YELLOW}Go binary not found: $INTEROP_CACHE_GO (Go tests will be skipped)${NC}" >&2
    fi
}

run_test() {
    local name="$1"
    local server_bin="$2"
    local client_bin="$3"
    local service="$4"

    # Skip if either binary is missing
    if [[ ! -x "$server_bin" || ! -x "$client_bin" ]]; then
        echo -e "  $name ... ${YELLOW}SKIP${NC}"
        SKIP=$((SKIP + 1))
        return
    fi

    echo -n "  $name ... "

    # Clean any stale socket/shm
    rm -f "${RUN_DIR}/${service}.sock"
    rm -f "${RUN_DIR}/${service}.ipcshm"

    # Start server in background, wait for READY
    local server_pid
    env NIPC_PROFILE="${NIPC_PROFILE:-}" "$server_bin" server "$RUN_DIR" "$service" > /tmp/nipc_cache_server_out_$$ 2>&1 &
    server_pid=$!

    local waited=0
    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo -e "${RED}FAIL${NC} (server exited early)"
            cat /tmp/nipc_cache_server_out_$$ >&2 2>/dev/null || true
            FAIL=$((FAIL + 1))
            rm -f /tmp/nipc_cache_server_out_$$
            return
        fi
        if grep -q "^READY$" /tmp/nipc_cache_server_out_$$ 2>/dev/null; then
            break
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    if [[ $waited -ge $((TIMEOUT * 10)) ]]; then
        echo -e "${RED}FAIL${NC} (server not ready after ${TIMEOUT}s)"
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        FAIL=$((FAIL + 1))
        rm -f /tmp/nipc_cache_server_out_$$
        return
    fi

    # Run client
    local client_out
    if client_out=$(env NIPC_PROFILE="${NIPC_PROFILE:-}" "$client_bin" client "$RUN_DIR" "$service" 2>&1); then
        if echo "$client_out" | grep -q "^PASS$"; then
            echo -e "${GREEN}PASS${NC}"
            PASS=$((PASS + 1))
        else
            echo -e "${RED}FAIL${NC} (client output: $client_out)"
            FAIL=$((FAIL + 1))
        fi
    else
        echo -e "${RED}FAIL${NC} (client exit code $?, output: $client_out)"
        FAIL=$((FAIL + 1))
    fi

    # Kill server and wait
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -f /tmp/nipc_cache_server_out_$$
}

main() {
    echo "=== L3 Cache Cross-Language Interop Tests ==="
    echo ""

    check_binaries
    mkdir -p "$RUN_DIR"

    echo "C binary:    $INTEROP_CACHE_C"
    echo "Rust binary: $INTEROP_CACHE_RS"
    echo "Go binary:   $INTEROP_CACHE_GO"
    echo ""

    # --- Same-language pairs (3 tests) ---
    # Server is pure L2, client uses L3 cache

    run_test "C server, C cache client" \
        "$INTEROP_CACHE_C" "$INTEROP_CACHE_C" "cache_c_c"

    run_test "Rust server, Rust cache client" \
        "$INTEROP_CACHE_RS" "$INTEROP_CACHE_RS" "cache_rs_rs"

    run_test "Go server, Go cache client" \
        "$INTEROP_CACHE_GO" "$INTEROP_CACHE_GO" "cache_go_go"

    # --- Cross-language pairs (6 tests) ---

    run_test "C server, Rust cache client" \
        "$INTEROP_CACHE_C" "$INTEROP_CACHE_RS" "cache_c_rs"

    run_test "Rust server, C cache client" \
        "$INTEROP_CACHE_RS" "$INTEROP_CACHE_C" "cache_rs_c"

    run_test "C server, Go cache client" \
        "$INTEROP_CACHE_C" "$INTEROP_CACHE_GO" "cache_c_go"

    run_test "Go server, C cache client" \
        "$INTEROP_CACHE_GO" "$INTEROP_CACHE_C" "cache_go_c"

    run_test "Rust server, Go cache client" \
        "$INTEROP_CACHE_RS" "$INTEROP_CACHE_GO" "cache_rs_go"

    run_test "Go server, Rust cache client" \
        "$INTEROP_CACHE_GO" "$INTEROP_CACHE_RS" "cache_go_rs"

    echo ""
    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="

    [[ $FAIL -eq 0 ]]
}

main "$@"
