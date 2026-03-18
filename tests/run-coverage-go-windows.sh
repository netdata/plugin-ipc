#!/bin/bash
# Windows Go coverage measurement for the native win11 environment.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GO_DIR="$ROOT_DIR/src/go"

PACKAGES=(
    "./pkg/netipc/protocol/"
    "./pkg/netipc/service/cgroups/"
    "./pkg/netipc/transport/windows/"
)

THRESHOLD=${1:-80}

run() {
    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q " "$@"
    printf >&2 "${NC}\n"
    if ! "$@"; then
        local exit_code=$?
        echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: $*"
        return $exit_code
    fi
}

echo -e "${CYAN}=== Windows Go Coverage ===${NC}"
echo

if [[ "$(uname -s)" != *MINGW* ]] && [[ "$(uname -s)" != *MSYS* ]] && [[ "${OS:-}" != "Windows_NT" ]]; then
    echo -e "${RED}ERROR:${NC} This script must run on Windows."
    exit 1
fi

export PATH="/c/Program Files/Go/bin:/mingw64/bin:$PATH"

if ! command -v go >/dev/null 2>&1; then
    echo -e "${RED}ERROR:${NC} Go is not available in PATH."
    exit 1
fi

cd "$GO_DIR"

COVERDIR=$(mktemp -d)
trap "rm -rf \"$COVERDIR\"" EXIT

total_covered=0
total_statements=0

for pkg in "${PACKAGES[@]}"; do
    pkg_name=$(echo "$pkg" | sed 's|^\./pkg/netipc/||; s|/$||')
    coverfile="$COVERDIR/${pkg_name//\//_}.out"

    echo -e "${YELLOW}Testing $pkg_name...${NC}"
    if CGO_ENABLED=0 go test -count=1 -timeout=300s \
        -coverprofile="$coverfile" \
        -covermode=count \
        "$pkg" 2>&1; then
        echo -e "  ${GREEN}PASSED${NC}"
    else
        echo -e "  ${RED}FAILED${NC}"
        exit 1
    fi
    echo
done

MERGED="$COVERDIR/merged.out"
echo "mode: count" > "$MERGED"
for f in "$COVERDIR"/*.out; do
    [[ "$f" == "$MERGED" ]] && continue
    [[ -f "$f" ]] || continue
    tail -n +2 "$f" >> "$MERGED"
done

if ! grep -q "transport/windows" "$MERGED"; then
    echo -e "${RED}ERROR:${NC} No Windows transport coverage was collected."
    exit 1
fi

echo
echo -e "${CYAN}=== Per-Function Coverage ===${NC}"
go tool cover -func="$MERGED" 2>&1

echo
echo -e "${CYAN}=== Per-File Coverage Summary ===${NC}"
echo

declare -A file_covered
declare -A file_total

while IFS= read -r line; do
    [[ "$line" == "mode:"* ]] && continue

    file=$(echo "$line" | cut -d: -f1)
    rest=$(echo "$line" | cut -d: -f2)
    stmts=$(echo "$rest" | awk '{print $2}')
    count=$(echo "$rest" | awk '{print $3}')
    short_file=$(echo "$file" | sed 's|.*/pkg/netipc/||')

    file_total[$short_file]=$(( ${file_total[$short_file]:-0} + stmts ))
    if [[ "$count" -gt 0 ]]; then
        file_covered[$short_file]=$(( ${file_covered[$short_file]:-0} + stmts ))
    fi
done < "$MERGED"

printf "%-40s %8s %12s\n" "File" "Coverage" "Stmts"
printf "%-40s %8s %12s\n" "----" "--------" "-----"

for file in $(echo "${!file_total[@]}" | tr ' ' '\n' | sort); do
    cov=${file_covered[$file]:-0}
    tot=${file_total[$file]}
    if [[ $tot -gt 0 ]]; then
        pct=$(awk -v c="$cov" -v t="$tot" 'BEGIN { printf "%.1f", (c/t)*100 }')
    else
        pct="0.0"
    fi

    pct_int=$(echo "$pct" | cut -d. -f1)
    if [[ $pct_int -ge $THRESHOLD ]]; then
        color=$GREEN
    elif [[ $pct_int -ge 70 ]]; then
        color=$YELLOW
    else
        color=$RED
    fi

    printf "%-40s ${color}%6s%%${NC} %6d/%-6d\n" "$file" "$pct" "$cov" "$tot"
    total_covered=$((total_covered + cov))
    total_statements=$((total_statements + tot))
done

echo
if [[ $total_statements -gt 0 ]]; then
    total_pct=$(awk -v c="$total_covered" -v t="$total_statements" 'BEGIN { printf "%.1f", (c/t)*100 }')
    printf "%-40s %6s%% %6d/%-6d\n" "TOTAL" "$total_pct" "$total_covered" "$total_statements"
else
    echo "TOTAL: No coverage data collected"
    exit 1
fi

echo
total_pct_int=$(echo "$total_pct" | cut -d. -f1)
if [[ $total_pct_int -ge $THRESHOLD ]]; then
    echo -e "${GREEN}Windows Go coverage ${total_pct}% meets threshold ${THRESHOLD}%.${NC}"
    exit 0
fi

echo -e "${RED}Windows Go coverage ${total_pct}% is below threshold ${THRESHOLD}%.${NC}"
exit 1
