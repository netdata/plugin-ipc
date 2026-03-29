#!/bin/bash
# Native Windows Application Verifier + PageHeap validation for the core
# Windows C runtime executables.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${NETIPC_BUILD_DIR:-$ROOT_DIR/build}"
BIN_DIR="$BUILD_DIR/bin"
LOG_DIR="${NETIPC_VERIFIER_LOG_DIR:-$BUILD_DIR/verifier-logs}"
VERIFIER_LAYERS="${NETIPC_VERIFIER_LAYERS:-Handles Heaps Locks}"
TIMEOUT_SECONDS="${NETIPC_VERIFIER_TIMEOUT:-300}"
read -r -a VERIFIER_LAYER_ARGS <<<"$VERIFIER_LAYERS"

TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(
        test_named_pipe.exe
        test_win_shm.exe
        test_win_service.exe
        test_win_service_extra.exe
    )
fi

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
        return $exit_code
    fi
}

cleanup_target() {
    local target=$1
    set +e
    "$APPVERIF_BIN" -delete logs -for "$target" >/dev/null 2>&1
    "$APPVERIF_BIN" -delete settings -for "$target" >/dev/null 2>&1
    "$GFLAGS_BIN" /p /disable "$target" >/dev/null 2>&1
    set -e
}

export_log() {
    local target=$1
    local xml_out=$2
    set +e
    local output
    output=$("$APPVERIF_BIN" -export log -for "$target" -with "To=$xml_out" 2>&1)
    local exit_code=$?
    set -e

    if [[ $exit_code -ne 0 ]]; then
        echo "$output"
        return $exit_code
    fi

    echo "$output"
    if grep -qi "there is no valid log file" <<<"$output"; then
        return 2
    fi

    return 0
}

if [[ "$(uname -s)" != *MINGW* ]] && [[ "$(uname -s)" != *MSYS* ]] && [[ "${OS:-}" != "Windows_NT" ]]; then
    echo -e "${RED}ERROR:${NC} This script must run on native Windows."
    exit 1
fi

export PATH="/c/Users/costa/.cargo/bin:/c/Program Files/Go/bin:/mingw64/bin:$PATH"
export MSYSTEM=MINGW64
export TMP=/tmp
export TEMP=/tmp
export TMPDIR=/tmp

APPVERIF_BIN=""
for candidate in /c/WINDOWS/system32/appverif.exe "$(command -v appverif.exe 2>/dev/null || true)" "$(command -v appverif 2>/dev/null || true)"; do
    if [[ -n "$candidate" ]] && [[ -x "$candidate" ]]; then
        APPVERIF_BIN="$candidate"
        break
    fi
done
if [[ -z "$APPVERIF_BIN" ]]; then
    echo -e "${RED}ERROR:${NC} appverif.exe not found."
    exit 1
fi

GFLAGS_BIN=""
for candidate in \
    "$(command -v gflags.exe 2>/dev/null || true)" \
    "$(command -v gflags 2>/dev/null || true)" \
    "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/gflags.exe" \
    "/c/Program Files (x86)/Windows Kits/10/Debuggers/x86/gflags.exe"; do
    if [[ -n "$candidate" ]] && [[ -x "$candidate" ]]; then
        GFLAGS_BIN="$candidate"
        break
    fi
done
if [[ -z "$GFLAGS_BIN" ]]; then
    echo -e "${RED}ERROR:${NC} gflags.exe not found."
    exit 1
fi

for tool in timeout cygpath; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo -e "${RED}ERROR:${NC} Required tool not found: $tool"
        exit 1
    fi
done

if [[ ! -d "$BIN_DIR" ]]; then
    echo -e "${RED}ERROR:${NC} Build directory not found: $BIN_DIR"
    exit 1
fi

run rm -rf "$LOG_DIR"
run mkdir -p "$LOG_DIR"

echo -e "${CYAN}=== Windows Application Verifier + PageHeap ===${NC}"
echo "Build dir: $BUILD_DIR"
echo "Log dir:   $LOG_DIR"
echo "Layers:    $VERIFIER_LAYERS"
echo "Timeout:   ${TIMEOUT_SECONDS}s"
echo

all_pass=true
for target in "${TARGETS[@]}"; do
    exe_path="$BIN_DIR/$target"
    if [[ ! -f "$exe_path" ]]; then
        echo -e "${RED}ERROR:${NC} Missing executable: $exe_path"
        all_pass=false
        continue
    fi

    xml_out="$LOG_DIR/${target%.exe}.xml"
    stdout_log="$LOG_DIR/${target%.exe}.stdout.log"
    stderr_log="$LOG_DIR/${target%.exe}.stderr.log"

    echo -e "${YELLOW}--- Validating $target ---${NC}"
    cleanup_target "$target"

    run "$APPVERIF_BIN" -enable "${VERIFIER_LAYER_ARGS[@]}" -for "$target"
    run "$GFLAGS_BIN" /p /enable "$target" /full

    printf >&2 "${GRAY}$(pwd) >${NC} "
    printf >&2 "${YELLOW}"
    printf >&2 "%q %q %q " timeout "$TIMEOUT_SECONDS" "$exe_path"
    printf >&2 "${NC}\n"
    set +e
    timeout "$TIMEOUT_SECONDS" "$exe_path" >"$stdout_log" 2>"$stderr_log"
    exit_code=$?
    set -e

    verifier_result=0
    verifier_output=$(export_log "$target" "$xml_out") || verifier_result=$?

    if [[ $verifier_result -eq 2 ]]; then
        verifier_clean=true
    elif [[ $verifier_result -eq 0 ]] && [[ -f "$xml_out" ]] && ! grep -Eq "<[^>]*logEntry" "$xml_out"; then
        verifier_clean=true
    else
        verifier_clean=false
    fi

    if [[ $exit_code -ne 0 ]]; then
        echo -e "${RED}Executable failed:${NC} $target (exit $exit_code)"
        cat "$stdout_log" || true
        cat "$stderr_log" || true
        all_pass=false
    fi

    if ! $verifier_clean; then
        echo -e "${RED}Verifier findings recorded for:${NC} $target"
        [[ -f "$xml_out" ]] && sed -n '1,120p' "$xml_out" || true
        all_pass=false
    else
        echo -e "${GREEN}No verifier findings for:${NC} $target"
    fi

    cleanup_target "$target"
    echo
done

if ! $all_pass; then
    echo -e "${RED}Windows verifier validation failed.${NC}"
    exit 1
fi

echo -e "${GREEN}Windows verifier validation passed for all targets.${NC}"
