#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "mousepoll"
echo "current value:  $(cat /sys/module/usbhid/parameters/mousepoll)"
echo "/proc/cmdline:  $(grep -o 'usbhid[^ ]*' /proc/cmdline || echo none)"
echo "cmdline.txt:    $(grep -o 'usbhid[^ ]*' /boot/firmware/cmdline.txt || echo none)"

section "Controller: missing buttons and stick values (report 0x42)"
python3 - <<'EOF'
import glob
import os
import select
import termios
import time

PUCK_ID = '000028DE:00001304'
MOUSE_COLLECTION_PREFIX = bytes([0x05, 0x01, 0x09, 0x02])
CONTROLLER_REPORT_ID = 0x42
SETTLE_SECONDS = 1.5
CAPTURE_SECONDS = 1.5
BUTTON_BYTES = (2, 3, 4)                  # where the buttons live; bytes 30+ are sensor noise
STICK_OFFSETS = range(10, 30, 2)          # candidate signed 16-bit little-endian values

BUTTON_NAMES = ['A', 'Left bumper']
STICK_POSITIONS = [
    'Left stick at rest (hands off)',
    'Left stick pushed fully RIGHT',
    'Left stick pushed fully LEFT',
    'Left stick pushed fully UP',
    'Left stick pushed fully DOWN',
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

def capture_while_held(file_descriptors, instruction):
    tell_user(f'>>> {instruction} ... ')
    capture_reports(file_descriptors, SETTLE_SECONDS)       # discards reports from before
    held_reports = capture_reports(file_descriptors, CAPTURE_SECONDS)
    tell_user('release.\n')
    capture_reports(file_descriptors, 1.0)
    return held_reports

def set_button_bits(reports):
    """Bits in the button bytes that are 1 in every report."""
    return {(byte_index, bit_index)
            for byte_index in BUTTON_BYTES
            for bit_index in range(8)
            if reports and all((report[byte_index] >> bit_index) & 1 for report in reports)}

def median_signed_16(reports, offset):
    values = sorted(int.from_bytes(report[offset:offset + 2], 'little', signed=True) for report in reports)
    return values[len(values) // 2]

slot_file_descriptors = open_slot_interfaces()
if not slot_file_descriptors:
    print('No puck slot interfaces found (or permission denied).')
    raise SystemExit

tell_user('>>> Hands off the buttons (holding the controller is fine)...\n')
capture_reports(slot_file_descriptors, SETTLE_SECONDS)
baseline_button_bits = set_button_bits(capture_reports(slot_file_descriptors, 2.0))

for button_name in BUTTON_NAMES:
    held_reports = capture_while_held(slot_file_descriptors, f'Press and HOLD: {button_name}')
    newly_set_bits = sorted(set_button_bits(held_reports) - baseline_button_bits)
    description = ', '.join(f'byte {byte_index} bit {bit_index}' for byte_index, bit_index in newly_set_bits)
    print(f'{button_name:32} {description or "no button bit found"}')

print()
print(f'{"":32} ' + ' '.join(f'{offset:>7}' for offset in STICK_OFFSETS))
for position_name in STICK_POSITIONS:
    held_reports = capture_while_held(slot_file_descriptors, position_name)
    values = ' '.join(f'{median_signed_16(held_reports, offset):>7}' for offset in STICK_OFFSETS)
    print(f'{position_name:32} {values}')

with open('/dev/tty') as terminal_input:
    termios.tcflush(terminal_input, termios.TCIFLUSH)
tell_user('>>> Done, sending...\n')
EOF