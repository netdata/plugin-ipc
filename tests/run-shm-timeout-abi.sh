#!/usr/bin/env bash
# Compile just the production SHM transport and its public wait regression.
# Pass the compiler and its flags as arguments; NIPC_TEST_RUNNER may name QEMU.
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/nipc-timeout-abi.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
if (( $# == 0 )); then
    set -- cc
fi
"$@" -O2 -Wall -Wextra -Werror \
    -I "$root/src/libnetdata/netipc/include" \
    "$root/tests/fixtures/c/test_shm_timeout.c" \
    "$root/src/libnetdata/netipc/src/transport/posix/netipc_shm.c" \
    -o "$build_dir/test_shm_timeout"
if [[ -n "${NIPC_TEST_RUNNER:-}" ]]; then
    "$NIPC_TEST_RUNNER" "$build_dir/test_shm_timeout"
else
    "$build_dir/test_shm_timeout"
fi
