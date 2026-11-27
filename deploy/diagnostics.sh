#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.
#
# Currently: the controller hand-off to Steam Link. Run it from the text console (Ctrl+Alt+F2)
# WHILE Steam Link is running.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "Daemon and control socket"
systemctl is-active steampi-cec
ls -la /run/steampi 2>&1

section "Running apps (launch clients and Steam Link)"
ps -eo pid,pgid,user,args | grep -E 'steampi-cec --launch|steamlink|[s]hell' | grep -v grep

section "Puck hidraw devices: permissions, and which processes have them open"
for hidraw_path in /sys/class/hidraw/hidraw*; do
    grep -q '000028DE:00001304' "${hidraw_path}/device/uevent" 2>/dev/null || continue
    device_path="/dev/${hidraw_path##*/}"
    interface_name="$(grep HID_PHYS "${hidraw_path}/device/uevent" | sed 's|.*/||')"
    echo "-- ${device_path} (${interface_name})"
    ls -l "${device_path}"
    getfacl -p "${device_path}" 2>/dev/null | grep -E '^user:'
    for fd_path in /proc/[0-9]*/fd/*; do
        if [ "$(readlink "${fd_path}" 2>/dev/null)" = "${device_path}" ]; then
            process_id="${fd_path#/proc/}"; process_id="${process_id%%/*}"
            echo "   open by ${process_id}: $(tr '\0' ' ' < "/proc/${process_id}/cmdline" | cut -c1-80)"
        fi
    done
done

section "Groups of the TV session user"
id jeppe

section "Recent daemon log: leases, controller modes, Steam Link output"
grep -E 'Lease|Controller: mode|lizard|Closing|exited with code|Control socket|\[steamlink\]' \
    /home/jeppe/cec.log | tail -n 80
