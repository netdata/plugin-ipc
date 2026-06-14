#!/usr/bin/env bash
#
# Cross-language Level 2 lookup-scale and mixed-status interop tests.
#
# Runs APPS_LOOKUP and CGROUPS_LOOKUP scale and mixed-status cases across all
# directed C/Rust/Go client/server pairs. The binaries are the existing
# interop_service fixtures with lookup-specific modes.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0
TIMEOUT=30
LOOKUP_ITEMS="${NIPC_LOOKUP_SCALE_ITEMS:-8192}"
SERVER_PIDS=()

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

: "${INTEROP_SVC_C:=${ROOT_DIR}/build/bin/interop_service_c}"
: "${INTEROP_SVC_RS:=${ROOT_DIR}/build/bin/interop_service_rs}"
: "${INTEROP_SVC_GO:=${ROOT_DIR}/build/bin/interop_service_go}"

if [[ ! -x "$INTEROP_SVC_RS" ]]; then
    CARGO_BIN="${ROOT_DIR}/src/crates/netipc/target/debug/interop_service"
    if [[ -x "$CARGO_BIN" ]]; then
        INTEROP_SVC_RS="$CARGO_BIN"
    fi
fi

HAS_GO=0
if [[ -x "$INTEROP_SVC_GO" ]]; then
    HAS_GO=1
fi

RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/nipc_lookup_scale_interop.XXXXXX")"

cleanup() {
    for pid in "${SERVER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$RUN_DIR"
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
        echo "Build C and Rust interop_service binaries first." >&2
        exit 1
    fi
    if [[ $HAS_GO -eq 0 ]]; then
        echo -e "${YELLOW}Go binary not found: $INTEROP_SVC_GO (Go tests will be skipped)${NC}" >&2
    fi
}

wait_for_server() {
    local server_pid="$1"
    local server_log="$2"
    local service="$3"
    local waited=0

    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            return 1
        fi
        if grep -q "^READY$" "$server_log" 2>/dev/null; then
            break
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    if [[ $waited -ge $((TIMEOUT * 10)) ]]; then
        return 2
    fi

    waited=0
    while [[ $waited -lt $((TIMEOUT * 10)) ]]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            return 3
        fi
        if [[ -S "${RUN_DIR}/${service}.sock" ]]; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done

    return 4
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

    rm -f "${RUN_DIR}/${service}.sock"
    rm -f "${RUN_DIR}/${service}-"*.ipcshm

    local server_log="${RUN_DIR}/${service}.server.log"
    env NIPC_PROFILE="${NIPC_PROFILE:-}" \
        NIPC_LOOKUP_SCALE_ITEMS="$LOOKUP_ITEMS" \
        "$server_bin" "$server_mode" "$RUN_DIR" "$service" >"$server_log" 2>&1 &
    local server_pid=$!
    SERVER_PIDS+=("$server_pid")

    if ! wait_for_server "$server_pid" "$server_log" "$service"; then
        local rc=$?
        echo -e "${RED}FAIL${NC} (server readiness rc=${rc})"
        cat "$server_log" >&2 2>/dev/null || true
        FAIL=$((FAIL + 1))
        return
    fi

    local client_out
    if client_out=$(env NIPC_PROFILE="${NIPC_PROFILE:-}" \
            NIPC_LOOKUP_SCALE_ITEMS="$LOOKUP_ITEMS" \
            "$client_bin" "$client_mode" "$RUN_DIR" "$service" 2>&1); then
        if echo "$client_out" | grep -q "^PASS$"; then
            echo -e "${GREEN}PASS${NC}"
            PASS=$((PASS + 1))
        else
            echo -e "${RED}FAIL${NC} (client output: $client_out)"
            FAIL=$((FAIL + 1))
        fi
    else
        echo -e "${RED}FAIL${NC} (client output: $client_out)"
        FAIL=$((FAIL + 1))
    fi

    if kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
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
    echo "=== Lookup-Scale Cross-Language Interop Tests ==="
    echo

    check_binaries
    mkdir -p "$RUN_DIR"

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
