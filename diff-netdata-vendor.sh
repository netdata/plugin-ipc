#!/usr/bin/env bash
#
# diff-netdata-vendor.sh — compare upstream plugin-ipc against vendored netipc
# trees inside a Netdata checkout.
#
# Usage:
#   ./diff-netdata-vendor.sh [netdata-repo-root] [--unified]
#
# Default Netdata repo:
#   ~/src/netdata-ktsaou.git
#
# The script separates expected Netdata-local differences from real vendored
# library drift:
# - C wrappers: netipc_netdata.c/h
# - Rust workspace/package files: Cargo.toml, Cargo.lock
# - Go import-path rewrites from the upstream module path to the Netdata module
#   path

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
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
    echo -e >&2 "${RED}        Full command:${NC} $*"
    echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    return $exit_code
  fi
}

usage() {
  cat <<'EOF'
Usage:
  ./diff-netdata-vendor.sh [netdata-repo-root] [--unified]

Examples:
  ./diff-netdata-vendor.sh
  ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git
  ./diff-netdata-vendor.sh ~/src/netdata-ktsaou.git --unified
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPSTREAM_ROOT="$SCRIPT_DIR"
NETDATA_ROOT="${HOME}/src/netdata-ktsaou.git"
UNIFIED=0

for arg in "$@"; do
  case "$arg" in
    --unified)
      UNIFIED=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      NETDATA_ROOT="$arg"
      ;;
  esac
done

NETDATA_ROOT="$(cd "$NETDATA_ROOT" && pwd)"

if [ ! -d "$UPSTREAM_ROOT/src/libnetdata/netipc" ]; then
  echo -e "${RED}[ERROR]${NC} $UPSTREAM_ROOT does not look like plugin-ipc"
  exit 1
fi

if [ ! -d "$NETDATA_ROOT/src/libnetdata/netipc" ]; then
  echo -e "${RED}[ERROR]${NC} $NETDATA_ROOT does not look like a Netdata repo"
  exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

UP_C="$TMPDIR/upstream-c"
ND_C="$TMPDIR/netdata-c"
UP_RUST="$TMPDIR/upstream-rust"
ND_RUST="$TMPDIR/netdata-rust"
UP_GO="$TMPDIR/upstream-go"
ND_GO="$TMPDIR/netdata-go"

mkdir -p "$UP_C" "$ND_C" "$UP_RUST" "$ND_RUST" "$UP_GO" "$ND_GO"

echo -e "${GREEN}Upstream:${NC} $UPSTREAM_ROOT"
echo -e "${GREEN}Netdata :${NC} $NETDATA_ROOT"
echo ""

echo -e "${YELLOW}=== Preparing normalized trees ===${NC}"

run rsync -a \
  --exclude='netipc_netdata.c' \
  --exclude='netipc_netdata.h' \
  "$UPSTREAM_ROOT/src/libnetdata/netipc/" "$UP_C/"
run rsync -a \
  --exclude='netipc_netdata.c' \
  --exclude='netipc_netdata.h' \
  "$NETDATA_ROOT/src/libnetdata/netipc/" "$ND_C/"

run rsync -a \
  --exclude='Cargo.lock' \
  --exclude='Cargo.toml' \
  --exclude='target/' \
  --exclude='Testing/' \
  "$UPSTREAM_ROOT/src/crates/netipc/" "$UP_RUST/"
run rsync -a \
  --exclude='Cargo.lock' \
  --exclude='Cargo.toml' \
  --exclude='target/' \
  --exclude='Testing/' \
  "$NETDATA_ROOT/src/crates/netipc/" "$ND_RUST/"

run rsync -a "$UPSTREAM_ROOT/src/go/pkg/netipc/" "$UP_GO/"
run rsync -a "$NETDATA_ROOT/src/go/pkg/netipc/" "$ND_GO/"

UPSTREAM_GO_IMPORT='github.com/netdata/plugin-ipc/go/pkg/netipc'
NETDATA_GO_IMPORT='github.com/netdata/netdata/go/plugins/pkg/netipc'

while IFS= read -r -d '' file; do
  run sed -i "s|${NETDATA_GO_IMPORT}|${UPSTREAM_GO_IMPORT}|g" "$file"
done < <(find "$ND_GO" -type f -name '*.go' -print0)

show_diff_section() {
  local title="$1"
  local left="$2"
  local right="$3"

  echo -e "${YELLOW}=== ${title} ===${NC}"

  if diff -rq "$left" "$right" >/dev/null; then
    echo "No differences."
    echo ""
    return 0
  fi

  if [ "$UNIFIED" -eq 1 ]; then
    diff -ru "$left" "$right" || true
  else
    diff -rq "$left" "$right" || true
  fi
  echo ""
}

echo -e "${YELLOW}=== Expected Netdata-local differences ===${NC}"
echo "C wrappers present only in Netdata:"
echo "  - src/libnetdata/netipc/netipc_netdata.c"
echo "  - src/libnetdata/netipc/netipc_netdata.h"
echo "Rust packaging/workspace files not compared for source drift:"
echo "  - src/crates/netipc/Cargo.toml"
echo "  - src/crates/netipc/Cargo.lock"
echo "Go import paths normalized before substantive comparison:"
echo "  - ${NETDATA_GO_IMPORT}"
echo "  - ${UPSTREAM_GO_IMPORT}"
echo ""

show_diff_section "C vendored library diff (wrappers excluded)" "$UP_C" "$ND_C"
show_diff_section "Rust vendored source diff (workspace files excluded)" "$UP_RUST" "$ND_RUST"
show_diff_section "Go vendored source diff (import paths normalized)" "$UP_GO" "$ND_GO"
