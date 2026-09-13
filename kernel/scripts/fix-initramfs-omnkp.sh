#!/bin/bash
# fix-initramfs-omnkp.sh — fix the linux-omnkp initramfs (add the encrypt hook / dm-crypt)
# WITHOUT rebuilding the kernel. Counterpart of fix-initramfs-nvkp.sh for om-kernel: kernel build
# step 8 uses -c /etc/mkinitcpio-omnkp.conf — if the config is missing / has no
# encrypt, LUKS will not be unlocked → root does not come up → black screen.
#
# NOTE (lesson from nv-kepler): `mkinitcpio -c <file>` does NOT read the drop-ins
# /etc/mkinitcpio.conf.d/ (omarchy_hooks.conf adds encrypt to the system
# config) — that is why the full hook set is listed EXPLICITLY here in a separate config.
#
# Full image (without autodetect) = 948MB — does NOT fit on the 2GB ESP.
# The config uses autodetect + encrypt → ~230MB.
#
# What it does: 1) writes /etc/mkinitcpio-omnkp.conf (udev + autodetect + encrypt)
#          2) regenerates /boot/initramfs-linux-omnkp.img with this config
#          3) verifies dm-crypt/dm-mod/cryptsetup in the image
#          4) limine-entry-tool --add-kernel — refreshes the COPY on the ESP (fallback flow;
#             the main omnkp entry is the UKI from build-uki-omnkp.sh)
#          5) shows --tree.
# Reboot and picking an entry in the Limine menu — your call.
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
# KREL auto: newest *-omnkp* directory in /usr/lib/modules (like build-uki-omnkp);
# env KREL overrides.
KREL="${KREL:-$(cd /usr/lib/modules 2>/dev/null && ls -1d -- *-omnkp* 2>/dev/null | sort -V | tail -1 || true)}"
[ -n "$KREL" ] || { echo "ERROR: no *-omnkp* directory in /usr/lib/modules — run ./scripts/build-om-kernel.sh first" >&2; exit 1; }
KNAME="linux-omnkp"              # FAT32-safe kernel entry name
CONF="/etc/mkinitcpio-omnkp.conf"
INITRAMFS="/boot/initramfs-${KNAME}.img"
VMLINUZ="/boot/vmlinuz-${KNAME}"
MODULES_DIR="/usr/lib/modules/${KREL}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

bold "=== fix-initramfs-omnkp.sh — initramfs with the encrypt hook (no rebuild) ==="
echo "Kernel: ${KREL}  (modules: ${MODULES_DIR})"
echo

# --- preliminary checks ------------------------------------------------------
if ! sudo -n true 2>/dev/null; then
  red "ERROR: sudo -n does not work (passwordless sudo required)."
  exit 1
fi
if ! command -v mkinitcpio >/dev/null 2>&1; then
  red "ERROR: mkinitcpio does not exist."
  exit 1
fi
if ! sudo -n test -d "${MODULES_DIR}"; then
  red "ERROR: ${MODULES_DIR} does not exist — omnkp modules are not installed."
  exit 1
fi
if ! sudo -n test -f "${MODULES_DIR}/kernel/drivers/md/dm-crypt.ko.zst"; then
  red "ERROR: dm-crypt.ko.zst missing in ${MODULES_DIR}/kernel/drivers/md/."
  red "       The modules ARE on disk — this check should not fail. Check manually."
  exit 1
fi
if ! sudo -n test -f /usr/lib/initcpio/hooks/encrypt; then
  red "ERROR: the encrypt hook does not exist (/usr/lib/initcpio/hooks/encrypt)."
  exit 1
fi
green "OK: modules + encrypt hook + mkinitcpio present."
echo

# --- 1. mkinitcpio-omnkp.conf config -----------------------------------------
bold "[1/5] ${CONF} (udev + autodetect + encrypt)"
# The working UKI (omarchy_linux.efi) uses udev + the legacy encrypt hook — it handles
# cryptdevice=PARTUUID=...:root. systemd-cryptsetup-generator only knows rd.luks.*,
# so the systemd hook will NOT unlock LUKS.
# Full image (without autodetect) = 948MB — too big for the 2GB ESP. autodetect scans
# sysfs (nvme/i915/nouveau/usb) + encrypt adds dm-crypt → ~230MB, fits.
# CONTENT = exactly like /etc/mkinitcpio-nvkp.conf on this machine (udev + autodetect
# + encrypt + FILES keyfile) — with the kernel name changed in the comment. If LUKS
# auto-unlock is set up (/crypto_keyfile.bin exists), FILES contains the keyfile.
if [ -f /crypto_keyfile.bin ]; then
  FILES_COMMENT="# FILES: /crypto_keyfile.bin — LUKS auto-unlock (like /etc/mkinitcpio-nvkp.conf)."
  FILES_LINE="FILES=(/crypto_keyfile.bin)"
else
  FILES_COMMENT="# FILES: empty — no auto-unlock (the keyfile is added by setup-luks-autounlock.sh)."
  FILES_LINE="FILES=()"
fi
sudo -n tee "${CONF}" >/dev/null <<EOF
# mkinitcpio config for the linux-omnkp kernel (${KREL})
# udev + legacy encrypt hook (cryptdevice=) + autodetect (small image ~230MB).
# Full image (without autodetect) = 948MB — too big for the 2GB ESP.
# NOTE: mkinitcpio -c ${CONF} does NOT read drop-ins from /etc/mkinitcpio.conf.d/ —
# that is why the full hook set is listed here EXPLICITLY (lesson from nv-kepler).
${FILES_COMMENT}
MODULES=()
BINARIES=()
${FILES_LINE}
HOOKS=(base udev autodetect microcode modconf kms keyboard keymap consolefont block encrypt filesystems fsck resume)
EOF
green "    Written ${CONF} (autodetect + encrypt)."
echo

# --- 2. initramfs regeneration -------------------------------------------------
bold "[2/5] mkinitcpio -k ${KREL} -c ${CONF} -g ${INITRAMFS}"
echo "    (this is NOT a kernel rebuild — only the initramfs, ~1-2 min)"
sudo -n mkinitcpio -k "${KREL}" -c "${CONF}" -g "${INITRAMFS}"
sudo -n test -f "${INITRAMFS}" || { red "ERROR: the initramfs was not created."; exit 1; }
green "    Initramfs generated: ${INITRAMFS}"
echo

# --- 3. verification of dm-crypt in the image ---------------------------------
# Note (lesson from nv-kepler): NEVER `cmd | grep -q` + pipefail — grep -q closes the
# pipe after a hit → cmd gets SIGPIPE (141) → false FAIL. Instead: capture the whole
# output into a variable and grep it without a pipe (here-string).
bold "[3/5] Verifying dm-crypt/dm-mod/cryptsetup in the image"
lsinit_out="$(sudo -n lsinitcpio "${INITRAMFS}" 2>/dev/null)" \
  || { red "    ERROR: lsinitcpio could not read ${INITRAMFS} — check the image manually."; exit 1; }
if grep -qE 'dm-crypt|dm-mod' <<<"${lsinit_out}"; then
  green "    OK: dm-crypt/dm-mod in the image."
  grep -E 'dm-crypt|dm-mod|cryptsetup' <<<"${lsinit_out}" | sed 's/^/      /'
else
  red "    ERROR: dm-crypt NOT in the image — something is wrong with the config."
  exit 1
fi
echo

# --- 4. limine-entry-tool --add-kernel (refresh the copy on the ESP) -----------
bold "[4/5] limine-entry-tool --add-kernel ${KNAME}"
echo "    The limine entry points at a COPY of the initramfs on the ESP (/boot/<hash>/linux-omnkp/),"
echo "    NOT at ${INITRAMFS} — the copy must be refreshed, otherwise boot will use the old image."
echo "    (The main omnkp entry is the UKI from build-uki-omnkp.sh — this step is the fallback.)"
if ! command -v limine-entry-tool >/dev/null 2>&1; then
  red "ERROR: limine-entry-tool does not exist — install limine-mkinitcpio-hook."
  exit 1
fi
sudo -n limine-entry-tool --add-kernel "${KNAME}" \
  "${INITRAMFS}" \
  "${VMLINUZ}" \
  --comment "om-kernel ${KREL} (series 0001–0018, initramfs+encrypt)" \
  --quiet
green "    Entry refreshed (new initramfs copy on the ESP)."
echo

# --- 5. summary ----------------------------------------------------------------
bold "[5/5] Limine menu structure"
limine-entry-tool --tree 2>&1 | sed 's/^/    /'
echo
bold "=== Done — next step: reboot ==="
echo "1. Reboot: sudo reboot"
echo "2. In the Limine menu pick 'omnkp' (under Omarchy, next to 'linux' and Snapshots)."
echo "   (if you use the protocol: linux fallback — the main entry is the UKI)."
echo "3. A LUKS unlock prompt (password) or auto-unlock should appear."
echo "4. After boot verify:"
echo "     uname -r                          → ${KREL}"
echo "     journalctl -b | grep -iE 'bcm5974.*(error|fail)'"
echo
yellow "If omnkp does not come up: pick the old 'linux' entry (Omarchy) in the Limine menu."
yellow "limine.conf backup: /boot/limine.conf.bak-nvkp (restore: sudo cp ...)."
yellow "Removing the omnkp entry: sudo limine-entry-tool --remove-kernel ${KNAME}"