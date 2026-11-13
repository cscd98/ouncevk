#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Configures and builds the Vulkan-only (headless) Horizon cross build.
# It builds the static archives that a program links.
# Build the host tools first with switch2/toolchain/build-host-tools.sh.
# Run it in the ouncevk container:
#   podman run --rm -v "$PWD:/work" -v <libounce>/nx:/ounce-nx:ro -w /work \
#       ouncevk:latest bash switch2/build/configure-native-headless.sh
# Set NVK_LIBOUNCE_NX (for example to /ounce-nx) to build the native
# memory-map backend too.

set -euo pipefail

SRC="$(cd "${SRC:-.}" && pwd)"
CROSS_BUILD="${CROSS_BUILD:-$SRC/build/native-headless}"
NATIVE_PREFIX="${NATIVE_PREFIX:-$SRC/build/host-tools}"
NVK_LIBOUNCE_NX="${NVK_LIBOUNCE_NX:-}"

# Meson finds mesa_clc and vtn_bindgen2 here.
export PATH="$NATIVE_PREFIX/bin:$PATH"

# Refresh the stub headers so edits apply without rebuilding the image.
cp -r "$SRC/switch2/toolchain/cross-include/." /opt/switch-cross-include/ \
    2> /dev/null || true

options=(
    --cross-file "$SRC/switch2/crossfiles/horizon.cross"
    --cross-file "$SRC/switch2/crossfiles/rust.cross"
    --native-file "$SRC/switch2/crossfiles/host.ini"
    --buildtype release
    -Dmesa-clc=system
    -Dprecomp-compiler=system
    -Dllvm=disabled
    -Dvulkan-drivers=nouveau
    -Dvideo-codecs=
    -Dvulkan-layers=
    -Dvulkan-beta=false
    -Dshader-cache=enabled
    -Dshader-cache-max-size=256M
    -Dxmlconfig=disabled
    -Ddriconf-policy=defaults-only
    -Dzstd=disabled
    -Dlibunwind=disabled
    -Dlmsensors=disabled
    -Dvalgrind=disabled
    -Dperfetto=false
    -Dandroid-libbacktrace=disabled
    -Dbuild-tests=false
    -Dgallium-drivers=
    -Dplatforms=
    -Dglx=disabled
    -Degl=disabled
    -Dgbm=disabled
    -Dopengl=false
    -Dgles1=disabled
    -Dgles2=disabled
)
if [[ -n "$NVK_LIBOUNCE_NX" ]]; then
    options+=("-Dnvk-libounce-nx=$NVK_LIBOUNCE_NX")
fi

cd "$SRC"
if [[ -f "$CROSS_BUILD/meson-private/coredata.dat" ]]; then
    # Regenerate the existing build first, then apply the options.
    meson setup --reconfigure "$CROSS_BUILD"
    meson setup --reconfigure "$CROSS_BUILD" "$SRC" "${options[@]}"
else
    meson setup "$CROSS_BUILD" "$SRC" "${options[@]}"
fi

# The static archives that a program links: NVK with its compilers (NAK,
# NIR, SPIR-V), NIL, the NVIDIA headers, MME, the winsys, the Vulkan runtime
# and Mesa's utility libraries.
archives=(
    src/nouveau/vulkan/libnvk.a
    src/nouveau/compiler/libnak.a
    src/nouveau/compiler/libnak_rs.a
    src/compiler/rust/libcompiler_c_helpers.a
    src/nouveau/nil/libnil.a
    src/nouveau/nil/liblibnil_format_table.a
    src/nouveau/headers/libnvidia_headers_c.a
    src/nouveau/mme/libnouveau_mme.a
    src/nouveau/winsys/libnouveau_ws.a
    src/compiler/spirv/libvtn.a
    src/compiler/nir/libnir.a
    src/compiler/libcompiler.a
    src/vulkan/util/libvulkan_util.a
    src/util/libxmlconfig.a
    src/util/libmesa_util.a
    src/util/libmesa_util_simd.a
    src/util/blake3/libblake3.a
    src/c11/impl/libmesa_util_c11.a
)

if [[ -n "$NVK_LIBOUNCE_NX" ]]; then
    archives+=(src/nouveau/vulkan/libnvk_native_map.a)
fi

ninja -C "$CROSS_BUILD" -j 6 "${archives[@]}"
