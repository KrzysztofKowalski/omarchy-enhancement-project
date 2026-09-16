# ROADMAP

> Open-source enhancements keeping a 2013 laptop productive with fully open
> drivers. Modules are at different maturity — the legend: 🟢 stable-ish,
> 🟡 experimental, 🔴 in bring-up.

## Modules

| Module | Status | Next milestone |
|---|---|---|
| kernel | 🟡 | Patches 0001–0020 working on 7.1.9; `update-kernel.sh` flow landed 2026-09 — verify full build on 7.2.3⁠-omnkp |
| gpu | 🟡 | `reclocked` v5.16 (fan floor) merged; `omarchy/reclockbar` new — hardening on live sessions, NVRAM switch caveats |
| wifi | 🔴 | brcmfmac bring-up: patches 0001–0020 + final integration 0036–0039; olmsg transport prepared (up to T308) — on-hardware run of the bridge hooks still to do |
| touchpad | 🟢 | Userspace resume hook live; migration to the kernel fix (0016–0018) after the next kernel build |
| audio | 🟢 | BT speaker AVRCP fix done (`fix-bt-zielony-krasnal.sh`); preview fix (`fix-audio-preview.sh`) landed |
| aec | 🟡 | On-demand NLMS/AVX2 backend validated (24.7–28.7 dB, 0% idle CPU) — packaging polish, docs |
| mikrofon | 🟢 | Default-source fix documented; repeat script at user level |
| theme | 🟢 | 4 Omarchy-4 themes + tooling (las, ogien, ciemny-las, ciemny-ogien) — TODO: "Bialowieza" wallpaper variant, license decision, WebP serving on the Omarchy side |
| music | 🟡 | Reason + Komplete under Wine → Ardour prototype works — `fetch-installers.sh` packaging, stability notes |
| gaming | 🟡 | RimWorld clicks/scroll fixed via custom gamescope — exit-crash investigation (gamescope-shdr) open |
| llm | 🟡 | Haswell-free ollama gateway works; usage guard in — hardening the account switching |
| system | 🟢 | Backups + scripts; Iris panel plan: Stage 0 done, Stage 1 (`apple_set_os.efi` chainload) pending |
| clock | 🟢 | Bar clock with ticking seconds + system-locale day names (`clock/`); install/check/uninstall verified against a sandbox `$HOME` |

## Cross-cutting

- [ ] Repo publication (first public release of the collection)
- [ ] Per-module sanity CI: `bash -n`, `shellcheck`, `python -m py_compile`
- [ ] Wiki-style module docs for grant/sustainability reviewers
- [ ] WebP pipeline fully owned by the theme server (no JPG sources in the tree)
- [x] English-only pass on deep source comments (done 2026-09-14)

## Notes on process

- Working chronicles (PL) stay local in `notes/` — they are not published.
- Busy bring-up means **on-hardware test discipline** (see `wifi/README.md`
  warnings) — sessions are deliberate, reboots are the only state reset.