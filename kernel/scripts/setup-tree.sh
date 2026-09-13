#!/usr/bin/env bash
# setup-tree.sh — seed the om-kernel kernel source tree (idempotent)
#
# Creates $PROJ/tmp/linux-omkp. Preferred path: `git clone --shared` from the nv-kepler
# tree (tmp/linux-nouveau) — shared objects, saves ~1GB of downloading.
# The nv-kepler tree is only READ, never modified. Fallback (no source
# or --fresh): a fresh `git clone --filter=blob:none` from kernel.org (stable) + fetch of the tag.
# After setup the tree has a `stable` remote (git fetch --depth 1 stable tag ...) —
# build-om-kernel.sh --kver=... uses it for updates.
#
# Usage:
#   ./scripts/setup-tree.sh             — seed to v7.1.9 (default)
#   ./scripts/setup-tree.sh --kver=7.1.9
#   ./scripts/setup-tree.sh --fresh     — force a fresh clone (without --shared)
set -euo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${SRC:-$PROJ/tmp/linux-omkp}"
SRC_NVKP="${SRC_NVKP:-$HOME/Projects/nv-kepler/tmp/linux-nouveau}"
STABLE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
KVER="7.1.9"
FRESH=false

for arg in "$@"; do
  case "$arg" in
    --kver=*) KVER="${arg#*=}" ;;
    --fresh)  FRESH=true ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "ERROR: unknown argument: $arg" >&2; exit 1 ;;
  esac
done
TARGET="v${KVER#v}"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

echo "=== setup-tree.sh — seeding tree $SRC to tag $TARGET ==="

if [ -e "$SRC/.git" ]; then
  echo ">>> $SRC already exists (git clone) — idempotent mode: making sure the stable remote and the tag are there"
  git -C "$SRC" remote add stable "$STABLE_URL" 2>/dev/null || true
  if [ "$(git -C "$SRC" describe --tags 2>/dev/null || true)" != "$TARGET" ]; then
    echo ">>> checkout -f $TARGET (syncing to the tag)"
    if ! git -C "$SRC" checkout -f "$TARGET" 2>/dev/null; then
      echo ">>> tag $TARGET missing — fetch from stable"
      git -C "$SRC" fetch --depth 1 stable tag "$TARGET" \
        || die "fetch of $TARGET from stable failed — check the network and retry"
      git -C "$SRC" checkout -f "$TARGET"
    fi
  else
    echo ">>> tree already at $TARGET — no changes"
  fi
else
  if [ -d "$SRC" ]; then
    die "$SRC exists, but is not a git clone (no .git) — remove the directory manually or set a different SRC"
  fi
  mkdir -p "$(dirname "$SRC")"

  if [ "$FRESH" = true ]; then
    echo ">>> --fresh: fresh blob:none clone from stable ($STABLE_URL)"
    git clone --filter=blob:none "$STABLE_URL" "$SRC"
    git -C "$SRC" remote add stable "$STABLE_URL" 2>/dev/null || true
  elif [ -d "$SRC_NVKP/.git" ]; then
    echo ">>> git clone --shared from the nv-kepler tree ($SRC_NVKP) — shared objects (saves ~1GB)"
    echo "    (the nv-kepler tree is READ-ONLY — not modified)"
    git clone --shared "$SRC_NVKP" "$SRC"
    git -C "$SRC" remote add stable "$STABLE_URL" 2>/dev/null || true
    # the origin remote in the cloned repo = the source's origin (torvalds); stable is added above.
  else
    echo "NOTE: no --shared source ($SRC_NVKP) — fresh blob:none clone from stable ($STABLE_URL)"
    git clone --filter=blob:none "$STABLE_URL" "$SRC"
    git -C "$SRC" remote add stable "$STABLE_URL" 2>/dev/null || true
  fi

  if ! git -C "$SRC" rev-parse --verify "$TARGET" >/dev/null 2>&1; then
    echo ">>> fetching tag $TARGET (stable; fallback: origin)"
    git -C "$SRC" fetch --depth 1 stable tag "$TARGET" 2>/dev/null \
      || git -C "$SRC" fetch --depth 1 origin tag "$TARGET" \
      || die "fetch of $TARGET failed — check the network (git.kernel.org) and retry"
  fi
  git -C "$SRC" checkout -f "$TARGET"
fi

# the full tree is required for the build — disable any sparse-checkout
git -C "$SRC" sparse-checkout disable 2>/dev/null || true

echo
echo "=== Done ==="
echo "Tree:     $SRC"
echo "Tag:      $(git -C "$SRC" describe --tags 2>/dev/null || git -C "$SRC" rev-parse --short HEAD)"
echo "Remotes:"
git -C "$SRC" remote -v | sed 's/^/    /'
echo
echo "Next step:  verify the patches:  ./scripts/patch-check.sh"
echo "            full build:          sudo ./scripts/build-om-kernel.sh --full-tree --clean"
echo "            initramfs config:    sudo ./scripts/fix-initramfs-omnkp.sh"
echo "            UKI + Limine entry:  sudo ./scripts/build-uki-omnkp.sh"