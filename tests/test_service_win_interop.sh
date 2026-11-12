#!/usr/bin/env bash
#
# test_service_win_interop.sh - Cross-language L2 service interop tests (Windows).
#
# Tests all 9 directed pairs: C, Rust, and Go servers and clients.
# Each test starts a managed server with a cgroups handler (3 items),
# the client connects, calls snapshot, verifies items.
# All pairs run twice: once normally and once with a 300 ms server handler.
#
# Expects:
#   $INTEROP_SVC_C    path to the C interop_service_win binary
#   $INTEROP_SVC_RS   path to the Rust interop_service_win binary
#   $INTEROP_SVC_GO   path to the Go interop_service_win binary (optional)
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
HANDLER_DELAY_MS=""

# Resolve binary paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_SVC_C:=${ROOT_DIR}/build/bin/interop_service_win_c.exe}"
: "${INTEROP_SVC_RS:=${ROOT_DIR}/src/crates/netipc/target/debug/interop_service_win.exe}"
: "${INTEROP_SVC_GO:=${ROOT_DIR}/build/bin/interop_service_win_go.exe}"

HAS_GO=0
if [[ -x "$INTEROP_SVC_GO" ]]; then
    HAS_GO=1
fi

RUN_DIR_HOST="$(mktemp -d "${TMPDIR:-/tmp}/nipc_svc_win_interop.XXXXXX")"
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
    if [[ ! -x "$INTEROP_SVC_C" ]]; then
        echo -e "${RED}Missing C binary: $INTEROP_SVC_C${NC}" >&2
        missing=1
    fi
    if [[ ! -x "$INTEROP_SVC_RS" ]]; then
        echo -e "${RED}Missing Rust binary: $INTEROP_SVC_RS${NC}" >&2
        missing=1
    fi
    if [[ $missing -ne 0 ]]; then
        echo "Build both C and Rust binaries first." >&2
        exit 1
    fi
    if [[ $HAS_GO -eq 0 ]]; then
        echo -e "${YELLOW}Go binary not found: $INTEROP_SVC_GO (Go tests will be skipped)${NC}" >&2
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
        env NIPC_PROFILE="${NIPC_PROFILE:-}" NIPC_HANDLER_DELAY_MS="$HANDLER_DELAY_MS" \
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
        FAIL=$((FAIL + 1))
        return
    fi

    echo -e "${GREEN}PASS${NC}"
    PASS=$((PASS + 1))
}

# Run all 9 directed pairs; HANDLER_DELAY_MS slows every server handler.
run_all_pairs() {
    local suffix="$1"
    local label="$2"

    run_test "C server, C client${label}" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_C" "svc_c_c${suffix}"

    run_test "Rust server, Rust client${label}" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_RS" "svc_rs_rs${suffix}"

    run_test "Go server, Go client${label}" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_GO" "svc_go_go${suffix}"

    run_test "C server, Rust client${label}" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_RS" "svc_c_rs${suffix}"

    run_test "Rust server, C client${label}" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_C" "svc_rs_c${suffix}"

    run_test "C server, Go client${label}" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_GO" "svc_c_go${suffix}"

    run_test "Go server, C client${label}" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_C" "svc_go_c${suffix}"

    run_test "Rust server, Go client${label}" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_GO" "svc_rs_go${suffix}"

    run_test "Go server, Rust client${label}" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_RS" "svc_go_rs${suffix}"
}

main() {
    echo "=== L2 Service Cross-Language Interop Tests (Windows) ==="
    echo ""

    check_binaries

    echo "C binary:    $INTEROP_SVC_C"
    echo "Rust binary: $INTEROP_SVC_RS"
    echo "Go binary:   $INTEROP_SVC_GO"
    echo ""

    run_all_pairs "" ""

    # Handlers slower than the clients' 100 ms abort-poll slice must still
    # complete within the call timeout (SOW-0041).
    HANDLER_DELAY_MS=300
    run_all_pairs "_slow" " (300 ms handler)"

    echo ""
    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="

    [[ $FAIL -eq 0 ]]
}

main "$@"
