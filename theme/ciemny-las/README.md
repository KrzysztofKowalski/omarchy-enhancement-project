# Ciemny Las

Midnight variant of "Las": **surface** lightness (backgrounds, selection,
wallpapers) = 0.10× the original; text and accent colors keep full lightness
for readable contrast on the near-black base. Omarchy 4 format.

## Generation

```bash
python3 tools/darkify.py las ciemny-las 0.10
omarchy theme set ciemny-las
```

Icons: Yaru-olive-dark.

## Contents

- `colors.toml`, per-app configs (`btop.theme`, `chromium.theme`, `eza.yml`, `gtk.css`, `icons.theme`)
- `backgrounds/*.webp` — 11 wallpapers (WebP 2880×1800)
- `preview.webp`, `LICENSE-upstream-MIT`

## Provenance

Generated from `../las/` by `tools/darkify.py`. The base is Abhijeet Swami's
theme (MIT — `LICENSE-upstream-MIT`).

## Status

Works; key token-pair contrasts 9.0–14.3:1 (WCAG AA/AAA).