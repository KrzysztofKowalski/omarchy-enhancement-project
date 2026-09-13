# music/ — Reason 14 + Komplete Kontrol under Wine → Ardour (Omarchy/Arch)

State as of 2026-09. Everything below was tested on this machine: wine
11.16 (WoW64), Ardour 9.7, PipeWire 1.6.8 + pipewire-jack, yabridge 5.1.1
from the official repo.

**Nothing here is pirated**: the installers are official; activation goes
through your own Native Instruments / Reason Studios accounts. Installers
are downloaded by the scripts directly from the vendors — they are not
bundled in this repo.

## What it is

Production music setup on Omarchy: **Reason 14** (DAW + Rack Plugin VST3)
and **Native Instruments Komplete Kontrol** (Komplete Ultimate instruments)
run under Wine, bridged into **Ardour** via yabridge, with WineASIO for
standalone Reason.

## Quick start

```bash
# 0. One-time: environment (packages, prefixes, winetricks, yabridgectl)
./setup-wine.sh

# (handy replacement for steps 1–4) — GUI installers one by one, yabridgectl sync at the end
./run-installers.sh

# 1. Native Access (sign in to your NI account, download Komplete Ultimate)
WINEPREFIX=~/winprefixes/ni wine installers/Native-Access_2.exe

# 2. Komplete Kontrol (VST3 plugin + app)
WINEPREFIX=~/winprefixes/ni wine "installers/KK334/Komplete Kontrol 3.3.4 Setup PC.exe"

# 3. Reason 14 (DAW + Reason Rack Plugin VST3)
WINEPREFIX=~/winprefixes/reason wine "installers/Reason1410/Install Reason 14.exe"

# 4. After EVERY product install in Native Access (new plugins appear):
yabridgectl sync

# 5. Ardour: Edit → Preferences → VST — make sure it scans ~/.vst3 (it does by default)
#    Instrument = MIDI track → right-click → Add Plugin → VST3
```

`setup-wine.sh` installs `yabridge` + `yabridgectl` + `winetricks`,
creates the prefixes `~/winprefixes/ni` and `~/winprefixes/reason` (each
Windows 10), runs `winetricks corefonts d3dcompiler_47 vcrun2022` in both,
unpacks the zips, registers the VST dirs in `yabridgectl` and does the
first `sync`.

The installers come from official, unauthenticated CDNs — the script prints
the URLs so you can fetch them from any machine:

| File | Size | Source |
|---|---|---|
| `Native-Access_2.exe` | 186 MB | https://storage.googleapis.com/ni-assets/downloads/Native-Access_2.exe |
| `Komplete_Kontrol_334_PC.zip` | 268 MB | Native Instruments CDN (inmusicbrands) |
| `Komplete_Kontrol_2.9.6_Installer.zip` | 213 MB | same CDN (VST2 fallback) |
| `Reason_1410_Win.zip` | 6.9 GB | https://cdn.reasonstudios.com/update/Stable/… (stable `updates_api/latest_reason_installer/win` link) |

> Komplete Ultimate (libraries/instruments) only via Native Access after
> logging in — the stand-alone Kontrol installer above is separate.

## Option B — Reason standalone as a DAW (next to Ardour)

Reason on Windows requires ASIO → under Wine that is **WineASIO**. The
AUR `wineasio` 1.3.0 **does not work** on wine 11 WoW64 — that is why there
is `setup-wineasio.sh`, which builds a fork of `giang17/wineasio` 1.5.0
(the only version supporting Wine 11 WoW64):

```bash
./setup-wineasio.sh       # mingw-w64-gcc + build + install + register
WINEPREFIX=~/winprefixes/reason wine reg query "HKLM\Software\ASIO\WineASIO"  # verify
```

Audio path: Reason (ASIO) → wineasio → JACK (pipewire-jack) → output.
Sample rate must match the system (e.g. 48 kHz in both Reason and PW).
The most stable route is the **Reason Rack Plugin through yabridge in
Ardour** (option A) — standalone Reason under wine 10/11 can be finicky.

### Reason license activation under Wine

Reason 12.7+/14 licenses through **Reason Companion**, whose UI is
WebView2 — under Wine it can crash on DirectComposition. Workaround (same
prefix):

```bash
WINEPREFIX=~/winprefixes/reason wine reg add "HKCU\Software\Wine\AppDefaults\msedgewebview2.exe" /v Version /t REG_SZ /d win8 /f
```

If Companion still will not start: install Reason once on a real Windows
(or in a VM), then copy the program dir and activation state
(`AppData\Propellerhead` + `Program Files\Propellerhead\Reason 14`) into
the prefix — the license state travels with the files. The free "Reason
Free" (15 devices) also works as a Rack Plugin with no activation.

## Troubleshooting (short)

| Symptom | What to do |
|---|---|
| Plugin missing in Ardour | `yabridgectl status` (64-bit + "OK"), then `yabridgectl sync`, restart Ardour |
| Plugin GUI not redrawing | `WINEPREFIX=~/winprefixes/ni winetricks -q dxvk` — on this machine (Haswell/GT 750M without hardware Vulkan) DXVK falls back to software lavapipe; slower, but draws |
| yabridge crashes with wine 10/11 | `yay -S yabridge-wine10-git` (new-wine10-embedding branch) + `yabridgectl sync`; emergency: wine 9.21 from archive.archlinux.org |
| "Internal Error" on Reason start | win10 in winecfg (set), copy `d3d9.dll`/`dxgi.dll` from the prefix into the Reason program dir; virtual desktop in winecfg; optionally `winetricks -q dxvk` |
| Reason standalone: menu won't open | WineHQ bug 10845 (unmerged MR 11667) — use keyboard shortcuts / Rack Plugin in Ardour |
| Native Access stuck "Installing, please wait..." | the bundled `powershell.exe` is a stub — install `winetricks -q powershell` (PowerShell Core 7.4 + wrapper replacing `powershell.exe`) |
| Ardour doesn't see VST3 | Preferences → VST → add `~/.vst3/yabridge`; do not use Flatpak Ardour (yabridge does not work with Flatpak) |

## Notes

- Prefixes `~/winprefixes/*` are disposable/rebuildable; libraries and
  projects live outside them (NI libraries in `~/komplete-libraries`,
  = `Z:\home\…` seen from wine — hundreds of GB, easy backup).
- Real-time group for low latency: `sudo usermod -aG realtime "$USER"`
  (then log out and back in).
- Known Wine issue: the Reason menu bar won't open (WineHQ bug 10845,
  fix still unmerged); the rest of the UI works.

## Status

Working prototype (2026-09). TODO: MIDI controller (NKS) mapping notes,
H/WK latency tuning on the built-in audio, packaging the installer
downloads into a single `fetch-installers.sh`.