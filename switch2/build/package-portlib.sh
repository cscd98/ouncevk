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
# - lib/shared/: the libraries as shared libraries, when OUNCE_NX names a
#   libounce that has its runtime library built (make shared in its nx/).
#   Put lib/shared before lib in the library path.
#   - libouncevk.so.1: Vulkan (NVK), the driver.
#   - libouncevk-gl.so.1: EGL, OpenGL and OpenGL ES (Zink). It has no driver
#     of its own: it needs libouncevk.so.1.
#   - libvulkan.so, libouncevk.so, libEGL.so, libGLESv2.so, libGL.so: linker
#     scripts, for -lvulkan, -louncevk, -lEGL, -lGLESv2 and -lGL.
#   - libvulkan.so.1, libEGL.so.1, libGLESv2.so.2, libGL.so.1: the names that
#     a program for Linux loads with dlopen. Each is a library with no code
#     that needs the one above, so dlsym on its handle finds the functions.
#   - nro/: all of those as NROs, which go on the SD card beside
#     libounce.so.1 and libstdc++.so.6.
# - lib/pkgconfig/: vulkan, egl, glesv2, gl and glapi.
# - bin/aarch64-none-elf-pkg-config: pkg-config for Switch 2.
# - share/licenses/ouncevk/: the license texts.
#
# Each library holds one relocatable object, made with ld -r. Mesa's
# dispatch tables refer to the driver entry points with weak references, and
# a weak reference does not pull a member out of an archive. In one object,
# all entry points come in together. Only the API symbols stay global, so the
# Mesa symbols cannot collide with the symbols of a program. The API is the
# Vulkan, EGL and GL functions, the driver's entry points for a Vulkan loader
# (vk_icd*), the profile functions and libounce's exit hook. The Vulkan
# object also keeps the two functions that the EGL driver calls to register
# the GPU platform: the shared GL library takes them from the shared driver.

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
# given patterns global. DRIVER is "driver" for an object that holds NVK, and
# "no-driver" for one that takes Vulkan from another library: it then holds
# only the members of DRIVER_ARCHIVES that its own code needs.
#   prelink OUTPUT BUILD WHOLE_ARCHIVES DRIVER PATTERN...
prelink() {
    output="$1"
    build="$2"
    whole="$3"
    driver_objects=""
    if [ "$4" = driver ]; then
        driver_objects="$(nvk_objects "$build")"
    fi
    shift 4

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
        $driver_objects \
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

# Links the relocatable object as a shared library on libounce.so and
# libstdc++.so, and makes its NRO. The object's global symbols, the API, are
# what the library exports. Its own references to them bind to itself. The
# link fails when the object needs a symbol that those libraries do not
# have: a driver cannot need one from the program. NEEDED names the shared
# libraries of this package that the object also takes symbols from.
#   shared_library OBJECT SONAME [NEEDED...]
shared_library() {
    object="$1"
    soname="$2"
    shift 2
    needed=""
    for library in "$@"; do
        needed="$needed $DEST/lib/shared/$library"
    done
    # shellcheck disable=SC2086
    "$PREFIX-gcc" -mcpu=cortex-a78c+crypto -mtp=soft -fPIC -shared \
        -nostdlib -nostartfiles -Wl,-T,"$OUNCE_NX/switch.ld" \
        -Wl,-soname,"$soname" -Wl,-Bsymbolic -Wl,--gc-sections -Wl,-z,defs \
        -Wl,-z,text -Wl,-z,now -Wl,-z,pack-relative-relocs \
        -Wl,--build-id=sha1 -Wl,--hash-style=gnu -Wl,--eh-frame-hdr \
        "$OUNCE_NX/lib/ounce_module_crt0.o" "$object" $needed \
        "$OUNCE_NX/lib/shared/libstdc++.so.6" \
        "$OUNCE_NX/lib/shared/libounce.so.1" -lgcc \
        -o "$DEST/lib/shared/$soname"
    /opt/devkitpro/tools/bin/elf2nro "$DEST/lib/shared/$soname" \
        "$DEST/lib/shared/nro/$soname" > /dev/null
}

# Makes a shared library with no code that needs the library TARGET, and its
# NRO. A program that loads SONAME with dlopen gets a handle whose dlsym
# looks in TARGET and in what TARGET needs. This is for the names that code
# written for Linux loads: the SD card's file system has no links to give a
# library two names.
#   alias_library SONAME TARGET
alias_library() {
    "$PREFIX-gcc" -mcpu=cortex-a78c+crypto -mtp=soft -fPIC -shared \
        -nostdlib -nostartfiles -Wl,-T,"$OUNCE_NX/switch.ld" \
        -Wl,-soname,"$1" -Wl,-z,now -Wl,--build-id=sha1 \
        -Wl,--hash-style=gnu -Wl,--eh-frame-hdr \
        "$OUNCE_NX/lib/ounce_module_crt0.o" \
        -Wl,--no-as-needed "$DEST/lib/shared/$2" \
        -o "$DEST/lib/shared/$1"
    /opt/devkitpro/tools/bin/elf2nro "$DEST/lib/shared/$1" \
        "$DEST/lib/shared/nro/$1" > /dev/null
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
#   pkgconfig_file FILE PREFIX LIBDIR NAME DESCRIPTION LIBS [REQUIRES]
pkgconfig_file() {
    {
        echo "prefix=$2"
        echo 'includedir=${prefix}/include'
        echo "libdir=$3"
        echo
        echo "Name: $4"
        echo "Description: $5"
        echo "Version: $VERSION"
        if [ -n "${7:-}" ]; then
            echo "Requires: $7"
        fi
        echo "Libs: -L\${libdir} $6"
        echo 'Cflags: -I${includedir} -D__SWITCH2__'
    } > "$1"
}

# The pkg-config file of a static library, in lib/pkgconfig.
#   pkgconfig NAME DESCRIPTION LIBS [REQUIRES]
pkgconfig() {
    pkgconfig_file "$DEST/lib/pkgconfig/$1.pc" '${pcfiledir}/../..' \
        '${prefix}/lib' "$@"
}

# The pkg-config file of a shared library, in lib/shared/pkgconfig. It names
# lib/shared as the library's folder. aarch64-none-elf-pkg-config looks there
# first for a dynamically linked program.
#   pkgconfig_shared NAME DESCRIPTION LIBS [REQUIRES]
pkgconfig_shared() {
    pkgconfig_file "$DEST/lib/shared/pkgconfig/$1.pc" \
        '${pcfiledir}/../../..' '${prefix}/lib/shared' "$@"
}

VERSION="$(sed -n '1p' "$SRC/VERSION")"
ZLIB_ARCHIVE="${ZLIB_ARCHIVE:-/opt/devkitpro/portlibs/switch/lib/libz.a}"
ZLIB_LICENSE="${ZLIB_LICENSE:-/opt/devkitpro/portlibs/switch/licenses/switch-zlib/LICENSE}"

mkdir -p "$DEST/lib/pkgconfig" "$DEST/include" "$DEST/bin" \
    "$DEST/share/licenses/ouncevk"

# Libraries.
prelink "$WORK/vulkan.o" "$VULKAN_BUILD" "" driver \
    'vk[A-Z]*' 'vk_icd*' 'ouncevk_profile_*' '__nx_gpu_exit' \
    'nvkmd_nvgpu_native_*_default_platform'
archive "$DEST/lib/libouncevk.a" "$WORK/vulkan.o"
echo 'INPUT(-louncevk -lstdc++ -lm)' > "$DEST/lib/libvulkan.a"

prelink "$WORK/egl.o" "$GL_BUILD" "$GL_ARCHIVES" driver \
    'vk[A-Z]*' 'vk_icd*' 'egl[A-Z]*' 'gl[A-Z]*' 'ouncevk_profile_*' \
    '__nx_gpu_exit'
archive "$DEST/lib/libouncevk-gl.a" "$WORK/egl.o"
echo 'INPUT(-louncevk-gl -lstdc++ -lm)' > "$DEST/lib/libEGL.a"

# The shared libraries. The GL library is the GL frontend and Zink without
# NVK: Zink gets the driver's functions from vk_icdGetInstanceProcAddr, which
# libouncevk.so.1 exports, so a process has one driver whichever of the two
# libraries its modules use.
OUNCE_NX="${OUNCE_NX:-}"
if [ -n "$OUNCE_NX" ] && [ -f "$OUNCE_NX/lib/shared/libounce.so.1" ]; then
    mkdir -p "$DEST/lib/shared/nro"
    shared_library "$WORK/vulkan.o" libouncevk.so.1
    echo 'INPUT(libouncevk.so.1)' > "$DEST/lib/shared/libouncevk.so"
    echo 'INPUT(libouncevk.so.1)' > "$DEST/lib/shared/libvulkan.so"

    prelink "$WORK/gl.o" "$GL_BUILD" "$GL_ARCHIVES" no-driver \
        'egl[A-Z]*' 'gl[A-Z]*'
    shared_library "$WORK/gl.o" libouncevk-gl.so.1 libouncevk.so.1
    # The scripts also name the driver, which the GL library needs: the
    # linker then finds it without -rpath-link. A program that calls no
    # Vulkan function itself does not get it as a needed library.
    for name in EGL GLESv2 GL; do
        echo 'INPUT(libouncevk-gl.so.1 AS_NEEDED(libouncevk.so.1))' \
            > "$DEST/lib/shared/lib$name.so"
    done

    alias_library libvulkan.so.1 libouncevk.so.1
    alias_library libEGL.so.1 libouncevk-gl.so.1
    alias_library libGLESv2.so.2 libouncevk-gl.so.1
    alias_library libGL.so.1 libouncevk-gl.so.1

    mkdir -p "$DEST/lib/shared/pkgconfig"
    pkgconfig_shared vulkan "Vulkan for Switch 2 (OunceVK)" "-lvulkan"
    pkgconfig_shared egl "EGL for Switch 2 (OunceVK and Zink)" "-lEGL"
    pkgconfig_shared glesv2 "OpenGL ES for Switch 2 (OunceVK and Zink)" \
        "-lGLESv2" egl
    pkgconfig_shared gl "OpenGL for Switch 2 (OunceVK and Zink)" "-lGL" egl
    echo "The shared libraries are in $DEST/lib/shared"
else
    echo "No shared library: OUNCE_NX names no libounce with libounce.so.1"
fi

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
# the Switch 1 portlibs. For a dynamically linked program (OUNCE_SHARED set,
# as libounce's CMake toolchain and switch2vars.sh do), the shared libraries
# of the Switch 2 portlibs come before all of them.
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
if [[ -n "${OUNCE_SHARED:-}" ]]; then
    PKG_CONFIG_LIBDIR="${DEVKITPRO}/portlibs/switch2/lib/shared/pkgconfig:${PKG_CONFIG_LIBDIR}"
fi

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
