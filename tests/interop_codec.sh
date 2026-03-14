#!/usr/bin/env bash
#
# interop_codec.sh - Cross-language codec interop test.
#
# Verifies that C, Rust, and Go produce identical wire bytes for the same
# logical messages, and that each can decode every other's output.
#
# Matrix: C->Rust, C->Go, Rust->C, Rust->Go, Go->C, Go->Rust
# Plus byte-identical comparison across all three.
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
GO_FIXTURE_DIR="${REPO_ROOT}/tests/fixtures/go"

# Temp dirs for encoded files
C_OUT=$(mktemp -d)
RUST_OUT=$(mktemp -d)
GO_OUT=$(mktemp -d)

cleanup() {
    rm -rf "$C_OUT" "$RUST_OUT" "$GO_OUT"
}
trap cleanup EXIT

echo -e "${YELLOW}=== Building C interop binary ===${NC}"
run cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug
run cmake --build "${BUILD_DIR}" --target interop_codec_c

echo ""
echo -e "${YELLOW}=== Building Rust interop binary ===${NC}"
run cargo build --bin interop_codec --manifest-path "${RUST_CRATE_DIR}/Cargo.toml"

echo ""
echo -e "${YELLOW}=== Building Go interop binary ===${NC}"
run env CGO_ENABLED=0 go build -C "${GO_FIXTURE_DIR}" -o "${BUILD_DIR}/bin/interop_codec_go" .

C_BIN="${BUILD_DIR}/bin/interop_codec_c"
RUST_BIN="${RUST_CRATE_DIR}/target/debug/interop_codec"
GO_BIN="${BUILD_DIR}/bin/interop_codec_go"

FAILED=0

FILES="header.bin chunk_header.bin hello.bin hello_ack.bin cgroups_req.bin cgroups_resp.bin cgroups_resp_empty.bin"

# --- Encode with all three ---
echo ""
echo -e "${YELLOW}=== C encode ===${NC}"
run "${C_BIN}" encode "${C_OUT}"

echo ""
echo -e "${YELLOW}=== Rust encode ===${NC}"
run "${RUST_BIN}" encode "${RUST_OUT}"

echo ""
echo -e "${YELLOW}=== Go encode ===${NC}"
run "${GO_BIN}" encode "${GO_OUT}"

# --- Cross-decode: full 6-pair matrix ---
cross_decode() {
    local src_name="$1" dst_name="$2" src_dir="$3" dst_bin="$4"
    echo ""
    echo -e "${YELLOW}=== ${dst_name} decodes ${src_name} output ===${NC}"
    if ! run "${dst_bin}" decode "${src_dir}"; then
        FAILED=1
    fi
}

cross_decode "C"    "Rust" "${C_OUT}"    "${RUST_BIN}"
cross_decode "C"    "Go"   "${C_OUT}"    "${GO_BIN}"
cross_decode "Rust" "C"    "${RUST_OUT}" "${C_BIN}"
cross_decode "Rust" "Go"   "${RUST_OUT}" "${GO_BIN}"
cross_decode "Go"   "C"    "${GO_OUT}"   "${C_BIN}"
cross_decode "Go"   "Rust" "${GO_OUT}"   "${RUST_BIN}"

# --- Byte-identical comparison: all three must match ---
echo ""
echo -e "${YELLOW}=== Byte-identical comparison (C vs Rust vs Go) ===${NC}"

compare_pair() {
    local name_a="$1" dir_a="$2" name_b="$3" dir_b="$4"
    for f in ${FILES}; do
        if cmp -s "${dir_a}/${f}" "${dir_b}/${f}"; then
            echo -e "  ${GREEN}MATCH${NC}: ${name_a} vs ${name_b}: ${f}"
        else
            echo -e "  ${RED}MISMATCH${NC}: ${name_a} vs ${name_b}: ${f}"
            echo "    ${name_a}: $(xxd -p "${dir_a}/${f}" | head -1)"
            echo "    ${name_b}: $(xxd -p "${dir_b}/${f}" | head -1)"
            FAILED=1
        fi
    done
}

compare_pair "C"    "${C_OUT}"    "Rust" "${RUST_OUT}"
compare_pair "C"    "${C_OUT}"    "Go"   "${GO_OUT}"
compare_pair "Rust" "${RUST_OUT}" "Go"   "${GO_OUT}"

echo ""
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}=== ALL INTEROP TESTS PASSED ===${NC}"
    exit 0
else
    echo -e "${RED}=== INTEROP TESTS FAILED ===${NC}"
    exit 1
fi
