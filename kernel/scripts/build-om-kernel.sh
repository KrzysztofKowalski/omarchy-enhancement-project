#!/usr/bin/env bash
# build-om-kernel.sh — build + install a kernel with om-kernel patches (switchd + bcm5974)
#
# Builds a SEPARATE kernel entry with LOCALVERSION=-omnkp — does NOT overwrite the running kernel:
#   - modules       → /lib/modules/<rev>-omnkp
#   - /boot         → vmlinuz-linux-omnkp, initramfs-linux-omnkp.img, System.map-linux-omnkp
#   - bootloader    → the //omnkp entry is made by build-uki-omnkp.sh (UKI, protocol: efi)
#
# Same patch series as nv-kepler (0001–0015: nouveau reclock, gmux, i915, switchd —
# GPU/dGPU) PLUS 0016–0018 for the bcm5974 touchpad (usb quirks + reset_resume + pre/post
# reset — drivers/usb/core/quirks.c + drivers/input/mouse/bcm5974.c). The bcm5974 patches
# build as modules =m (they do not touch early boot). Apply order: 0001 → 0018.
#
# Thermal: compiling heats up — reclocked [compiler] detects gcc/make and raises
# the fans to 100% automatically → full -j$(nproc) is safe.
#
# Usage:
#   ./build-om-kernel.sh                — full path (config → build → modules → /boot)
#   ./build-om-kernel.sh --kver=7.1.9   — sync tree to v7.1.9 (fetch from stable + checkout) and build
#   ./build-om-kernel.sh --jobs=N       — limit parallelism (default: $(nproc))
#   ./build-om-kernel.sh --no-install   — build only (no modules//boot/initramfs)
#   ./build-om-kernel.sh --clean        — make mrproper before building
#   ./build-om-kernel.sh --full-tree    — switch sparse-checkout to FULL tree (required for build)
set -euo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${SRC:-$PROJ/tmp/linux-omkp}"
PATCH_KERNEL="$PROJ/patches/kernel"
PATCH_GENERIC="$PROJ/patches"
LOCALVERSION="-omnkp"
BOOT_PREFIX="linux-omnkp"            # files in /boot: vmlinuz-linux-omnkp, initramfs-linux-omnkp.img
LOG="${LOG:-$PROJ/tmp/build-om-kernel.log}"
PATCH_STAMP="$SRC/.omnkp-patches.stamp"   # "<git HEAD>:<patch digest>" — idempotency across re-runs
                                        # (a tree version change alters HEAD → patches re-apply from scratch)

# ----------------------------------------------------------------------------
# Arguments
# ----------------------------------------------------------------------------
CLEAN=false
NO_INSTALL=false
FULL_TREE=false
KVER=""
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() {
  sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --jobs=*)       JOBS="${1#*=}" ;;
    --clean)        CLEAN=true ;;
    --no-install)   NO_INSTALL=true ;;
    --full-tree)    FULL_TREE=true ;;
    --kver=*)       KVER="${1#*=}" ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $1" >&2; usage; exit 1 ;;
  esac
  shift
done
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS="$(nproc 2>/dev/null || echo 4)"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------
need_sudo() {
  # All privileged steps go through `sudo -n` (passwordless on this machine).
  # Check once at the start to avoid failing halfway through.
  if ! sudo -n true 2>/dev/null; then
    echo "ERROR: sudo -n (passwordless) does not work — required for installation." >&2
    echo "      Add NOPASSWD in /etc/sudoers.d or run first: sudo -v" >&2
    exit 1
  fi
}

# kernel_release — the tree's real release number (from LOCALVERSION).
# After building, scripts/ exists → make kernelrelease works.
# Before the build (informational only) fallback: parse the Makefile.
kernel_release() {
  local out
  if out="$(make -s kernelrelease LOCALVERSION="$LOCALVERSION" 2>/dev/null)" && [ -n "$out" ]; then
    printf '%s\n' "$out"
    return 0
  fi
  local v p s e
  v="$(awk '$1=="VERSION" {print $3; exit}' Makefile)"
  p="$(awk '$1=="PATCHLEVEL" {print $3; exit}' Makefile)"
  s="$(awk '$1=="SUBLEVEL" {print $3; exit}' Makefile)"
  e="$(awk '$1=="EXTRAVERSION" {print $3; exit}' Makefile)"
  printf '%s.%s.%s%s%s\n' "$v" "$p" "$s" "$e" "$LOCALVERSION"
}

# cfg_state — print the state of a CONFIG_* symbol from .config (report only).
cfg_state() {
  local sym="$1"
  local v
  v="$(grep -E "^$sym=" .config 2>/dev/null | head -1 || true)"
  if [ -n "$v" ]; then printf '  %-26s %s\n' "$sym" "${v#*=}"; else printf '  %-26s (not set)\n' "$sym"; fi
}

# --- patches ---
# om-kernel keeps ALL patches (0001–0018) in patches/kernel/ — unlike nv-kepler,
# where 0001 lived in patches/ (outside kernel/). We build the list by globbing
# with sort -V — order 0001→0018 = apply order.
# (find_patch still looks in PATCH_KERNEL first, then PATCH_GENERIC —
#  as in the original, in case a future patch lands in patches/.)
PATCH_NAMES=()
mapfile -t PATCH_NAMES < <(cd "$PATCH_KERNEL" && shopt -s nullglob && printf '%s\n' 0*-*.patch | LC_ALL=C sort -V)
if [ "${#PATCH_NAMES[@]}" -lt 18 ]; then
  die "expected ≥18 patches in $PATCH_KERNEL (0001–0018), found ${#PATCH_NAMES[@]} — check the directory"
fi

find_patch() {
  local name="$1"
  if [ -f "$PATCH_KERNEL/$name" ]; then printf '%s\n' "$PATCH_KERNEL/$name"; return 0; fi
  if [ -f "$PATCH_GENERIC/$name" ]; then printf '%s\n' "$PATCH_GENERIC/$name"; return 0; fi
  return 1
}

patches_digest() {
  # Hash of the content of all patches + their file NAMES, in apply order. A change
  # to any patch (content OR name) → new digest → the next run re-applies
  # everything from a clean tree.
  # (om-kernel: names resolved to full paths via find_patch — in the original
  #  nv-kepler `cat "$@"` over bare names missed the files (cwd=$SRC), so the
  #  digest only covered names; here the content is hashed too.)
  local p
  (
    for name in "$@"; do
      printf '%s\n' "[$name]"
      p="$(find_patch "$name" 2>/dev/null || true)"
      [ -n "$p" ] && cat "$p" 2>/dev/null || true
    done
  ) | sha256sum | cut -d' ' -f1
}

apply_patch() {
  local p="$1" name
  name="$(basename "$p")"
  if git apply --check "$p" 2>/dev/null; then
    git apply "$p"
    echo "    OK   $name — applied"
  elif git apply --check --reverse "$p" 2>/dev/null; then
    echo "    SKIP $name — already applied"
  else
    echo "ERROR: $name does not apply cleanly and is not applied." >&2
    echo "      Not using --3way or --reject — resolve manually." >&2
    exit 1
  fi
}

# --- check completeness of the source tree ---
check_full_tree() {
  local sparse
  sparse="$(git sparse-checkout list 2>/dev/null || true)"
  if [ -n "$sparse" ]; then
    if [ "$FULL_TREE" = true ]; then
      echo ">>> Tree is sparse-checkout — materializing FULL tree..."
      echo "    Note: the clone is blob:none — git will fetch the missing files from git.kernel.org"
      echo "    (can be >1 GB and needs network; one-time)."
      git sparse-checkout disable
      sparse="$(git sparse-checkout list 2>/dev/null || true)"
      [ -n "$sparse" ] && die "git sparse-checkout disable did not work"
    else
      echo "ERROR: the source tree is sparse-checkout — it cannot be built." >&2
      echo "      Only visible:" >&2
      printf '%s\n' "$sparse" | sed 's/^/        /' >&2
      echo "      Prepare a full tree:  ./build-om-kernel.sh --full-tree" >&2
      exit 1
    fi
  fi
  if [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = "true" ]; then
    echo "NOTE: the clone is shallow — fine for building, but git describe may have no tags."
  fi
}

# ==========================================================================
echo "=== om-kernel: building a kernel with LOCALVERSION=$LOCALVERSION (series 0001–0018: switchd v5.0 + bcm5974 0016–0018)"
echo "=== src: $SRC"

[ -d "$SRC/.git" ] || die "$SRC is not a git clone (no .git) — prepare the tree: ./scripts/setup-tree.sh"
cd "$SRC"

# full tree (sparse → full), before we do anything else
check_full_tree

# after materializing the full tree, scripts/ MUST exist
[ -d scripts ] || { echo "ERROR: no scripts/ in $SRC — tree is incomplete." >&2; exit 1; }

# --------------------------------------------------------------------------
# 0. Sync tree to a given tag (--kver) — when a new kernel release comes out
# --------------------------------------------------------------------------
if [ -n "$KVER" ]; then
  TARGET="v${KVER#v}"
  if [ "$(git describe --tags 2>/dev/null || true)" = "$TARGET" ]; then
    echo ">>> [0/9] tree already at $TARGET — no fetch/checkout"
  else
    echo ">>> [0/9] Sync tree to $TARGET (fetch from stable + checkout -f)..."
    echo "    NOTE: checkout -f DISCARDS uncommitted changes in the tree"
    echo "    (old patches + experiments). The real patches are applied in step 2."
    if ! git rev-parse --verify "$TARGET" >/dev/null 2>&1; then
      git fetch --depth 1 stable tag "$TARGET" \
        || die "fetch of $TARGET from stable failed — check the network (git.kernel.org) and retry"
    fi
    if ! git checkout -f "$TARGET" 2>"$PROJ/tmp/checkout-$TARGET.err"; then
      sed 's/^/    /' "$PROJ/tmp/checkout-$TARGET.err" >&2 || true
      die "checkout of $TARGET failed — error above (see also git status)"
    fi
    rm -f "$PROJ/tmp/checkout-$TARGET.err"
    # build artifacts of the previous version (generated, untracked) — checkout does not touch them
    git clean -fd lib/raid/raid6 2>/dev/null || true
    echo "    tree: $(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"
  fi
else
  echo ">>> [0/9] without --kver — building from the current tree ($(git describe --tags 2>/dev/null || git rev-parse --short HEAD))"
fi

# --------------------------------------------------------------------------
# 1. [optional] make mrproper
# --------------------------------------------------------------------------
if [ "$CLEAN" = true ]; then
  echo ">>> [1/9] make mrproper (clean)..."
  make mrproper
else
  echo ">>> [1/9] without --clean (tree left dirty)"
fi

# --------------------------------------------------------------------------
# 2. Patching (git apply, idempotent)
# --------------------------------------------------------------------------
echo ">>> [2/9] Applying patches from $PATCH_KERNEL (and $PATCH_GENERIC)..."
digest="$(patches_digest "${PATCH_NAMES[@]}")"
stamp_new="$(git rev-parse HEAD):$digest"
if [ -f "$PATCH_STAMP" ] && [ "$(cat "$PATCH_STAMP" 2>/dev/null)" = "$stamp_new" ]; then
  echo "    SKIP all ${#PATCH_NAMES[@]} patches — already applied (stamp matches: $PATCH_STAMP)"
else
  # Per-patch "already applied" detection (git apply --check --reverse) does NOT work
  # for this set: patches 0006/0007/0008/0010/0011 touch the same file
  # (nouveau_drm.c), so the reverse-check of an earlier patch fails once a
  # later one changed the context.
  # Therefore: clean tree → apply all in order → write the stamp.
  if ! git diff --quiet; then
    echo "    Tree has changes (earlier patches?) — resetting to a clean base: git checkout -f"
    git checkout -f
  fi
  for name in "${PATCH_NAMES[@]}"; do
    p="$(find_patch "$name")" || die "patch $name not found in $PATCH_KERNEL or $PATCH_GENERIC"
    apply_patch "$p"
  done
  printf '%s\n' "$stamp_new" > "$PATCH_STAMP"
  echo "    Stamp written: $PATCH_STAMP (re-run will skip patch application)"
fi

# --------------------------------------------------------------------------
# 3. Configuration
# --------------------------------------------------------------------------
echo ">>> [3/9] .config configuration..."
if [ ! -f .config ]; then
  if [ -r /proc/config.gz ]; then
    echo "    no .config → base = the running Arch kernel's configuration (/proc/config.gz)"
    zcat /proc/config.gz > .config
  else
    echo "NOTE: /proc/config.gz unavailable — make defconfig (risky, check manually)"
    make defconfig
  fi
else
  echo "    .config exists — keeping it, only olddefconfig"
fi
make olddefconfig

echo "    --- key options (report only, NOT modifying) ---"
cfg_state CONFIG_DRM_NOUVEAU
cfg_state CONFIG_DRM_APPLE_GMUX
cfg_state CONFIG_APPLE_GMUX
cfg_state CONFIG_VGA_SWITCHEROO
cfg_state CONFIG_DRM_I915
# new (bcm5974): 0017/0018 touch drivers/input/mouse/bcm5974.c — it must be
# enabled (CONFIG_MOUSE_BCM5974=y/=m) for the patches to enter the build.
cfg_state CONFIG_MOUSE_BCM5974
if ! grep -q '^CONFIG_MOUSE_BCM5974=' .config 2>/dev/null; then
  echo "    NOTE: CONFIG_MOUSE_BCM5974 not set — bcm5974 patches (0016–0018) will NOT"
  echo "           enter the build. Enable before building:"
  echo "             ./scripts/config --file .config --module MOUSE_BCM5974"
  echo "             make olddefconfig"
fi

# --------------------------------------------------------------------------
# 4. Build
# --------------------------------------------------------------------------
echo ">>> [4/9] Build: make -j$JOBS LOCALVERSION=$LOCALVERSION (log: $LOG)"
mkdir -p "$(dirname "$LOG")"
echo "=== make -j$JOBS LOCALVERSION=$LOCALVERSION ($(date)) ===" > "$LOG"
if ! make -j"$JOBS" LOCALVERSION="$LOCALVERSION" 2>&1 | tee -a "$LOG"; then
  echo "ERROR: build failed — log: $LOG (last 15 lines):" >&2
  tail -15 "$LOG" >&2 || true
  exit 1
fi
echo "    build OK"

# --------------------------------------------------------------------------
# 5. Kernelrelease (the real number — may carry a -g<hash>[-dirty] suffix,
#    because CONFIG_LOCALVERSION_AUTO=y in the Arch config)
# --------------------------------------------------------------------------
KREL="$(kernel_release)"
echo ">>> [5/9] kernelrelease: $KREL"

# --------------------------------------------------------------------------
# 6. modules_install
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  need_sudo
  echo ">>> [6/9] sudo -n make modules_install LOCALVERSION=$LOCALVERSION"
  sudo -n make modules_install LOCALVERSION="$LOCALVERSION"
  [ -d "/lib/modules/$KREL" ] || die "modules did not land in /lib/modules/$KREL"
  echo "    modules in /lib/modules/$KREL"
else
  echo ">>> [6/9] skipping modules_install (--no-install)"
fi

# --------------------------------------------------------------------------
# 7. /boot: bzImage + System.map (no make install — controlled copying)
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  echo ">>> [7/9] Installing to /boot (copying bzImage/System.map)..."
  [ -f arch/x86/boot/bzImage ] || die "no arch/x86/boot/bzImage after the build"
  sudo -n install -m 0644 arch/x86/boot/bzImage "/boot/vmlinuz-$BOOT_PREFIX"
  if [ -f System.map ]; then
    sudo -n install -m 0644 System.map "/boot/System.map-$BOOT_PREFIX"
  fi
  echo "    /boot/vmlinuz-$BOOT_PREFIX + /boot/System.map-$BOOT_PREFIX"
else
  echo ">>> [7/9] skipping /boot (--no-install)"
fi

# --------------------------------------------------------------------------
# 8. initramfs (mkinitcpio)
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  echo ">>> [8/9] initramfs: mkinitcpio -k $KREL..."
  # The config MUST have the `encrypt` hook (udev + encrypt + keyfile), not the system
  # /etc/mkinitcpio.conf (systemd, no encrypt). Without dm-crypt, LUKS won't start →
  # black screen. Also, `-c` disables the /etc/mkinitcpio.conf.d/ drop-ins (where
  # omarchy_hooks.conf adds encrypt), so we explicitly point at the omnkp config.
  # The config is created by fix-initramfs-omnkp.sh (udev + autodetect + encrypt + FILES keyfile).
  # On the first run the config doesn't exist yet (fix-initramfs needs the modules
  # from step 6) — then fix-initramfs-omnkp.sh makes the initramfs and step 8 is skipped.
  if ! sudo -n test -f /etc/mkinitcpio-omnkp.conf; then
    echo "    NOTE: no /etc/mkinitcpio-omnkp.conf — fix-initramfs-omnkp.sh will build the initramfs"
    echo "    (step 8 skipped; order: build → fix-initramfs → build-uki)"
  else
    sudo -n mkinitcpio -k "$KREL" -c /etc/mkinitcpio-omnkp.conf -g "/boot/initramfs-$BOOT_PREFIX.img"
    sudo -n test -f "/boot/initramfs-$BOOT_PREFIX.img" || die "initramfs was not created"
    echo "    /boot/initramfs-$BOOT_PREFIX.img"
  fi
else
  echo ">>> [8/9] skipping initramfs (--no-install)"
fi

# --------------------------------------------------------------------------
# 9. Bootloader — the Limine //omnkp entry is made by build-uki-omnkp.sh (UKI, protocol: efi)
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  echo ">>> [9/9] Bootloader: boot via UKI (protocol: efi — like Omarchy)."
  # On this MBP /boot is an ESP FAT32 mounted with fmask=0077 (drwx------) → a normal
  # user CANNOT read /boot. Detect with `sudo -n test ...`, not `[ -f ... ]`.
  if sudo -n test -f /boot/limine.conf; then
    echo "    The //omnkp entry is updated by a separate script (UKI + limine-entry-tool):"
    echo "      sudo bash $PROJ/scripts/build-uki-omnkp.sh"
    echo "    No protocol: linux entry is created anymore — black screen (VGACON on the"
    echo "    disabled dGPU; boot goes through the omnkp UKI)."
  else
    echo "    NOTE: no /boot/limine.conf — check the bootloader manually."
  fi
else
  echo ">>> [9/9] skipping bootloader (--no-install)"
fi

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
echo
echo "=== SUMMARY ==="
echo "kernelrelease: $KREL"
if [ "$NO_INSTALL" = false ]; then
  echo "--- new files in /boot ---"
  sudo -n ls -la /boot/vmlinuz-"$BOOT_PREFIX"* /boot/initramfs-"$BOOT_PREFIX"* /boot/System.map-"$BOOT_PREFIX"* 2>/dev/null || true
  echo "--- old kernel (unchanged) ---"
  sudo -n ls -la /boot/vmlinuz-* /boot/initramfs-* 2>/dev/null | grep -v "$BOOT_PREFIX" || true
  echo "On this system the old kernel is a UKI at /boot/EFI/Linux/omarchy_linux.efi — untouched."
fi
echo
echo "Next step: sudo bash $PROJ/scripts/build-uki-omnkp.sh   (UKI + //omnkp entry)"
echo "Then reboot and pick 'omnkp' in the Limine menu. The old Omarchy entry still works."