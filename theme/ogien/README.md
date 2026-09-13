# Ogien

"Las" variant with a **green → violet** hue rotation (+140° in HSL space):
palette and wallpapers read as an entirely different theme while the
structure (terminal color semantics) stays identical. Omarchy 4 format.

## Generation

The theme is fully generated from `../las/`:

```bash
python3 tools/ogien-hsl-remap.py
```

Rule: palette tokens — rotate only the green band `[60°, 174°)`;
wallpaper pixels — full color-wheel rotation (+140°), banding-free.
Saturation and lightness unchanged.

## Contents

- `colors.toml` — palette (accent becomes a violet hue, e.g. `#c678dd`)
- per-app configs: `btop.theme`, `chromium.theme`, `eza.yml`, `gtk.css`, `icons.theme`
- `backgrounds/*.webp` — 11 wallpapers (WebP 2880×1800)
- `preview.webp`, `LICENSE-upstream-MIT`

## Install

```bash
omarchy theme set ogien
omarchy theme refresh
```

## Provenance

Derived from `../las/` (base platform: Abhijeet Swami's theme, MIT —
`LICENSE-upstream-MIT`).

## Status

Works; experimental variant (aesthetics review in progress).