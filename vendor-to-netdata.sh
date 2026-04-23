#!/usr/bin/env bash
#
# vendor-to-netdata.sh — copy netipc library sources into a Netdata repo.
#
# Usage:
#   ./vendor-to-netdata.sh [netdata-repo-root]   (defaults to .)
#
# Copies C, Rust, and Go sources from this plugin-ipc repo into the
# corresponding paths inside a Netdata checkout. Does NOT touch
# Netdata-specific wrapper files (netipc_netdata.c/h) or build system
# files — those are maintained in the Netdata repo.
#
# All C files are included (including Windows transport). The build
# system decides what to compile per platform.

set -euo pipefail

RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' GRAY='\033[0;90m' NC='\033[0m'
run() {
  printf >&2 "${GRAY}$(pwd) >${NC} ${YELLOW}"; printf >&2 "%q " "$@"; printf >&2 "${NC}\n"
  if ! "$@"; then
    local exit_code=$?
    echo -e >&2 "${RED}[ERROR]${NC} Exit code ${exit_code}: ${YELLOW}$*${NC} (in $(pwd))"
    return $exit_code
  fi
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR/src"

DST="$(cd "${1:-.}" && pwd)"

if [ "$SCRIPT_DIR" = "$DST" ]; then
    echo -e >&2 "${RED}[ERROR]${NC} Source and destination are the same directory: $DST"
    exit 1
fi

if [ ! -d "$DST/src/libnetdata" ]; then
    echo -e >&2 "${RED}[ERROR]${NC} $DST does not look like a Netdata repo (missing src/libnetdata/)"
    exit 1
fi

# ── C headers ─────────────────────────────────────────────────────────
C_HEADERS=(
    netipc_named_pipe.h
    netipc_protocol.h
    netipc_service.h
    netipc_shm.h
    netipc_uds.h
    netipc_win_shm.h
)

# ── C sources ────────────────────────────────────────────────────────
C_SOURCES=(
    src/protocol/netipc_protocol.c
    src/service/netipc_service.c
    src/service/netipc_service_win.c
    src/transport/posix/netipc_shm.c
    src/transport/posix/netipc_uds.c
    src/transport/windows/netipc_named_pipe.c
    src/transport/windows/netipc_win_shm.c
)

# ── Rust crate (entire src/ tree + Cargo files) ──────────────────────
# ── Go package (entire pkg/netipc/ tree + go.mod) ────────────────────

echo -e "${GREEN}Vendoring netipc from${NC} $SRC"
echo -e "${GREEN}                  to${NC} $DST"
echo ""

# ── C ────────────────────────────────────────────────────────────────
echo -e "${YELLOW}=== C headers ===${NC}"
for h in "${C_HEADERS[@]}"; do
    run cp -f "$SRC/libnetdata/netipc/include/netipc/$h" \
              "$DST/src/libnetdata/netipc/include/netipc/$h"
done

echo -e "${YELLOW}=== C sources ===${NC}"
for s in "${C_SOURCES[@]}"; do
    d="$DST/src/libnetdata/netipc/$s"
    run mkdir -p "$(dirname "$d")"
    run cp -f "$SRC/libnetdata/netipc/$s" "$d"
done

# ── Rust ─────────────────────────────────────────────────────────────
# Only the src/ tree is synced. Cargo.toml and Cargo.lock are NOT
# copied because the Netdata repo has its own workspace-level Cargo
# config with different path dependencies.
echo -e "${YELLOW}=== Rust crate ===${NC}"
RUST_SRC="$SRC/crates/netipc"
RUST_DST="$DST/src/crates/netipc"
run mkdir -p "$RUST_DST"

# Format before copying
if command -v cargo >/dev/null 2>&1; then
    echo -e "  ${GRAY}running cargo fmt...${NC}"
    (cd "$RUST_SRC" && cargo fmt --quiet 2>/dev/null) || true
else
    echo -e "  ${YELLOW}cargo not found — skipping Rust formatting${NC}"
fi

# Sync the Rust src/ tree (preserves structure, deletes stale files)
run rsync -a --delete \
    --exclude='target/' \
    "$RUST_SRC/src/" "$RUST_DST/src/"

# ── Go ───────────────────────────────────────────────────────────────
# Only pkg/netipc/ is synced. go.mod is NOT copied because the Netdata
# repo has its own module path and dependencies. After syncing, Go
# import paths (github.com/netdata/plugin-ipc/go/...) must be adjusted
# to match the Netdata module path if they differ.
echo -e "${YELLOW}=== Go package ===${NC}"
GO_SRC="$SRC/go"
GO_DST="$DST/src/go"
run mkdir -p "$GO_DST/pkg/netipc"

# Format before copying
if command -v gofmt >/dev/null 2>&1; then
    echo -e "  ${GRAY}running gofmt...${NC}"
    find "$GO_SRC/pkg/netipc" -name '*.go' -exec gofmt -w {} +
else
    echo -e "  ${YELLOW}gofmt not found — skipping Go formatting${NC}"
fi

# Sync only the netipc package (not the entire pkg/ tree — Netdata
# has many other Go packages under src/go/pkg/ that must not be touched).
run rsync -a --delete \
    "$GO_SRC/pkg/netipc/" "$GO_DST/pkg/netipc/"

# ── Summary ──────────────────────────────────────────────────────────
echo ""
C_COUNT=$(( ${#C_HEADERS[@]} + ${#C_SOURCES[@]} ))
RUST_COUNT=$(find "$RUST_SRC/src" -type f | wc -l)
GO_COUNT=$(find "$GO_SRC/pkg" -type f | wc -l)
echo -e "${GREEN}Done.${NC} Vendored ${C_COUNT} C files, ${RUST_COUNT} Rust files, ${GO_COUNT} Go files."
echo ""

# ── Post-vendor instructions ─────────────────────────────────────────
GO_SRC_MODULE=$(head -1 "$GO_SRC/go.mod" | awk '{print $2}')
GO_DST_MODULE=$(head -1 "$GO_DST/go.mod" 2>/dev/null | awk '{print $2}')

echo -e "${YELLOW}=== Next steps ===${NC}"
echo ""
echo "  1. Fix Go import paths:"
if [ -n "$GO_DST_MODULE" ] && [ "$GO_SRC_MODULE" != "$GO_DST_MODULE" ]; then
    echo -e "     ${GRAY}cd $DST${NC}"
    echo -e "     ${GRAY}find src/go/pkg/netipc -name '*.go' -exec sed -i 's|${GO_SRC_MODULE}|${GO_DST_MODULE}|g' {} +${NC}"
else
    echo -e "     ${GRAY}(check if Go module path differs between repos and sed-replace)${NC}"
fi
echo ""
echo "  2. Verify C build integration:"
echo "     - Ensure CMakeLists.txt includes any new .c files"
echo "     - Netdata wrapper files (netipc_netdata.c/h) are NOT overwritten"
echo ""
echo "  3. Verify Rust build integration:"
echo "     - Ensure Cargo.toml in the Netdata workspace references any new"
echo "       dependencies or features added in the plugin-ipc crate"
echo ""
echo "  4. Build and test:"
echo -e "     ${GRAY}cd $DST && cmake --build build && go test ./src/go/pkg/netipc/...${NC}"
echo ""
