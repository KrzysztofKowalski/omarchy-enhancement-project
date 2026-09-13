#!/usr/bin/env bash
# fetch-nouveau-sources.sh
# Fetches/updates the COMPLETE set of nouveau driver sources for patching on the GT 750M
# (Kepler GK107 / NVE7). Three driver layers:
#   1. KERNEL  — drivers/gpu/drm/nouveau (DRM/KMS, nvkm, reclocking)
#   2. MESA    — gallium nouveau (OpenGL/Vulkan) + VA-API userspace
#   3. LIBDRM  — userspace DRM/nouveau API
# Plus (optional) the nouveau team tree (bleeding-edge) and envytools (RE).
#
# Project rules:
#   * clone into src/ inside the project dir — NOT /tmp (a restart wipes tmpfs)
#   * git clone --depth 1 (shallow)
#   * idempotent: skip already-cloned repos
#   * no sudo (user project dir)
set -euo pipefail

SRC_DIR="${SRC_DIR:-$HOME/Projects/optymalizacja/src}"
RUNNING_KERNEL="$(uname -r)"          # np. 7.1.8-arch1-3
KVER="${RUNNING_KERNEL%%-arch*}"       # np. 7.1.8 (mainline tag = v7.1.8)
mkdir -p "$SRC_DIR"
cd "$SRC_DIR"

clone_if_absent() {   # strict — for the 3 driver layers
  local url="$1" dir="$2"
  if [ -d "$dir/.git" ]; then
    echo "✓ $dir — already cloned (skipping)"
  else
    echo "⬇ cloning $dir  <-  $url"
    git clone --depth 1 "$url" "$dir"
  fi
}

clone_optional() {     # tolerant — may not exist / may fail
  local url="$1" dir="$2"
  if [ -d "$dir/.git" ]; then
    echo "✓ $dir — already cloned (skipping)"
    return
  fi
  echo "⬇ (optional) $dir  <-  $url"
  git clone --depth 1 "$url" "$dir" || echo "  ⚠ $dir: failed/missing — skipped (optional)"
}

echo "### Nouveau sources — GT 750M (Kepler GK107/NVE7) ###"
echo "Directory: $SRC_DIR"
echo "Running kernel: $RUNNING_KERNEL   (mainline tag: v$KVER)"
echo

# --- 1. KERNEL (mainline torvalds) — the main nouveau driver ---
clone_if_absent https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux
# Match the running kernel's tag (reference + out-of-tree build):
if git -C linux rev-parse -q --verify "refs/tags/v$KVER" >/dev/null 2>&1; then
  echo "→ checkout linux → v$KVER (matching the kernel)"
  git -C linux fetch --depth 1 origin "refs/tags/v$KVER" 2>/dev/null || true
  git -C linux checkout -q "v$KVER" 2>/dev/null || true
else
  echo "ℹ no v$KVER tag in mainline — linux stays on the current HEAD"
fi

# Nouveau team tree (drm/nouveau) — nouveau patches land here before mainline:
clone_optional https://gitlab.freedesktop.org/drm/nouveau.git linux-nouveau

# --- 2. MESA — gallium nouveau (+ VA-API) ---
clone_if_absent https://gitlab.freedesktop.org/mesa/mesa.git mesa

# --- 3. LIBDRM — userspace API ---
clone_if_absent https://gitlab.freedesktop.org/mesa/libdrm.git libdrm

# --- 4. (optional) envytools — GPU register RE tools ---
clone_optional https://gitlab.freedesktop.org/nouveau/envytools.git envytools

echo
echo "### Kepler code map (GK107 / NVE7) — where to look / patch ###"
echo "KERNEL nouveau  ($SRC_DIR/linux):"
echo "  drivers/gpu/drm/nouveau/                  — DRM/KMS, driver core"
echo "  drivers/gpu/drm/nouveau/nvkm/subdev/clk/   — REclocking (Kepler performance KEY)"
echo "  drivers/gpu/drm/nouveau/nvkm/subdev/clk/gk104.c   — GK10x clocks (GK107=NVE7)"
echo "  drivers/gpu/drm/nouveau/nvkm/subdev/fb/    — memory (GDDR5 reclock)"
echo "  drivers/gpu/drm/nouveau/nvkm/subdev/volt/  — voltage"
echo "  drivers/gpu/drm/nouveau/nvkm/engine/device/gk104.c — GK107 identifier"
echo "MESA nouveau   ($SRC_DIR/mesa):"
echo "  src/gallium/drivers/nouveau/              — OpenGL (gallium)"
echo "  src/gallium/frontends/va/                  — VA-API (no vaExportSurfaceHandle = Chromium problem)"
echo "LIBDRM         ($SRC_DIR/libdrm):"
echo "  nouveau/                                   — userspace API"
echo
echo "### Build (for later — this script does not run it) ###"
echo "  linux-headers: $(pacman -Q linux-headers 2>/dev/null | tr '\n' ' ')"
echo "  Out-of-tree nouveau.ko against KDIR=/usr/src/linux-headers-$RUNNING_KERNEL"
echo "  Full patched kernel: Arch PKGBUILD (asp/abs) — a separate step."
echo
echo "⚠ Realism: Kepler reclocking is often firmware-gated (NVIDIA signed microcode)."
echo "  High-value patch targets: stability, VA-API dma-buf export (Chromium),"
echo "  minor KMS/cursor fixes — full core-reclock GK107 may be blocked."
echo
echo "Done. Trees in: $SRC_DIR"