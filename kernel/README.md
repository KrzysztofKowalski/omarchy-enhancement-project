# kernel/ — the `-omnkp` kernel (7.1.9) for MacBook Pro Late 2013

## What it does

A patch series on stable kernel **v7.1.9** building the
`7.1.9-omnkp-dirty` kernel (LOCALVERSION `-omnkp`, Limine entry `//omnkp`,
UKI `omarchy_omnkp.efi`). A full replacement for the `-nvkp` kernel from the
nv-kepler project: the same **0001–0015** series (nouveau/i915/gmux/applesmc)
plus new **0016–0020**.

## Problem solved

**Symptom:** after suspend/resume (S3 deep) the `05ac:0262`
(WELLSPRING7_ANSI) touchpad stays on USB but has dead URBs —
`bcm5974: mode switch failed` / `resume error -5`.

**Root cause:** `bcm5974_driver` has no `.reset_resume` — after a USB reset
the interface goes into `needs_binding` and races the built-in `usbhid`;
and `USB_QUIRK_RESET_RESUME` in quirks.c only covers `05ac:021a`, not
`05ac:0262`, so the device is not reset on resume (upstream fix `fc1e8a6`
is in 7.1 but does not prevent the EIO mode switch on resume). Plus dGPU
power-on fixes after S3: `nvkm_object_init` oops without a client (0019,
report 115) and dead MMIO at first power-ON (0020, report 116).

## Contents

`patches/` — series 0001–0020 (0001–0015 shared with the nv-kepler project;
0016–0020 new, verified with `git apply --check` against clean v7.1.9):

- `0001` — nouveau auto-reclock (ondemand GK107 policy; enabled via
  `nouveau.config=NvAutoClk=1` or `echo auto > debugfs pstate`)
- `0002–0005` — gmux/switcheroo: reinit callback, power-cycle, backlight
  min-to-off, eDP DPCD retry; `0006–0011` — nouveau runtime PM: hybrid
  scanout gate, GK107 reinit after power cut, D3hot fallback, PCIe link
  retrain/wait, GR idle gate
- `0012–0015` — nouveau timeouts/tweaks (PMU send-reply, pstate calc, MSI
  IRQ) + applesmc without backlight trigger
- `0016` — quirks.c: `USB_QUIRK_RESET_RESUME` for `05ac:0262`
- `0017` / `0018` — bcm5974: `.reset_resume` + `.pre_reset/.post_reset`
- `0019` — nouveau: guard `!object->client` in `nvkm_object_init/fini`
- `0020` — nouveau: refuse init on dead dGPU MMIO (switcheroo after S3)

`scripts/` — `setup-tree.sh` (seed `tmp/linux-omkp`, `--shared` clone from
nv-kepler or fresh from kernel.org), `patch-check.sh` (sequential
verification on a scratch worktree — 0006/0007/0008/0010/0011 touch the
same file, never per-patch), `build-om-kernel.sh` (sync tree → apply
patches idempotently, `.omnkp-patches.stamp` → config from
`/proc/config.gz` → build → modules_install → /boot → initramfs),
`fix-initramfs-omnkp.sh` (creates `/etc/mkinitcpio-omnkp.conf` with the
`encrypt` hook EXPLICIT + regenerates), `build-uki-omnkp.sh` (UKI + Limine
`protocol: efi` entry, embedded cmdline), `install-omnkp-entry.sh`
(historical fallback flow), **`build-om-wifi.sh`** (builds the Wi-Fi
modules — the `brcmfmac` BCM4360 series — against the omnkp tree; build
ownership moved here from `../om-wifi`), **`update-kernel.sh`** (update
flow to a new stable tag: phases 1–5, patch-check gate, Wi-Fi series
check). The build runs as the user (no-builds rule), privileged steps via
`sudo -n`; details: `scripts/README-SKRYPTY.md` (in this directory).

## First-time use

```bash
./scripts/setup-tree.sh
./scripts/patch-check.sh
sudo ./scripts/build-om-kernel.sh --full-tree --clean   # ~30–60 min
sudo ./scripts/fix-initramfs-omnkp.sh
sudo ./scripts/build-uki-omnkp.sh                       # verify the printed KREL!
sudo reboot                                             # Limine menu → //omnkp
```

Update to a new tag: `patch-check.sh --tag vX.Y.Z` →
`build-om-kernel.sh --kver=X.Y.Z --clean` → `build-uki-omnkp.sh` → reboot;
`uname -r` should read `7.1.9-omnkp-dirty` afterwards.

## Warnings

- **Bootloader:** `protocol: linux` on this MBP = black screen (vgacon on a
  disabled dGPU). Always use UKI (`protocol: efi`); chainloading does not
  hand over initramfs. After regenerating the UKI refresh the entry via
  `limine-entry-tool --add-uki` (the entry points at the initramfs copy on
  the ESP).
- **Initramfs:** `mkinitcpio -c <file>` does not see `mkinitcpio.conf.d/`
  drop-ins — the `encrypt` (LUKS) hook must be EXPLICIT in the `-omnkp`
  config.
- **Vermagic:** empty `CONFIG_LOCALVERSION` in the tree = modules won't load
  into `-omnkp`. **`CONFIG_MOUSE_BCM5974`** is not set in the working
  config — without enabling it, patches 0016–0018 won't make it into the
  build.
- **`linux-modules-cleanup.service`** moves hand-built modules to `.old/` —
  disable it (done by `install-omnkp-entry.sh`).