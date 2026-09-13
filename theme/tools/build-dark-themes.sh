#!/usr/bin/env bash
# Build + install midnight variants (surfaces at 10% lightness, text intact).
# Usage: build-dark-themes.sh [active-theme]   (default: ciemny-las)
set -euo pipefail
cd "$(dirname "$0")/.."

python3 tools/darkify.py las ciemny-las 0.10
python3 tools/darkify.py ogien ciemny-ogien 0.10

ln -sfn "$PWD/ciemny-las" "$HOME/.config/omarchy/themes/ciemny-las"
ln -sfn "$PWD/ciemny-ogien" "$HOME/.config/omarchy/themes/ciemny-ogien"

ACTIVE="${1:-ciemny-las}"
omarchy theme set "$ACTIVE"
omarchy theme bg set "$PWD/$ACTIVE/backgrounds/las_01.jpg"

echo "Installed: ciemny-las, ciemny-ogien (current: $ACTIVE)"