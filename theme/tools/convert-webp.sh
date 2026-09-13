#!/usr/bin/env bash
# Convert theme images to WebP (lossy q88) without changing the resolution.
# Usage: tools/convert-webp.sh [file|directory]...
# With no arguments, converts all themes in this repo.
set -euo pipefail

args=("$@"); [ ${#args[@]} -gt 0 ] || args=(./las ./ogien ./ciemny-las ./ciemny-ogien)

convert_one() {
    local src="$1" dst
    case "${src,,}" in
    *.jpg|*.jpeg|*.png) ;;
    *) return ;;
    esac
    dst="${src%.*}.webp"
    [ "$dst" != "$src" ] || return
    magick "$src" -quality 88 "$dst"
    echo "OK: $src -> $dst"
}

for src in "${args[@]}"; do
    if [ -d "$src" ]; then
        find "$src" -type f \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \) -print0 |
            while IFS= read -r -d '' f; do convert_one "$f"; done
    else
        convert_one "$src"
    fi
done