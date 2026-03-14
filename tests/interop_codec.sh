#!/usr/bin/env bash
#
# interop_codec.sh - Cross-language codec interop test.
#
# Verifies that C and Rust produce identical wire bytes for the same
# logical messages, and that each can decode the other's output.
#
# Steps:
#   1. Build C interop binary
#   2. Build Rust interop binary
#   3. C encodes -> Rust decodes
#   4. Rust encodes -> C decodes
#   5. Byte-identical comparison (C output vs Rust output)
#
# Returns 0 if all checks pass, 1 otherwise.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"
    if ! "$@"; then
        local exit_code=$?
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
        return $exit_code
    fi
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build"
RUST_CRATE_DIR="${REPO_ROOT}/src/crates/netipc"

# Temp dirs for encoded files
C_OUT=$(mktemp -d)
RUST_OUT=$(mktemp -d)

cleanup() {
    rm -rf "$C_OUT" "$RUST_OUT"
}
trap cleanup EXIT

echo -e "${YELLOW}=== Building C interop binary ===${NC}"
run cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug
run cmake --build "${BUILD_DIR}" --target interop_codec_c

echo ""
echo -e "${YELLOW}=== Building Rust interop binary ===${NC}"
run cargo build --bin interop_codec --manifest-path "${RUST_CRATE_DIR}/Cargo.toml"

C_BIN="${BUILD_DIR}/bin/interop_codec_c"
RUST_BIN="${RUST_CRATE_DIR}/target/debug/interop_codec"

FAILED=0

# Step 1: C encodes to files
echo ""
echo -e "${YELLOW}=== C encode ===${NC}"
run "${C_BIN}" encode "${C_OUT}"

# Step 2: Rust decodes C output
echo ""
echo -e "${YELLOW}=== Rust decodes C output ===${NC}"
if ! run "${RUST_BIN}" decode "${C_OUT}"; then
    FAILED=1
fi

# Step 3: Rust encodes to files
echo ""
echo -e "${YELLOW}=== Rust encode ===${NC}"
run "${RUST_BIN}" encode "${RUST_OUT}"

# Step 4: C decodes Rust output
echo ""
echo -e "${YELLOW}=== C decodes Rust output ===${NC}"
if ! run "${C_BIN}" decode "${RUST_OUT}"; then
    FAILED=1
fi

# Step 5: Byte-identical comparison
echo ""
echo -e "${YELLOW}=== Byte-identical comparison ===${NC}"
ALL_MATCH=1
for f in header.bin chunk_header.bin hello.bin hello_ack.bin cgroups_req.bin \
         cgroups_resp.bin cgroups_resp_empty.bin; do
    if cmp -s "${C_OUT}/${f}" "${RUST_OUT}/${f}"; then
        echo -e "  ${GREEN}MATCH${NC}: ${f}"
    else
        echo -e "  ${RED}MISMATCH${NC}: ${f}"
        echo "    C output:    $(xxd -p "${C_OUT}/${f}" | head -1)"
        echo "    Rust output: $(xxd -p "${RUST_OUT}/${f}" | head -1)"
        ALL_MATCH=0
        FAILED=1
    fi
done

echo ""
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}=== ALL INTEROP TESTS PASSED ===${NC}"
    exit 0
else
    echo -e "${RED}=== INTEROP TESTS FAILED ===${NC}"
    exit 1
fi
