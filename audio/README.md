# Audio — BT speaker "Zielony Krasnal": connected, no sound

MacBook Pro Late 2013, Omarchy/Arch: PipeWire + WirePlumber + BlueZ.
Script: `fix-bt-zielony-krasnal.sh` (fix confirmed 2026-08-24).

## Symptom and root cause

The Bluetooth speaker connects fine, the A2DP sink is active (transport
RUNNING, `pactl` shows the stream) and the speaker outputs **silence**.
Root cause: the speaker (A2DP sink) keeps its DAC muted until the source
reports a "Playing" status over **AVRCP**. BlueZ without a registered AVRCP
player always answers "Stopped" — the stream flows, the transport is
active, but the speaker deliberately stays silent. This is not a PipeWire
or profile problem; a single AVRCP message is missing.

## Fix

Force BlueZ to register a **dummy AVRCP player** — one property in the
WirePlumber config:

```ini
# ~/.config/wireplumber/wireplumber.conf.d/60-bluez-dummy-avrcp.conf
monitor.bluez.properties = {
    bluez5.dummy-avrcp-player = true
}
```

The `fix-bt-zielony-krasnal.sh` script does this end-to-end:

1. writes the config above to `~/.config/wireplumber/wireplumber.conf.d/`,
2. restarts `wireplumber.service` (user),
3. disconnects and re-connects the speaker so the profile comes up with the
   player already registered,
4. sets the BT sink default (`pactl set-default-sink`),
5. raises volume to 100% (a reconnect resets BT absolute volume),
6. plays a 3-second 440 Hz test tone (`paplay`) — if you hear it, the fix
   worked.

The fix is persistent: the speaker reconnects on its own after a reboot and
the player comes up with WirePlumber.

## Also in this directory

- `fix-audio-preview.sh` — restores audio previews in the file manager
  (Nautilus "space preview"): installs `gst-plugins-good` (wavparse/
  pulsesink for WAV) and `gst-plugins-bad` (openmptdec/modplug for
  .s3m/.MOD tracker files). Idempotent, knows when the plugins are already
  present.

## Use

```sh
./fix-bt-zielony-krasnal.sh
```

Needs a working user session (user systemd, PipeWire) and `ffmpeg` for the
test tone. The speaker MAC and node id are in the script header
(`wpctl status | grep -A2 Sinks`).

## Diagnostics when the silence returns

Check in this order (the script also prints these at the end):

- `pactl get-default-sink` — must point at a `bluez_output.…` sink,
- `pactl list sink-inputs` — the stream must not be Corked/Mute,
- `pactl list cards | grep -A30 bluez_card.` — active profile =
  `a2dp-sink` (not headset); `wpctl status` — node id and default sink,
- transport state: `gdbus introspect` on the BlueZ device object
  (`State = active`, `Volume > 0`).

## Warnings

- The script restarts WirePlumber — all audio streams in the session drop
  for a moment (including the built-in speakers).
- `bluetoothctl disconnect/connect` targets the exact MAC in the script
  header; change it for a different speaker.
- Do not switch the profile to HFP/HSP "just in case" — it lowers quality;
  with another speaker update the MAC and node id in the header.
- The `60-bluez-dummy-avrcp.conf` config is persistent — removing it
  reverts the fix.