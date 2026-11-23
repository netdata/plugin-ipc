#!/usr/bin/env bash
#
# test_cache_win_interop.sh - Cross-language L3 cache interop tests (Windows).
#
# Tests all 9 directed pairs: C, Rust, and Go servers and clients.
# Server is pure L2, client uses L3 cache.
#
# Expects:
#   $INTEROP_CACHE_C    path to the C interop_cache_win binary
#   $INTEROP_CACHE_RS   path to the Rust interop_cache_win binary
#   $INTEROP_CACHE_GO   path to the Go interop_cache_win binary (optional)
#
# Returns 0 on all-pass, 1 on any failure.

set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
source "$(dirname "${BASH_SOURCE[0]}")/interop-lib.sh"
netipc_low_priority_self "$@"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0
TIMEOUT=10

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_CACHE_C:=${ROOT_DIR}/build/bin/interop_cache_win_c.exe}"
: "${INTEROP_CACHE_RS:=${ROOT_DIR}/src/crates/netipc/target/debug/interop_cache_win.exe}"
: "${INTEROP_CACHE_GO:=${ROOT_DIR}/build/bin/interop_cache_win_go.exe}"

HAS_GO=0
if [[ -x "$INTEROP_CACHE_GO" ]]; then
    HAS_GO=1
fi

RUN_DIR_HOST="$(mktemp -d "${TMPDIR:-/tmp}/nipc_cache_win_interop.XXXXXX")"
if command -v cygpath >/dev/null 2>&1; then
    RUN_DIR="$(cygpath -w "$RUN_DIR_HOST")"
else
    RUN_DIR="$RUN_DIR_HOST"
fi

cleanup() {
    interop_stop_server
    rm -rf "$RUN_DIR_HOST"
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

    if [[ ! -x "$server_bin" || ! -x "$client_bin" ]]; then
        echo -e "  $name ... ${YELLOW}SKIP${NC}"
        SKIP=$((SKIP + 1))
        return
    fi

    echo -n "  $name ... "

    local server_log="${RUN_DIR_HOST}/${service}.server.log"
    if ! interop_start_server "$TIMEOUT" "$server_log" \
        env NIPC_PROFILE="${NIPC_PROFILE:-}" \
        "$server_bin" server "$RUN_DIR" "$service"; then
        echo -e "${RED}FAIL${NC} (server not ready)"
        cat "$server_log" >&2 2>/dev/null || true
        interop_stop_server
        FAIL=$((FAIL + 1))
        return
    fi

    # The server printed READY after binding, so the client connects once.
    local client_out rc
    set +e
    client_out=$(env NIPC_PROFILE="${NIPC_PROFILE:-}" "$client_bin" client "$RUN_DIR" "$service" 2>&1)
    rc=$?
    set -e

    interop_stop_server

    if [[ $rc -ne 0 ]] || ! echo "$client_out" | grep -q "^PASS$"; then
        echo -e "${RED}FAIL${NC} (client exit code $rc, output: $client_out)"
        echo "server output:" >&2
        cat "$server_log" >&2 2>/dev/null || true
        FAIL=$((FAIL + 1))
        return
    fi

    echo -e "${GREEN}PASS${NC}"
    PASS=$((PASS + 1))
}

main() {
    echo "=== L3 Cache Cross-Language Interop Tests (Windows) ==="
    echo ""

    check_binaries

    echo "C binary:    $INTEROP_CACHE_C"
    echo "Rust binary: $INTEROP_CACHE_RS"
    echo "Go binary:   $INTEROP_CACHE_GO"
    echo ""

    run_test "C server, C cache client" \
        "$INTEROP_CACHE_C" "$INTEROP_CACHE_C" "cache_c_c"

    run_test "Rust server, Rust cache client" \
        "$INTEROP_CACHE_RS" "$INTEROP_CACHE_RS" "cache_rs_rs"

    run_test "Go server, Go cache client" \
        "$INTEROP_CACHE_GO" "$INTEROP_CACHE_GO" "cache_go_go"

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
