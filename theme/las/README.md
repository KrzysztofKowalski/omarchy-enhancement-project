# Las

Base dark forest-green theme for Omarchy — Omarchy 4 format (`colors.toml`
+ generated per-app configs). Green palette over near-black backgrounds,
wallpapers at native panel resolution (2880×1800).

## Palette (key colors)

| Token | Value |
|---|---|
| `background` | `#0f1a14` |
| `lighter_background` | `#1e2a24` |
| `selection` | `#2e4d3d` |
| `accent` | `#6bbf7a` |
| `foreground` | `#a8e6a1` |
| active border (hyprland) | `#6bbf7a → #cddc39`, 45° |

Full palette: `colors.toml`.

## Contents

- `colors.toml` — Omarchy 4 palette
- per-app configs: `btop.theme`, `chromium.theme`, `eza.yml`, `gtk.css`, `icons.theme`
- `backgrounds/*.webp` — 11 wallpapers (`1-FG … 10-FG`, `las_01`), WebP 2880×1800
- `preview.webp` — theme preview
- `LICENSE-upstream-MIT` — base theme license (MIT)

## Install

```bash
omarchy theme set las
omarchy theme refresh
omarchy theme bg set theme/las/backgrounds/5-FG.webp
```

## Provenance

The palette and wallpapers derive from an Omarchy theme by **Abhijeet
Swami** (MIT) — the license text is kept in `LICENSE-upstream-MIT` and the
attribution follows the license. This variant drops that repo's Omarchy 2.x
files and ships only what Omarchy 4 reads.

## Status

Works on Omarchy 4. Planned: a "Bialowieza" wallpaper variant, license
decision for the derived variant (upstream MIT applies for now).