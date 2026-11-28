#!/usr/bin/env bash
# Pulls debian:trixie (the same Debian as Raspberry Pi OS on the Pi) and writes BUILD_IMAGE=<image>
# to $GITHUB_ENV. Registries sometimes refuse anonymous pulls ("Data limit exceeded"), so several
# are tried in turn instead of letting one failed download fail the build.
set -uo pipefail

for image in public.ecr.aws/docker/library/debian:trixie \
             mirror.gcr.io/library/debian:trixie \
             docker.io/library/debian:trixie; do
    for attempt in 1 2; do
        if docker pull --quiet "${image}"; then
            echo "BUILD_IMAGE=${image}" >> "${GITHUB_ENV}"
            exit 0
        fi
        echo "::warning::Pulling ${image} failed (attempt ${attempt})"
        sleep 10
    done
done
echo "::error::No registry could provide debian:trixie"
exit 1
