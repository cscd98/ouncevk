# SPDX-License-Identifier: MIT
#
# Build image for cross-compiling OunceVK (NVK, NAK and Zink) for Switch 2.
# Build it from this directory: podman build -t ouncevk switch2/toolchain
# The repository is mounted at /work and libounce's nx/ at /ounce-nx.

FROM docker.io/devkitpro/devkita64:20260219

ENV DEBIAN_FRONTEND=noninteractive

# Host packages for Meson, Mesa's host tools and the cross build.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        python3-pip bison flex curl ca-certificates pkg-config git \
        build-essential cmake llvm-15-dev clang-15 libclang-15-dev \
        libclc-15-dev libclc-15 llvm-spirv-15 libllvmspirvlib-15-dev \
        libdrm-dev glslang-tools && \
    rm -rf /var/lib/apt/lists/*

# SPIRV-Tools comes from trixie. The low pin keeps every other package
# on the base release unless it is requested with -t trixie.
RUN echo 'deb http://deb.debian.org/debian trixie main' \
        > /etc/apt/sources.list.d/trixie.list && \
    printf 'Package: *\nPin: release n=trixie\nPin-Priority: 100\n' \
        > /etc/apt/preferences.d/trixie && \
    apt-get update && \
    apt-get install -y --no-install-recommends -t trixie spirv-tools && \
    rm -rf /var/lib/apt/lists/*

# Meson and the Python modules that Mesa's generators need.
RUN pip3 install --break-system-packages --no-cache-dir \
        'meson>=1.4' mako pyyaml

# Rust nightly for NAK. build-std needs rust-src.
ENV RUSTUP_HOME=/opt/rust/rustup \
    CARGO_HOME=/opt/rust/cargo \
    RUST_NIGHTLY=nightly-2026-07-02
ENV PATH=/opt/rust/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --no-modify-path --profile minimal \
            --default-toolchain "$RUST_NIGHTLY" \
            --component rust-src,rustfmt && \
    cargo install --locked bindgen-cli cbindgen && \
    rm -rf /opt/rust/cargo/registry /opt/rust/cargo/git && \
    chmod -R a+rwX /opt/rust

ENV LLVM_CONFIG=/usr/bin/llvm-config-15 \
    LIBCLANG_PATH=/usr/lib/llvm-15/lib

# Headers for the cross build: libdrm from the host, then the stubs.
RUN mkdir -p /opt/switch-cross-include/libdrm && \
    cp /usr/include/xf86drm.h /usr/include/xf86drmMode.h \
        /opt/switch-cross-include/ && \
    cp -r /usr/include/libdrm/. /opt/switch-cross-include/ && \
    cp -r /usr/include/libdrm/. /opt/switch-cross-include/libdrm/
COPY cross-include/ /opt/switch-cross-include/

# Host pkg-config files that the Mesa cross configuration reads.
RUN mkdir -p /opt/devkitpro/portlibs/switch/lib/pkgconfig && \
    for pc in libclc.pc LLVMSPIRVLib.pc SPIRV-Tools.pc \
              SPIRV-Tools-shared.pc SPIRV-Headers.pc; do \
        path=$(find /usr -name "$pc" -print -quit 2>/dev/null); \
        if [ -n "$path" ]; then \
            cp "$path" /opt/devkitpro/portlibs/switch/lib/pkgconfig/; \
        fi; \
    done

# Horizon has no dynamic loader. An empty libdl satisfies -ldl.
RUN rm -f /opt/devkitpro/portlibs/switch/lib/libdl.a && \
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs \
        /opt/devkitpro/portlibs/switch/lib/libdl.a

WORKDIR /work
