#!/usr/bin/env bash
# =============================================================================
# setup-wine.sh — Reason 14 + Komplete Kontrol Ultimate under Wine for Ardour
# Omarchy (Arch) + PipeWire. Run from the project directory:  ./setup-wine.sh
# GUI steps (installers, login) are done by hand — the commands are in README.md.
# =============================================================================
set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
INST="$PROJ/installers"
NI_PFX="$HOME/winprefixes/ni"        # prefix: Native Access + Komplete Kontrol
RSN_PFX="$HOME/winprefixes/reason"   # prefix: Reason 14 + Reason Rack Plugin
LIB_DIR="$HOME/komplete-libraries"   # NI libraries outside the prefix (mapped into Wine as Z:\home\<user>\komplete-libraries)

echo "=== [1/7] System packages ==="
sudo pacman -S --needed --noconfirm yabridge yabridgectl winetricks

echo "=== [2/7] Creating the wine prefixes ==="
mkdir -p "$NI_PFX" "$RSN_PFX" "$LIB_DIR"
for P in "$NI_PFX" "$RSN_PFX"; do
    echo "  -> init $P"
    WINEPREFIX="$P" wineboot -i >/dev/null 2>&1
    WINEPREFIX="$P" winecfg -v win10 2>/dev/null
done

echo "=== [3/7] winetricks (fonts, VC++ runtime, d3dcompiler) ==="
# vcrun2022 = ~50-100 MB download per prefix; d3dcompiler_47 helps Electron apps (Native Access)
WINEPREFIX="$NI_PFX"   winetricks -q corefonts d3dcompiler_47 vcrun2022
WINEPREFIX="$RSN_PFX"  winetricks -q corefonts d3dcompiler_47 vcrun2022

echo "=== [4/7] Unpacking the installers ==="
if [ ! -f "$INST/KK334/Komplete Kontrol 3.3.4 Setup PC.exe" ]; then
    unzip -o "$INST/Komplete_Kontrol_334_PC.zip" -d "$INST/KK334"
fi
if [ ! -f "$INST/Reason1410/Install Reason 14.exe" ]; then
    unzip -o "$INST/Reason_1410_Win.zip" -d "$INST/Reason1410"
fi

echo "=== [5/7] yabridgectl: registering plugin directories ==="
yabridgectl add "$NI_PFX/drive_c/Program Files/Common Files/VST3"  2>/dev/null || true
yabridgectl add "$RSN_PFX/drive_c/Program Files/Common Files/VST3" 2>/dev/null || true
# VST2 (only if you install Komplete Kontrol 2.9.6 as a fallback)
for d in "$NI_PFX/drive_c/Program Files/Common Files/VST2" \
         "$NI_PFX/drive_c/Program Files/Steinberg/VstPlugins"; do
    [ -d "$d" ] && yabridgectl add "$d" 2>/dev/null || true
done
yabridgectl list || true

echo "=== [6/7] yabridgectl sync ==="
yabridgectl sync

echo "=== [7/7] wineasio (Reason standalone, option B) ==="
echo "  The AUR wineasio does NOT work on wine 11 WoW64. Use ./setup-wineasio.sh"
echo "  (builds the giang17/wineasio 1.5.0 fork, which supports Wine 11 WoW64)."

echo ""
echo "DONE — now the manual steps (GUI in wine), see README.md:"
echo "  1. Native Access:     WINEPREFIX=$NI_PFX   wine \"$INST/Native-Access_2.exe\""
echo "  2. Komplete Kontrol:  WINEPREFIX=$NI_PFX   wine \"$INST/KK334/Komplete Kontrol 3.3.4 Setup PC.exe\""
echo "  3. Reason 14:         WINEPREFIX=$RSN_PFX  wine \"$INST/Reason1410/Install Reason 14.exe\""
echo "  4. After every product install through Native Access:  yabridgectl sync"