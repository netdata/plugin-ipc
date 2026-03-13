#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

export NETIPC_SUPPORTED_PROFILES="${NETIPC_SUPPORTED_PROFILES:-2}"
export NETIPC_PREFERRED_PROFILES="${NETIPC_PREFERRED_PROFILES:-2}"

exec "${ROOT_DIR}/tests/run-live-cgroups-bench.sh"
