#!/usr/bin/env bash
# =============================================================================
# setup-wineasio.sh — wineasio under Wine 11 WoW64 (giang17/wineasio 1.5.0 fork)
# Needed ONLY for option B: Reason standalone (ASIO → JACK/PipeWire).
# The AUR wineasio 1.3.0 does not work on WoW64 — do not install it.
# =============================================================================
set -euo pipefail

SRC="${HOME}/src/wineasio-giang17"

echo "=== [1/4] Dependencies (mingw-w64 for building the PE dll) ==="
sudo pacman -S --needed --noconfirm mingw-w64-gcc

echo "=== [2/4] Sources ==="
mkdir -p "$HOME/src"
if [ ! -d "$SRC" ]; then
    git clone https://github.com/giang17/wineasio "$SRC"
else
    git -C "$SRC" pull --ff-only || true
fi

echo "=== [3/4] Build (64-bit — Reason is 64-bit) ==="
# Arch: wine headers in /usr/include/wine (not /opt/wine-stable like WineHQ)
make -C "$SRC" -f Makefile.wine11 64 WINE_PREFIX=/usr

echo "=== [4/4] Install to system wine (/usr/lib/wine) + register in the prefixes ==="
# Arch wine: PE dll → /usr/lib/wine/x86_64-windows/, unix lib → /usr/lib/wine/x86_64-unix/
sudo cp "$SRC"/build_wine11/wineasio64.dll /usr/lib/wine/x86_64-windows/
sudo cp "$SRC"/build_wine11/wineasio64.so  /usr/lib/wine/x86_64-unix/

for P in "$HOME/winprefixes/reason" "$HOME/winprefixes/ni"; do
    if [ -d "$P" ]; then
        # The fork does not export DllInstall → `regsvr32 /i` does nothing; go WITHOUT /i.
        # DllRegisterServer writes the full path C:\windows\system32\wineasio64.dll into InprocServer32,
        # so the PE dll must physically live in the prefix's system32 (not just /usr/lib/wine).
        cp "$SRC"/build_wine11/wineasio64.dll "$P/drive_c/windows/system32/"
        WINEPREFIX="$P" wine regsvr32 wineasio64.dll || true
        # Verification
        WINEPREFIX="$P" wine reg query "HKLM\Software\ASIO\WineASIO" 2>/dev/null \
            && echo "  -> OK: WineASIO registered in $P" \
            || echo "  -> WARNING: no ASIO entry in $P — check the regsvr32 output above"
    fi
done
echo "DONE — check: WINEPREFIX=~/winprefixes/reason wine reg query \"HKLM\Software\ASIO\WineASIO\""
echo "Note: after a wine package update on Arch the files in /usr/lib/wine are overwritten — rerun this script."