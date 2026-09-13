#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Fix: Bluetooth speaker "Zielony Krasnal" connected, sink RUNNING, but SILENT.
# Applies to: MBP late 2013 / Omarchy / Arch, PipeWire 1.6.8 + WirePlumber.
# Root cause: the speaker (A2DP sink) keeps its DAC muted until the source
# reports AVRCP "Playing". BlueZ without a registered player always answers
# "Stopped" → the stream plays, but there is no sound. Fix = dummy AVRCP player.
# Confirmed working 2026-08-24.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SPEAKER_MAC="AA:BB:CC:DD:EE:FF"
WP_CONF_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
CONF_FILE="$WP_CONF_DIR/60-bluez-dummy-avrcp.conf"
SINK="bluez_output.AA_BB_CC_DD_EE_FF.1"
WP_NODE=73   # node id of the Zielony Krasnal (change if different: `wpctl status | grep -A2 Sinks`)

echo "==> 1. Creating WirePlumber config: dummy AVRCP player"
mkdir -p "$WP_CONF_DIR"
cat > "$CONF_FILE" <<'EOF'
# Force BlueZ to register a dummy AVRCP player so the source reports
# playback status "Playing" to A2DP sink devices (speakers/receivers)
# that keep their DAC muted until they see AVRCP Playing.
# Fixes "connected, sink RUNNING, transport active, but no sound".
monitor.bluez.properties = {
    bluez5.dummy-avrcp-player = true
}
EOF
echo "    saved: $CONF_FILE"

echo "==> 2. Restarting WirePlumber (to load the new config)"
systemctl --user restart wireplumber.service
sleep 2
systemctl --user is-active wireplumber.service

echo "==> 3. Reconnecting the speaker (to apply the profile with the new player)"
bluetoothctl disconnect "$SPEAKER_MAC" || true
sleep 2
bluetoothctl connect "$SPEAKER_MAC"
sleep 3

echo "==> 4. Make sure the Zielony Krasnal is the default sink"
pactl set-default-sink "$SINK"

echo "==> 5. Volume at 100% (reconnect resets the BT absolute volume)"
wpctl set-volume "$WP_NODE" 1.0
pactl set-sink-volume "$SINK" 100%

echo "==> 6. Test tone (3s sine 440Hz) — you will hear sound = OK"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=3" \
    -ar 48000 -ac 2 -f wav /tmp/bt_test.wav
paplay --device="$SINK" /tmp/bt_test.wav
echo "    done."

echo
echo "==> Diagnostics (when it is silent again — check in this order):"
echo "    pactl list sink-inputs        # Corked/Mute/Volume per stream"
echo "    pactl list cards | grep -A30 bluez_card.   # Active Profile = a2dp-sink"
echo "    pactl get-default-sink        # must be bluez_output...."
echo "    wpctl status | grep -A3 Sinks # node id + default (*)"
echo "    # transport state:"
echo "    gdbus introspect --system --dest org.bluez \\"
echo "      --object-path /org/bluez/hci0/dev_${SPEAKER_MAC//:/_} --recurse \\"
echo "      | grep -iE 'State =|Volume ='   # State=active, Volume>0"
echo
echo "Done. The fix is permanent — after a restart the speaker reconnects on"
echo "its own (bluez5.auto-connect) and the AVRCP player is active from WirePlumber start."