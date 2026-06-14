#!/usr/bin/env bash
#
# Windows lookup-scale interop tests over SHM transport.

export NIPC_PROFILE=shm
exec "$(dirname "$0")/test_lookup_scale_win_interop.sh" "$@"
