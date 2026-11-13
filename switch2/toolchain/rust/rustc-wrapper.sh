#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Meson's Rust compiler for the Horizon cross build.
# It runs rustc for aarch64-switch-horizon with the prebuilt sysroot from
# build-sysroot.sh. Rust code is only built as static libraries, so the
# linker that Meson selects is dropped.

set -euo pipefail

RUST_DIR=/work/switch2/toolchain/rust

export RUSTC_BOOTSTRAP=1
export RUST_TARGET_PATH="$RUST_DIR"

args=()
while (($# > 0)); do
    case "$1" in
        -C)
            if (($# >= 2)) && [[ "$2" == linker=* ]]; then
                shift 2
                continue
            fi
            ;;
        -Clinker=*)
            shift
            continue
            ;;
    esac
    args+=("$1")
    shift
done

exec rustc -Zunstable-options \
    --sysroot "$RUST_DIR/sysroot" \
    --target aarch64-switch-horizon \
    -L /opt/devkitpro/devkitA64/aarch64-none-elf/lib \
    ${args[@]+"${args[@]}"}
