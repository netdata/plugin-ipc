#!/usr/bin/env bash
# Generate C, Rust, and Go coverage reports for Codacy upload.

set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/coverage/codacy}"
C_BUILD_DIR="$ROOT_DIR/build-codacy-coverage"
RUST_CRATE_DIR="$ROOT_DIR/src/crates/netipc"
GO_DIR="$ROOT_DIR/src/go"

RUST_IGNORE_REGEX='(src[\\/]+service[\\/]+cgroups_windows_tests\.rs|src[\\/]+transport[\\/]+windows\.rs|src[\\/]+transport[\\/]+win_shm\.rs)'

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"

    if "$@"; then
        return 0
    else
        local exit_code=$?
        echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
        echo -e >&2 "${RED}        Full command:${NC} $*"
        echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
        echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        return "$exit_code"
    fi
}

require_tool() {
    local tool="$1"
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e >&2 "${RED}ERROR:${NC} Missing required tool: $tool"
        exit 1
    fi
}

rewrite_go_cover_paths() {
    local input="$1"
    local output="$2"

    awk '
        $1 == "mode:" { print; next }
        {
            sub(/^github\.com\/netdata\/plugin-ipc\/go\//, "src/go/")
            print
        }
    ' "$input" > "$output"
}

rewrite_lcov_paths() {
    local report="$1"

    python3 - "$ROOT_DIR" "$report" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
report = pathlib.Path(sys.argv[2])
prefix = f"SF:{root}/"
rewritten = []

for line in report.read_text(encoding="utf-8").splitlines():
    if line.startswith("VER:"):
        continue
    if line.startswith("DA:"):
        line = ",".join(line.split(",", 2)[:2])
    if line.startswith(prefix):
        line = "SF:" + line[len(prefix):]
    rewritten.append(line)

report.write_text("\n".join(rewritten) + "\n", encoding="utf-8")
PY
}

generate_c_coverage() {
    echo -e "${YELLOW}Generating C coverage...${NC}"

    rm -rf "$C_BUILD_DIR"
    run cmake -S "$ROOT_DIR" -B "$C_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DNETIPC_COVERAGE=ON \
        -DCMAKE_C_COMPILER=gcc
    run cmake --build "$C_BUILD_DIR" -j"$(nproc)"

    local tests=(
        test_protocol
        test_uds
        test_shm
        test_service
        test_service_extra
        test_service_payload_limits
        test_service_method_limits
        test_cache
        test_multi_server
        test_chaos
        test_hardening
        test_ping_pong
        test_stress
    )

    for test_name in "${tests[@]}"; do
        local binary="$C_BUILD_DIR/bin/$test_name"
        if [[ ! -x "$binary" ]]; then
            echo -e >&2 "${RED}ERROR:${NC} Missing C coverage test binary: $binary"
            exit 1
        fi
        run "$binary"
    done

    run gcovr \
        --root "$ROOT_DIR" \
        --object-directory "$C_BUILD_DIR" \
        --filter 'src/libnetdata/netipc/src/protocol/.*' \
        --filter 'src/libnetdata/netipc/src/transport/posix/.*' \
        --filter 'src/libnetdata/netipc/src/service/netipc_service\.c' \
        --exclude '.*_win\.c' \
        --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
        --merge-mode-functions merge-use-line-min \
        --lcov "$OUT_DIR/c-lcov.info"

    rewrite_lcov_paths "$OUT_DIR/c-lcov.info"
}

generate_rust_coverage() {
    echo -e "${YELLOW}Generating Rust coverage...${NC}"

    if ! command -v cargo-llvm-cov >/dev/null 2>&1 && ! cargo llvm-cov --version >/dev/null 2>&1; then
        run cargo install cargo-llvm-cov --locked
    fi

    run rustup component add llvm-tools-preview

    (
        cd "$RUST_CRATE_DIR"
        run cargo llvm-cov clean --workspace
        run cargo llvm-cov --lib --no-report -- --test-threads=1
        run cargo llvm-cov report \
            --lcov \
            --ignore-filename-regex "$RUST_IGNORE_REGEX" \
            --output-path "$OUT_DIR/rust-lcov.info"
    )

    rewrite_lcov_paths "$OUT_DIR/rust-lcov.info"
}

generate_go_coverage() {
    echo -e "${YELLOW}Generating Go coverage...${NC}"

    local tmp_dir
    tmp_dir="$(mktemp -d)"

    (
        cd "$GO_DIR"
        run env CGO_ENABLED=0 go test \
            -count=1 \
            -timeout=120s \
            -covermode=count \
            -coverpkg=./... \
            -coverprofile="$tmp_dir/go-coverage.raw.out" \
            ./...
        run go tool cover -func="$tmp_dir/go-coverage.raw.out"
    )

    rewrite_go_cover_paths "$tmp_dir/go-coverage.raw.out" "$OUT_DIR/go-coverage.out"
    rm -rf "$tmp_dir"
}

main() {
    require_tool cmake
    require_tool gcc
    require_tool gcovr
    require_tool go
    require_tool cargo
    require_tool rustup
    require_tool python3

    rm -rf "$OUT_DIR"
    mkdir -p "$OUT_DIR"

    generate_c_coverage
    generate_rust_coverage
    generate_go_coverage

    echo
    echo -e "${GREEN}Codacy coverage reports generated:${NC}"
    run ls -lh "$OUT_DIR"
}

main "$@"
