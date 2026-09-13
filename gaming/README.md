# gaming/ — RimWorld on the Retina panel via a custom gamescope

RimWorld on the MacBook Pro Late 2013 (retina 2880×1800 @ scale 2) — the
game runs fullscreen through a **custom-built gamescope fork** that fixes
the dead-click/scroll input problem.

## Context

- Panel 2880×1800 @ scale 2; game 1440×900 fullscreen (50% retina) — the
  game config is NOT modified (full respect for
  `~/.config/unity3d/Ludeon Studios/RimWorld by Ludeon Studios/…`).
- Hyprland rule `~/.config/hypr/hyprland.lua`: RimWorld = float ONLY
  (any WM-resize desynchronizes Unity input).
- Root cause of dead clicks: RimWorld 1.6 Linux bug — game resolution ≠
  desktop resolution → clicks land on the wrong UI layer. Windowed/
  native-2880 were rejected by the user (fullscreen is mandatory).

## The custom gamescope build

- Fork (this project): build with `./build-gamescope.sh` (idempotent;
  binary at `gamescope/build/src/gamescope`, NOT installed system-wide —
  the system gamescope 3.16.25 stays as fallback).
- Extra dep: `vulkan-headers` (pkg-config passes on the icd-loader alone,
  but the header check fails without it).
- Steam launch options:

  ```
  /path/to/gaming/gamescope/build/src/gamescope -W 2880 -H 1800 -w 1440 -h 900 -f -S integer -F nearest -- %command%
  ```

  `-S integer` = hard ×2 scaling; `-F nearest` = nearest-neighbor, no
  blur/aliasing (user's choice; FSR excluded).
- Vulkan: `intel_hasvk` (experimental, Haswell) — works, warnings benign.

## Status

- **Click works** — confirmed; solution: nested gamescope.
- **Scroll works** — confirmed in game (save list + zoom); sensitivity ×4,
  tunable via `GAMESCOPE_SCROLL_SCALE`.
- Known crash (2026-09-02): after the game exits, gamescope occasionally
  SIGABRTs in `gamescope-shdr` (`compileAllPipelines`) — glibc assert on a
  corrupted mutex; cosmetic (happens on exit), still under investigation.

## scroll-tester

`scroll-tester.c` — small tool that prints scroll events (with the
`GAMESCOPE_SCROLL_SCALE` multiplier applied) to verify gamescope's scroll
scaling in isolation.

```bash
cc scroll-tester.c -o scroll-tester $(pkg-config --cflags --libs libinput xkbcommon)
# or use the prebuilt ./scroll-tester in the source repo
```

## Files

- `build-gamescope.sh` — builds the gmescope fork (no system install)
- `scroll-tester.c` — scroll-scale verification tool
- `gamescope/` — the fork checkout (added via `git submodule`, not vendored)