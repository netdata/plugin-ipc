#!/usr/bin/env bash
# Run local validation at low scheduler priority so the desktop stays usable.

set -euo pipefail

netipc_low_priority_prefix() {
    local prefix=()

    if command -v ionice >/dev/null 2>&1 &&
        ionice -c 3 true >/dev/null 2>&1; then
        prefix+=(ionice -c 3)
    fi

    if command -v nice >/dev/null 2>&1; then
        prefix+=(nice -n 19)
    fi

    printf '%s\0' "${prefix[@]}"
}

netipc_low_priority_exec() {
    if [[ "${NIPC_LOW_PRIORITY_DISABLE:-0}" == "1" ||
          "${NIPC_LOW_PRIORITY_ACTIVE:-0}" == "1" ]]; then
        exec "$@"
    fi

    local prefix=()
    local word
    while IFS= read -r -d '' word; do
        prefix+=("$word")
    done < <(netipc_low_priority_prefix)

    export NIPC_LOW_PRIORITY_ACTIVE=1
    if (( ${#prefix[@]} > 0 )); then
        exec "${prefix[@]}" "$@"
    fi
    exec "$@"
}

netipc_low_priority_self() {
    if [[ "${NIPC_LOW_PRIORITY_DISABLE:-0}" == "1" ||
          "${NIPC_LOW_PRIORITY_ACTIVE:-0}" == "1" ]]; then
        return 0
    fi

    local caller="${BASH_SOURCE[1]:-$0}"
    netipc_low_priority_exec bash "$caller" "$@"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    if (( $# == 0 )); then
        echo "usage: $0 <command> [args...]" >&2
        exit 2
    fi
    netipc_low_priority_exec "$@"
fi
