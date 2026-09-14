#!/usr/bin/env bash
# build-uki-omnkp.sh — build a UKI from the omnkp kernel and boot it via protocol: efi (like Omarchy)
#
# WHY: the omnkp kernel boots via UKI (like the nvkp kernel) — `protocol: linux`
# (legacy) on Apple firmware could not find GOP → screen_info zeroed → vgacon
# on the DISABLED dGPU → black screen. UKI + `protocol: efi` → the kernel's EFI stub
# grabs GOP itself → efifb on the iGPU → visible boot.
#
# Switching protocol: linux → efi on a bare vmlinuz is NOT enough: Limine chainload
# does NOT pass the initramfs → the kernel cannot mount the root (LUKS). A UKI is needed.
#
# What it does: 0) when the ESP runs out of space — cleans up unused artifacts itself
#             (tiers: loose omnkp kernel files → limine_history → old nvkp UKI)
#          1) regenerates the initramfs (config with encrypt + keyfile)
#          2) builds the UKI (vmlinuz + initramfs + cmdline in one .efi)
#          3) limine-entry-tool --add-uki (protocol: efi entry)
#          4) removes cmdline: from the omnkp entry (use the embedded cmdline, not the
#             drop-ins with quiet/loglevel=0, which would hide the output)
#          5) removes the old //linux-omnkp entry (protocol: linux, broken)
# Reboot and picking the 'omnkp' entry in the menu — your call.
set -euo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
# KREL: newest *-omnkp* directory in /usr/lib/modules (e.g. 7.1.9-omnkp-dirty —
# CONFIG_LOCALVERSION_AUTO=y + uncommitted patches = dirty). Run THIS script
# AFTER build-om-kernel.sh (modules + vmlinuz already installed); env KREL overrides.
KREL="${KREL:-$(cd /usr/lib/modules 2>/dev/null && ls -1d -- *-omnkp* 2>/dev/null | sort -V | tail -1 || true)}"
[ -n "$KREL" ] || { echo "ERROR: no *-omnkp* directory in /usr/lib/modules — run ./scripts/build-om-kernel.sh first" >&2; exit 1; }
KNAME="linux-omnkp"             # files in /boot: vmlinuz-linux-omnkp, initramfs-linux-omnkp.img
UKI_NAME="omnkp"                # entry name in the Limine menu (//omnkp)
CONF="/etc/mkinitcpio-omnkp.conf"
INITRAMFS="/boot/initramfs-${KNAME}.img"
VMLINUZ="/boot/vmlinuz-${KNAME}"
# UKI file: omarchy_omnkp.efi — omarchy_* snapshots are made by limine-snapper-sync,
# so the omarchy_ prefix is MANDATORY (like omarchy_nvkp.efi / omarchy_linux.efi).
UKI="/boot/EFI/Linux/omarchy_omnkp.efi"
MODULES_DIR="/usr/lib/modules/${KREL}"
TMP="$PROJ/tmp/uki-build"
CMDLINE_FILE="$TMP/omnkp-cmdline.txt"

# ----------------------------------------------------------------------------
# cmdline (verbose — diagnostic). Once stability is confirmed, switch to the
# quiet version (see the comment at the bottom of the file). FILL IN: the PARTUUID
# of the root partition (`blkid`) and resume_offset (`filefrag -v` on the swapfile).
# Same disk/partitions as nvkp — omnkp REPLACES nvkp, it is not a separate system.
# ----------------------------------------------------------------------------
CMDLINE="cryptdevice=PARTUUID=<PARTUUID-of-root-partition-via-blkid>:root root=/dev/mapper/root zswap.enabled=0 rootflags=subvol=@ rw rootfstype=btrfs resume=/dev/mapper/root resume_offset=<OFFSET-from-filefrag> initramfs_async=0 cryptkey=rootfs:/crypto_keyfile.bin loglevel=7 systemd.show_status=true"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

bold "=== build-uki-omnkp.sh — UKI from the omnkp kernel (protocol: efi) ==="
echo "Kernel: ${KREL}  (modules: ${MODULES_DIR})"
echo "UKI:    ${UKI}"
echo

# --- preliminary checks ------------------------------------------------------
if ! sudo -n true 2>/dev/null; then
  red "ERROR: sudo -n does not work (passwordless sudo required)."
  exit 1
fi
for c in mkinitcpio limine-entry-tool; do
  command -v "$c" >/dev/null 2>&1 || { red "ERROR: $c does not exist."; exit 1; }
done
sudo -n test -d "${MODULES_DIR}" || { red "ERROR: ${MODULES_DIR} does not exist — omnkp modules are not installed."; exit 1; }
sudo -n test -f "${VMLINUZ}" || { red "ERROR: ${VMLINUZ} does not exist."; exit 1; }
sudo -n test -f "${CONF}" || { red "ERROR: ${CONF} does not exist — run ./scripts/fix-initramfs-omnkp.sh first."; exit 1; }
sudo -n test -f /crypto_keyfile.bin || { yellow "WARNING: /crypto_keyfile.bin does not exist — LUKS auto-unlock will not work (a password prompt will appear)."; }
green "OK: mkinitcpio + limine-entry-tool + modules + vmlinuz + config present."
echo

# --- 1. free-space check on the ESP (/boot) + auto-cleanup --------------------
# The UKI build writes the initramfs (~267M) and UKI (~300M) to the ESP at the same
# time. Symptom of running out of space: cryptic objcopy error "No space left on
# device" (2026-08-31). Not enough space → the script ITSELF cleans up unused
# artifacts (tiers from least destructive; user decision 2026-09-01), checking
# free space after each tier.
bold "[1/6] Free-space check on /boot (ESP) (+ auto-cleanup)"
esp_free_kb() { sudo -n df -Pk /boot 2>/dev/null | awk 'NR==2 {print $4}' || true; }

INITRAMFS_KB="$(sudo -n stat -c%s "${INITRAMFS}" 2>/dev/null || echo 0)"
INITRAMFS_KB="${INITRAMFS_KB:-0}"
# the initramfs is regenerated during the build — if the file is missing (e.g. after
# ESP cleanup), estimate from the default size (~280M like nvkp/omnkp).
# Mind the units: stat -c%s = BYTES; below we divide by 1024 everywhere.
if [ "$INITRAMFS_KB" -eq 0 ]; then
  INITRAMFS_KB=$(( 280 * 1024 * 1024 ))
fi
VMLINUZ_KB="$(sudo -n stat -c%s "${VMLINUZ}" 2>/dev/null || echo 0)"
VMLINUZ_KB="${VMLINUZ_KB:-0}"
# UKI ≈ initramfs + vmlinuz + ~30M (microcode + stub + margin)
UKI_KB=$(( INITRAMFS_KB / 1024 + VMLINUZ_KB / 1024 + 30720 ))
# during the build, the initramfs and UKI are on the ESP at the same time
NEED_KB=$(( INITRAMFS_KB / 1024 + UKI_KB ))
ESP_FREE_KB="$(esp_free_kb)"
ESP_FREE_KB="${ESP_FREE_KB:-0}"
esp_ok() { [ "${1:-0}" -ge "$NEED_KB" ]; }
echo "    free: $((ESP_FREE_KB/1024)) MiB | needed: ~$((NEED_KB/1024)) MiB (initramfs $((INITRAMFS_KB/1024/1024)) MiB + UKI ~$((UKI_KB/1024)) MiB)"

clean_esp() {
  # Auto-cleanup tiers; returns 0 when free space >= NEED_KB after one of them.
  # Tier 1: loose omnkp kernel artifacts on /boot — the UKI embeds them in the .efi
  # file, and step [3] (mkinitcpio -g) regenerates them anyway.
  local f sz
  for f in "/boot/initramfs-${KNAME}.img" "/boot/vmlinuz-${KNAME}" "/boot/System.map-${KNAME}"; do
    if sudo -n test -f "$f"; then
      sz=$(( $(sudo -n stat -c%s "$f" 2>/dev/null || echo 0) / 1024 / 1024 ))
      sudo -n rm -f "$f" && green "    [tier 1] removed ${f} (${sz} MiB)"
    fi
  done
  esp_ok "$(esp_free_kb)" && return 0

  # Tier 2: limine-snapper-sync snapshot history (/boot/<hash>/limine_history)
  # — unnecessary for boot, can eat hundreds of MiB.
  local d
  while IFS= read -r d; do
    [ -n "$d" ] || continue
    sudo -n rm -rf "$d" && green "    [tier 2] removed ${d}"
  done < <(sudo -n find /boot -maxdepth 2 -type d -name limine_history 2>/dev/null)
  esp_ok "$(esp_free_kb)" && return 0

  # Tier 3: the old nvkp UKI (omarchy_nvkp.efi — kernel from nv-kepler). The current
  # system boots omnkp; the fallback is the 'linux' entry (Omarchy), not nvkp.
  if sudo -n test -f /boot/EFI/Linux/omarchy_nvkp.efi; then
    sz=$(( $(sudo -n stat -c%s /boot/EFI/Linux/omarchy_nvkp.efi) / 1024 / 1024 ))
    if ! sudo -n limine-entry-tool --remove-uki nvkp --quiet 2>/dev/null; then
      yellow "    [tier 3] //nvkp entry did not exist (orphan) — removing just the file"
    fi
    sudo -n rm -f /boot/EFI/Linux/omarchy_nvkp.efi && green "    [tier 3] removed omarchy_nvkp.efi (${sz} MiB)"
  else
    echo "    [tier 3] omarchy_nvkp.efi does not exist — nothing to remove"
  fi
  esp_ok "$(esp_free_kb)" && return 0
  return 1
}

if ! esp_ok "$ESP_FREE_KB"; then
  yellow "    Not enough space — auto-cleanup of the ESP (tiers from least destructive):"
  if ! clean_esp; then
    red "ERROR: still not enough space on /boot (ESP) after auto-cleanup."
    red "  free: $((ESP_FREE_KB/1024)) MiB, needed ~$((NEED_KB/1024)) MiB."
    yellow "  Left on the ESP: omarchy_linux.efi (STABLE kernel — do NOT remove) and"
    yellow "  omarchy_omnkp.efi (will be overwritten) — manual user decision."
    exit 1
  fi
  ESP_FREE_KB="$(esp_free_kb)"
  ESP_FREE_KB="${ESP_FREE_KB:-0}"
  green "    After cleanup: free $((ESP_FREE_KB/1024)) MiB."
fi
green "    OK: enough space."
echo

# --- 2. cmdline file ---------------------------------------------------------
bold "[2/6] cmdline (verbose) → ${CMDLINE_FILE}"
mkdir -p "${TMP}"
cat > "${CMDLINE_FILE}" <<EOF
# omnkp UKI cmdline — verbose (diagnostic). Remove loglevel=7/systemd.show_status
# and add quiet splash loglevel=0 once stability is confirmed.
${CMDLINE}
EOF
green "    Written cmdline (verbose)."
echo

# --- 2. initramfs + UKI (mkinitcpio -U) -------------------------------------
bold "[3/6] mkinitcpio -k ${KREL} -c ${CONF} -g ${INITRAMFS} -U ${UKI}"
echo "    (this is NOT a kernel rebuild — only initramfs + UKI, ~1-2 min)"
sudo -n mkinitcpio -k "${KREL}" -c "${CONF}" -g "${INITRAMFS}" -U "${UKI}" --cmdline "${CMDLINE_FILE}"
sudo -n test -f "${UKI}" || { red "ERROR: the UKI was not created (${UKI})."; exit 1; }
green "    UKI generated: ${UKI}"
echo

# --- 3. UKI verification (.linux/.initrd/.cmdline sections) ------------------
bold "[4/6] UKI verification (systemd-stub sections)"
if command -v objdump >/dev/null 2>&1; then
  sudo -n objdump -h "${UKI}" 2>/dev/null | grep -E '\.(linux|initrd|cmdline|osrel)' | sed 's/^/      /' \
    || red "    WARNING: no .linux/.initrd/.cmdline sections found — the UKI may be bad."
else
  yellow "    objdump missing — skipping section verification (check the size: $(sudo -n stat -c%s "${UKI}") B)."
fi
echo

# --- 4. Limine entry (--add-uki) + removing cmdline: from the entry -----------
bold "[5/6] limine-entry-tool --add-uki ${UKI_NAME}"
sudo -n limine-entry-tool --add-uki "${UKI_NAME}" "${UKI}" \
  --comment "om-kernel ${KREL} (UKI, verbose)" \
  --quiet
green "    //${UKI_NAME} entry added (protocol: efi)."
echo
bold "    Removing cmdline: from the //${UKI_NAME} entry (use the embedded cmdline, not the quiet drop-ins)"
# limine-entry-tool drop-ins (omarchy-defaults.conf) add `quiet splash loglevel=0 ...`
# to EVERY entry — this would override our embedded loglevel=7.
# We remove the cmdline: line from the //omnkp entry so systemd-stub uses the embedded cmdline.
# (lesson: do NOT sed the whole limine.conf — the quiet pattern is in both entries;
#  edit by entry scope, as here.)
sudo -n cp /boot/limine.conf /boot/limine.conf.bak-uki
sudo -n awk -v name="${UKI_NAME}" '
  $0 == "  //" name { in_entry=1; print; next }
  in_entry && /^  cmdline:/ { next }
  in_entry && !/^  [^ ]/ { in_entry=0 }
  { print }
' /boot/limine.conf > "${TMP}/limine.conf.new"
sudo -n cp "${TMP}/limine.conf.new" /boot/limine.conf
green "    cmdline: removed from the //${UKI_NAME} entry (backup: /boot/limine.conf.bak-uki)."
echo

# --- 5. remove the old //linux-omnkp entry (protocol: linux, broken) ----------
bold "[6/6] Removing the old //linux-omnkp entry (protocol: linux)"
# Note: do NOT `limine-entry-tool --tree | grep -q` — pipefail + grep -q closes the pipe
# early → limine-entry-tool gets SIGPIPE → false negative (pipefail lesson).
if TREE_OUT="$(limine-entry-tool --tree 2>/dev/null)"; then
  case "$TREE_OUT" in
    *linux-omnkp*)
      sudo -n limine-entry-tool --remove-kernel "${KNAME}" --quiet \
        && green "    Old //${KNAME} entry removed." \
        || yellow "    Could not remove //${KNAME} — remove manually: sudo limine-entry-tool --remove-kernel ${KNAME}"
      ;;
    *)
      yellow "    //${KNAME} entry does not exist — skipping."
      ;;
  esac
fi
echo

# --- summary ----------------------------------------------------------------
bold "=== Limine menu structure ==="
limine-entry-tool --tree 2>&1 | sed 's/^/    /'
echo
bold "=== Done — next step: reboot ==="
echo "1. Reboot: sudo reboot"
echo "2. In the Limine menu pick '${UKI_NAME}' (UKI, protocol: efi — like Omarchy)."
echo "3. Expected: visible boot (efifb on the iGPU) + LUKS auto-unlock (no password)."
echo "4. After boot verify:"
echo "     uname -r                          → ${KREL}"
echo "     lsmod | grep nouveau               → loaded"
echo "     journalctl -b | grep -iE 'bcm5974.*(error|fail)'"
echo
yellow "If omnkp does not come up: pick 'linux' (Omarchy) — it still works."
yellow "limine.conf backup: /boot/limine.conf.bak-uki (restore: sudo cp ...)."
yellow "Removing the UKI entry: sudo limine-entry-tool --remove-uki ${UKI_NAME}"

# ----------------------------------------------------------------------------
# Quiet version (once stability is confirmed) — replace CMDLINE with:
#   cryptdevice=PARTUUID=<PARTUUID-of-root-partition-via-blkid>:root root=/dev/mapper/root zswap.enabled=0 rootflags=subvol=@ rw rootfstype=btrfs resume=/dev/mapper/root resume_offset=<OFFSET-from-filefrag> initramfs_async=0 cryptkey=rootfs:/crypto_keyfile.bin quiet splash loglevel=0 systemd.show_status=false rd.udev.log_level=0 vt.global_cursor_default=0
# and run the script again (it will rebuild the UKI with the new cmdline).
# ----------------------------------------------------------------------------