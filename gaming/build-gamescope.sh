#!/usr/bin/env bash
# Build gamescope (fork KrzysztofKowalski) — no system install, binary in gamescope/build/src/gamescope.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/gamescope"

git submodule update --init --recursive

if [ ! -f build/build.ninja ]; then
  meson setup build --prefix=/usr -Dwerror=false
fi

ninja -C build

echo
echo "OK: $ROOT/gamescope/build/src/gamescope"