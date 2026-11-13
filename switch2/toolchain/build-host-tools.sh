#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds Mesa's host programs mesa_clc and vtn_bindgen2 for x86-64.
# The cross build uses them through -Dmesa-clc=system and
# -Dprecomp-compiler=system. Run it in the ouncevk container:
#   podman run --rm -v "$PWD:/work" -w /work ouncevk:latest \
#       bash switch2/toolchain/build-host-tools.sh

set -euo pipefail

SRC="$(cd "${SRC:-.}" && pwd)"
HOST_BUILD="${HOST_BUILD:-$SRC/build/host}"
NATIVE_PREFIX="${NATIVE_PREFIX:-$SRC/build/host-tools}"

setup_args=(
    "$HOST_BUILD" "$SRC"
    --native-file "$SRC/switch2/crossfiles/host.ini"
    --prefix "$NATIVE_PREFIX"
    --buildtype release
    -Dmesa-clc=enabled
    -Dinstall-mesa-clc=true
    -Dprecomp-compiler=auto
    -Dllvm=enabled
    -Dshared-llvm=enabled
    -Dvulkan-drivers=
    -Dgallium-drivers=
    -Dplatforms=
    -Dglx=disabled
    -Degl=disabled
    -Dgbm=disabled
    -Dopengl=false
    -Dgles1=disabled
    -Dgles2=disabled
    -Dvideo-codecs=
    -Dvulkan-layers=
    -Dbuild-tests=false
)

# An existing build directory is configured again from scratch.
if [[ -f "$HOST_BUILD/meson-private/coredata.dat" ]]; then
    meson setup --wipe "${setup_args[@]}"
else
    meson setup "${setup_args[@]}"
fi

ninja -C "$HOST_BUILD" \
    src/compiler/clc/mesa_clc \
    src/compiler/spirv/vtn_bindgen2

BIN_DIR="$NATIVE_PREFIX/bin"
install -D -m 755 "$HOST_BUILD/src/compiler/spirv/vtn_bindgen2" \
    "$BIN_DIR/vtn_bindgen2"
install -D -m 755 "$HOST_BUILD/src/compiler/clc/mesa_clc" \
    "$BIN_DIR/mesa_clc.real"

# mesa_clc needs clang's OpenCL headers. The wrapper finds them and adds
# their directory to the include path.
cat > "$BIN_DIR/mesa_clc" <<'EOF'
#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Runs mesa_clc.real with clang's OpenCL resource headers on the include
# path, when they can be found.

set -euo pipefail

REAL="$(dirname "$(readlink -f "$0")")/mesa_clc.real"

find_opencl_include() {
    local compiler resource_dir dir
    for compiler in clang clang-15 clang-16 clang-17 clang-18; do
        if ! command -v "$compiler" > /dev/null 2>&1; then
            continue
        fi
        resource_dir="$("$compiler" -print-resource-dir 2> /dev/null)" || continue
        if [[ -n "$resource_dir" && -f "$resource_dir/include/opencl-c-base.h" ]]; then
            printf '%s\n' "$resource_dir/include"
            return 0
        fi
    done
    for dir in /usr/lib/llvm-*/lib/clang/*/include /usr/lib/clang/*/include; do
        if [[ -f "$dir/opencl-c-base.h" ]]; then
            printf '%s\n' "$dir"
            return 0
        fi
    done
    return 1
}

if include_dir="$(find_opencl_include)"; then
    exec "$REAL" "$@" "-I$include_dir"
fi
exec "$REAL" "$@"
EOF
chmod 755 "$BIN_DIR/mesa_clc"

echo "Installed host tools:"
ls -l "$BIN_DIR/mesa_clc" "$BIN_DIR/mesa_clc.real" "$BIN_DIR/vtn_bindgen2"
