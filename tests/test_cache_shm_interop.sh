#!/usr/bin/env bash
#
# test_cache_shm_interop.sh - L3 cache interop tests over SHM transport.
#
# Runs the same 9-pair cross-language tests as test_cache_interop.sh,
# but with NIPC_PROFILE=shm so both sides negotiate SHM_HYBRID.

export NIPC_PROFILE=shm
exec "$(dirname "$0")/test_cache_interop.sh" "$@"
