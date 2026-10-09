#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "Steam Link: polling-rate code"
grep -rn -i -B2 -A4 'poll' /usr/bin/steamlink "$HOME"/.local/share/SteamLink/*.sh 2>/dev/null

section "usbhid parameters"
grep -H . /sys/module/usbhid/parameters/* 2>/dev/null

section "Valve hidraw devices"
for hidraw_path in /sys/class/hidraw/hidraw*; do
    grep -q 28DE "${hidraw_path}/device/uevent" 2>/dev/null || continue
    echo "-- ${hidraw_path##*/}"
    grep -E 'HID_ID|HID_NAME|HID_PHYS' "${hidraw_path}/device/uevent"
    od -An -tx1 -N8 "${hidraw_path}/device/report_descriptor"
done