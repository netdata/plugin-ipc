#!/usr/bin/env bash
#
# run-linux-target.sh - Build and run the C, Rust and Go suites plus the
# cross-language interop tests for one Netdata-supported Linux target.
#
# Usage: tests/run-linux-target.sh TARGET [c] [rust] [go] [interop]
#
# TARGET is one of:
#   x86_64-musl  i386-gnu  i386-musl  armv7-gnu  armv7-gnu-time64
#   armv7-musl  armv6-musl  aarch64-gnu  aarch64-musl
# (x86_64 glibc is the host build covered by the regular CTest jobs.)
# 32-bit glibc C builds use the legacy 32-bit time_t/off_t layout, as Netdata
# builds without _TIME_BITS/_FILE_OFFSET_BITS on Debian 12; armv7-gnu-time64
# is the layout newer distributions' compilers default to.
# Without parts, all four run. Every part runs even if an earlier one fails;
# the exit status is non-zero if any part failed.
#
# Requirements:
#   - glibc cross compilers: i686-linux-gnu-gcc, arm-linux-gnueabihf-gcc,
#     aarch64-linux-gnu-gcc (a versioned name such as -gcc-14 is also found);
#   - zig on PATH for musl C builds;
#   - rustup targets for the selected target; Go; cmake and ninja;
#   - QEMU user-mode binfmt handlers for foreign architectures. binfmt is
#     required rather than an explicit runner because tests re-exec their own
#     binaries and interop scripts start foreign servers and clients.
#
# All binaries are linked statically, so no target sysroot is needed at run
# time. QEMU emulates the baseline CPU of each target (arm1176, cortex-a7,
# cortex-a53) rather than its default "max" model, so an instruction newer
# than the target baseline traps.
#
# Environment:
#   NIPC_TARGET_BUILD_ROOT  build directory root (default: build-targets/)
#   NIPC_TARGET_CC          override the C compiler for the target
#   NIPC_TARGET_RUSTUP_ADD  set to 1 to run `rustup target add` (CI)

set -uo pipefail

# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/run-low-priority.sh"
netipc_low_priority_self "$@"

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

usage() {
    sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

(( $# >= 1 )) || usage
TARGET=$1
shift
PARTS=("$@")
(( ${#PARTS[@]} > 0 )) || PARTS=(c rust go interop)

LIBC=musl
[[ "$TARGET" == *-gnu* ]] && LIBC=gnu
C_LAYOUT_FLAGS=
case "$TARGET" in
    i386-gnu|armv7-gnu) C_LAYOUT_FLAGS="-U_TIME_BITS -U_FILE_OFFSET_BITS" ;;
    armv7-gnu-time64) C_LAYOUT_FLAGS="-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64" ;;
esac
case "$TARGET" in
    x86_64-musl)
        PROC=x86_64; RUST_TARGET=x86_64-unknown-linux-musl
        ZIG_TARGET=x86_64-linux-musl; ZIG_CPU=; GNU_PREFIX=
        GOARCH_VAL=amd64; GOARM_VAL=; BINFMT= ;;
    i386-gnu|i386-musl)
        PROC=i686; RUST_TARGET=i686-unknown-linux-$LIBC
        ZIG_TARGET=x86-linux-musl; ZIG_CPU=i686; GNU_PREFIX=i686-linux-gnu
        GOARCH_VAL=386; GOARM_VAL=; BINFMT= ;;
    armv7-gnu|armv7-gnu-time64)
        PROC=arm; RUST_TARGET=armv7-unknown-linux-gnueabihf
        ZIG_TARGET=; ZIG_CPU=; GNU_PREFIX=arm-linux-gnueabihf
        GOARCH_VAL=arm; GOARM_VAL=7; BINFMT=qemu-arm
        export QEMU_CPU=cortex-a7 ;;
    armv7-musl)
        PROC=arm; RUST_TARGET=armv7-unknown-linux-musleabihf
        ZIG_TARGET=arm-linux-musleabihf; ZIG_CPU=cortex_a7; GNU_PREFIX=arm-linux-gnueabihf
        GOARCH_VAL=arm; GOARM_VAL=7; BINFMT=qemu-arm
        export QEMU_CPU=cortex-a7 ;;
    armv6-musl)
        PROC=arm; RUST_TARGET=arm-unknown-linux-musleabihf
        ZIG_TARGET=arm-linux-musleabihf; ZIG_CPU=arm1176jzf_s; GNU_PREFIX=arm-linux-gnueabihf
        GOARCH_VAL=arm; GOARM_VAL=6; BINFMT=qemu-arm
        export QEMU_CPU=arm1176 ;;
    aarch64-gnu|aarch64-musl)
        PROC=aarch64; RUST_TARGET=aarch64-unknown-linux-$LIBC
        ZIG_TARGET=aarch64-linux-musl; ZIG_CPU=; GNU_PREFIX=aarch64-linux-gnu
        GOARCH_VAL=arm64; GOARM_VAL=; BINFMT=qemu-aarch64
        export QEMU_CPU=cortex-a53 ;;
    *)
        usage ;;
esac

BUILD_ROOT=${NIPC_TARGET_BUILD_ROOT:-$ROOT_DIR/build-targets}
OUT=$BUILD_ROOT/$TARGET
mkdir -p "$OUT/bin" || exit 1
OUT=$(cd "$OUT" && pwd)

FAILED=()
fail() { FAILED+=("$1"); printf '\n*** FAILED: %s (%s)\n' "$1" "$TARGET" >&2; }
section() { printf '\n===== %s: %s =====\n' "$TARGET" "$1"; }

find_gnu_tool() {
    local tool=$1 candidate
    for candidate in "$GNU_PREFIX-$tool" \
        $(compgen -c "$GNU_PREFIX-$tool-" | grep -E -- "-$tool-[0-9]+$" | sort -rV); do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    echo "missing $GNU_PREFIX-$tool" >&2
    return 1
}

if [[ -n "$BINFMT" && ! -e "/proc/sys/fs/binfmt_misc/$BINFMT" ]]; then
    echo "missing binfmt handler $BINFMT (install qemu-user-binfmt)" >&2
    exit 1
fi

# Target C compiler: glibc cross gcc, or zig cc for musl.
if [[ -n "${NIPC_TARGET_CC:-}" ]]; then
    TARGET_CC=$NIPC_TARGET_CC
elif [[ "$LIBC" == gnu ]]; then
    TARGET_CC=$(find_gnu_tool gcc) || exit 1
else
    command -v zig >/dev/null 2>&1 || { echo "zig is required for musl C builds" >&2; exit 1; }
    TARGET_CC=$OUT/zig-cc
    {
        echo '#!/bin/sh'
        echo "exec zig cc -target $ZIG_TARGET${ZIG_CPU:+ -mcpu=$ZIG_CPU} \"\$@\""
    } > "$TARGET_CC"
    chmod +x "$TARGET_CC"
fi

# Rust links musl targets with Rust's self-contained musl; any gcc for the
# architecture works as the linker driver.
RUST_LINKER=
if [[ -n "$GNU_PREFIX" ]]; then
    RUST_LINKER=$(find_gnu_tool gcc) || exit 1
fi
if [[ "${NIPC_TARGET_RUSTUP_ADD:-0}" == 1 ]]; then
    rustup target add "$RUST_TARGET" || exit 1
fi
RUST_TARGET_ENV=$(echo "$RUST_TARGET" | tr 'a-z-' 'A-Z_')
export CARGO_TARGET_DIR=$OUT/cargo
if [[ -n "$RUST_LINKER" ]]; then
    export "CARGO_TARGET_${RUST_TARGET_ENV}_LINKER=$RUST_LINKER"
fi
export "CARGO_TARGET_${RUST_TARGET_ENV}_RUSTFLAGS=-C target-feature=+crt-static"

run_c() {
    section "C build"
    local b=$OUT/c
    local emulator=()
    if [[ -n "$BINFMT" ]]; then
        emulator=("-DCMAKE_CROSSCOMPILING_EMULATOR=$BINFMT")
    fi
    if ! cmake -S "$ROOT_DIR" -B "$b" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$PROC" \
        -DCMAKE_C_COMPILER="$TARGET_CC" \
        "-DCMAKE_C_FLAGS=$C_LAYOUT_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS=-static \
        -DCARGO_EXECUTABLE=CARGO_EXECUTABLE-NOTFOUND \
        -DGO_EXECUTABLE=GO_EXECUTABLE-NOTFOUND \
        "${emulator[@]}" >/dev/null ||
       ! cmake --build "$b"; then
        fail "C build"
        return
    fi
    section "C tests"
    ctest --test-dir "$b" --output-on-failure --timeout 300 \
        -E '^(test_windows_bench_stability_policy|fuzz_.*)$' || fail "C tests"
}

run_rust() {
    section "Rust build"
    local manifest=$ROOT_DIR/src/crates/netipc/Cargo.toml
    if ! cargo build --manifest-path "$manifest" --target "$RUST_TARGET" \
            --bin interop_uds --bin interop_shm --bin interop_service --bin interop_cache ||
       ! cargo test --manifest-path "$manifest" --target "$RUST_TARGET" --lib --no-run; then
        fail "Rust build"
        return
    fi
    section "Rust tests"
    cargo test --manifest-path "$manifest" --target "$RUST_TARGET" --lib || fail "Rust tests"
}

run_go() {
    # Pure Go does not link libc: run it once per architecture, on the gnu
    # target, except ARMv6, which only exists as a musl target.
    if [[ "$LIBC" == musl && "$TARGET" != armv6-musl ]]; then
        section "Go (skipped: pure Go does not link libc; covered by the gnu target)"
        return
    fi
    section "Go tests"
    (
        cd "$ROOT_DIR/src/go" &&
        CGO_ENABLED=0 GOARCH=$GOARCH_VAL GOARM=$GOARM_VAL go test ./...
    ) || fail "Go tests"
}

run_interop() {
    section "Interop build"
    local rs=$CARGO_TARGET_DIR/$RUST_TARGET/debug name
    if [[ ! -x "$OUT/c/bin/interop_uds_c" ]]; then
        cmake --build "$OUT/c" --target interop_uds_c interop_shm_c \
            interop_service_c interop_cache_c 2>/dev/null ||
            { fail "Interop build (run the c part first)"; return; }
    fi
    if [[ ! -x "$rs/interop_uds" ]]; then
        cargo build --manifest-path "$ROOT_DIR/src/crates/netipc/Cargo.toml" \
            --target "$RUST_TARGET" --bin interop_uds --bin interop_shm \
            --bin interop_service --bin interop_cache ||
            { fail "Interop build (Rust)"; return; }
    fi
    for name in interop_uds interop_shm interop_service interop_cache; do
        (
            cd "$ROOT_DIR/tests/fixtures/go" &&
            CGO_ENABLED=0 GOARCH=$GOARCH_VAL GOARM=$GOARM_VAL \
                go build -o "$OUT/bin/${name}_go" "./cmd/$name"
        ) || { fail "Interop build (Go $name)"; return; }
    done

    local c=$OUT/c/bin g=$OUT/bin script
    export INTEROP_UDS_C=$c/interop_uds_c INTEROP_UDS_RS=$rs/interop_uds INTEROP_UDS_GO=$g/interop_uds_go
    export INTEROP_SHM_C=$c/interop_shm_c INTEROP_SHM_RS=$rs/interop_shm INTEROP_SHM_GO=$g/interop_shm_go
    export INTEROP_SVC_C=$c/interop_service_c INTEROP_SVC_RS=$rs/interop_service INTEROP_SVC_GO=$g/interop_service_go
    export INTEROP_CACHE_C=$c/interop_cache_c INTEROP_CACHE_RS=$rs/interop_cache INTEROP_CACHE_GO=$g/interop_cache_go
    for script in test_uds_interop test_shm_interop test_service_interop \
                  test_service_shm_interop test_cache_interop test_cache_shm_interop; do
        section "Interop $script"
        bash "$ROOT_DIR/tests/$script.sh" || fail "Interop $script"
    done
}

for part in "${PARTS[@]}"; do
    case "$part" in
        c) run_c ;;
        rust) run_rust ;;
        go) run_go ;;
        interop) run_interop ;;
        *) usage ;;
    esac
done

printf '\n===== %s summary =====\n' "$TARGET"
if (( ${#FAILED[@]} > 0 )); then
    printf 'FAILED: %s\n' "${FAILED[@]}"
    exit 1
fi
echo "all parts passed"
