#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.

section() {
    printf '\n===== %s =====\n' "$1"
}

section "usbhid parameters (mousepoll should be 2 after a reboot)"
grep -H . /sys/module/usbhid/parameters/mousepoll 2>/dev/null

section "Valve hidraw devices: full report descriptors"
for hidraw_path in /sys/class/hidraw/hidraw*; do
    grep -q 28DE "${hidraw_path}/device/uevent" 2>/dev/null || continue
    hidraw_name="${hidraw_path##*/}"
    interface_name="$(grep HID_PHYS "${hidraw_path}/device/uevent" | sed 's|.*/||')"
    descriptor_hex="$(od -An -tx1 -v -w4096 "${hidraw_path}/device/report_descriptor" | tr -s ' ')"
    echo "-- ${hidraw_name} (${interface_name})"
    # "85 xx" is how a descriptor declares report ID xx (a rough hint; the full dump follows).
    echo "   report IDs: $(grep -o '85 [0-9a-f][0-9a-f]' <<< "${descriptor_hex}" | cut -d' ' -f2 | sort -u | tr '\n' ' ')"
    echo "  ${descriptor_hex}"
done

section "hidraw permissions"
ls -l /dev/hidraw*

section "Live reports"
echo ">>> Move the controller's stick and press buttons for the next 5 seconds..." > /dev/tty
python3 - <<'EOF'
import glob
import os
import select
import time

SAMPLE_SECONDS = 5
REPORTS_TO_SHOW = 3

names_by_file_descriptor = {}
for uevent_path in glob.glob('/sys/class/hidraw/hidraw*/device/uevent'):
    with open(uevent_path) as uevent_file:
        uevent_text = uevent_file.read()
    if '28DE' not in uevent_text:
        continue
    hidraw_name = uevent_path.split('/')[4]
    interface_name = next(line for line in uevent_text.splitlines()
                          if line.startswith('HID_PHYS')).rsplit('/', 1)[1]
    try:
        file_descriptor = os.open('/dev/' + hidraw_name, os.O_RDONLY | os.O_NONBLOCK)
        names_by_file_descriptor[file_descriptor] = f'{hidraw_name} ({interface_name})'
    except OSError as error:
        print(f'-- {hidraw_name} ({interface_name}): cannot open: {error.strerror}')

report_counts = {file_descriptor: 0 for file_descriptor in names_by_file_descriptor}
sample_reports = {file_descriptor: [] for file_descriptor in names_by_file_descriptor}
deadline = time.monotonic() + SAMPLE_SECONDS
while names_by_file_descriptor and (remaining_seconds := deadline - time.monotonic()) > 0:
    ready_file_descriptors, _, _ = select.select(list(names_by_file_descriptor), [], [], remaining_seconds)
    for file_descriptor in ready_file_descriptors:
        report = os.read(file_descriptor, 256)   # one read returns exactly one report
        report_counts[file_descriptor] += 1
        if len(sample_reports[file_descriptor]) < REPORTS_TO_SHOW:
            sample_reports[file_descriptor].append(report)

for file_descriptor, display_name in sorted(names_by_file_descriptor.items(), key=lambda item: item[1]):
    print(f'-- {display_name}: {report_counts[file_descriptor]} reports in {SAMPLE_SECONDS} s')
    for report in sample_reports[file_descriptor]:
        print(f'   {len(report):3} bytes: {report.hex(" ")}')
EOF
echo ">>> Done, sending..." > /dev/tty