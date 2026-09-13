#!/usr/bin/env python3
"""Las -> Ogien: HSL hue rotation greens -> violets.

Rule:
  palette tokens: hue in green band [60°, 174°)  ->  hue += 140°  (mod 360)
  wallpaper pixels: EVERY hue                    ->  hue += 140°  (full
  color-wheel rotation — banding-free, reads as one coherent recolor;
  the band restriction stays palette-only so red/yellow/cyan terminal
  semantics survive)
  saturation, lightness: unchanged everywhere.

Usage: python3 tools/ogien-hsl-remap.py [src] [dst]
Builds the "Ogien" variant (green->violet HSL remap) from the "Las" theme.
Defaults to the sibling directories in this repo (theme/las -> theme/ogien).
"""

import colorsys
import re
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "las"
DST = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "ogien"

DELTA = 140.0            # green ~131° -> violet ~271°
WIN_LO, WIN_HI = 60.0, 174.0   # green band for palette tokens

HEX_RE = re.compile(r"#([0-9a-fA-F]{6})(?![0-9a-fA-F])")
RGBA_RE = re.compile(r"rgba\(([0-9a-fA-F]{6})ff\)")


def hex_to_hsl(h):
    r, g, b = (int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))
    hh, ll, ss = colorsys.rgb_to_hls(r, g, b)  # note HLS ordering
    return hh * 360.0, ss, ll


def hsl_to_hex(hdeg, s, l):
    r, g, b = colorsys.hls_to_rgb((hdeg % 360) / 360.0, l, s)
    return "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255))


def rotate_token(hdeg):
    """Binary rule for palette tokens."""
    return hdeg + DELTA if WIN_LO <= hdeg < WIN_HI else hdeg


def remap_hex(m):
    h, s, l = hex_to_hsl(m.group(1))
    return hsl_to_hex(rotate_token(h), s, l)


def remap_pixels(arr):
    """arr: float RGB 0..1 -> full color-wheel rotation by +140°."""
    mx = arr.max(-1)
    mn = arr.min(-1)
    d = mx - mn
    l = (mx + mn) / 2
    s = np.where(d == 0, 0.0, d / (1 - np.abs(2 * l - 1) + 1e-12))
    r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
    m = d > 0
    dm = np.where(m, d, 1)
    h = np.select(
        [mx == r, mx == g, mx == b],
        [((g - b) / dm) % 6, (b - r) / dm + 2, (r - g) / dm + 4],
    ) * 60.0
    h = np.where(m, h, 0.0)
    h = (h + DELTA) % 360

    c = (1 - np.abs(2 * l - 1)) * s
    hp = h / 60
    x = c * (1 - np.abs(hp % 2 - 1))
    zeros = np.zeros_like(c)
    r1 = np.select([hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [c, x, zeros, zeros, x], c)
    g1 = np.select([hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [x, c, c, x, zeros], zeros)
    b1 = np.select([hp < 1, hp < 2, hp < 3, hp < 4, hp < 5], [zeros, zeros, x, c, c], c)
    mv = l - c / 2
    return np.clip(np.stack([r1 + mv, g1 + mv, b1 + mv], -1), 0, 1)


def main():
    if DST.exists():
        shutil.rmtree(DST)
    DST.mkdir(parents=True)
    shutil.copy2(SRC / "LICENSE-upstream-MIT", DST / "LICENSE-upstream-MIT")

    # --- palette report + colors.toml ---
    report = []
    toml_out = []
    for line in (SRC / "colors.toml").read_text().splitlines():
        mm = re.match(r'(\w+) = "#([0-9a-f]{6})"$', line)
        if mm:
            key, hx = mm.groups()
            h, s, l = hex_to_hsl(hx)
            nh = rotate_token(h)
            new = hsl_to_hex(nh, s, l)
            report.append((key, hx, h, new, nh))
            toml_out.append(f'{key} = "{new}"')
        else:
            def sub_rgba(m):
                h, s, l = hex_to_hsl(m.group(1))
                return "rgba(%sff)" % hsl_to_hex(rotate_token(h), s, l)[1:]
            toml_out.append(RGBA_RE.sub(sub_rgba, line))
    (DST / "colors.toml").write_text("\n".join(toml_out) + "\n")

    print(f"{'token':<26}{'old':<10}{'hue':>7}   {'new':<10}{'hue\'':>7}")
    for key, hx, h, new, nh in report:
        mark = " *" if nh != h else ""
        print(f"{key:<26}{hx:<10}{h:>7.1f}   {new:<10}{nh:>7.1f}{mark}")

    # --- btop.theme, gtk.css, eza.yml: hex substitution ---
    for fname in ("btop.theme", "gtk.css", "eza.yml"):
        txt = (SRC / fname).read_text()
        if fname == "gtk.css":  # icon recolor filter follows the same shift
            txt = txt.replace("hue-rotate(192deg)", "hue-rotate(332deg)")
        out = HEX_RE.sub(remap_hex, txt)
        (DST / fname).write_text(out)
        n = len(HEX_RE.findall(txt))
        print(f"{fname}: {n} hexes remapped")

    # --- chromium.theme: "r,g,b" triplet ---
    trip = (SRC / "chromium.theme").read_text().strip()
    rgb = [int(v) for v in trip.split(",")]
    h, s, l = hex_to_hsl("%02x%02x%02x" % tuple(rgb))
    nh = rotate_token(h)
    nr, ng, nb = (round(v * 255) for v in colorsys.hls_to_rgb((nh % 360) / 360, l, s))
    print(f"chromium.theme: rgb({rgb[0]},{rgb[1]},{rgb[2]}) hue {h:.1f} -> ({nr},{ng},{nb}) hue {nh:.1f}")
    (DST / "chromium.theme").write_text(f"{nr},{ng},{nb}\n")

    # --- icons: olive -> purple ---
    (DST / "icons.theme").write_text("Yaru-purple\n")

    # --- backgrounds + preview ---
    bsrc, bdst = SRC / "backgrounds", DST / "backgrounds"
    bdst.mkdir()
    for img in sorted(list(bsrc.glob("*.jpg")) + [SRC / "preview.png"]):
        dest = bdst / img.name if img.parent == bsrc else DST / img.name
        im = Image.open(img).convert("RGB")
        out = (remap_pixels(np.asarray(im) / 255.0) * 255).round().astype(np.uint8)
        q = 90 if img.name == "las_01.jpg" else 85
        if img.suffix == ".png":
            Image.fromarray(out).save(dest, optimize=True)
        else:
            Image.fromarray(out).save(dest, quality=q, optimize=True)
        print(f"{img.name}: {im.size[0]}x{im.size[1]} remapped ({dest.stat().st_size // 1024} KB)")

    print("\nOK ->", DST)


if __name__ == "__main__":
    main()