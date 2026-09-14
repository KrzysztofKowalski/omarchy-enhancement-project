# Mikrofon — Discord (web app) on Chromium cannot hear the user

MacBook Pro Late 2013, Omarchy: PipeWire + WirePlumber, Chromium, Discord
in the browser. Fix from 2026-09-01; this directory is a description only —
the fix itself is configuration steps.

## Symptom and root cause

The mic does not work in Discord (web app on Chromium) while audio output
(BT speaker) works fine. Discord's mic test can work while a voice call
does not; a system test recording shows peak 0.0.

Root cause: the default audio **source** in PipeWire pointed at
`bluez_input.…`, i.e. the BT speaker's "microphone". The speaker plays on
the **A2DP** profile, which has no mic — that source delivers only silence,
so Chromium recorded nothing. Additionally Chromium had been running
uninterrupted since before the broken state and held a stale device map
(Discord's localStorage also cached a `deviceId` of a nonexistent source).
Key diagnostic: **"but it works in Firefox!"** — if another browser sees
the mic, the system and PipeWire are 100% fine; Chromium's stale state is
the culprit.

## Step-by-step fix

1. **Default source → built-in microphone** (CS4208 codec,
   `alsa_input.pci-…analog-stereo`): `wpctl status` (find the node) →
   `wpctl set-default <id_of_builtin>`. The BT speaker profile stays A2DP
   (full output quality — do not move it to HFP "for the mic").
2. **Raise and unmute the built-in mic**: 36% → 75%
   (`wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 0.75`,
   `wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0`).
3. **Persist in WirePlumber** — the default source is saved in
   `~/.local/state/wireplumber/default-nodes`
   (`default.configured.audio.source` = builtin; the BT speaker only stays
   on the fallback list). Survives reboots.
4. **Repeat script** (user side): `~/.local/bin/fix-audio.sh` — after
   plugging in the BT speaker it restores mic = builtin (75%, unmuted) and
   output = A2DP speaker.
5. **Full Chromium restart** — last, decisive step: `pkill chromium`,
   then `uwsm-app -- chromium`. Tabs return on their own
   (`restore_on_startup: 1`); fresh audio device list, and in Discord pick
   the input manually if needed (the concrete mic, not "Default").

## Dead ends (to not waste time)

- Red mic icon in Discord = **self-mute** (unmute manually), not a system
  problem; the red dot on the Discord icon = unread notifications
  (Shift+Esc), not a mic error.
- The login indicator may not light up in a VC even while recording;
  `pgrep` catches your own commands containing "chromium" — verify via
  `hyprctl -j clients`.

## Diagnostics

```sh
pactl info                          # default source/sinks
wpctl status                        # devices, streams, filters
pactl list source-outputs short     # who is recording right now
pw-record --target=<serial> x.raw   # record a specific stream
~/.local/bin/fix-audio.sh           # run after plugging in the BT speaker
```

## Warnings

- A Chromium restart closes all sessions/windows — save your work before
  `pkill`.
- GUI processes started from an automated sandbox may die; verify via
  `hyprctl -j clients`. The HFP/HSP profile ruins output quality — the
  correct source is the built-in mic.