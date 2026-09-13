#!/bin/bash
# install-omnkp-entry.sh — add a Limine boot entry for linux-omnkp + restore the modules
# WITHOUT rebuilding the kernel. FALLBACK flow — the main omnkp entry is the UKI (build-uki-omnkp.sh).
# Counterpart of install-nvkp-entry.sh: entry via `limine-entry-tool --add-kernel`
# (protocol: linux) — historically broken on this MBP (black screen: vgacon on the
# disabled dGPU), hence the UKI, but the script stays in case the old flow is needed.
#
# Previously the script manually appended a top-level /+Linux-omnkp section with protocol: linux
# — that does NOT work in Limine 12.6.0 + Omarchy (empty OS section, invisible in the menu).
# The correct way is `limine-entry-tool --add-kernel`: it creates a //linux-omnkp sub-entry
# under the /+Omarchy OS entry (visible in the menu like "linux" and the snapshots), copies the files
# to the ESP, and uses the full cmdline from the /etc/limine-entry-tool.d/ drop-ins (omarchy-
# defaults.conf + resume.conf = the same cmdline as the working system).
#
# Does: 1) backup limine.conf  2) remove the old broken manual /+Linux-omnkp entry
#       3) limine-entry-tool --add-kernel (correct entry)  4) disable linux-modules-cleanup.service
#       5) restore the omnkp modules from /usr/lib/modules/.old/  6) show --tree.
# Reboot and picking an entry in the Limine menu — your call.
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
# KREL auto: newest *-omnkp* directory in /usr/lib/modules; env KREL overrides.
KREL="${KREL:-$(cd /usr/lib/modules 2>/dev/null && ls -1d -- *-omnkp* 2>/dev/null | sort -V | tail -1 || true)}"
[ -n "$KREL" ] || { echo "ERROR: no *-omnkp* directory in /usr/lib/modules — run ./scripts/build-om-kernel.sh first" >&2; exit 1; }
KNAME="linux-omnkp"              # FAT32-safe kernel entry name (letters/digits/_/.-)
LIMINE="/boot/limine.conf"
BAK="/boot/limine.conf.bak-omnkp"
MODULES_SRC="/usr/lib/modules/.old/${KREL}"
MODULES_DST="/usr/lib/modules/${KREL}"
CLEANUP_SVC="linux-modules-cleanup.service"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

bold "=== install-omnkp-entry.sh — Limine entry + modules (no rebuild, fallback flow) ==="
echo "Kernel entry: ${KNAME}  (uname after boot: ${KREL})"
echo

# --- preliminary checks ------------------------------------------------------
if ! sudo -n true 2>/dev/null; then
  red "ERROR: sudo -n does not work (passwordless sudo required)."
  exit 1
fi
if ! command -v limine-entry-tool >/dev/null 2>&1; then
  red "ERROR: limine-entry-tool does not exist — install limine-mkinitcpio-hook."
  exit 1
fi
if ! sudo -n test -f /boot/vmlinuz-${KNAME}; then
  red "ERROR: /boot/vmlinuz-${KNAME} does not exist — the build is not installed."
  red "       Run ./scripts/build-om-kernel.sh --full-tree --clean (full build)."
  exit 1
fi
if ! sudo -n test -f /boot/initramfs-${KNAME}.img; then
  red "ERROR: /boot/initramfs-${KNAME}.img does not exist — the initramfs was not generated."
  red "       Run ./scripts/build-om-kernel.sh --full-tree --clean (full build)."
  exit 1
fi
if ! sudo -n test -f "${LIMINE}"; then
  red "ERROR: ${LIMINE} does not exist — isn't this Omarchy/Limine?"
  exit 1
fi
green "OK: vmlinuz + initramfs + limine.conf + limine-entry-tool present."
echo

# --- 1. backup limine.conf (only if not already present) ---------------------
bold "[1/5] Backup ${LIMINE} → ${BAK}"
if sudo -n test -f "${BAK}"; then
  yellow "    Backup ${BAK} already exists — keeping it (not overwriting)."
else
  sudo -n cp "${LIMINE}" "${BAK}"
  green "    Backup created."
fi
echo

# --- 2. remove the old broken manual /+Linux-omnkp entry (top-level empty section) --
bold "[2/5] Cleaning up the old manual /+Linux-omnkp entry (if present)"
# Note: this is `sudo -n grep -q` WITHOUT a pipe — pipefail-safe (lesson: `cmd | grep -q`
# with pipefail gives a false negative through SIGPIPE; here there is no pipe).
if sudo -n grep -q '^/+Linux-omnkp$' "${LIMINE}"; then
  yellow "    Found the broken manual /+Linux-omnkp entry — restoring ${LIMINE} from the backup ${BAK}."
  sudo -n cp "${BAK}" "${LIMINE}"
  green "    Cleaned up (limine.conf = state from the backup, without the broken entry)."
else
  green "    No broken entry — OK."
fi
echo

# --- 3. limine-entry-tool --add-kernel (CORRECT entry) -----------------------
bold "[3/5] limine-entry-tool --add-kernel ${KNAME}"
echo "    The tool will create a //${KNAME} sub-entry under /+Omarchy (visible in the menu)"
echo "    next to 'linux' and the snapshots. It copies vmlinuz+initramfs to the ESP."
echo "    Cmdline: from the /etc/limine-entry-tool.d/ drop-ins = full, like the system."
echo "    (idempotent: create-or-update)"
echo
sudo -n limine-entry-tool --add-kernel "${KNAME}" \
  "/boot/initramfs-${KNAME}.img" \
  "/boot/vmlinuz-${KNAME}" \
  --comment "om-kernel ${KREL} (series 0001–0018)" \
  --quiet
green "    Entry added/refreshed."
echo

# --- 4. disable linux-modules-cleanup.service ---------------------------------
bold "[4/5] linux-modules-cleanup.service (moves the omnkp modules to .old/ at boot)"
if systemctl is-enabled "${CLEANUP_SVC}" >/dev/null 2>&1; then
  sudo -n systemctl disable "${CLEANUP_SVC}"
  green "    Disabled."
  echo "    This service moves every /usr/lib/modules/* not matching uname -r"
  echo "    and not owned by pacman to .old/ — deadly for the hand-built"
  echo "    omnkp kernel. Now the modules survive a reboot."
else
  yellow "    Already disabled (or does not exist) — OK."
fi
echo

# --- 5. restore the modules from .old/ (or confirm they are already in dst) ---
bold "[5/5] Modules ${KREL}"
dst_count=$(sudo -n find "${MODULES_DST}" -name '*.ko*' 2>/dev/null | wc -l)
if [ -n "${dst_count}" ] && [ "${dst_count}" -gt 0 ] 2>/dev/null; then
  green "    Modules already in place: ${MODULES_DST} (${dst_count} .ko/.ko.zst)"
  echo "    (rsync from .old/ not needed — the service is disabled, the modules survived the reboot)"
elif [ -d "${MODULES_SRC}" ]; then
  yellow "    ${MODULES_DST} empty — rsync from ${MODULES_SRC}/"
  sudo -n rsync -AHXal "${MODULES_SRC}/" "${MODULES_DST}/"
  green "    Modules restored: ${MODULES_DST}"
  sudo -n bash -c "echo \"    Number of .ko/.ko.zst: \$(find '${MODULES_DST}' -name '*.ko*' | wc -l)\""
else
  red "    WARNING: modules missing in both ${MODULES_DST} and ${MODULES_SRC}."
  red "           The modules_install step (build step 6) probably did not work."
  red "           Run ./scripts/build-om-kernel.sh --full-tree --clean (reinstalls the modules)."
fi
echo

# --- summary + --tree ----------------------------------------------------------
bold "=== Limine menu structure after the changes ==="
limine-entry-tool --tree 2>&1 | sed 's/^/    /'
echo
bold "=== Done — next step: reboot ==="
echo "1. Reboot: sudo reboot"
echo "2. In the Limine menu pick 'linux-omnkp' (under Omarchy, next to 'linux' and Snapshots)."
echo "   If the menu does not show up — hold a key at startup."
echo "3. After boot verify:"
echo "     uname -r                          → ${KREL}"
echo "     journalctl -b | grep -iE 'bcm5974.*(error|fail)'"
echo
yellow "If omnkp does not come up: pick the old 'linux' entry (Omarchy) in the Limine menu."
yellow "limine.conf backup: ${BAK}  (restore: sudo cp ${BAK} ${LIMINE})."
yellow "Removing the omnkp entry: sudo limine-entry-tool --remove-kernel ${KNAME}"