# touchpad/ — stopgap bcm5974 fix after sleep/resume (S3)

## What it does

A **systemd system-sleep** hook that forces a fresh probe of the Apple
touchpad (bcm5974, `05ac:0262`, WELLSPRING7_ANSI) after resume without a
reboot. This is **layer 0** (stopgap, userspace) — the proper fix is the
kernel patches `0016–0018` in `../kernel/` (the `-omnkp` kernel).

## Problem solved

**Symptom:** after suspend/resume (S3 deep) the touchpad stops working. The
device does NOT disappear from USB — `05ac:0262` is present and bound to
bcm5974, but has dead URBs: `bcm5974: mode switch failed` /
`resume error -5` (EIO).

**Root cause:** `bcm5974_driver` has no `.reset_resume` — after a USB reset
(the port loses its power session during S3) the interface goes into
`needs_binding` → race with the built-in `usbhid`; and
`USB_QUIRK_RESET_RESUME` in quirks.c does not cover `05ac:0262` (it only has
`05ac:021a`/appletouch). Upstream fix `fc1e8a6` is already in 7.1 but only
prevents the "bad trackpad package" symptom during normal operation — not
the EIO mode switch on resume.

**Why this mechanism works:** after S3 the device is usually NOT
re-enumerated, so a bare `modprobe` does not fire a probe (no device-add
event). The hook runs on resume: `modprobe bcm5974` → `sleep 1` (xHCI
finishes resuming) → **toggle `authorized` 0→1** on the device
(`usb_set_configuration` → fresh probe of all interfaces; evidence
`hub.c:2752/2767`), with escalation: `bConfigurationValue` →
unbind/bind → port `disable` 1→0 (full re-enumeration). Idempotent,
timeout < 12 s, always `exit 0`. The same effect ("toggling authorized
revives the touchpad") was verified live — it is exactly what the
`RESET_RESUME` quirk from patch 0016 does, so the kernel fix is guaranteed.

## Contents

`scripts/`:

- `install-sleep-hook.sh` — installer: copies the hook to
  `/usr/lib/systemd/system-sleep/010-bcm5974-rebind`; `--uninstall` removes it
- `sleep-bcm5974-rebind` — the hook itself (sh, POSIX): post-only bcm5974
  recovery (the `authorized`-toggle variant per the design-agent report
  `resume-recovery-design.md`)

## Use

```bash
./scripts/install-sleep-hook.sh             # install (applies from the next suspend-resume cycle, no reboot)
# ... test: suspend → resume → touchpad alive
./scripts/install-sleep-hook.sh --uninstall # remove
```

End-to-end dry-run: OK (post: toggle authorized → re-probe, bind =
bcm5974). First test on a real suspend: see NOTES in the source repo
(om-touchpad).

## Warnings

- **After moving to the `-omnkp` kernel (patches 0016–0018) the hook is
  redundant — uninstall it** (`install-sleep-hook.sh --uninstall`),
  otherwise two fixes do the same thing at once.
- **Do not go back to hook v1** (`pre: modprobe -r`): it was
  counterproductive — it left the interface unbound, and `modprobe` after
  resume does not fire a probe (the device does not re-enumerate → no add
  event).
- The hook is post-only on purpose; it acts only on the `05ac:0262`
  device (VID/PID in the script) — other WELLSPRING ids are not covered.
- Quirk 0016 adds only `05ac:0262` (verified on this machine); the other
  WELLSPRING ids (0259/025a/0268/0290) are intentionally not in the quirk.