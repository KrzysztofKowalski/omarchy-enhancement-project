#!/usr/bin/env bash
# build-mesa.sh — installs dependencies + builds the patched Mesa (nouveau/nvc0, Kepler GK107)
# Patch: patches/0002-mesa-nvc0-sched-data.patch (NAK sm30 latencies → SchedDataCalculator).
# Does NOT install into the system — the build goes to tmp/mesa/build-nouveau, you run via env.
#
# Usage:  ./build-mesa.sh
# Rebuild: ./build-mesa.sh   (incremental, skips setup if the build exists)
set -euo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
MESA="$PROJ/tmp/mesa"
PATCH="$PROJ/patches/0002-mesa-nvc0-sched-data.patch"
BUILD="$MESA/build-nouveau"

# ----------------------------------------------------------------------------
# 1. Build dependencies (only the missing ones via --needed)
# ----------------------------------------------------------------------------
echo ">>> [1/5] Installing dependencies (sudo pacman --needed)..."
sudo pacman -S --needed --noconfirm \
    meson python-mako python-ply \
    libdrm wayland-protocols pkgconf \
    flex bison

# ----------------------------------------------------------------------------
# 2. Mesa source tree
# ----------------------------------------------------------------------------
echo ">>> [2/5] Checking tmp/mesa..."
if [ ! -d "$MESA/.git" ]; then
    echo "ERROR: $MESA does not exist. First:"
    echo "  git clone https://gitlab.freedesktop.org/mesa/mesa.git $MESA"
    exit 1
fi
cd "$MESA"

# ----------------------------------------------------------------------------
# 3. Patch (idempotent — skips if already applied)
# ----------------------------------------------------------------------------
echo ">>> [3/5] Patch NAK→SchedDataCalculator..."
if git diff --quiet -- src/gallium/drivers/nouveau/codegen/nv50_ir_emit_nvc0.cpp \
                    src/gallium/drivers/nouveau/codegen/nv50_ir_target_nvc0.cpp; then
    # tree clean on the target files → apply the patch
    if git apply --check "$PATCH" 2>/dev/null; then
        git apply "$PATCH"
        echo "    applied."
    else
        echo "ERROR: git apply --check failed — the patch does not match this tree."
        echo "       Refresh the patch (rebase) or reset the tree: (cd $MESA && git checkout -- .)"
        exit 1
    fi
else
    echo "    already applied (target files modified in the tree)."
fi

# ----------------------------------------------------------------------------
# 4. meson configuration — ONLY nouveau, minimal, no vulkan/opencl/rust/llvm
#    (the patch touches the classic gallium nvc0 C++ driver, not NAK/rusticl)
# ----------------------------------------------------------------------------
# A valid build dir has meson-private/coredata.dat. If not (e.g. setup failed
# during a previous run) — wipe and do a fresh setup.
if [ -d "$BUILD" ] && [ -f "$BUILD/meson-private/coredata.dat" ]; then
    echo ">>> [4/5] build dir OK — skipping setup (incrementally)."
else
    echo ">>> [4/5] meson setup (nouveau-only)..."
    rm -rf "$BUILD"
    # Options verified under mesa 26.x (meson.options):
    #   gallium-opencl / gallium-vdpau DO NOT EXIST (clover removed) — do not use.
    #   gallium-rusticl = boolean (default false), gallium-va = feature, llvm = feature.
    meson setup "$BUILD" "$MESA" \
        -Dgallium-drivers=nouveau \
        -Dvulkan-drivers= \
        -Dgallium-rusticl=false \
        -Dgallium-va=disabled \
        -Dgles1=disabled \
        -Dglx=dri \
        -Dplatforms=wayland,x11 \
        -Dllvm=disabled
    # NOTE: if meson requires llvm, install: sudo pacman -S llvm
    # and remove -Dllvm=disabled above, then: rm -rf "$BUILD" && ./build-mesa.sh
fi

# ----------------------------------------------------------------------------
# 5. Build
# ----------------------------------------------------------------------------
echo ">>> [5/5] meson compile (this will take a while)..."
meson compile -C "$BUILD"

# ----------------------------------------------------------------------------
# 6. How to use it (without overwriting the system Mesa)
# ----------------------------------------------------------------------------
DRI="$(find "$BUILD/src/gallium" -name 'nouveau_dri.so' 2>/dev/null | head -1)"
[ -z "$DRI" ] && DRI="$(find "$BUILD" -name 'libgallium_dri.so' 2>/dev/null | head -1)"
DRIDIR="$(dirname "$DRI")"

cat <<EOF

=== BUILD OK ===
Patched Mesa (nouveau/nvc0) built in:
  $BUILD

Run apps with it (without overwriting the system Mesa):
  export LIBGL_DRIVERS_PATH="$DRIDIR"

Test (the renderer should show NVE7 / nouveau, not Intel):
  DRI_PRIME=0 glxinfo | grep -i renderer
  DRI_PRIME=1 glxinfo | grep -i renderer

App:
  LIBGL_DRIVERS_PATH="$DRIDIR" DRI_PRIME=1 firefox

Shader-bound comparison before/after the patch (same scene):
  # without the patch (system Mesa):
  DRI_PRIME=1 glxgears -geometry 800x600   # note the FPS
  # with the patch:
  LIBGL_DRIVERS_PATH="$DRIDIR" DRI_PRIME=1 glxgears -geometry 800x600

To install into the system (NOT recommended without a backup — user gate):
  sudo meson install -C "$BUILD"
EOF