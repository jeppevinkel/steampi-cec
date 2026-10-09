#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "mousepoll"
echo "current value:  $(cat /sys/module/usbhid/parameters/mousepoll)"
echo "/proc/cmdline:  $(grep -o 'usbhid[^ ]*' /proc/cmdline || echo none)"
echo "cmdline.txt:    $(grep -o 'usbhid[^ ]*' /boot/firmware/cmdline.txt || echo none)"
echo "usbhid built in: $(grep -c usbhid "/lib/modules/$(uname -r)/modules.builtin")"
echo "modprobe.d options:"
grep -rs mousepoll /etc/modprobe.d /lib/modprobe.d /usr/lib/modprobe.d || echo "  none"

section "Controller button map (report 0x42)"
python3 - <<'EOF'
import glob
import os
import select
import termios
import time

PUCK_ID = '000028DE:00001304'
MOUSE_COLLECTION_PREFIX = bytes([0x05, 0x01, 0x09, 0x02])   # slot interfaces start like this
CONTROLLER_REPORT_ID = 0x42
SETTLE_SECONDS = 1.5
CAPTURE_SECONDS = 1.5

BUTTON_NAMES = [
    'A', 'B', 'X', 'Y',
    'D-pad up', 'D-pad down', 'D-pad left', 'D-pad right',
    'Steam button', 'Menu (three lines)', 'View (two squares)',
    'Left bumper', 'Right bumper',
    'Left stick pushed fully right', 'Left stick pushed fully up',
]

terminal = open('/dev/tty', 'w')

def tell_user(message):
    terminal.write(message)
    terminal.flush()

def open_slot_interfaces():
    file_descriptors = []
    for uevent_path in sorted(glob.glob('/sys/class/hidraw/hidraw*/device/uevent')):
        with open(uevent_path) as uevent_file:
            if PUCK_ID not in uevent_file.read():
                continue
        with open(os.path.join(os.path.dirname(uevent_path), 'report_descriptor'), 'rb') as descriptor_file:
            if not descriptor_file.read().startswith(MOUSE_COLLECTION_PREFIX):
                continue
        hidraw_name = uevent_path.split('/')[4]
        file_descriptors.append(os.open('/dev/' + hidraw_name, os.O_RDONLY | os.O_NONBLOCK))
    return file_descriptors

def capture_reports(file_descriptors, seconds):
    controller_reports = []
    deadline = time.monotonic() + seconds
    while (remaining_seconds := deadline - time.monotonic()) > 0:
        ready_file_descriptors, _, _ = select.select(file_descriptors, [], [], remaining_seconds)
        for file_descriptor in ready_file_descriptors:
            report = os.read(file_descriptor, 256)
            if report and report[0] == CONTROLLER_REPORT_ID:
                controller_reports.append(report)
    return controller_reports

def constant_bits(reports):
    """{(byte_index, bit_index): value} for every bit that never changes across the reports."""
    if not reports:
        return {}
    report_length = min(len(report) for report in reports)
    bits = {}
    for byte_index in range(1, report_length):            # byte 0 is the report ID
        byte_values = {report[byte_index] for report in reports}
        for bit_index in range(8):
            bit_values = {(byte_value >> bit_index) & 1 for byte_value in byte_values}
            if len(bit_values) == 1:
                bits[(byte_index, bit_index)] = bit_values.pop()
    return bits

slot_file_descriptors = open_slot_interfaces()
if not slot_file_descriptors:
    print('No puck slot interfaces found (or permission denied).')
    raise SystemExit

tell_user('>>> Put the controller down and do NOT touch it...\n')
capture_reports(slot_file_descriptors, SETTLE_SECONDS)
baseline_reports = capture_reports(slot_file_descriptors, 2.0)
baseline_bits = constant_bits(baseline_reports)
print(f'Baseline: {len(baseline_reports)} reports, {len(baseline_bits)} stable bits')

for button_name in BUTTON_NAMES:
    tell_user(f'>>> Press and HOLD: {button_name} ... ')
    capture_reports(slot_file_descriptors, SETTLE_SECONDS)   # discards reports from before the press
    held_reports = capture_reports(slot_file_descriptors, CAPTURE_SECONDS)
    tell_user('release.\n')
    capture_reports(slot_file_descriptors, 1.0)

    held_bits = constant_bits(held_reports)
    changed_bits = sorted(position for position, value in held_bits.items()
                          if position in baseline_bits and baseline_bits[position] != value)
    description = ', '.join(f'byte {byte_index} bit {bit_index} -> {held_bits[(byte_index, bit_index)]}'
                            for byte_index, bit_index in changed_bits)
    print(f'{button_name:32} {description or "no stable change found"}')

# Lizard mode may have typed into this console while buttons were pressed; throw that away.
with open('/dev/tty') as terminal_input:
    termios.tcflush(terminal_input, termios.TCIFLUSH)
tell_user('>>> Done, sending...\n')
EOF