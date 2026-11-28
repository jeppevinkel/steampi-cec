#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.
#
# Currently: Wi-Fi and Steam Link streaming quality. Quick (about 15 s), no password needed.
# The longer power-saving measurement (off vs on, about 3 minutes) is in git history:
#   git show d81b177:deploy/diagnostics.sh
#
# The network name (SSID) and the router's MAC address are left out, in case the output is shared:
# a router's MAC address can be looked up in public Wi-Fi maps.

readonly PING_COUNT=50   # 0.2 s apart: 10 seconds

section() {
    printf '\n===== %s =====\n' "$1"
}

wifi_interface="$(iw dev 2>/dev/null | awk '/Interface/ { print $2; exit }')"
if [[ -z "${wifi_interface}" ]]; then
    echo "No Wi-Fi interface found (or iw isn't installed)."
else
    section "Wi-Fi connection (${wifi_interface})"
    # Leaves out the SSID and the "Connected to <MAC>" line.
    iw dev "${wifi_interface}" link | grep -E 'freq|signal|bitrate'
    iw dev "${wifi_interface}" info | grep -E 'channel|txpower'
    echo "power saving: $(iw dev "${wifi_interface}" get power_save | sed 's/.*: //')"

    section "Connection statistics since it connected"
    # Failures show how often packets couldn't be delivered even after retrying.
    iw dev "${wifi_interface}" station dump \
        | grep -E 'signal|bitrate|tx retries|tx failed|beacon loss|connected time|expected throughput'
fi

section "Latency to the router (${PING_COUNT} pings)"
router_address="$(ip route | awk '/^default/ { print $3; exit }')"
# Summary only: packet loss, and min/avg/max/mdev (mdev = how much it jumps around).
ping -q -c "${PING_COUNT}" -i 0.2 "${router_address}" | tail -n 2

section "Steam Link streaming quality (latest lines from the daemon log)"
# qual = share of packets that arrived; CaptureFailed = the PC couldn't capture the game (black
# screen without any network problem); the session summary gives frame loss for the whole stream;
# the exit code shows whether Steam Link crashed on the way out (139).
grep -E 'Lease (started|ended): steamlink|steamlink exited with code|\[steamlink\].*(qual|CaptureFailed|Session state|session complete|Video size)' \
    /home/jeppe/cec.log | tail -n 60
