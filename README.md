# omarchy-enhancement-project

## WORK IN PROGRESS

A collection of enhancements for **Omarchy on a MacBook Pro Late 2013**
([MacBookPro11,3 at EveryMac](https://everymac.com/ultimate-mac-lookup/?identify=MacBookPro11,3)):
hybrid Intel Iris Pro 5200 (`8086:0d26`) + NVIDIA GT 750M (Kepler, GK107M),
BCM4360 Wi-Fi, bcm5974 touchpad, Bluetooth audio. Every directory is a
self-contained module with its own README.

The project keeps aging x86 hardware running with fully open drivers —
replacing the proprietary Broadcom `wl` stack, AMD/NVIDIA closed blobs and
proprietary AEC paths with open implementations.

> **Status: active development.** Modules vary in maturity (from stable
> userspace fixes to experimental kernel bring-up). See
> [ROADMAP.md](ROADMAP.md) for where each module stands.

## Modules

| Directory | What it does |
|---|---|
| [`kernel/`](kernel/) | `-omnkp` kernel: patches **0001–0020** (nouveau auto-reclock, gmux/switcheroo, runtime PM, bcm5974 touchpad after S3, applesmc) + kernel build/UKI scripts |
| [`gpu/`](gpu/) | **reclocked** — GK107 reclocking daemon (unfreezes Kepler after power cut), Mesa patches, gmux-io, scripts (mesa-manage / CUDA 10.2 / pstate / recover-gpu), **reclockbar** (Hyprland status bar helper) |
| [`wifi/`](wifi/) | BCM4360: **brcmfmac** patch series (open replacement for the proprietary `wl`), build against the module kernel, test/fire scripts |
| [`touchpad/`](touchpad/) | Sleep/resume-dead bcm5974 touchpad — system-sleep hook (rebinds the module); kernel fix lives in `kernel/` (0016–0018) |
| [`audio/`](audio/) | Bluetooth speaker "Zielony Krasnal": RUNNING sink but silence — dummy AVRCP player for WirePlumber; plus a GStreamer audio-preview fix |
| [`mic/`](mic/) | Mic in Discord on Chromium: wrong default source (A2DP has no mic) — fix description |
| [`theme/`](theme/) | **4 Omarchy 4 themes** — [`las`](theme/las/), [`ogien`](theme/ogien/), [`ciemny-las`](theme/ciemny-las/), [`ciemny-ogien`](theme/ciemny-ogien/) + variant tooling |
| [`aec/`](aec/) | `pw-aec-avx` — on-demand acoustic echo cancellation backend for PipeWire (NLMS, AVX2/FMA) |
| [`music/`](music/) | Music production on Omarchy — Reason 14 + Native Instruments under Wine, bridged into Ardour (Wine + ASIO + yabridge) |
| [`gaming/`](gaming/) | RimWorld on the Retina panel via a custom gamescope build (click/scroll input fixes) |
| [`llm/`](llm/) | Haswell-free ollama build used as a Claude Code gateway (cloud models on a CPU that stock ollama refuses) |
| [`system/`](system/) | System changes: config backups (limine, hyprland, chromium-flags), fan scripts, `apple_set_os` / Iris panel plan, NAS mount helpers |
| [`workspaces/`](workspaces/) | **20 workspaces** in the bar — two banks of 10 (`SUPER+ALT+1..0` = 11..20), plus a patched `omarchy.workspaces` widget that renders all of them |

## Install

```bash
./install.sh                     # list modules
./install.sh theme audio touchpad   # specific modules
```

The installer covers the safe modules (theme, audio, touchpad, workspaces).
`kernel`, `gpu`, `wifi`, `aec` are build/install scripts run manually as
described in each module README — they touch bootloaders/initramfs or the audio
daemon and may require a reboot.

## Working notes

Detailed notes, research and diagnostic reports (Polish working chronicles)
are kept **locally** in `notes/` and are not part of this repository.

## License

MIT — see [`LICENSE`](LICENSE). Exception: `theme/*/` carry the base theme's
MIT license (`theme/las/LICENSE-upstream-MIT`).
