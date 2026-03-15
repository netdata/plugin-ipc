#!/usr/bin/env bash
#
# test_service_win_interop.sh - Cross-language L2 service interop tests (Windows).
#
# Tests all 9 directed pairs: C, Rust, and Go servers and clients.
# Each test starts a managed server with a cgroups handler (3 items),
# the client connects, calls snapshot, verifies items.
#
# Expects:
#   $INTEROP_SVC_C    path to the C interop_service_win binary
#   $INTEROP_SVC_RS   path to the Rust interop_service_win binary
#   $INTEROP_SVC_GO   path to the Go interop_service_win binary (optional)
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
RUN_DIR="C:\\Temp\\nipc_svc_win_interop"
TIMEOUT=10

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

cleanup() {
    local pids
    pids=$(jobs -p 2>/dev/null) || true
    if [[ -n "$pids" ]]; then
        kill $pids 2>/dev/null || true
        wait $pids 2>/dev/null || true
    fi
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

    local server_pid
    "$server_bin" server "$RUN_DIR" "$service" > /tmp/nipc_svc_win_server_out_$$ 2>&1 &
    server_pid=$!

    local waited=0
    while [[ $waited -lt $TIMEOUT ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo -e "${RED}FAIL${NC} (server exited early)"
            cat /tmp/nipc_svc_win_server_out_$$ >&2 2>/dev/null || true
            FAIL=$((FAIL + 1))
            rm -f /tmp/nipc_svc_win_server_out_$$
            return
        fi
        if grep -q "^READY$" /tmp/nipc_svc_win_server_out_$$ 2>/dev/null; then
            break
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    if [[ $waited -ge $TIMEOUT ]]; then
        echo -e "${RED}FAIL${NC} (server not ready after ${TIMEOUT}s)"
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        FAIL=$((FAIL + 1))
        rm -f /tmp/nipc_svc_win_server_out_$$
        return
    fi

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

    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -f /tmp/nipc_svc_win_server_out_$$
}

main() {
    echo "=== L2 Service Cross-Language Interop Tests (Windows) ==="
    echo ""

    check_binaries

    echo "C binary:    $INTEROP_SVC_C"
    echo "Rust binary: $INTEROP_SVC_RS"
    echo "Go binary:   $INTEROP_SVC_GO"
    echo ""

    run_test "C server, C client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_C" "svc_c_c"

    run_test "Rust server, Rust client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_RS" "svc_rs_rs"

    run_test "Go server, Go client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_GO" "svc_go_go"

    run_test "C server, Rust client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_RS" "svc_c_rs"

    run_test "Rust server, C client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_C" "svc_rs_c"

    run_test "C server, Go client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_GO" "svc_c_go"

    run_test "Go server, C client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_C" "svc_go_c"

    run_test "Rust server, Go client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_GO" "svc_rs_go"

    run_test "Go server, Rust client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_RS" "svc_go_rs"

    echo ""
    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="

    [[ $FAIL -eq 0 ]]
}

main "$@"
