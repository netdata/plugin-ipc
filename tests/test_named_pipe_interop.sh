#!/usr/bin/env bash
#
# test_named_pipe_interop.sh - Cross-language Named Pipe transport interop tests.
#
# Tests all 9 directed pairs: C, Rust, and Go servers and clients.
# All binaries use identical wire protocol, FNV-1a pipe name derivation,
# and auth tokens.
#
# Expects:
#   $INTEROP_NP_C     path to the C interop_named_pipe binary
#   $INTEROP_NP_RS    path to the Rust interop_named_pipe binary
#   $INTEROP_NP_GO    path to the Go interop_named_pipe binary (optional)
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
TIMEOUT=10

# Resolve binary paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_NP_C:=${ROOT_DIR}/build/bin/interop_named_pipe_c.exe}"
: "${INTEROP_NP_RS:=${ROOT_DIR}/build/bin/interop_named_pipe_rs.exe}"
: "${INTEROP_NP_GO:=${ROOT_DIR}/build/bin/interop_named_pipe_go.exe}"

# Auto-discover: try cargo target directory for Rust binary
if [[ ! -x "$INTEROP_NP_RS" ]]; then
    CARGO_BIN="${ROOT_DIR}/src/crates/netipc/target/debug/interop_named_pipe.exe"
    if [[ -x "$CARGO_BIN" ]]; then
        INTEROP_NP_RS="$CARGO_BIN"
    fi
fi

HAS_GO=0
if [[ -x "$INTEROP_NP_GO" ]]; then
    HAS_GO=1
fi

RUN_DIR_HOST="$(mktemp -d "${TMPDIR:-/tmp}/nipc_named_pipe_interop.XXXXXX")"
if command -v cygpath >/dev/null 2>&1; then
    RUN_DIR="$(cygpath -w "$RUN_DIR_HOST")"
else
    RUN_DIR="$RUN_DIR_HOST"
fi

cleanup() {
    local pids
    pids=$(jobs -p 2>/dev/null) || true
    if [[ -n "$pids" ]]; then
        kill $pids 2>/dev/null || true
        wait $pids 2>/dev/null || true
    fi
    rm -rf "$RUN_DIR_HOST"
}
trap cleanup EXIT

check_binaries() {
    local missing=0
    if [[ ! -x "$INTEROP_NP_C" ]]; then
        echo -e "${RED}Missing C binary: $INTEROP_NP_C${NC}" >&2
        missing=1
    fi
    if [[ ! -x "$INTEROP_NP_RS" ]]; then
        echo -e "${RED}Missing Rust binary: $INTEROP_NP_RS${NC}" >&2
        missing=1
    fi
    if [[ $missing -ne 0 ]]; then
        echo "Build both C and Rust binaries first." >&2
        exit 1
    fi
    if [[ $HAS_GO -eq 0 ]]; then
        echo -e "${YELLOW}Go binary not found: $INTEROP_NP_GO (Go tests will be skipped)${NC}" >&2
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

    # Start server in background, wait for READY
    local server_pid
    local server_log="${RUN_DIR_HOST}/${service}.server.log"
    "$server_bin" server "$RUN_DIR" "$service" > "$server_log" 2>&1 &
    server_pid=$!

    # Wait for READY line (up to TIMEOUT seconds)
    local waited=0
    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo -e "${RED}FAIL${NC} (server exited early)"
            cat "$server_log" >&2 2>/dev/null || true
            FAIL=$((FAIL + 1))
            return
        fi
        if grep -q "^READY$" "$server_log" 2>/dev/null; then
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
        return
    fi

    # Run client
    local client_out
    if client_out=$("$client_bin" client "$RUN_DIR" "$service" 2>&1); then
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

    # Wait for server to exit
    wait "$server_pid" 2>/dev/null || true
}

main() {
    echo "=== Named Pipe Transport Cross-Language Interop Tests ==="
    echo ""

    check_binaries

    echo "C binary:    $INTEROP_NP_C"
    echo "Rust binary: $INTEROP_NP_RS"
    echo "Go binary:   $INTEROP_NP_GO"
    echo ""

    # --- Same-language baseline pairs (3 tests) ---

    run_test "C server, C client" \
        "$INTEROP_NP_C" "$INTEROP_NP_C" "interop_np_c_c"

    run_test "Rust server, Rust client" \
        "$INTEROP_NP_RS" "$INTEROP_NP_RS" "interop_np_rs_rs"

    run_test "Go server, Go client" \
        "$INTEROP_NP_GO" "$INTEROP_NP_GO" "interop_np_go_go"

    # --- Cross-language pairs (6 tests) ---

    run_test "C server, Rust client" \
        "$INTEROP_NP_C" "$INTEROP_NP_RS" "interop_np_c_rs"

    run_test "Rust server, C client" \
        "$INTEROP_NP_RS" "$INTEROP_NP_C" "interop_np_rs_c"

    run_test "C server, Go client" \
        "$INTEROP_NP_C" "$INTEROP_NP_GO" "interop_np_c_go"

    run_test "Go server, C client" \
        "$INTEROP_NP_GO" "$INTEROP_NP_C" "interop_np_go_c"

    run_test "Rust server, Go client" \
        "$INTEROP_NP_RS" "$INTEROP_NP_GO" "interop_np_rs_go"

    run_test "Go server, Rust client" \
        "$INTEROP_NP_GO" "$INTEROP_NP_RS" "interop_np_go_rs"

    echo ""
    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="

    [[ $FAIL -eq 0 ]]
}

main "$@"
