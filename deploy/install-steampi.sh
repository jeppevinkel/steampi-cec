#!/usr/bin/env bash
# Sets up the SteamPi TV session on Raspberry Pi OS Lite (64-bit):
# Xorg + Openbox + Flex Launcher + Steam Link, an optional animated background,
# automatic login into the session on tty1, and a quiet boot.
#
# Safe to re-run: unchanged files are left alone, replaced files are backed up first.
#
# Usage:  sudo bash install-steampi.sh
#
# Optional files next to this script are picked up automatically:
#   background.mkv          looping background video (enables picom + mpv)
#   flex-launcher/          mirrors ~/.config/flex-launcher (config, icons, scripts)
#   update-steampi-cec.sh   the daemon updater; installed to ~ and run once

set -euo pipefail

readonly TARGET_USER="jeppe"
readonly FLEX_LAUNCHER_URL="https://github.com/complexlogic/flex-launcher/releases/download/v2.2/flex-launcher_2.2_arm64.deb"
readonly PACKAGES=(
    xserver-xorg-core xserver-xorg-input-libinput xinit x11-xserver-utils
    openbox picom mpv steamlink curl
)
readonly BOOT_CONFIG_PATH="/boot/firmware/config.txt"
readonly KERNEL_COMMAND_LINE_PATH="/boot/firmware/cmdline.txt"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
readonly TIMESTAMP

# Re-run as root if needed.
if [[ ${EUID} -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
working_directory="$(mktemp -d)"
chmod 755 "${working_directory}"   # lets apt read the downloaded .deb without warnings
trap 'rm -rf "${working_directory}"' EXIT

# ---------- Checks ----------
if [[ "$(dpkg --print-architecture)" != "arm64" ]]; then
    echo "This needs 64-bit Raspberry Pi OS (arm64)." >&2
    exit 1
fi
if ! id "${TARGET_USER}" > /dev/null 2>&1; then
    echo "User ${TARGET_USER} does not exist." >&2
    exit 1
fi
target_home="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
target_group="$(id -gn "${TARGET_USER}")"

# ---------- Helpers ----------
log_step() {
    printf '\n==> %s\n' "$1"
}

# Installs a staged file; keeps a timestamped backup if a different file is replaced.
install_staged_file() {
    local staged_path="$1" destination_path="$2" file_mode="$3" file_owner="$4"
    local file_group
    file_group="$(id -gn "${file_owner}")"

    if [[ -f "${destination_path}" ]] && cmp -s "${staged_path}" "${destination_path}"; then
        echo "    unchanged  ${destination_path}"
        return
    fi
    if [[ -f "${destination_path}" ]]; then
        cp -p "${destination_path}" "${destination_path}.bak-${TIMESTAMP}"
        echo "    backup     ${destination_path}.bak-${TIMESTAMP}"
    fi
    if [[ "${file_owner}" == "root" ]]; then
        mkdir -p "$(dirname "${destination_path}")"
    else
        # Created as the user, so the directories aren't left owned by root.
        runuser -u "${file_owner}" -- mkdir -p "$(dirname "${destination_path}")"
    fi
    install -m "${file_mode}" -o "${file_owner}" -g "${file_group}" \
        "${staged_path}" "${destination_path}"
    echo "    wrote      ${destination_path}"
}

# Same as install_staged_file, with the content read from stdin.
write_file() {
    local destination_path="$1" file_mode="$2" file_owner="$3"
    local staged_path
    staged_path="$(mktemp -p "${working_directory}")"
    cat > "${staged_path}"
    install_staged_file "${staged_path}" "${destination_path}" "${file_mode}" "${file_owner}"
}

# Boot files live on a FAT partition: only replace content, never ownership or mode.
replace_boot_file() {
    local staged_path="$1" destination_path="$2"
    if cmp -s "${staged_path}" "${destination_path}"; then
        echo "    unchanged  ${destination_path}"
        return
    fi
    cp "${destination_path}" "${destination_path}.bak-${TIMESTAMP}"
    echo "    backup     ${destination_path}.bak-${TIMESTAMP}"
    cat "${staged_path}" > "${destination_path}"
    echo "    wrote      ${destination_path}"
}

# ---------- Packages ----------
log_step "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y "${PACKAGES[@]}"

# ---------- pitv commands ----------
log_step "Installing pitv commands"
for command_name in pitv-update pitv-diag; do
    install_staged_file "${script_directory}/${command_name}" \
        "/usr/local/bin/${command_name}" 0755 root
done

log_step "Installing Flex Launcher"
flex_launcher_package="${working_directory}/$(basename "${FLEX_LAUNCHER_URL}")"
curl --fail --location --silent --show-error --retry 3 \
     --output "${flex_launcher_package}" "${FLEX_LAUNCHER_URL}"
apt-get install -y "${flex_launcher_package}"

# ---------- Flex Launcher config and background ----------
log_step "Configuring Flex Launcher"
flex_launcher_config_directory="${target_home}/.config/flex-launcher"
if [[ ! -d "${flex_launcher_config_directory}" ]]; then
    runuser -u "${TARGET_USER}" -- mkdir -p "${target_home}/.config"
    cp -r /usr/share/flex-launcher "${flex_launcher_config_directory}"
    chown -R "${TARGET_USER}:${target_group}" "${flex_launcher_config_directory}"
    echo "    copied     default config to ${flex_launcher_config_directory}"
fi
# Files in flex-launcher/ on the flash drive mirror ~/.config/flex-launcher.
flex_launcher_source_directory="${script_directory}/flex-launcher"
if [[ -d "${flex_launcher_source_directory}" ]]; then
    while IFS= read -r -d '' source_file; do
        relative_path="${source_file#"${flex_launcher_source_directory}/"}"
        if [[ "${relative_path}" == *.sh ]]; then
            file_mode=0755   # FAT drives don't keep the executable bit; set it here.
        else
            file_mode=0644
        fi
        install_staged_file "${source_file}" \
            "${flex_launcher_config_directory}/${relative_path}" "${file_mode}" "${TARGET_USER}"
    done < <(find "${flex_launcher_source_directory}" -type f -print0)

    # The restart loop in .xinitrc brings it straight back with the new config.
    if pkill -x flex-launcher; then
        echo "    restarted  flex-launcher"
    fi
fi

if [[ -f "${script_directory}/background.mkv" ]]; then
    log_step "Installing background video"
    install_staged_file "${script_directory}/background.mkv" \
        "${target_home}/background.mkv" 0644 "${TARGET_USER}"
fi

# ---------- Xorg ----------
log_step "Configuring Xorg"
write_file /etc/X11/xorg.conf.d/99-vc4-display.conf 0644 root <<'EOF'
# The Pi 5 has two DRM devices (v3d for 3D, vc4 for display).
# Make Xorg drive the display one. Harmless where it isn't needed.
Section "OutputClass"
    Identifier "vc4"
    MatchDriver "vc4"
    Driver "modesetting"
    Option "PrimaryGPU" "true"
EndSection
EOF

# ---------- Openbox ----------
log_step "Configuring Openbox"
write_file "${target_home}/.config/openbox/rc.xml" 0644 "${TARGET_USER}" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!-- Managed by install-steampi.sh. Kiosk config: one desktop, no keyboard shortcuts. -->
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops>
    <number>1</number>
  </desktops>
  <applications>
    <application class="*">
      <decor>no</decor>
    </application>
    <application name="steampi-background">
      <layer>below</layer>
      <focus>no</focus>
      <fullscreen>yes</fullscreen>
    </application>
  </applications>
</openbox_config>
EOF

# ---------- X session ----------
log_step "Configuring the X session"
write_file "${target_home}/.xinitrc" 0755 "${TARGET_USER}" <<'EOF'
#!/bin/sh
# Managed by install-steampi.sh.

# Disable X's own blanking; the daemon and CEC decide when the screen matters.
xset s off -dpms s noblank

if [ -f "$HOME/background.mkv" ]; then
    # Compositor, needed for the transparent launcher over the video.
    # --unredir-if-possible turns compositing off under fullscreen apps (Steam Link).
    picom --backend glx --vsync --unredir-if-possible &

    # Background video: starts paused; the daemon decides when it plays.
    mpv --loop-file=inf --no-audio --fullscreen --no-osc --no-input-default-bindings \
        --pause --x11-name=steampi-background \
        --input-ipc-server="$XDG_RUNTIME_DIR/steampi-background.sock" \
        "$HOME/background.mkv" &
fi

# Launcher after the background so it starts on top; restarted if it exits.
( while true; do flex-launcher; sleep 1; done ) &

exec openbox
EOF

# ---------- Start the session on tty1 ----------
log_step "Starting the session on tty1 login"
if [[ -f "${target_home}/.bash_profile" ]]; then
    login_profile_path="${target_home}/.bash_profile"
else
    # Don't create .bash_profile: bash would then stop reading .profile.
    login_profile_path="${target_home}/.profile"
fi
if grep -q '>>> steampi session >>>' "${login_profile_path}" 2> /dev/null; then
    echo "    unchanged  ${login_profile_path}"
else
    cat >> "${login_profile_path}" <<'EOF'

# >>> steampi session >>>
# Start the TV session on tty1 only; SSH and other consoles get a normal shell.
if [ -z "$DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    exec startx -- -nocursor > "$HOME/.startx.log" 2>&1
fi
# <<< steampi session <<<
EOF
    chown "${TARGET_USER}:${target_group}" "${login_profile_path}"
    echo "    appended   ${login_profile_path}"
fi
runuser -u "${TARGET_USER}" -- touch "${target_home}/.hushlogin"

write_file /etc/systemd/system/getty@tty1.service.d/autologin.conf 0644 root <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin ${TARGET_USER} --noissue %I \$TERM
EOF
systemctl daemon-reload   # takes effect on reboot; tty1 isn't restarted now

# ---------- Input devices ----------
log_step "Configuring input devices"
write_file /etc/udev/rules.d/60-steampi-input.rules 0644 root <<'EOF'
# Managed by install-steampi.sh.
# The CEC daemon (group input) creates the TV remote's virtual keyboard through uinput.
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
# Valve controllers: raw access for the logged-in user, same as Valve's steam-devices rules,
# so programs using SDL can talk to the Steam Controller directly.
SUBSYSTEM=="usb", ATTRS{idVendor}=="28de", MODE="0660", GROUP="input", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="28de", MODE="0660", GROUP="input", TAG+="uaccess"
KERNEL=="hidraw*", KERNELS=="*28DE:*", MODE="0660", GROUP="input", TAG+="uaccess"
EOF
write_file /etc/modules-load.d/steampi-uinput.conf 0644 root <<'EOF'
uinput
EOF
modprobe uinput
udevadm control --reload-rules
udevadm trigger

# ---------- Quiet boot ----------
log_step "Configuring a quiet boot"
staged_boot_config="${working_directory}/config.txt"
cp "${BOOT_CONFIG_PATH}" "${staged_boot_config}"
if ! grep -qx 'disable_splash=1' "${staged_boot_config}"; then
    printf '\n[all]\ndisable_splash=1\n' >> "${staged_boot_config}"
fi
replace_boot_file "${staged_boot_config}" "${BOOT_CONFIG_PATH}"

read -r -a original_kernel_arguments < "${KERNEL_COMMAND_LINE_PATH}"
updated_kernel_arguments=()
for kernel_argument in "${original_kernel_arguments[@]}"; do
    case "${kernel_argument}" in
        # Dropped, or re-added below with our values.
        console=tty1 | splash | quiet | logo.nologo | loglevel=* | vt.global_cursor_default=*) ;;
        *) updated_kernel_arguments+=("${kernel_argument}") ;;
    esac
done
updated_kernel_arguments+=(quiet loglevel=3 logo.nologo vt.global_cursor_default=0)
staged_kernel_command_line="${working_directory}/cmdline.txt"
printf '%s\n' "${updated_kernel_arguments[*]}" > "${staged_kernel_command_line}"
replace_boot_file "${staged_kernel_command_line}" "${KERNEL_COMMAND_LINE_PATH}"

# ---------- CEC daemon ----------
if [[ -f "${script_directory}/update-steampi-cec.sh" ]]; then
    log_step "Installing the CEC daemon"
    install_staged_file "${script_directory}/update-steampi-cec.sh" \
        "${target_home}/update-steampi-cec.sh" 0755 "${TARGET_USER}"
    if ! bash "${target_home}/update-steampi-cec.sh"; then
        echo "    WARNING: the daemon install failed; run ~/update-steampi-cec.sh later." >&2
    fi
fi

log_step "Done"
echo "Reboot to start the TV session:  sudo reboot"
echo "Text console after reboot:       Ctrl+Alt+F2"