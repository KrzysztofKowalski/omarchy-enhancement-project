# om-kernel scripts (scripts/)

A set of scripts to build and install the `-omnkp` kernel (LOCALVERSION) — a full
replacement for `-nvkp` from nv-kepler, on the same patch series 0001–0015 PLUS the new
bcm5974 touchpad patches (0016–0018). The build is run by the USER (not agents).

All scripts: bash, `set -euo pipefail`, `chmod +x`.
Privileged steps go through `sudo -n` (passwordless).

---

## Call order (first time)

```bash
cd ~/Projects/om-kernel

# 1. Prepare the source tree (once) — clone --shared from nv-kepler, checkout v7.1.9
./scripts/setup-tree.sh              # options: --kver=7.1.9, --fresh

# 2. Check the patches SEQUENTIALLY on a scratch worktree (without touching the nv-kepler tree)
./scripts/patch-check.sh

# 3. Full build + install (modules, /boot, initramfs)
sudo ./scripts/build-om-kernel.sh --full-tree --clean
#    (approx. 30–60 min; --no-install = build only, without installing)

# 4. Initramfs config (creates /etc/mkinitcpio-omnkp.conf) + regeneration
sudo ./scripts/fix-initramfs-omnkp.sh

# 5. UKI + Limine //omnkp entry (protocol: efi — like Omarchy)
sudo ./scripts/build-uki-omnkp.sh

# 6. Reboot and pick 'omnkp' in the Limine menu
sudo reboot
```

## Order (update when a new upstream kernel appears)

```bash
# 0. Verify the patches against the new tag (rebase preview) — SEQUENTIALLY
./scripts/patch-check.sh --tag vX.Y.Z

# 1. Sync the tree (fetch stable + checkout -f) + rebuild + install
sudo ./scripts/build-om-kernel.sh --kver=X.Y.Z --clean

# 2. UKI + Limine entry (KREL auto = newest *-omnkp* in /usr/lib/modules)
#    VERIFY the printed KREL before rebooting!
sudo ./scripts/build-uki-omnkp.sh

# 3. Reboot
sudo reboot
```

---

## Script descriptions

### `setup-tree.sh`
Seeds the source tree. If `tmp/linux-omkp` does not exist — `git clone
--shared $HOME/Projects/nv-kepler/tmp/linux-nouveau` (shared objects,
saves ~1GB of downloading; the nv-kepler tree is READ-ONLY), adds the `stable`
remote, fetches the tag + `checkout -f`. Fallback (no source or `--fresh`):
a fresh `git clone --filter=blob:none` from stable (kernel.org). Idempotent —
re-running does no harm. Options: `--kver=7.1.9` (default), `--fresh`.

### `patch-check.sh`
Verifies whether the 0001–0018 patches apply on a given tag. Scratch
worktree (`tmp/linux-patch-check`), materializing ONLY the files touched
by the patches (`git show <tag>:<path>`), then SEQUENTIAL `git apply
--check` + `git apply` — each patch's context sees the changes of the previous ones
(NOT per-patch on a clean tree). Arguments: `--series DIR`, `--tree DIR`,
`--tag vX.Y.Z`, `--check-only` (only check, no application — weaker
verification), `--log PATH`, `--worktree PATH`. Log by default:
`tmp/patch-check.log`.

### `build-om-kernel.sh`
The main script. Order: tree sync (`--kver`) → `make mrproper` (`--clean`)
→ apply the patches idempotently (stamp `.omnkp-patches.stamp`) → config
from `/proc/config.gz` + `olddefconfig` → `make -j$(nproc) LOCALVERSION=-omnkp`
(log only from make: `tmp/build-om-kernel.log`) → `kernelrelease` →
`modules_install` → /boot (vmlinuz-linux-omnkp, System.map-linux-omnkp) →
`mkinitcpio -k $KREL -c /etc/mkinitcpio-omnkp.conf` → hint to
`build-uki-omnkp.sh`. PATCH_NAMES is built by globbing `0*-*.patch`
(order 0001→0018). Options: `--kver=`, `--jobs=`, `--clean`, `--no-install`,
`--full-tree`. Thermal: reclocked [compiler] spins the fans to 100% itself.
If `tmp/linux-omkp` is missing — the message points to `./scripts/setup-tree.sh`.

### `fix-initramfs-omnkp.sh`
Creates `/etc/mkinitcpio-omnkp.conf` (udev + legacy `encrypt` hook + `resume`,
FILES with `/crypto_keyfile.bin` when auto-unlock is set up) — content exactly
like `/etc/mkinitcpio-nvkp.conf` on this machine, with the name changed. Handles
the trap: `mkinitcpio -c <file>` does NOT see the `/etc/mkinitcpio.conf.d/`
drop-ins (which is why the full hook set is explicit in the config). Regenerates
the initramfs `/boot/initramfs-linux-omnkp.img`, verifies dm-crypt in the image
and refreshes the copy on the ESP via `limine-entry-tool --add-kernel`
(fallback flow). Does NOT rebuild the kernel. KREL is given via the env
`KREL=...` or auto-detected as the newest `*-omnkp*` in `/usr/lib/modules`.

### `build-uki-omnkp.sh`
Builds the UKI `/boot/EFI/Linux/omarchy_omnkp.efi` (the `omarchy_*` prefix —
omarchy's snapper makes snapshots) and the Limine `//omnkp` entry
(`limine-entry-tool --add-uki`) with `protocol: efi`. It writes the cmdline
(verbose) to `tmp/uki-build/omnkp-cmdline.txt` and EMBEDS it in the UKI; after
adding the entry it removes the `cmdline:` line from the `//omnkp` entry
(awk over the entry scope — NOT sed over the whole limine.conf; lesson), so
systemd-stub uses the embedded cmdline instead of the drop-in `quiet`. It removes
the old broken `//linux-omnkp` entry (protocol: linux). KREL auto = the newest
`*-omnkp*` (env `KREL` overrides) — VERIFY the printed KREL.

### `install-omnkp-entry.sh`
Fallback flow (historical): entry via `limine-entry-tool --add-kernel`
(protocol: linux) under `/+Omarchy` + backup of limine.conf, removal of the
broken manual `/+Linux-omnkp` entry, disabling `linux-modules-cleanup.service`
(otherwise it eats the omnkp modules at boot) and restoring the modules from
`/usr/lib/modules/.old/`. The main omnkp entry is the UKI (build-uki-omnkp.sh) —
this script is only for when the old flow is needed.

---

## Traps handled (lessons from nv-kepler)

- **PROJ:** `PROJ="$(cd "$(dirname "$0")/.." && pwd)"` — the script lives in scripts/.
- **`cmd | grep -q` + pipefail:** `grep -q` closes the pipe → SIGPIPE 141 → false
  negative. In conditional checks use temporary files / `[ -d ]` / `sudo -n test`
  (no pipe); for removing the `//linux-omnkp` entry, capture the `--tree` output
  into a variable.
- **`awk file | tee file`:** tee truncates the input before awk reads it → NEVER;
  go through tmp + `cp`.
- **`sudo -n test` for /boot:** the ESP `fmask=0077` = root-only → a normal user
  has no read access; a plain `[ -f ]` would give a false "no Limine".
- **Verify the patches SEQUENTIALLY** (patch-check + build step 2), not
  per-patch on a clean tree — 0006/0007/0008/0010/0011 touch the same file.
- **`mkinitcpio -c <file>` skips the mkinitcpio.conf.d/ drop-ins** — full config
  in `/etc/mkinitcpio-omnkp.conf`.
- **`--add-kernel` refreshes the copy on the ESP**, not `/boot/initramfs-...` —
  otherwise boot will use the old image.
- **linux-modules-cleanup.service** moves hand-built modules to `.old/` →
  disable (install-omnkp-entry.sh step 4).
- **Note:** `CONFIG_MOUSE_BCM5974` is not set in the running config —
  build-om-kernel.sh reports this in step 3 and suggests how to enable it
  (otherwise patches 0016–0018 will not enter the build).

## Verification after boot

```bash
uname -r                      # → 7.1.9-omnkp-dirty
lsmod | grep -E 'nouveau|i915'
journalctl -b | grep -iE 'bcm5974.*(error|fail)'
```