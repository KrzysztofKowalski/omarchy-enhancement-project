#!/usr/bin/env python3
"""Ciemny (midnight) variant builder.

Default: only surfaces dim — background/selection tokens and wallpapers
get HSL lightness *= FACTOR; text & accent colors keep full lightness
(dimming text too collapses WCAG contrast to ~1:1 = unreadable).
--all: literal behavior — every token and pixel *= FACTOR.

Palette tokens: HSL roundtrip, L' = L*FACTOR, hue & saturation kept.
Images: RGB * FACTOR — HSL lightness is linear in channels, so L scales
exactly by FACTOR; cheaper and banding-free vs. a per-pixel HSL loop.

Usage: python3 tools/darkify.py [--all] <src> <dst> [factor=0.10]
"""

import colorsys
import re
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent

HEX_RE = re.compile(r"#([0-9a-fA-F]{6})(?![0-9a-fA-F])")
RGBA_RE = re.compile(r"rgba\(([0-9a-fA-F]{6})ff\)")
TOML_LINE = re.compile(r'^(\w+) = "#([0-9a-f]{6})"$')

SURFACES = {"background", "dark_background", "darker_background",
            "lighter_background", "selection"}

DISPLAY = {"las": "Las", "ogien": "Ogien",
           "ciemny-las": "Ciemny Las", "ciemny-ogien": "Ciemny Ogien"}


def scale_hex(hx, factor):
    r, g, b = (int(hx[i:i + 2], 16) / 255 for i in (0, 2, 4))
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    return "#%02x%02x%02x" % tuple(round(v * 255) for v in colorsys.hls_to_rgb(h, l * factor, s))


def sub_hexes(text, factor):
    return HEX_RE.sub(lambda m: scale_hex(m.group(1), factor), text)


def scale_triplet(text, factor):
    rgb = [int(v) for v in text.strip().split(",")]
    h, l, s = colorsys.rgb_to_hls(*(v / 255 for v in rgb))
    return "%d,%d,%d\n" % tuple(round(v * 255) for v in colorsys.hls_to_rgb(h, l * factor, s))


def main():
    args = [a for a in sys.argv[1:] if a != "--all"]
    all_tokens = "--all" in sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, dst = Path(args[0]), Path(args[1])
    factor = float(args[2]) if len(args) > 2 else 0.10
    src = src if src.is_absolute() else REPO / src
    dst = dst if dst.is_absolute() else REPO / dst
    if not (src / "colors.toml").exists():
        sys.exit(f"no colors.toml in {src}")

    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir()
    shutil.copy2(src / "LICENSE-upstream-MIT", dst / "LICENSE-upstream-MIT")

    # colors.toml: dim surface tokens only, keep text/accents at full L
    out = []
    for line in (src / "colors.toml").read_text().splitlines():
        m = TOML_LINE.match(line)
        if m:
            key, hx = m.groups()
            if all_tokens or key in SURFACES:
                hx = scale_hex(hx, factor)[1:]
            out.append(f'{key} = "#{hx}"')
        elif all_tokens:
            out.append(RGBA_RE.sub(
                lambda mm: "rgba(%sff)" % scale_hex(mm.group(1), factor)[1:], line))
        else:
            out.append(line)
    (dst / "colors.toml").write_text("\n".join(out) + "\n")
    scope = "every token" if all_tokens else "surfaces only (text/accents keep full lightness)"
    print(f"colors.toml: lightness *= {factor} ({scope})")

    for fname in ("btop.theme", "gtk.css", "eza.yml"):
        txt = (src / fname).read_text()
        if all_tokens:
            txt = sub_hexes(txt, factor)
        elif fname == "btop.theme":  # only *_bg entries are surfaces
            txt = re.sub(r'theme\[(\w+)\]="#([0-9a-fA-F]{6})"',
                         lambda m: f'theme[{m.group(1)}]="' +
                                   (scale_hex(m.group(2), factor)
                                    if m.group(1).endswith("_bg") else m.group(2)) + '"',
                         txt)
        elif fname == "gtk.css":  # only the background/black surface definitions
            txt = re.sub(r'(@define-color (?:background|black)\s+)#([0-9a-fA-F]{6})',
                         lambda m: m.group(1) + scale_hex(m.group(2), factor), txt)
        # eza.yml: all text colors — untouched unless --all
        (dst / fname).write_text(txt)
    print("btop.theme / gtk.css / eza.yml: surfaces dimmed, text kept"
          if not all_tokens else "btop.theme / gtk.css / eza.yml remapped")

    (dst / "chromium.theme").write_text(scale_triplet((src / "chromium.theme").read_text(), factor))

    icons = (src / "icons.theme").read_text().strip()
    if not icons.endswith("-dark"):
        icons += "-dark"
    (dst / "icons.theme").write_text(icons + "\n")
    print(f"icons: {icons}")

    # wallpapers + preview: RGB * factor == lightness * factor
    bdst = dst / "backgrounds"
    bdst.mkdir()
    for img in sorted(list((src / "backgrounds").glob("*.jpg")) + [src / "preview.png"]):
        dest = (bdst / img.name) if img.parent == src / "backgrounds" else dst / img.name
        im = Image.open(img).convert("RGB")
        dim = (np.asarray(im) * factor).round().clip(0, 255).astype(np.uint8)
        if img.suffix == ".png":
            Image.fromarray(dim).save(dest, optimize=True)
        else:
            Image.fromarray(dim).save(
                dest, quality=90 if img.name == "las_01.jpg" else 85, optimize=True)
        print(f"{img.name}: dimmed ({dest.stat().st_size // 1024} KB)")

    name = DISPLAY.get(dst.name, dst.name)
    src_name = DISPLAY.get(src.name, src.name)
    if all_tokens:
        rule = (f"every color's HSL lightness is {factor:.2f}x the original "
                "(hue & saturation unchanged); wallpapers scale the same way")
    else:
        rule = (f"surface lightness (backgrounds, selection, wallpapers) is "
                f"{factor:.2f}x the original; text & accent colors keep full "
                "lightness for readable contrast on the near-black base")
    (dst / "README.md").write_text(f"""# {name}

Midnight variant of {src_name}: {rule}. Built for the Omarchy 4 theme
format (`colors.toml` + generated per-app configs). Not published yet.

**Author:** [KrzysztofKowalski](https://github.com/KrzysztofKowalski)

Generated by `../tools/darkify.py` from `{src.name}/`. Icons {icons}.
Upstream lineage: Abhijeet Swami's MIT-licensed theme (license text kept in
`LICENSE-upstream-MIT`).
""")

    print("OK ->", dst)


if __name__ == "__main__":
    main()