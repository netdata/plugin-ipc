#!/usr/bin/env bash
#
# Cross-language Level 2 lookup-scale and mixed-status interop tests (Windows).
#
# Runs APPS_LOOKUP and CGROUPS_LOOKUP scale and mixed-status cases across all
# directed C/Rust/Go client/server pairs using the Windows interop_service
# fixtures.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0
TIMEOUT=60
LOOKUP_ITEMS="${NIPC_LOOKUP_SCALE_ITEMS:-8192}"
CLIENT_FAIL_REASON=""
SERVER_PIDS=()

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_SVC_C:=${ROOT_DIR}/build/bin/interop_service_win_c.exe}"
: "${INTEROP_SVC_RS:=${ROOT_DIR}/src/crates/netipc/target/debug/interop_service_win.exe}"
: "${INTEROP_SVC_GO:=${ROOT_DIR}/build/bin/interop_service_win_go.exe}"

HAS_GO=0
if [[ -x "$INTEROP_SVC_GO" ]]; then
    HAS_GO=1
fi

RUN_DIR_HOST="$(mktemp -d "${TMPDIR:-/tmp}/nipc_lookup_scale_win_interop.XXXXXX")"
if command -v cygpath >/dev/null 2>&1; then
    RUN_DIR="$(cygpath -w "$RUN_DIR_HOST")"
else
    RUN_DIR="$RUN_DIR_HOST"
fi

cleanup() {
    for pid in "${SERVER_PIDS[@]}"; do
        stop_server "$pid" >/dev/null 2>&1 || true
    done
    rm -rf "$RUN_DIR_HOST"
}
trap cleanup EXIT

stop_server() {
    local server_pid="$1"
    local waited=0
    local win_pid

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

    win_pid=$(ps -W 2>/dev/null | awk -v pid="$server_pid" '$1 == pid {print $4; exit}')
    if [[ -n "$win_pid" ]] && command -v taskkill.exe >/dev/null 2>&1; then
        taskkill.exe //PID "$win_pid" //T //F >/dev/null 2>&1 || true
    elif [[ -n "$win_pid" ]] && command -v taskkill >/dev/null 2>&1; then
        taskkill //PID "$win_pid" //T //F >/dev/null 2>&1 || true
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
        echo "Build C and Rust Windows interop_service binaries first." >&2
        exit 1
    fi
    if [[ $HAS_GO -eq 0 ]]; then
        echo -e "${YELLOW}Go binary not found: $INTEROP_SVC_GO (Go tests will be skipped)${NC}" >&2
    fi
}

wait_for_ready() {
    local server_pid="$1"
    local server_log="$2"
    local waited=0

    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            return 1
        fi
        if grep -q "^READY$" "$server_log" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    return 2
}

run_client_with_ready_retry() {
    local client_bin="$1"
    local client_mode="$2"
    local service="$3"
    local max_attempts=$((TIMEOUT * 10))
    local attempt
    local client_out
    local rc

    CLIENT_FAIL_REASON=""

    for ((attempt = 1; attempt <= max_attempts; attempt++)); do
        set +e
        client_out=$(env NIPC_PROFILE="${NIPC_PROFILE:-}" \
            NIPC_LOOKUP_SCALE_ITEMS="$LOOKUP_ITEMS" \
            "$client_bin" "$client_mode" "$RUN_DIR" "$service" 2>&1)
        rc=$?
        set -e

        if [[ $rc -eq 0 ]] && echo "$client_out" | grep -q "^PASS$"; then
            return 0
        fi

        if echo "$client_out" | grep -q "not ready" &&
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

run_lookup_test() {
    local method="$1"
    local name="$2"
    local server_bin="$3"
    local client_bin="$4"
    local service="$5"
    local server_mode="${method}-server"
    local client_mode="${method}-client"

    if [[ ! -x "$server_bin" || ! -x "$client_bin" ]]; then
        echo -e "  $name ... ${YELLOW}SKIP${NC}"
        SKIP=$((SKIP + 1))
        return
    fi

    echo -n "  $name ... "

    local server_log="${RUN_DIR_HOST}/${service}.server.log"
    env NIPC_PROFILE="${NIPC_PROFILE:-}" \
        NIPC_LOOKUP_SCALE_ITEMS="$LOOKUP_ITEMS" \
        "$server_bin" "$server_mode" "$RUN_DIR" "$service" >"$server_log" 2>&1 &
    local server_pid=$!
    SERVER_PIDS+=("$server_pid")

    local ready_rc=0
    set +e
    wait_for_ready "$server_pid" "$server_log"
    ready_rc=$?
    set -e
    if [[ $ready_rc -ne 0 ]]; then
        echo -e "${RED}FAIL${NC} (server readiness rc=${ready_rc})"
        cat "$server_log" >&2 2>/dev/null || true
        FAIL=$((FAIL + 1))
        return
    fi

    local test_ok=1
    local fail_reason=""

    if ! run_client_with_ready_retry "$client_bin" "$client_mode" "$service"; then
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

run_matrix_for_method() {
    local method="$1"
    local label="$2"
    local item_label="${3:-${LOOKUP_ITEMS} items}"

    echo "$label (${item_label})"

    run_lookup_test "$method" "C server, C client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_C" "${method}_c_c"
    run_lookup_test "$method" "Rust server, Rust client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_RS" "${method}_rs_rs"
    run_lookup_test "$method" "Go server, Go client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_GO" "${method}_go_go"

    run_lookup_test "$method" "C server, Rust client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_RS" "${method}_c_rs"
    run_lookup_test "$method" "Rust server, C client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_C" "${method}_rs_c"
    run_lookup_test "$method" "C server, Go client" \
        "$INTEROP_SVC_C" "$INTEROP_SVC_GO" "${method}_c_go"
    run_lookup_test "$method" "Go server, C client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_C" "${method}_go_c"
    run_lookup_test "$method" "Rust server, Go client" \
        "$INTEROP_SVC_RS" "$INTEROP_SVC_GO" "${method}_rs_go"
    run_lookup_test "$method" "Go server, Rust client" \
        "$INTEROP_SVC_GO" "$INTEROP_SVC_RS" "${method}_go_rs"

    echo
}

main() {
    echo "=== Lookup-Scale Cross-Language Interop Tests (Windows) ==="
    echo

    check_binaries

    echo "C binary:    $INTEROP_SVC_C"
    echo "Rust binary: $INTEROP_SVC_RS"
    echo "Go binary:   $INTEROP_SVC_GO"
    echo "Items:       $LOOKUP_ITEMS"
    echo

    run_matrix_for_method apps "APPS_LOOKUP scale"
    run_matrix_for_method cgroups "CGROUPS_LOOKUP scale"
    run_matrix_for_method apps-mixed "APPS_LOOKUP mixed status" "5 items"
    run_matrix_for_method cgroups-mixed "CGROUPS_LOOKUP mixed status" "5 items"

    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="
    [[ $FAIL -eq 0 ]]
}

main "$@"
