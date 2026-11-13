#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Configures the Horizon cross build with OpenGL (Zink on NVK, EGL, GLES).
# It only configures. Build with ninja -C build/native-gl afterwards.
# Build the host tools first with switch2/toolchain/build-host-tools.sh.
# Run it in the ouncevk container:
#   podman run --rm -v "$PWD:/work" -v <libounce>/nx:/ounce-nx:ro -w /work \
#       ouncevk:latest sh switch2/build/configure-gl.sh

set -eu

SRC=/work
BUILD="$SRC/build/native-gl"
NATIVE_PREFIX="${NATIVE_PREFIX:-$SRC/build/host-tools}"

# Meson finds mesa_clc and vtn_bindgen2 here.
PATH="$NATIVE_PREFIX/bin:$PATH"
export PATH

set -- "$BUILD" "$SRC" \
    --cross-file "$SRC/switch2/crossfiles/horizon.cross" \
    --cross-file "$SRC/switch2/crossfiles/rust.cross" \
    --native-file "$SRC/switch2/crossfiles/host.ini" \
    --buildtype release \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dllvm=disabled \
    -Dvulkan-drivers=nouveau \
    -Dvideo-codecs= \
    -Dvulkan-layers= \
    -Dvulkan-beta=false \
    -Dshader-cache=enabled \
    -Dshader-cache-max-size=256M \
    -Dxmlconfig=disabled \
    -Ddriconf-policy=defaults-only \
    -Dzstd=disabled \
    -Dlibunwind=disabled \
    -Dlmsensors=disabled \
    -Dvalgrind=disabled \
    -Dperfetto=false \
    -Dandroid-libbacktrace=disabled \
    -Dbuild-tests=false \
    -Dnvk-libounce-nx=/ounce-nx \
    -Dgallium-drivers=zink \
    -Dopengl=true \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Degl=enabled \
    -Dglvnd=disabled \
    -Dplatforms=horizon \
    -Dglx=disabled \
    -Dgbm=disabled

# An existing build directory is configured again from scratch.
if [ -f "$BUILD/meson-private/coredata.dat" ]; then
    set -- --wipe "$@"
fi

cd "$SRC"
meson setup "$@"
