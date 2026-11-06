#!/usr/bin/env bash
#
# test_named_pipe_interop.sh - Cross-language Named Pipe transport interop tests.
#
# Tests all 9 directed pairs: C, Rust, and Go servers and clients, once with
# one request and once with a client that pipelines more than the pipe
# buffers hold before reading any response.
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
    interop_stop_server
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
    local mode_prefix="${5:-}"

    # Skip if either binary is missing
    if [[ ! -x "$server_bin" || ! -x "$client_bin" ]]; then
        echo -e "  $name ... ${YELLOW}SKIP${NC}"
        SKIP=$((SKIP + 1))
        return
    fi

    echo -n "  $name ... "

    local server_log="${RUN_DIR_HOST}/${service}.server.log"
    if ! interop_start_server "$TIMEOUT" "$server_log" \
        "$server_bin" "${mode_prefix}server" "$RUN_DIR" "$service"; then
        echo -e "${RED}FAIL${NC} (server not ready)"
        cat "$server_log" >&2 2>/dev/null || true
        interop_stop_server
        FAIL=$((FAIL + 1))
        return
    fi

    # The server printed READY after creating its endpoint, so the client
    # connects once.
    local client_out rc
    set +e
    client_out=$("$client_bin" "${mode_prefix}client" "$RUN_DIR" "$service" 2>&1)
    rc=$?
    set -e

    if [[ $rc -ne 0 ]] || ! echo "$client_out" | grep -q "^PASS$"; then
        echo -e "${RED}FAIL${NC} (client exit code $rc, output: $client_out)"
        FAIL=$((FAIL + 1))
        # The server exits after serving one client; after a client failure
        # it may still be waiting for one.
        interop_stop_server
        return
    fi

    echo -e "${GREEN}PASS${NC}"
    PASS=$((PASS + 1))
    interop_wait_server
}

main() {
    echo "=== Named Pipe Transport Cross-Language Interop Tests ==="
    echo ""

    check_binaries

    echo "C binary:    $INTEROP_NP_C"
    echo "Rust binary: $INTEROP_NP_RS"
    echo "Go binary:   $INTEROP_NP_GO"
    echo ""

    local pass
    for pass in single pipeline; do
        local prefix="" label="" tag=""
        if [[ $pass == pipeline ]]; then
            prefix="pipeline-" label=" (pipelining)" tag="_pl"
        fi

        run_test "C server, C client${label}" \
            "$INTEROP_NP_C" "$INTEROP_NP_C" "interop_np_c_c${tag}" "$prefix"
        run_test "Rust server, Rust client${label}" \
            "$INTEROP_NP_RS" "$INTEROP_NP_RS" "interop_np_rs_rs${tag}" "$prefix"
        run_test "Go server, Go client${label}" \
            "$INTEROP_NP_GO" "$INTEROP_NP_GO" "interop_np_go_go${tag}" "$prefix"
        run_test "C server, Rust client${label}" \
            "$INTEROP_NP_C" "$INTEROP_NP_RS" "interop_np_c_rs${tag}" "$prefix"
        run_test "Rust server, C client${label}" \
            "$INTEROP_NP_RS" "$INTEROP_NP_C" "interop_np_rs_c${tag}" "$prefix"
        run_test "C server, Go client${label}" \
            "$INTEROP_NP_C" "$INTEROP_NP_GO" "interop_np_c_go${tag}" "$prefix"
        run_test "Go server, C client${label}" \
            "$INTEROP_NP_GO" "$INTEROP_NP_C" "interop_np_go_c${tag}" "$prefix"
        run_test "Rust server, Go client${label}" \
            "$INTEROP_NP_RS" "$INTEROP_NP_GO" "interop_np_rs_go${tag}" "$prefix"
        run_test "Go server, Rust client${label}" \
            "$INTEROP_NP_GO" "$INTEROP_NP_RS" "interop_np_go_rs${tag}" "$prefix"
    done

    echo ""
    echo -e "=== Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${YELLOW}${SKIP} skipped${NC} ==="

    [[ $FAIL -eq 0 ]]
}

main "$@"
