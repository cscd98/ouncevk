#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Packages the OunceVK builds as OunceVK, a Switch 2 portlib: the tree that
# goes into $DEVKITPRO/portlibs/switch2. Build build/native-headless and
# build/native-gl first. Run it in the ouncevk container:
#   podman run --rm -v "$PWD:/work" -v <out>:/out -w /work ouncevk:latest \
#       sh switch2/build/package-portlib.sh /out/portlibs/switch2
#
# The output has these files:
# - lib/libouncevk.a: Vulkan (NVK).
# - lib/libouncevk-gl.a: EGL, OpenGL and OpenGL ES (Zink), and also Vulkan.
# - lib/libvulkan.a, lib/libEGL.a: linker scripts. They select libouncevk.a or
#   libouncevk-gl.a, and also libstdc++ and libm. With them, -lvulkan and
#   -lEGL are sufficient. A program that uses OpenGL links only -lEGL, which
#   also supplies Vulkan. Each library contains its own private zlib.
# - lib/libGLESv2.a, lib/libglapi.a, lib/libdrm_nouveau.a: empty archives.
#   A Switch 1 Makefile links them for Mesa's GL. They hide the Switch 1
#   libraries of the same name, which do not work on Switch 2, and they let
#   the link succeed when devkitPro's switch-mesa is not installed.
# - include/: the Vulkan, EGL, OpenGL and OpenGL ES headers.
# - lib/pkgconfig/: vulkan, egl, glesv2, gl and glapi.
# - bin/aarch64-none-elf-pkg-config: pkg-config for Switch 2.
# - share/licenses/ouncevk/: the license texts.
#
# Each library holds one relocatable object, made with ld -r. Mesa's
# dispatch tables refer to the driver entry points with weak references, and
# a weak reference does not pull a member out of an archive. In one object,
# all entry points come in together. Only the API symbols stay global, so the
# Mesa symbols cannot collide with the symbols of a program.

set -eu

DEST="${1:?usage: package-portlib.sh DEST}"
SRC="${SRC:-/work}"
PREFIX=/opt/devkitpro/devkitA64/bin/aarch64-none-elf

VULKAN_BUILD="$SRC/build/native-headless"
GL_BUILD="$SRC/build/native-gl"

# The archives after libnvk.a, in link order.
DRIVER_ARCHIVES="
src/nouveau/vulkan/libnvk_native_map.a
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
"

# The GL frontend reaches its drivers through tables, so all members of
# these archives go in.
GL_ARCHIVES="
src/egl/libEGL.a
src/mesa/libmesa.a
src/compiler/glsl/libglsl.a
src/compiler/glsl/glcpp/libglcpp.a
src/mesa/glapi/shared-glapi/libglapi.a
src/mesa/glapi/glapi/libglapi_bridge.a
src/gallium/auxiliary/libgallium.a
src/gallium/drivers/zink/libzink.a
src/gallium/winsys/zink/drm/libzinkwinsys.a
src/gallium/winsys/sw/null/libws_null.a
src/gallium/winsys/sw/wrapper/libwsw.a
"

# POSIX functions that newlib does not define. Mesa's Horizon glue
# (src/util/horizon and src/util/u_debug.c) supplies them. They stay global
# as weak symbols: a program can use them, and a definition of its own takes
# precedence. To find them, compare the global symbols of the object before
# objcopy with the functions of glibc that newlib, libstdc++, libgcc and
# libounce do not define. The other matches are Rust compiler builtins
# (f128 math, fmaximum, roundeven). They stay local.
POSIX_SHIMS="getuid geteuid getgid getegid sysconf pthread_sigmask
posix_memalign secure_getenv"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Writes the members of the thin archive libnvk.a, one time each. The thin
# archive lists some Vulkan runtime objects more than one time.
nvk_objects() {
    build="$1"
    (cd "$build" && "$PREFIX-ar" t src/nouveau/vulkan/libnvk.a | sort -u |
        while read -r member; do
            case "$member" in
            /*) echo "$member" ;;
            *) echo "$build/$member" ;;
            esac
        done)
}

# Makes one relocatable object and keeps only the symbols that match the
# given patterns global.
#   prelink OUTPUT BUILD WHOLE_ARCHIVES PATTERN...
prelink() {
    output="$1"
    build="$2"
    whole="$3"
    shift 3

    archives=""
    for archive in $DRIVER_ARCHIVES; do
        archives="$archives $build/$archive"
    done
    whole_archives=""
    for archive in $whole; do
        whole_archives="$whole_archives $build/$archive"
    done

    # zlib goes into the object too: the shader cache compresses its
    # entries. objcopy below makes its symbols local, so a program can link
    # its own zlib, and -lvulkan needs no -lz.
    # shellcheck disable=SC2046
    # The final objcopy localizes C++ template symbols. Their COMDAT groups
    # must be flattened first: otherwise a homebrew object with the same
    # template instantiation can discard this object's local definition.
    "$PREFIX-ld" -r --force-group-allocation -o "$output" \
        --whole-archive $whole_archives --no-whole-archive \
        $(nvk_objects "$build") \
        --start-group $archives "$ZLIB_ARCHIVE" --end-group

    keep=""
    for pattern in "$@"; do
        keep="$keep --keep-global-symbol=$pattern"
    done
    for symbol in $POSIX_SHIMS; do
        keep="$keep --keep-global-symbol=$symbol --weaken-symbol=$symbol"
    done
    # shellcheck disable=SC2086
    "$PREFIX-objcopy" --wildcard $keep "$output"
}

# Makes an archive with the one object in it.
#   archive LIBRARY OBJECT
archive() {
    rm -f "$1"
    "$PREFIX-ar" rcs "$1" "$2"
}

# Makes an archive with no members.
empty_archive() {
    printf '!<arch>\n' > "$1"
}

# Writes a pkg-config file. The prefix comes from the location of the file,
# so the tree can go into any devkitPro directory.
#   pkgconfig NAME DESCRIPTION LIBS [REQUIRES]
pkgconfig() {
    {
        echo 'prefix=${pcfiledir}/../..'
        echo 'includedir=${prefix}/include'
        echo 'libdir=${prefix}/lib'
        echo
        echo "Name: $1"
        echo "Description: $2"
        echo "Version: $VERSION"
        if [ -n "${4:-}" ]; then
            echo "Requires: $4"
        fi
        echo "Libs: -L\${libdir} $3"
        echo 'Cflags: -I${includedir} -D__SWITCH2__'
    } > "$DEST/lib/pkgconfig/$1.pc"
}

VERSION="$(sed -n '1p' "$SRC/VERSION")"
ZLIB_ARCHIVE="${ZLIB_ARCHIVE:-/opt/devkitpro/portlibs/switch/lib/libz.a}"
ZLIB_LICENSE="${ZLIB_LICENSE:-/opt/devkitpro/portlibs/switch/licenses/switch-zlib/LICENSE}"

mkdir -p "$DEST/lib/pkgconfig" "$DEST/include" "$DEST/bin" \
    "$DEST/share/licenses/ouncevk"

# Libraries.
prelink "$WORK/vulkan.o" "$VULKAN_BUILD" "" \
    'vk[A-Z]*' 'ouncevk_profile_*' '__nx_gpu_exit'
archive "$DEST/lib/libouncevk.a" "$WORK/vulkan.o"
echo 'INPUT(-louncevk -lstdc++ -lm)' > "$DEST/lib/libvulkan.a"

prelink "$WORK/egl.o" "$GL_BUILD" "$GL_ARCHIVES" \
    'vk[A-Z]*' 'egl[A-Z]*' 'gl[A-Z]*' 'ouncevk_profile_*' '__nx_gpu_exit'
archive "$DEST/lib/libouncevk-gl.a" "$WORK/egl.o"
echo 'INPUT(-louncevk-gl -lstdc++ -lm)' > "$DEST/lib/libEGL.a"

empty_archive "$DEST/lib/libGLESv2.a"
empty_archive "$DEST/lib/libglapi.a"
empty_archive "$DEST/lib/libdrm_nouveau.a"

# Headers. GLX and WGL are not for Horizon.
cp -r "$SRC/include/vulkan" "$SRC/include/vk_video" "$SRC/include/EGL" \
    "$SRC/include/KHR" "$SRC/include/GLES" "$SRC/include/GLES2" \
    "$SRC/include/GLES3" "$DEST/include/"
mkdir -p "$DEST/include/ouncevk"
cp "$SRC/include/ouncevk/profile.h" "$DEST/include/ouncevk/"
mkdir -p "$DEST/include/GL"
cp "$SRC/include/GL/gl.h" "$SRC/include/GL/glext.h" \
    "$SRC/include/GL/glcorearb.h" "$DEST/include/GL/"

# pkg-config files.
pkgconfig vulkan "Vulkan for Switch 2 (OunceVK)" "-lvulkan"
pkgconfig egl "EGL for Switch 2 (OunceVK and Zink)" "-lEGL"
pkgconfig glesv2 "OpenGL ES for Switch 2 (OunceVK and Zink)" "" egl
pkgconfig gl "OpenGL for Switch 2 (OunceVK and Zink)" "" egl
pkgconfig glapi "Empty: the GL dispatch is in libEGL" ""
pkgconfig libdrm_nouveau "Empty: Switch 2 does not use libdrm" ""

# pkg-config for Switch 2. It looks in the Switch 2 portlibs first, then in
# the Switch 1 portlibs.
cat > "$DEST/bin/aarch64-none-elf-pkg-config" <<'EOF'
#!/usr/bin/env bash
# pkg-config for Switch 2 portlibs.
command -v pkg-config >/dev/null 2>&1 || {
    echo >&2 "pkg-config is necessary. Install it first."
    exit 1
}

# Do not use pkg-config files of the host.
export PKG_CONFIG_DIR=
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR=
export PKG_CONFIG_LIBDIR="${DEVKITPRO}/portlibs/switch2/lib/pkgconfig:${DEVKITPRO}/portlibs/switch/lib/pkgconfig"

[[ "$1" == '--version' ]] && exec pkg-config --version
exec pkg-config --static "$@"
EOF
chmod +x "$DEST/bin/aarch64-none-elf-pkg-config"

# Licenses.
cp "$SRC/docs/license.rst" "$DEST/share/licenses/ouncevk/mesa-license.rst"
cp "$ZLIB_LICENSE" "$DEST/share/licenses/ouncevk/zlib-license.txt"
cp "$SRC/switch2/README.md" "$DEST/share/licenses/ouncevk/README.md" 2>/dev/null ||
    cp "$SRC/switch2/README.public.md" "$DEST/share/licenses/ouncevk/README.md"

echo "OunceVK $VERSION portlib in $DEST"
ls -la "$DEST/lib"
