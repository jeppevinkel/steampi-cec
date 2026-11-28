#!/usr/bin/env bash
# Builds Debian trixie's mpv source package with one upstream fix, inside a debian:trixie container,
# for the libmpv2 package Jellyfin Desktop plays video through. See render-api-fence-leak.patch:
# without it, Jellyfin Desktop runs out of file descriptors after ~5 minutes of video on the Pi.
# Only libmpv2 is installed on the Pi; the mpv program doesn't use it.
#
# Usage (in the container): build-package.sh <output directory>
set -euo pipefail

SCRIPT_DIRECTORY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIRECTORY
readonly OUTPUT_DIRECTORY="$1"
# The Debian version this is built from; the build stops if trixie has moved on, so the patch
# gets checked against the new version (it may already contain the fix).
readonly DEBIAN_VERSION="0.40.0-3+deb13u1"
readonly PACKAGE_VERSION="${DEBIAN_VERSION}+steampi1"
readonly PATCH_NAME="render-api-fence-leak.patch"

export DEBIAN_FRONTEND=noninteractive
# Source packages: the image only lists binary ones.
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update
apt-get install -y --no-install-recommends ca-certificates devscripts equivs dpkg-dev quilt

mkdir -p /tmp/mpv && cd /tmp/mpv
apt-get source "mpv=${DEBIAN_VERSION}"
cd "mpv-${DEBIAN_VERSION%%-*}"

cp "${SCRIPT_DIRECTORY}/${PATCH_NAME}" debian/patches/
echo "${PATCH_NAME}" >> debian/patches/series
DEBFULLNAME="steampi-cec CI" DEBEMAIL="ci@steampi.invalid" \
    dch --newversion "${PACKAGE_VERSION}" --distribution trixie --force-distribution \
    "Fix the GL fence leak in the render API (upstream f74adc4243, mpv #17217)."

mk-build-deps --install --remove --tool "apt-get -y --no-install-recommends" debian/control
# Apply the patches now, to check the new one took (dpkg-buildpackage unapplies them afterwards).
dpkg-source --before-build .
grep -q "gl->FenceSync && p->params.swap_buffers" video/out/opengl/context.c \
    || { echo "${PATCH_NAME} was not applied" >&2; exit 1; }
DEB_BUILD_OPTIONS="noddebs nocheck parallel=$(nproc)" dpkg-buildpackage -us -uc -b

mkdir -p "${OUTPUT_DIRECTORY}"
cp ../libmpv2_"${PACKAGE_VERSION}"_*.deb "${OUTPUT_DIRECTORY}/"
ls -l "${OUTPUT_DIRECTORY}"
