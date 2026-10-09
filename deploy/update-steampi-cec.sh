#!/usr/bin/env bash
# Installs or updates the steampi-cec nightly build as a systemd service.
# Safe to run any time: it only restarts the service if something changed.
#
# Usage: ./update-steampi-cec.sh [--force]
#   --force   restart the service even if nothing changed

set -euo pipefail

readonly DOWNLOAD_URL="https://github.com/jeppevinkel/steampi-cec/releases/download/nightly/steampi-cec-arm64"
readonly SERVICE_NAME="steampi-cec"
readonly SERVICE_USER="jeppe"                  # must match the home dir in LOG_PATH
readonly INSTALL_PATH="/usr/local/bin/${SERVICE_NAME}"
readonly UNIT_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
readonly REQUIRED_LIBCEC="libcec.so.7"         # major version the build was compiled against

# Re-run as root if needed.
if [[ ${EUID} -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

force_restart=false
if [[ "${1:-}" == "--force" ]]; then
    force_restart=true
fi

working_directory="$(mktemp -d)"
trap 'rm -rf "${working_directory}"' EXIT
downloaded_binary="${working_directory}/${SERVICE_NAME}"
generated_unit="${working_directory}/${SERVICE_NAME}.service"

# ---------- Download ----------
echo "Downloading latest nightly build..."
curl --fail --location --silent --show-error --retry 3 \
     --output "${downloaded_binary}" "${DOWNLOAD_URL}"

# ---------- Verify before touching the installed build ----------
if ! cmp -s <(head -c 4 "${downloaded_binary}") <(printf '\x7fELF'); then
    echo "Downloaded file is not an executable (ELF) binary. Nothing was changed." >&2
    exit 1
fi
chmod 0755 "${downloaded_binary}"

# Catches missing shared libraries and a too-new glibc/libstdc++ (e.g. GLIBCXX_3.4.xx not found).
ldd_output="$(ldd "${downloaded_binary}" 2>&1 || true)"
if grep -qE 'not found|not a dynamic executable' <<< "${ldd_output}"; then
    echo "The new build cannot run on this Pi:" >&2
    grep -E 'not found|not a dynamic executable' <<< "${ldd_output}" >&2
    echo "Nothing was changed." >&2
    exit 1
fi

# libcec is loaded at runtime with dlopen (cecloader.h), so ldd doesn't list it. Check it directly.
if ! grep -q "${REQUIRED_LIBCEC}" <<< "$(ldconfig -p)"; then
    echo "${REQUIRED_LIBCEC} is not installed. Nothing was changed." >&2
    exit 1
fi

# ---------- Generate the service unit ----------
service_user_id="$(id -u "${SERVICE_USER}")"
cat > "${generated_unit}" <<EOF
[Unit]
Description=SteamPi CEC daemon
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
SupplementaryGroups=video input
WorkingDirectory=/home/${SERVICE_USER}
ExecStart=${INSTALL_PATH}
Restart=on-failure
# Long enough that a crash loop doesn't post a log chunk to ntfy every few seconds.
RestartSec=30
Environment=DISPLAY=:0
Environment=XAUTHORITY=/home/${SERVICE_USER}/.Xauthority
Environment=XDG_RUNTIME_DIR=/run/user/${service_user_id}

[Install]
WantedBy=multi-user.target
EOF

# ---------- Work out what changed ----------
binary_changed=true
if [[ -f "${INSTALL_PATH}" ]] && cmp -s "${downloaded_binary}" "${INSTALL_PATH}"; then
    binary_changed=false
fi

unit_changed=true
if [[ -f "${UNIT_PATH}" ]] && cmp -s "${generated_unit}" "${UNIT_PATH}"; then
    unit_changed=false
fi

if [[ ${binary_changed} == false && ${unit_changed} == false && ${force_restart} == false ]]; then
    echo "Already up to date (build $(sha256sum "${INSTALL_PATH}" | cut -c1-12))."
    systemctl enable --quiet --now "${SERVICE_NAME}"   # make sure it's running
    exit 0
fi

# ---------- Install ----------
if [[ ${binary_changed} == true ]]; then
    # Copy next to the target, then rename. The rename is atomic and works while the old
    # binary is still running; overwriting it in place would fail with "Text file busy".
    install -m 0755 -o root -g root "${downloaded_binary}" "${INSTALL_PATH}.new"
    mv -f "${INSTALL_PATH}.new" "${INSTALL_PATH}"
    echo "Installed new build $(sha256sum "${INSTALL_PATH}" | cut -c1-12)."
fi

if [[ ${unit_changed} == true ]]; then
    install -m 0644 -o root -g root "${generated_unit}" "${UNIT_PATH}"
    systemctl daemon-reload
    echo "Installed service unit."
fi

systemctl enable --quiet "${SERVICE_NAME}"
echo "Restarting ${SERVICE_NAME} (may take a moment while the final log upload finishes)..."
systemctl restart "${SERVICE_NAME}"

# ---------- Check it came up ----------
sleep 3
if systemctl is-active --quiet "${SERVICE_NAME}"; then
    echo "${SERVICE_NAME} is running."
else
    echo "${SERVICE_NAME} failed to start:" >&2
    systemctl status "${SERVICE_NAME}" --no-pager --lines 20 >&2 || true
    exit 1
fi
