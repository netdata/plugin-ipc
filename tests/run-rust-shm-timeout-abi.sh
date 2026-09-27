#!/usr/bin/env bash
# Requires rustup target add arm-unknown-linux-musleabihf and qemu-arm.
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
rust_host=$(rustc -vV | sed -n 's/^host: //p')
rust_sysroot=$(rustc --print sysroot)
export CARGO_TARGET_ARM_UNKNOWN_LINUX_MUSLEABIHF_LINKER="$rust_sysroot/lib/rustlib/$rust_host/bin/rust-lld"
export CARGO_TARGET_ARM_UNKNOWN_LINUX_MUSLEABIHF_RUSTFLAGS='-C linker-flavor=ld.lld'
export CARGO_TARGET_ARM_UNKNOWN_LINUX_MUSLEABIHF_RUNNER='timeout 25 qemu-arm'
for time64 in 0 1; do
    RUST_LIBC_UNSTABLE_MUSL_V1_2_3=$time64 cargo test \
        --manifest-path "$root/src/crates/netipc/Cargo.toml" \
        --target arm-unknown-linux-musleabihf --test shm_timeout -- --nocapture
done
