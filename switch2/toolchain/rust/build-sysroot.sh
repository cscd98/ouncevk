#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds the Rust standard library for aarch64-switch-horizon, for the CPU
# that its target file names. Run it again when that changes.
# The result goes to switch2/toolchain/rust/sysroot, which rustc-wrapper.sh
# uses. Run it in the ouncevk container with the repository at /work:
#   podman run --rm -v "$PWD:/work" -w /work ouncevk:latest \
#       bash switch2/toolchain/rust/build-sysroot.sh

set -euo pipefail

RUST_DIR=/work/switch2/toolchain/rust
TARGET=aarch64-switch-horizon
SYSROOT="$RUST_DIR/sysroot"
TOOLCHAIN="${RUST_NIGHTLY:-nightly}"
NEWLIB_LIB=/opt/devkitpro/devkitA64/aarch64-none-elf/lib

# Custom target JSON files are unstable, so allow nightly features.
export RUSTC_BOOTSTRAP=1
export RUST_TARGET_PATH="$RUST_DIR"
export RUSTFLAGS="-Zunstable-options -L $NEWLIB_LIB"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# A throwaway library crate. Building it with build-std builds the standard
# library for the target.
CRATE_DIR="$WORK_DIR/crate"
mkdir -p "$CRATE_DIR/src"
cat > "$CRATE_DIR/Cargo.toml" <<'EOF'
[package]
name = "horizon-sysroot"
version = "0.0.0"
edition = "2021"

[lib]
crate-type = ["rlib"]
path = "src/lib.rs"

[profile.release]
panic = "abort"
EOF
printf '#![no_main]\n' > "$CRATE_DIR/src/lib.rs"

(
    cd "$CRATE_DIR"
    cargo "+$TOOLCHAIN" build --release \
        -Zbuild-std=core,alloc,std,panic_abort \
        --target "$TARGET"
)

# Replace the sysroot with the new libraries.
DEPS_DIR="$CRATE_DIR/target/$TARGET/release/deps"
TARGET_LIB_DIR="$SYSROOT/lib/rustlib/$TARGET/lib"
rm -rf "$SYSROOT"
mkdir -p "$TARGET_LIB_DIR"
cp "$DEPS_DIR"/*.rlib "$DEPS_DIR"/*.rmeta "$TARGET_LIB_DIR/"

# Link the other targets and the sources from the installed toolchain.
HOST_RUSTLIB="$(rustc --print sysroot)/lib/rustlib"
for entry in "$HOST_RUSTLIB"/*; do
    name="$(basename "$entry")"
    link="$SYSROOT/lib/rustlib/$name"
    if [[ ! -e "$link" && ! -L "$link" ]]; then
        ln -s "$entry" "$link"
    fi
done

# Check that a crate using Vec compiles against the new sysroot.
cat > "$WORK_DIR/check.rs" <<'EOF'
pub fn squares(count: u32) -> Vec<u32> {
    (0..count).map(|value| value * value).collect()
}
EOF
if rustc -Zunstable-options --target "$TARGET" --sysroot "$SYSROOT" \
        --crate-type=rlib -C panic=abort \
        -o "$WORK_DIR/libcheck.rlib" "$WORK_DIR/check.rs"; then
    echo "sysroot check passed: $SYSROOT"
else
    echo "sysroot check failed: $SYSROOT" >&2
    exit 1
fi
