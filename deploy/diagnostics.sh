#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.
#
# Currently: the controller hand-off to Steam Link. Run it from the text console (Ctrl+Alt+F2)
# WHILE Steam Link is running, so the virtual gamepad exists.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "Daemon and control socket"
systemctl is-active steampi-cec
ls -la /run/steampi 2>&1

section "Running apps (launch clients and Steam Link)"
ps -eo pid,pgid,user,args | grep -E 'steampi-cec --launch|steamlink|[s]hell' | grep -v grep

section "Virtual devices created by the daemon"
awk 'BEGIN { RS = ""; ORS = "\n\n" } /SteamPi/' /proc/bus/input/devices

section "Virtual gamepad: what udev made of it (needs ID_INPUT_JOYSTICK and uaccess)"
for event_path in /sys/class/input/event*; do
    if grep -q 'SteamPi Steam Controller' "${event_path}/device/name" 2>/dev/null; then
        device_path="/dev/input/${event_path##*/}"
        ls -l "${device_path}"
        udevadm info --query=property --name="${device_path}" | grep -E 'ID_INPUT|TAGS|ID_VENDOR_ID|ID_MODEL_ID'
    fi
done

section "Groups of the TV session user"
id jeppe

section "Recent daemon log: leases and controller modes"
grep -E 'Lease|Controller: mode|Controller: virtual|lizard|Closing|exited with code|Control socket|\[steamlink\]' \
    /home/jeppe/cec.log | tail -n 80
