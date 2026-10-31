#!/usr/bin/env bash
#
# interop-lib.sh - shared helpers for the cross-language interop scripts.
#
# Fixture servers print READY only once their listener accepts connections
# (or, for L1 SHM, once the region exists), so a script blocks on that line
# instead of polling for it.

# interop_start_server TIMEOUT_S LOG CMD [ARGS...]
#
# Start CMD in the background and return once it printed READY. The server's
# output goes to LOG. Sets INTEROP_SERVER_PID. Returns 1 when the server
# exited, or printed nothing for TIMEOUT_S seconds, before READY.
interop_start_server() {
    local timeout_s="$1"
    local log="$2"
    shift 2
    local fd line

    : > "$log"
    INTEROP_SERVER_EXE="$(interop_command_binary "$@")"
    exec {fd}< <(exec "$@" 2>&1)
    INTEROP_SERVER_PID=$!
    INTEROP_DRAIN_PID=""
    while IFS= read -r -t "$timeout_s" -u "$fd" line; do
        printf '%s\n' "$line" >> "$log"
        if [[ "$line" == "READY" ]]; then
            # Keep draining so later server output never blocks on the pipe.
            cat <&"$fd" >> "$log" &
            INTEROP_DRAIN_PID=$!
            exec {fd}<&-
            return 0
        fi
    done
    exec {fd}<&-
    return 1
}

# interop_command_binary CMD [ARGS...]
#
# Print the basename of the program CMD runs, skipping an env prefix.
interop_command_binary() {
    local arg
    local skip_env=1
    for arg in "$@"; do
        if [[ $skip_env -eq 1 && ( "$arg" == env || "$arg" == *=* ) ]]; then
            continue
        fi
        skip_env=0
        printf '%s\n' "${arg##*/}"
        return
    done
}

# interop_pid_runs PID BINARY
#
# Report whether PID still runs BINARY. A server that exited may have been
# reaped already and its PID reused, so a kill must check identity first.
# Under a binfmt interpreter (QEMU for the cross targets) /proc/PID/exe names
# the interpreter, and the program path is argv[1]. Systems without a Linux
# style /proc (FreeBSD, macOS) report the command line through ps.
interop_pid_runs() {
    local pid="$1"
    local binary="${2%.exe}"
    local exe=""
    local -a argv=()
    [[ -n "$binary" ]] || return 1
    if [[ ! -d "/proc/$pid" ]]; then
        read -r exe _ < <(ps -o args= -p "$pid" 2>/dev/null) || exe=""
    elif [[ -r "/proc/$pid/exename" ]]; then
        exe="$(cat "/proc/$pid/exename" 2>/dev/null)" || exe=""
    else
        exe="$(readlink "/proc/$pid/exe" 2>/dev/null)" || exe=""
        mapfile -d '' -n 2 argv < "/proc/$pid/cmdline" 2>/dev/null || argv=()
    fi
    exe="${exe##*/}"
    [[ "${exe%.exe}" == "$binary" ]] && return 0
    [[ ${#argv[@]} -ge 2 && "${argv[1]##*/}" == "$binary" ]]
}

# interop_kill PID BINARY
#
# Kill PID at once if it still runs BINARY. On MSYS2 a Windows process blocked
# in a Win32 wait never acts on a POSIX signal, so terminate the Windows
# process instead.
interop_kill() {
    local pid="$1"
    local binary="$2"
    local win_pid=""
    interop_pid_runs "$pid" "$binary" || return 0
    if [[ -r "/proc/$pid/winpid" ]]; then
        win_pid="$(cat "/proc/$pid/winpid" 2>/dev/null)" || win_pid=""
    fi
    if [[ -n "$win_pid" ]]; then
        "$(interop_taskkill_bin)" //PID "$win_pid" //F >/dev/null 2>&1 || true
    else
        kill "$pid" 2>/dev/null || true
    fi
}

# taskkill lives in System32, which a minimal PATH may lack.
interop_taskkill_bin() {
    if command -v taskkill.exe >/dev/null 2>&1; then
        command -v taskkill.exe
    else
        cygpath -u "${SYSTEMROOT:-C:\\Windows}\\System32\\taskkill.exe"
    fi
}

# interop_stop_server
#
# Kill the server started by interop_start_server and reap it and its
# output drain.
interop_stop_server() {
    if [[ -n "${INTEROP_SERVER_PID:-}" ]]; then
        interop_kill "$INTEROP_SERVER_PID" "${INTEROP_SERVER_EXE:-}"
        wait "$INTEROP_SERVER_PID" 2>/dev/null || true
    fi
    interop_wait_server
}

# interop_wait_server
#
# Wait for a server that exits on its own, then for its output drain.
interop_wait_server() {
    if [[ -n "${INTEROP_SERVER_PID:-}" ]]; then
        wait "$INTEROP_SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "${INTEROP_DRAIN_PID:-}" ]]; then
        wait "$INTEROP_DRAIN_PID" 2>/dev/null || true
    fi
    INTEROP_SERVER_PID=""
    INTEROP_DRAIN_PID=""
    INTEROP_SERVER_EXE=""
}
