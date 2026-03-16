#!/usr/bin/env bash
#
# test_service_win_shm_interop.sh - Windows L2 service interop tests over SHM transport.
#
# Runs the same 9-pair cross-language tests as test_service_win_interop.sh,
# but with NIPC_PROFILE=shm so both sides negotiate SHM_HYBRID.

export NIPC_PROFILE=shm
exec "$(dirname "$0")/test_service_win_interop.sh" "$@"
