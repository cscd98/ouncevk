# SPDX-License-Identifier: MIT
#
# Builds OunceVK's pacman package in OunceVK's toolchain image. From the
# repository's top folder:
#   docker build -t ouncevk-toolchain switch2/toolchain
#   docker build -f packaging/Dockerfile --build-arg VERSION=20261117 --output out .
# writes out/ouncevk-20261117-1-any.pkg.tar.zst. release.sh builds it from a
# commit and publishes it.
ARG TOOLCHAIN_IMAGE=ouncevk-toolchain
FROM ${TOOLCHAIN_IMAGE} AS build

ARG VERSION
# The libounce release that OunceVK builds against.
ARG LIBOUNCE_VERSION=20261117
# The commit time, so that the package's file times do not depend on the
# build.
ARG SOURCE_DATE_EPOCH=0
# The commit, for Mesa's version string ("Mesa 26.2.2 (git-...)"): the build
# has the commit's files but no .git.
ARG GIT_SHA1=

# makepkg packages with fakeroot and zstd.
RUN apt-get update && \
    apt-get install -y --no-install-recommends fakeroot zstd && \
    rm -rf /var/lib/apt/lists/*

# libounce's package, checked against the SHA-256 of its release.
RUN release="https://github.com/ouncebrew/libounce/releases/download/$LIBOUNCE_VERSION" && \
    package="libounce-$LIBOUNCE_VERSION-1-any.pkg.tar.zst" && \
    cd /tmp && \
    curl -fsSLO "$release/$package" && \
    curl -fsSL "$release/$package.sha256" | sha256sum -c && \
    dkp-pacman -U --noconfirm "$package" && \
    rm "$package"

# OunceVK's build scripts take libounce's headers and libraries from
# /ounce-nx, laid out as libounce's nx/ folder. The package installs both
# header folders as one.
RUN mkdir -p /ounce-nx/external/bsd && \
    ln -s /opt/devkitpro/libounce/include /ounce-nx/include && \
    ln -s /opt/devkitpro/libounce/include /ounce-nx/external/bsd/include && \
    ln -s /opt/devkitpro/libounce/lib /ounce-nx/lib

# makepkg does not run as root. OunceVK builds in /work.
RUN useradd --create-home builder && \
    mkdir -p /work /home/builder/out && chown builder /work /home/builder/out
COPY --chown=builder . /work
USER builder
WORKDIR /work/packaging
RUN test -n "$VERSION" && \
    OUNCEVK_VERSION="$VERSION" SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    MESA_GIT_SHA1_OVERRIDE="$GIT_SHA1" \
    BUILDDIR=/tmp/makepkg PKGDEST=/home/builder/out PACKAGER=ouncebrew \
    dkp-makepkg --nodeps --noconfirm

FROM scratch
COPY --from=build /home/builder/out/ /
