# Ciemny Ogien

Midnight variant of "Ogien": surface lightness = 0.10× the original, text and
accents at full lightness. Omarchy 4 format.

## Generation

```bash
python3 tools/darkify.py ogien ciemny-ogien 0.10
omarchy theme set ciemny-ogien
```

Icons: Yaru-purple.

## Contents

- `colors.toml`, per-app configs (`btop.theme`, `chromium.theme`, `eza.yml`, `gtk.css`, `icons.theme`)
- `backgrounds/*.webp` — 11 wallpapers (WebP 2880×1800)
- `preview.webp`, `LICENSE-upstream-MIT`

## Provenance

Generated from `../ogien/` by `tools/darkify.py`; `ogien/` in turn from
`../las/`. Base: Abhijeet Swami's theme (MIT — `LICENSE-upstream-MIT`).

## Status

Works; contrast as in `ciemny-las` (surfaces dimmed, text full).