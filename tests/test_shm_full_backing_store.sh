#!/usr/bin/env bash
#
# Verify that every Linux SHM creator returns a normal allocation error instead
# of receiving SIGBUS when its tmpfs backing store cannot hold the region.

set -euo pipefail

# shellcheck disable=SC1091 # Resolved relative to this script at runtime.
source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'

run() {
  printf >&2 '%b%s >%b ' "${GRAY}" "$(pwd)" "${NC}"
  printf >&2 '%b' "${YELLOW}"
  printf >&2 "%q " "$@"
  printf >&2 '%b\n' "${NC}"

  "$@" || {
    local exit_code=$?
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e >&2 "${RED}[ERROR]${NC} Command failed with exit code ${exit_code}: ${YELLOW}$1${NC}"
    echo -e >&2 "${RED}        Full command:${NC} $*"
    echo -e >&2 "${RED}        Working dir:${NC} $(pwd)"
    echo -e >&2 "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    return "$exit_code"
  }
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

: "${INTEROP_SHM_C:=${ROOT_DIR}/build/bin/interop_shm_c}"
: "${INTEROP_SHM_RS:=${ROOT_DIR}/src/crates/netipc/target/debug/interop_shm}"
: "${INTEROP_SHM_GO:=${ROOT_DIR}/build/bin/interop_shm_go}"
: "${NIPC_REQUIRE_TMPFS_TEST:=0}"

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/nipc_shm_full.XXXXXX")"
MOUNT_DIR="${TMP_ROOT}/shm"
LOG_DIR="${TMP_ROOT}/logs"

cleanup() {
  run rm -rf "${TMP_ROOT}" || true
}
trap cleanup EXIT

skip_or_fail() {
  local reason="$1"
  if [[ "${NIPC_REQUIRE_TMPFS_TEST}" == "1" ]]; then
    echo -e >&2 "${RED}[FAIL]${NC} ${reason}"
    exit 1
  fi
  echo -e >&2 "${YELLOW}[SKIP]${NC} ${reason}"
  exit 77
}

for binary in "${INTEROP_SHM_C}" "${INTEROP_SHM_RS}" "${INTEROP_SHM_GO}"; do
  if [[ ! -x "${binary}" ]]; then
    echo -e >&2 "${RED}[FAIL]${NC} Missing SHM creator binary: ${binary}"
    exit 1
  fi
done

if ! command -v unshare >/dev/null 2>&1; then
  skip_or_fail "unshare is required for the private tmpfs regression test"
fi
if ! command -v mount >/dev/null 2>&1; then
  skip_or_fail "mount is required for the private tmpfs regression test"
fi
if ! command -v timeout >/dev/null 2>&1; then
  skip_or_fail "timeout is required to bound SHM creator subprocesses"
fi

run mkdir -p "${MOUNT_DIR}" "${LOG_DIR}"

NAMESPACE_CMD=()

if [[ ${EUID} -eq 0 ]]; then
  printf >&2 "${GRAY}$(pwd) >${NC} ${YELLOW}%q %q %q %q %q %q %q${NC}\n" \
    unshare -m sh -eu -c '<privileged private tmpfs capability probe>' "${MOUNT_DIR}"
  # shellcheck disable=SC2016 # The script is evaluated inside the new namespace.
  if unshare -m sh -eu -c '
    mount --make-rprivate /
    mount -t tmpfs -o size=64k,nr_inodes=128,mode=0700 tmpfs "$1"
    umount "$1"
  ' sh "${MOUNT_DIR}" >/dev/null 2>&1; then
    NAMESPACE_CMD=(unshare -m)
  fi
else
  printf >&2 "${GRAY}$(pwd) >${NC} ${YELLOW}%q %q %q %q %q %q %q${NC}\n" \
    unshare -Urnm sh -eu -c '<unprivileged private tmpfs capability probe>' "${MOUNT_DIR}"
  # shellcheck disable=SC2016 # The script is evaluated inside the new namespace.
  if unshare -Urnm sh -eu -c '
    mount --make-rprivate /
    mount -t tmpfs -o size=64k,nr_inodes=128,mode=0700 tmpfs "$1"
    umount "$1"
  ' sh "${MOUNT_DIR}" >/dev/null 2>&1; then
    NAMESPACE_CMD=(unshare -Urnm)
  fi
fi

if [[ ${#NAMESPACE_CMD[@]} -eq 0 ]]; then
  if [[ ${EUID} -eq 0 ]]; then
    skip_or_fail "a privileged mount namespace cannot mount a private tmpfs"
  fi
  skip_or_fail "unprivileged user/mount namespaces cannot mount a private tmpfs"
fi

# shellcheck disable=SC2016 # The script is evaluated inside the new namespace.
run "${NAMESPACE_CMD[@]}" bash -euo pipefail -c '
  mount_dir=$1
  log_dir=$2
  shift 2

  trace() {
    printf >&2 "%s > " "$PWD"
    printf >&2 "%q " "$@"
    printf >&2 "\n"
  }

  trace mount --make-rprivate /
  mount --make-rprivate /
  trace mount -t tmpfs -o size=64k,nr_inodes=128,mode=0700 tmpfs "$mount_dir"
  mount -t tmpfs -o size=64k,nr_inodes=128,mode=0700 tmpfs "$mount_dir"
  trap '\''trace umount "$mount_dir"; umount "$mount_dir" 2>/dev/null || true'\'' EXIT

  index=0
  while [[ $# -gt 0 ]]; do
    language=$1
    binary=$2
    shift 2
    index=$((index + 1))
    service="shm_full_${language}_${index}"
    log="${log_dir}/${language}.log"

    set +e
    trace timeout 10 "$binary" server "$mount_dir" "$service"
    timeout 10 "$binary" server "$mount_dir" "$service" >"$log" 2>&1
    rc=$?
    set -e

    if [[ $rc -ne 1 ]]; then
      echo "${language}: creator returned unexpected status ${rc}; expected allocation failure status 1" >&2
      cat "$log" >&2
      exit 1
    fi

    if ! grep -Fxq NIPC_SHM_ALLOCATE_ENOSPC "$log"; then
      echo "${language}: creator did not report the expected ENOSPC allocation-stage failure" >&2
      cat "$log" >&2
      exit 1
    fi
    if grep -Fxq READY "$log"; then
      echo "${language}: creator published readiness after allocation failure" >&2
      cat "$log" >&2
      exit 1
    fi
    trace find "$mount_dir" -mindepth 1 -print -quit
    if find "$mount_dir" -mindepth 1 -print -quit | grep -q .; then
      echo "${language}: failed creation leaked a backing-store entry" >&2
      find "$mount_dir" -mindepth 1 -maxdepth 1 -printf "%f\n" >&2
      exit 1
    fi
  done
' bash "${MOUNT_DIR}" "${LOG_DIR}" \
  c "${INTEROP_SHM_C}" \
  rust "${INTEROP_SHM_RS}" \
  go "${INTEROP_SHM_GO}"

echo -e "${GREEN}[PASS]${NC} C, Rust, and Go returned normal errors on a full tmpfs; no SIGBUS or leaked region"
