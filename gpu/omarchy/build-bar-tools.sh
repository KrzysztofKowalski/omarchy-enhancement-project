#!/usr/bin/env bash
# build-bar-tools.sh — builds the Omarchy bar binary tools (reclockbar)
# into ~/.config/omarchy/bar/bin/. Run by USER (pattern: reclocked-rebuild.sh).
#   bash omarchy/build-bar-tools.sh          # build
#   omarchy restart shell                    # after the build, so modules pick up the new code
set -euo pipefail
PROJ="$(cd "$(dirname "$0")/.." && pwd)"   # report 112: script in a subdir → PROJ = parent
BIN_DIR="$HOME/.config/omarchy/bar/bin"
mkdir -p "$BIN_DIR"

g++ -O2 -std=c++17 -Wall -Wextra -o "$BIN_DIR/reclockbar" "$PROJ/omarchy/reclockbar.cpp"
echo "OK: $BIN_DIR/reclockbar"