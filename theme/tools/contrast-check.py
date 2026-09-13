#!/usr/bin/env python3
"""WCAG 2.1 contrast ratios for key token pairs of a theme.

Usage: contrast-check.py <theme-dir> [theme-dir ...]
"""

import re
import sys
from pathlib import Path

LINE = re.compile(r'(\w+) = "#([0-9a-f]{6})"')

PAIRS = [("foreground", "background"), ("bright_foreground", "background"),
         ("accent", "background"), ("muted", "background"),
         ("foreground", "selection")]


def srgb_lin(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(hx):
    r, g, b = (int(hx[i:i + 2], 16) for i in (0, 2, 4))
    return 0.2126 * srgb_lin(r) + 0.7152 * srgb_lin(g) + 0.0722 * srgb_lin(b)


def ratio(a, b):
    hi, lo = sorted((lum(a), lum(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


for arg in sys.argv[1:]:
    tokens = {m.group(1): m.group(2)
              for line in Path(arg, "colors.toml").read_text().splitlines()
              if (m := LINE.match(line))}
    print(f"\n{Path(arg).name}:")
    for a, b in PAIRS:
        r = ratio(tokens[a], tokens[b])
        flag = "OK" if r >= 4.5 else ("large text only" if r >= 3.0 else "BROKEN")
        print(f"  {a:>18} on {b:<11} {r:>6.2f}:1  {flag}")