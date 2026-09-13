#!/usr/bin/env bash
# build-om-wifi.sh — wifi modules (brcmfmac with the BCM4360 series) against omnkp.
#
# The project's one common kernel is om-kernel (tmp/linux-omkp,
# kernel.release = 7.1.9-omnkp-dirty). This script moves the wifi build
# from ../om-wifi (which pointed at the old nv-kepler 7.1.8-nvkp) onto the tree
# of this repo. Output lands in ../om-wifi/driver/build/brcm80211/brcmfmac/,
# so the om-wifi fire scripts (scripts/fire-t308-olmsg-cdc.sh) keep working
# unchanged.
#
# What it does:
#   1. copies brcmfmac + include from tmp/linux-omkp (fresh each time,
#      idempotently — like build-module.sh in om-wifi)
#   2. injects forced-config.mk (PCIe-only bring-up configuration + -DDEBUG)
#   3. applies the patch series ../om-wifi/driver/patches/000[1-9]-*.patch
#   4. make modules out-of-tree against the same tree from which
#      the installed kernel /lib/modules/7.1.9-omnkp-dirty was built
#
# Run as USER (no-builds rule). Does not require sudo.
#
# Usage: ./scripts/build-om-wifi.sh

set -e

OMK="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="$OMK/tmp/linux-omkp"
WIFI="$OMK/../om-wifi/driver"
BRCM="$KSRC/drivers/net/wireless/broadcom/brcm80211"
BUILD="$WIFI/build"
FCFG="$WIFI/scripts/forced-config.mk"

KREL="$(cat "$KSRC/include/config/kernel.release")"
echo "=== build-om-wifi: kernel $KREL ==="

[ -f "$KSRC/Module.symvers" ] || { echo "ERROR: $KSRC not prepared — run build-om-kernel.sh first"; exit 1; }
[ -f "$FCFG" ] || { echo "ERROR: $FCFG missing"; exit 1; }

# 1. fresh copy of the brcm80211 tree (brcmfmac + include/brcm headers)
rm -rf "$BUILD/brcm80211"
mkdir -p "$BUILD/brcm80211"
cp -r "$BRCM/brcmfmac" "$BRCM/include" "$BUILD/brcm80211/"

# 2. forced config block after the Makefile header (nv-kepler DEBUG trap:
#    with M= the CONFIG_BRCMDBG flag does not propagate)
sed -i '6r '"$FCFG" "$BUILD/brcm80211/brcmfmac/Makefile"

# 3. BCM4360 patch series (0001..0099), -p1, relative to build/brcm80211/
#    Note: glob 00[0-9][0-9] — the 000[1-9] pattern silently dropped 0010+
#    (trap found 03.09, with patch 0010)
shopt -s nullglob
PATCHES=("$WIFI"/patches/00[0-9][0-9]-*.patch)
shopt -u nullglob
[ ${#PATCHES[@]} -gt 0 ] || { echo "ERROR: no patches in $WIFI/patches"; exit 1; }
for patch in "${PATCHES[@]}"; do
    echo "Applying $(basename "$patch")"
    patch -p1 -d "$BUILD/brcm80211" --no-backup-if-mismatch -i "$patch"
done

# 4. out-of-tree build
make -C "$KSRC" M="$BUILD/brcm80211/brcmfmac" modules -j"$(nproc)"

# 5. verify the test parameters (T276/T308 must be in the module)
echo ""
echo "=== verification ==="
for parm in bcm4360_test276_shared_info bcm4360_test308_olmsg_cdc; do
    if modinfo "$BUILD/brcm80211/brcmfmac/brcmfmac.ko" | grep -q "$parm"; then
        echo "param $parm: OK"
    else
        echo "param $parm: MISSING — something went wrong with the patches"
        exit 1
    fi
done
file "$BUILD/brcm80211/brcmfmac/brcmfmac.ko" | grep -o 'kernel 7[^ ]*' || true
echo ""
echo "Done. Modules: $BUILD/brcm80211/brcmfmac/{brcmfmac.ko,wcc/brcmfmac-wcc.ko}"
echo "Fire (supervised session): sudo $WIFI/scripts/fire-t308-olmsg-cdc.sh"