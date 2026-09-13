#!/usr/bin/env bash
# Regenerates the build/ tree from the nvkp kernel sources, injects the forced
# PCIe-only configuration, applies our BCM4360 patches and builds
# brcmfmac out-of-tree.
#
# Patches: driver/patches/*.patch (series 0001..0005, format-patch style).
# Strip level: -p1, applied from the build/brcm80211/ directory (the home
# directory of the copied tree). Paths in the patches are relative to that
# directory: "a/brcmfmac/pcie.c", "a/include/brcm_hw_ids.h".
# The script is idempotent: build/ is wiped and re-copied from scratch each
# time, so patches always land on fresh sources.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${KSRC:-$HOME/Projects/nv-kepler/tmp/linux-nouveau}"
BRCM="$KSRC/drivers/net/wireless/broadcom/brcm80211"

rm -rf "$ROOT/build"
mkdir -p "$ROOT/build/brcm80211"
cp -r "$BRCM/brcmfmac" "$BRCM/include" "$ROOT/build/brcm80211/"
sed -i '6r '"$ROOT"'/scripts/forced-config.mk' \
    "$ROOT/build/brcm80211/brcmfmac/Makefile"

# Apply the BCM4360 patch series (0001..0005) in order onto the fresh copy.
# -p1: paths in the patches ("a/brcmfmac/pcie.c") are relative to build/brcm80211/.
# --no-backup-if-mismatch: a hunk applying with an offset (normal for a series)
# must not create *.orig files in the build/ tree.
for patch in "$ROOT"/patches/000[1-9]-*.patch; do
    [ -e "$patch" ] || continue
    echo "Applying $(basename "$patch")"
    patch -p1 -d "$ROOT/build/brcm80211" --no-backup-if-mismatch -i "$patch"
done

make -C "$KSRC" M="$ROOT/build/brcm80211/brcmfmac" modules -j"$(nproc)" "$@"
