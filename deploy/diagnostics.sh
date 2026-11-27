#!/usr/bin/env bash
# Collects whatever we're currently investigating. Edit on the PC, push, run pitv-diag on the Pi.
#
# Currently: Wi-Fi. How good the Pi's connection is, and what Wi-Fi power saving costs or saves.
# Before running: close Steam Link and turn the Steam Controller off (Steam+Y), so nothing else
# keeps the Pi busy. Takes about 3 minutes. May ask for your password (sudo) to switch power saving.
#
# The ntfy topic is public, so the network name (SSID) and the router's MAC address are left out:
# a router's MAC address can be looked up in public Wi-Fi maps.

readonly ROUNDS=2                  # off/on pairs, alternated so slow drift (temperature) evens out
readonly SETTLE_SECONDS=10         # after switching, before measuring
readonly MEASURE_SECONDS=30        # power samples per state and round, one per second
readonly PING_COUNT=50             # per state, 0.2 s apart: 10 seconds

section() {
    printf '\n===== %s =====\n' "$1"
}

tell_user() {
    { printf '>>> %s\n' "$1" > /dev/tty; } 2> /dev/null || true
}

if ! command -v iw > /dev/null; then
    tell_user "Installing iw (the Wi-Fi tool) first..."
    sudo apt-get install -y iw > /dev/null
fi
wifi_interface="$(iw dev 2>/dev/null | awk '/Interface/ { print $2; exit }')"
if [[ -z "${wifi_interface}" ]]; then
    echo "No Wi-Fi interface found."
    exit 0
fi

# ---------- Connection ----------
section "Wi-Fi connection (${wifi_interface})"
# Leaves out the SSID and the "Connected to <MAC>" line.
iw dev "${wifi_interface}" link | grep -E 'freq|signal|bitrate'
iw dev "${wifi_interface}" info | grep -E 'channel|txpower'
echo "power saving now: $(iw dev "${wifi_interface}" get power_save | sed 's/.*: //')"

section "Connection statistics since it connected"
# Retries and failures show how often packets had to be sent again.
iw dev "${wifi_interface}" station dump \
    | grep -E 'signal|bitrate|tx retries|tx failed|beacon loss|connected time|expected throughput'

section "NetworkManager's power saving setting (0/absent = default, 1 = leave alone, 2 = off, 3 = on)"
grep -rhs 'powersave' /etc/NetworkManager/ || echo "not set"

# ---------- Power saving: measurement ----------
original_power_save="$(iw dev "${wifi_interface}" get power_save | sed 's/.*: //')"
restore_power_save() {
    sudo iw dev "${wifi_interface}" set power_save "${original_power_save}"
}

set_power_save() {
    sudo iw dev "${wifi_interface}" set power_save "$1"
}

# One reading of the PMIC: "<board watts> <wifi watts>". The board total is the sum over all
# rails the PMIC measures; the Wi-Fi part is the rail(s) with WL in the name (3V7_WL_SW).
read_power() {
    vcgencmd pmic_read_adc | awk '
        {
            rail = $1; sub(/_[AV]$/, "", rail)
            value = $2; sub(/.*=/, "", value); sub(/[AV]$/, "", value)
        }
        /current/ { current[rail] = value }
        /volt/    { voltage[rail] = value }
        END {
            for (rail in current) {
                if (!(rail in voltage)) continue
                watts = current[rail] * voltage[rail]
                board += watts
                if (rail ~ /WL/) wifi += watts
            }
            printf "%.4f %.4f\n", board, wifi
        }'
}

measured_samples="$(mktemp)"
# Also on Ctrl+C: put power saving back the way it was.
trap 'restore_power_save; rm -f "${measured_samples}"' EXIT

tell_user "Measuring power: about $(( ROUNDS * 2 * (SETTLE_SECONDS + MEASURE_SECONDS) / 60 )) minutes. Leave the Pi alone..."
for (( round = 1; round <= ROUNDS; round++ )); do
    for state in off on; do
        set_power_save "${state}"
        sleep "${SETTLE_SECONDS}"
        for (( sample = 0; sample < MEASURE_SECONDS; sample++ )); do
            echo "${state} $(read_power)" >> "${measured_samples}"
            sleep 1
        done
    done
done

section "Power with Wi-Fi power saving off vs on (idle, ${ROUNDS} alternating rounds)"
awk '
    { board[$1] += $2; wifi[$1] += $3; boardSquares[$1] += $2 * $2; count[$1]++ }
    END {
        printf "%-6s %8s %14s %12s %8s\n", "state", "samples", "board (W)", "board +/-", "wifi (W)"
        for (state in count) {
            mean = board[state] / count[state]
            variance = boardSquares[state] / count[state] - mean * mean
            spread = sqrt(variance > 0 ? variance : 0)   # rounding can make it a hair below 0
            printf "%-6s %8d %14.3f %12.3f %8.3f\n", state, count[state], mean, spread, wifi[state] / count[state]
        }
        if (("off" in count) && ("on" in count)) {
            printf "\nsaving with power saving on: board %.3f W, wifi rail %.3f W\n",
                board["off"] / count["off"] - board["on"] / count["on"],
                wifi["off"] / count["off"] - wifi["on"] / count["on"]
        }
    }' "${measured_samples}"

# ---------- Power saving: latency ----------
section "Latency to the router with power saving off vs on (${PING_COUNT} pings each)"
router_address="$(ip route | awk '/^default/ { print $3; exit }')"
for state in off on; do
    set_power_save "${state}"
    sleep 3
    echo "-- power saving ${state}"
    # Summary only: packet loss, and min/avg/max/mdev (mdev = how much it jumps around).
    ping -q -c "${PING_COUNT}" -i 0.2 "${router_address}" | tail -n 2
done

tell_user "Done, power saving set back to '${original_power_save}'. Sending..."
