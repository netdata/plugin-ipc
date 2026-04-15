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
TIMEOUT=10
CLIENT_FAIL_REASON=""

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
    local pids
    pids=$(jobs -p 2>/dev/null) || true
    if [[ -n "$pids" ]]; then
        for pid in $pids; do
            stop_server "$pid" >/dev/null 2>&1 || true
        done
    fi
    rm -rf "$RUN_DIR_HOST"
}
trap cleanup EXIT

stop_server() {
    local server_pid="$1"
    local waited=0

    if ! kill -0 "$server_pid" 2>/dev/null; then
        wait "$server_pid" 2>/dev/null || true
        return 0
    fi

    kill "$server_pid" 2>/dev/null || true

    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            wait "$server_pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    if command -v taskkill.exe >/dev/null 2>&1; then
        taskkill.exe /PID "$server_pid" /T /F >/dev/null 2>&1 || true
    elif command -v taskkill >/dev/null 2>&1; then
        taskkill /PID "$server_pid" /T /F >/dev/null 2>&1 || true
    fi

    waited=0
    while [[ $waited -lt 50 ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            wait "$server_pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    wait "$server_pid" 2>/dev/null || true
    return 1
}

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

run_client_with_ready_retry() {
    local client_bin="$1"
    local service="$2"
    local max_attempts=$((TIMEOUT * 10))
    local attempt
    local client_out
    local rc

    CLIENT_FAIL_REASON=""

    for ((attempt = 1; attempt <= max_attempts; attempt++)); do
        set +e
        client_out=$(env NIPC_PROFILE="${NIPC_PROFILE:-}" "$client_bin" client "$RUN_DIR" "$service" 2>&1)
        rc=$?
        set -e

        if [[ $rc -eq 0 ]] && echo "$client_out" | grep -q "^PASS$"; then
            return 0
        fi

        if echo "$client_out" | grep -q "^client: not ready$" &&
           [[ $attempt -lt $max_attempts ]]; then
            sleep 0.1
            continue
        fi

        if [[ $rc -eq 0 ]]; then
            CLIENT_FAIL_REASON="client output: $client_out"
        else
            CLIENT_FAIL_REASON="client exit code $rc, output: $client_out"
        fi
        return 1
    done

    CLIENT_FAIL_REASON="client did not become ready after ${TIMEOUT}s"
    return 1
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
    local server_log="${RUN_DIR_HOST}/${service}.server.log"
    env NIPC_PROFILE="${NIPC_PROFILE:-}" "$server_bin" server "$RUN_DIR" "$service" > "$server_log" 2>&1 &
    server_pid=$!

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

    local test_ok=1
    local fail_reason=""

    if ! run_client_with_ready_retry "$client_bin" "$service"; then
        test_ok=0
        fail_reason="$CLIENT_FAIL_REASON"
    fi

    if ! stop_server "$server_pid"; then
        test_ok=0
        if [[ -n "$fail_reason" ]]; then
            fail_reason="${fail_reason}; server did not exit cleanly"
        else
            fail_reason="server did not exit cleanly"
        fi
    fi

    if [[ $test_ok -eq 0 ]]; then
        echo -e "${RED}FAIL${NC} (${fail_reason})"
        FAIL=$((FAIL + 1))
        return
    fi

    echo -e "${GREEN}PASS${NC}"
    PASS=$((PASS + 1))
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
