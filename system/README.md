# system/ — survey, hybrid GPU, Chromium, backups

**MacBook Pro Late 2013** (MacBookPro11,3), Omarchy 4.0.0, Hyprland/Wayland,
Limine. CPU: Intel Core i7-4850HQ (Haswell). GPU: **iGPU Intel Iris Pro
5200** (`8086:0d26`, i915) + dGPU NVIDIA GT 750M (Kepler, nouveau).
"Crystal Well" is the name of a CPU package with eDRAM, not the GPU — the
graphics chip is the Iris Pro 5200.

## Baseline (survey 2026-08-23)

- The eDP panel is wired through `apple_gmux` to the **dGPU**: nouveau draws
  the desktop (card0), i915 (card1) hangs with no connectors; without
  reclocking (core ≤405 MHz) Hyprland got ~41.6 fps with gaps up to 49 ms,
  the cursor stays software (nouveau has no plane cursor).
- nouveau spam in dmesg = missing Kepler firmware; fixed by the
  `nouveau-fw` 340.108 package (AUR) → `nve7_fuc084/085` in
  `/usr/lib/firmware/nouveau/` (the msvld decoder works).
- RAM: 2×8 GB DDR3-1600 (model max), zram 15.5 GB zstd + swapfile; NVMe
  970 EVO Plus healthy; **fstrim.timer disabled**. CPU throttling at ~70 °C
  is an Apple SMC limit, not RAPL.
- Hyprland: animations OFF, `direct_scanout 1`, `repeat_rate 50` /
  `repeat_delay 200`. Hyprland 0.56 with a Lua config gotcha:
  `hyprctl keyword`/`dispatch` do not work — use `hyprctl eval`.

## Target GPU state

Iris Pro 5200 draws the desktop, GT 750M is off (0 W) or later CUDA-only
offload. Switching is possible **only at boot** (the gmux stands on the
dGPU; `vgaswitcheroo` at runtime = black screen). Mechanism: the EFI
variable `gpu-power-prefs` (NVRAM) + `apple_set_os.efi` chainload in
Limine — without it the MBP11,3 EFI disables the iGPU at every non-macOS
boot. Full plan: `PLAN-PANEL-NA-IRIS.md` (Etap 0 done; **do not select the
"Apple set OS" entry** until Etap 1 — the chainload itself switches the mux
and yields a black screen; recovery = cold reboot).

Note: `PLAN-PANEL-NA-IRIS.md` is Polish working documentation — kept local
(see the repo's `notes/` convention). The English summary lives here.

## Chromium — video acceleration

- Symptom: VP9/AV1 1080p+ eats 100% CPU and stutters. Root cause: the
  Chromium pipeline requires `vaExportSurfaceHandle(DRM_PRIME_2)` →
  dma-buf, and on nouveau (nvc0) that export **is not implemented** (libva
  test: error 36); everything decodes in software. VDPAU is dead.
- Hardware decoding on nouveau will not work; the real path is VA-API on
  the Iris after the panel switch (`libva-intel-driver` + `vainfo`).
  Meanwhile: `libva-utils` + the flags
  `--enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks` in
  `~/.config/chromium-flags.conf` (see `CHROMIUM-VIDEO.md`).

## What is in this directory

- `backups/` — config snapshots from before changes: `limine.conf.
  before-apple-entry` (Limine menu before the `apple_set_os.efi` chainload
  entry), `hyprland.lua.before-ds-off` (before direct scanout), `chromium
  -flags.conf.before-tuning` (before video tuning), `fullfan.sh.before-fix`
  + `cusfan.sh.before-fix` (fan scripts before the fix),
  `nvidia-470xx-utils.conf.original`. Restore: diff against the current
  file and copy after review — these are snapshots, not an installer.
- `fetch-nouveau-sources.sh` — fetches the complete nouveau driver sources
  for patching on the GT 750M (Kepler GK107/NVE7): kernel
  (drivers/gpu/drm/nouveau), Mesa (gallium nouveau + VA-API), libdrm.
  Shallow clone into `src/`, idempotent, no sudo (`SRC_DIR=...`).
- `mount/` — NAS mount helpers: `mount-smb.sh` (CIFS/SMB3 shares,
  credentials file at `~/.smbcred-*`) and `mount-sshfs.sh`. Edit the server
  address placeholder at the top.

## Warnings

- **Do not select the "Apple set OS" Limine entry** before Etap 1 — black
  screen at the EFI stage; recovery: cold reboot without selecting it.
- `vgaswitcheroo` needs debugfs mounted; `limine.conf` is auto-generated —
  keep custom entries in a hook that runs after regeneration.
- Open items from the survey: port 53317 listening on all interfaces,
  `asdcontrol NOPASSWD: ALL` in sudoers.d, Bluetooth soft-blocked, no
  `facetimehd` driver.