#!/usr/bin/env bash
# =============================================================================
# run-installers.sh — GUI installers under Wine: Native Access → Komplete
# Kontrol → Reason 14 → Reason Companion; sequential (waits until each
# process exits), then yabridgectl sync.
#
# !!! Do NOT run this script a second time while installer windows are still
#     open — two installers in the same prefix = overlapping wine windows,
#     only the top one clickable. The script guards itself (lockfile), but
#     if you interrupt it with Ctrl+C, wait for the wine processes to exit
#     (or: WINEPREFIX=... wineserver -k) before running it again.
#
# REQUIRES ./setup-wine.sh to have run first: it creates the ~/winprefixes/ni
# and ~/winprefixes/reason prefixes and unpacks the installers into installers/.
#
# The steps are interactive (GUI windows in wine) — read the script's messages.
# Each installer is idempotent (detects an existing install), so interrupting
# with Ctrl+C and rerunning the script is safe.
# =============================================================================
set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
INST="$PROJ/installers"
NI_PFX="$HOME/winprefixes/ni"        # prefix: Native Access + Komplete Kontrol
RSN_PFX="$HOME/winprefixes/reason"   # prefix: Reason 14 + Reason Rack Plugin

NA_BOOTSTRAP="$INST/Native-Access_2.exe"
NA_APP="$NI_PFX/drive_c/Program Files/Native Instruments/Native Access/Native Access.exe"
KK_SETUP="$INST/KK334/Komplete Kontrol 3.3.4 Setup PC.exe"
REASON_SETUP="$INST/Reason1410/Install Reason 14.exe"
# Reason Companion: we pick the newest existing version — 3.2.1 (downloaded,
# ~/Downloads) or 3.2.0 (from the official Reason zip, installers/Reason1410/).
COMPANION_NEW="$HOME/Downloads/Reason Companion 3.2.1-win.exe"
COMPANION_OLD="$INST/Reason1410/Reason Companion 3.2.0-win.exe"

pauza() {  # pause before a GUI step; without stdin (e.g. </dev/null) we continue
    read -r -p "  [Enter] to continue… " _ || true
}

# -----------------------------------------------------------------------------
# Guard against double invocation — two parallel installers in the same
# prefix mean overlapping wine windows (only the top one clickable).
# -----------------------------------------------------------------------------
LOCK="/tmp/run-installers.lock"
if [ -e "$LOCK" ]; then
    LPID="$(cat "$LOCK" 2>/dev/null || true)"
    if [ -n "$LPID" ] && kill -0 "$LPID" 2>/dev/null; then
        echo ""
        echo "ERROR: run-installers.sh is already running (PID $LPID, lock: $LOCK)." >&2
        echo "      A second run = two installers at once = overlapping windows." >&2
        echo "      Close the installer windows from the first run and wait," >&2
        echo "      until that process exits (or finish the steps manually)." >&2
        exit 1
    fi
    echo "WARNING: removing a stale lock from a previous run (PID $LPID no longer runs)."
    rm -f "$LOCK"
fi
echo "$$" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT

# -----------------------------------------------------------------------------
# 0. Environment check — did setup-wine.sh already run
# -----------------------------------------------------------------------------
echo "=== [0/5] Environment check ==="
echo "  Wine prefixes:"
BRAK_PREFIXU=0
for P in "$NI_PFX" "$RSN_PFX"; do
    if [ -d "$P" ]; then
        echo "    OK   $P"
    else
        echo "    MISSING $P"
        BRAK_PREFIXU=1
    fi
done
if [ "$BRAK_PREFIXU" -ne 0 ]; then
    echo ""
    echo "ERROR: required wine prefixes are missing."
    echo "      Run first:  ./setup-wine.sh"
    echo "      (creates the prefixes, installs packages + winetricks, registers yabridgectl)."
    exit 1
fi

# -----------------------------------------------------------------------------
# 1. Native Access — installer bootstrap, then the app (login,
#    download and install of Komplete Ultimate — THE LONGEST step, hours)
# -----------------------------------------------------------------------------
echo ""
echo "=== [1/5] Native Access — installing the app ==="
if [ ! -f "$NA_BOOTSTRAP" ]; then
    echo "ERROR: installer file missing: $NA_BOOTSTRAP"
    echo "      Download it without logging in (link in README):"
    echo "      https://storage.googleapis.com/ni-assets/downloads/Native-Access_2.exe"
    exit 1
fi

# Native Access runs scripts through powershell.exe — wine's built-in
# powershell.exe is a STUB ("fixme:powershell:wmain stub"), which leaves the
# installer hanging on "Installing, please wait...". Fix: winetricks powershell
# (PowerShell Core 7.4 + a wrapper powershell.exe replacing the stub). Idempotent.
PWSH_CORE="$NI_PFX/drive_c/Program Files/PowerShell/7/pwsh.exe"
PWSH_WRAP="$NI_PFX/drive_c/windows/system32/WindowsPowerShell/v1.0/powershell.exe"
if [ ! -f "$PWSH_CORE" ] || [ ! -f "$PWSH_WRAP" ]; then
    echo "  Real PowerShell missing (the wine stub is not enough for the NA installer)."
    echo "  Installing: winetricks powershell (PowerShell Core + wrapper)…"
    for i in 1 2 3; do
        if WINEPREFIX="$NI_PFX" winetricks -q powershell; then
            echo "  PowerShell installed (attempt $i)."
            break
        fi
        echo "  WARNING: attempt $i failed — retrying (winetricks can be flaky on wine 11)."
    done
    if [ ! -f "$PWSH_CORE" ] || [ ! -f "$PWSH_WRAP" ]; then
        echo "ERROR: PowerShell still not installed — check the winetricks output above." >&2
        exit 1
    fi
else
    echo "  PowerShell OK ($PWSH_CORE)."
fi
echo "  Launching: $NA_BOOTSTRAP"
echo "  In the GUI: install the Native Access app (this is just the bootstrap —"
echo "  login and libraries come in a moment, in the next step)."
pauza
WINEPREFIX="$NI_PFX" wine "$NA_BOOTSTRAP"

echo ""
echo "--- Native Access: login and Komplete Ultimate installation ---"
echo "  Installer finished. I will start the Native Access app in a moment:"
echo "    $NA_APP"
echo "  (You can also do this manually:  WINEPREFIX=$NI_PFX wine \"$NA_APP\")"
echo ""
echo "  In the Native Access GUI:"
echo "    * log in to your Native Instruments account,"
echo "    * install Komplete Ultimate (libraries/instruments)."
echo ""
echo "  This is THE LONGEST step — the download can take hours. When the"
echo "  products are installed, simply CLOSE Native Access — the script continues."
echo ""
echo "  You can interrupt any time with Ctrl+C and come back later — rerunning"
echo "  ./run-installers.sh is safe (the steps are idempotent)."
echo ""
echo "  If login/install gets stuck (see README):"
echo "    * winetricks powershell (PowerShell Core + wrapper — powershell_core alone"
echo "      is not enough: the NA installer calls powershell.exe, not pwsh.exe)"
echo "    * run the NTKDaemon installer manually from:"
echo "      $NI_PFX/drive_c/.../Native Access/resources/daemon/win/"
echo "    * on Hyprland you also need xdg-desktop-portal"
echo ""
if [ -f "$NA_APP" ]; then
    pauza
    WINEPREFIX="$NI_PFX" wine "$NA_APP"
    echo "  Native Access closed — assuming the Komplete products are installed."
else
    echo "  WARNING: Native Access app not found at the expected path:"
    echo "    $NA_APP"
    echo "  Run it manually (command above), log in and install"
    echo "  Komplete Ultimate; when closed, come back to the script:"
    pauza
fi

# -----------------------------------------------------------------------------
# 2. Komplete Kontrol 3.3.4 — app + VST3 plugin
# -----------------------------------------------------------------------------
echo ""
echo "=== [2/5] Komplete Kontrol 3.3.4 (app + VST3 plugin) ==="
if [ ! -f "$KK_SETUP" ]; then
    echo "ERROR: installer file missing: $KK_SETUP"
    echo "      Unpack the zip:  unzip -o \"$INST/Komplete_Kontrol_334_PC.zip\" -d \"$INST/KK334\""
    exit 1
fi
echo "  Launching: $KK_SETUP"
echo "  In the GUI: install Komplete Kontrol (standalone app + VST3 plugin;"
echo "  the libraries are NOT included here — they went in step 1 through Native Access)."
pauza
WINEPREFIX="$NI_PFX" wine "$KK_SETUP"
echo "  Komplete Kontrol installed (VST3: $NI_PFX/drive_c/Program Files/Common Files/VST3)."

# -----------------------------------------------------------------------------
# 3. Reason 14 — DAW + Reason Rack Plugin (VST3)
# -----------------------------------------------------------------------------
echo ""
echo "=== [3/5] Reason 14 — DAW + Reason Rack Plugin (VST3) ==="
if [ ! -f "$REASON_SETUP" ]; then
    echo "ERROR: installer file missing: $REASON_SETUP"
    echo "      Unpack the zip:  unzip -o \"$INST/Reason_1410_Win.zip\" -d \"$INST/Reason1410\""
    exit 1
fi
echo "  Launching: $REASON_SETUP"
for B in "$INST/Reason1410/Install Reason 14-1.bin" "$INST/Reason1410/Install Reason 14-2.bin"; do
    if [ ! -f "$B" ]; then
        echo "  WARNING: data file missing: $B — the installer may ask for the media."
    fi
done
echo "  In the GUI select the components:"
echo "    * Reason 14 (DAW)"
echo "    * Reason Rack Plugin — VST3 into C:\Program Files\Common Files\VST3"
echo "      (that is the 'Common Files/VST3' directory in the $RSN_PFX prefix, already"
echo "       registered in yabridgectl by setup-wine.sh)."
pauza
WINEPREFIX="$RSN_PFX" wine "$REASON_SETUP"
echo "  Reason 14 installed. License activation — in step 4 (Companion)."

# -----------------------------------------------------------------------------
# 4. Reason Companion — Reason activation + Factory Sound Bank download
# -----------------------------------------------------------------------------
echo ""
echo "=== [4/5] Reason Companion — Reason activation and Factory Sound Bank ==="
COMPANION=""
if [ -f "$COMPANION_NEW" ]; then
    COMPANION="$COMPANION_NEW"
elif [ -f "$COMPANION_OLD" ]; then
    COMPANION="$COMPANION_OLD"
fi
if [ -z "$COMPANION" ]; then
    echo "  WARNING: no Reason Companion installer found (looked for:"
    echo "    $COMPANION_NEW"
    echo "    $COMPANION_OLD"
    echo "  ). Skipping this step — do the activation and Factory Sound Bank later"
    echo "  manually (how in the README)."
else
    echo "  Using (newest found): $COMPANION"
    echo "  Workaround WebView2 (without it the Companion login window does not work under wine):"
    WINEPREFIX="$RSN_PFX" wine reg add "HKCU\Software\Wine\AppDefaults\msedgewebview2.exe" /v Version /t REG_SZ /d win8 /f
    echo "  Launching: $COMPANION"
    echo "  In the GUI:"
    echo "    * log in to your Reason Studios account — this ACTIVATES Reason 14,"
    echo "    * download the Factory Sound Bank through Companion"
    echo "      (it is NOT inside the Reason installers!)."
    pauza
    WINEPREFIX="$RSN_PFX" wine "$COMPANION"
    echo "  Reason Companion closed."
fi

# -----------------------------------------------------------------------------
# 5. yabridgectl sync + summary
# -----------------------------------------------------------------------------
echo ""
echo "=== [5/5] yabridgectl sync ==="
yabridgectl sync

echo ""
echo "============================================================"
echo " DONE — installers finished."
echo ""
echo " What next:"
echo "  1. The VST3 plugins live in the directories registered with yabridgectl:"
echo "       $NI_PFX/drive_c/Program Files/Common Files/VST3   (Komplete Kontrol)"
echo "       $RSN_PFX/drive_c/Program Files/Common Files/VST3  (Reason Rack Plugin)"
echo "     Check the list:  yabridgectl list"
echo "  2. Start Ardour → Edit → Preferences → VST: the ~/.vst3 directory is"
echo "     scanned by default (that is where yabridge exposes the plugin .so files)."
echo "  3. Instrument track (MIDI track) → right click → Add Plugin → VST3."
echo ""
echo " Reminders:"
echo "  * After EVERY future product install in Native Access:"
echo "      yabridgectl sync"
echo "  * After a wine package update rebuild wineasio:  ./setup-wineasio.sh"
echo "  * The Reason menu bar does not work under wine (known bug) — workaround"
echo "    WebView2 and the rest are in the Troubleshooting section of the README."
echo "============================================================"
