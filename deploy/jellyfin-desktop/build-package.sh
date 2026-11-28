#!/usr/bin/env bash
# Builds Jellyfin Desktop as a Debian package, inside a debian:trixie container. Upstream publishes
# no ARM Linux build; this one links the system's libmpv, which on the Pi uses Raspberry Pi's
# FFmpeg and with it the hardware HEVC decoder.
#
# Usage (in the container): build-package.sh <output directory>
# The version to build is the git tag in VERSION, next to this script.
set -euo pipefail

SCRIPT_DIRECTORY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPSTREAM_TAG="$(cat "${SCRIPT_DIRECTORY}/VERSION")"
readonly SCRIPT_DIRECTORY UPSTREAM_TAG
readonly OUTPUT_DIRECTORY="$1"
readonly SOURCE_DIRECTORY=/tmp/jellyfin-desktop
# v2.1.0-pre2 -> 2.1.0~pre2: "~" sorts a pre-release before the final release.
readonly UPSTREAM_VERSION="${UPSTREAM_TAG#v}"
readonly PACKAGE_VERSION="${UPSTREAM_VERSION//-/\~}-1+steampi1"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates git devscripts equivs debhelper

git clone --quiet --depth 1 --branch "${UPSTREAM_TAG}" --recurse-submodules --shallow-submodules \
    https://github.com/jellyfin/jellyfin-desktop.git "${SOURCE_DIRECTORY}"
cd "${SOURCE_DIRECTORY}"

# Upstream doesn't always bump debian/changelog; our own version keeps apt upgrades in order.
DEBFULLNAME="steampi-cec CI" DEBEMAIL="ci@steampi.invalid" \
    dch --newversion "${PACKAGE_VERSION}" --distribution trixie --force-distribution \
    "Built from ${UPSTREAM_TAG} for the steampi TV box."

# Upstream's debian/rules lags behind their other builds: they all build MpvQt from the bundled
# submodule, which debian/rules doesn't ask for. Updates come through pitv-update, so the app's
# own update check is off.
sed -i 's/-DCHECK_FOR_UPDATES=ON/-DCHECK_FOR_UPDATES=OFF -DUSE_STATIC_MPVQT=ON/' debian/rules
grep -q -- "-DUSE_STATIC_MPVQT=ON" debian/rules || { echo "debian/rules changed upstream; adjust build-package.sh" >&2; exit 1; }

mk-build-deps --install --remove --tool "apt-get -y --no-install-recommends" debian/control
DEB_BUILD_OPTIONS="noddebs parallel=$(nproc)" dpkg-buildpackage -us -uc -b

mkdir -p "${OUTPUT_DIRECTORY}"
cp ../jellyfin-desktop_*.deb "${OUTPUT_DIRECTORY}/"
ls -l "${OUTPUT_DIRECTORY}"
