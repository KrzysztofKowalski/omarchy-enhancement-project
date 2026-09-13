#!/usr/bin/env bash
# install-kepler-cuda.sh
# CUDA installation in C++ on a MacBook Late 2013 (NVIDIA GT 750M, Kepler sm_30) / Arch + Omarchy.
#
# Two variants (can be combined):
#   driver  – the legacy NVIDIA 470xx driver (replaces nouveau) + utils. Required for CUDA
#             to actually run kernels on the GPU.
#   cuda    – "pure CUDA": only the toolkit 10.2 (nvcc + libraries), without swapping the driver.
#             Lets you COMPILE CUDA code, but without the 'driver' variant the binaries won't
#             run on this card (no nvidia kernel module).
#   all     – driver + cuda (full, working set).
#
# Usage:
#   ./install-kepler-cuda.sh driver     # only the 470xx driver
#   ./install-kepler-cuda.sh cuda       # only the CUDA 10.2 toolkit
#   ./install-kepler-cuda.sh all        # both
#   ./install-kepler-cuda.sh --dry-run all   # show the commands without running them
#   ./install-kepler-cuda.sh status     # diagnostics of the current state
#
# After the 'driver' variant a REBOOT IS REQUIRED. The script rebuilds the initramfs and blacklists nouveau.

set -euo pipefail

# ---- configuration -----------------------------------------------------------
AUR_HELPER="yay"
CUDA_PKG="cuda-10.2"                 # AUR, toolkit 10.2 (the last one supporting sm_30)
NVIDIA_DKMS="nvidia-470xx-dkms"      # legacy 470xx for Kepler
NVIDIA_UTILS="nvidia-470xx-utils"
CUDA_HOME_TARGET="/opt/cuda"         # default install target of cuda-10.2 from AUR
DRY_RUN=0

# ---- colors / logs ----------------------------------------------------------
C_RUN() { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
C_WARN(){ printf '\033[1;33m!!\033[0m %s\n' "$*"; }
C_ERR() { printf '\033[1;31mXX\033[0m %s\n' "$*" >&2; }
C_INF() { printf '\033[1;36m..\033[0m %s\n' "$*"; }

run() {           # run the command (or only print it in dry-run)
  if (( DRY_RUN )); then
    printf '\033[2m$ %s\033[0m\n' "$*"
  else
    C_RUN "$*"
    "$@"
  fi
}

need_root() {
  if (( DRY_RUN )); then return 0; fi
  if [[ $EUID -ne 0 ]]; then
    C_ERR "this operation needs root — run the script via:  sudo $0 $*"
    exit 1
  fi
}

# ---- detection ---------------------------------------------------------------
detect() {
  KERNEL_PKG=$(pacman -Qq 2>/dev/null | grep -E '^(linux|linux-lts|linux-zen|linux-hardened)$' | head -1 || true)
  [[ -z "$KERNEL_PKG" ]] && KERNEL_PKG="linux"
  HAVE_HELPER=$(command -v "$AUR_HELPER" 2>/dev/null || command -v yay 2>/dev/null || command -v paru 2>/dev/null || true)
  HAVE_NVIDIA=$(pacman -Qq 2>/dev/null | grep -E '^nvidia(-470xx)?(-dkms|-lts|-utils)?$' | head -1 || true)
  HAVE_CUDA=$(pacman -Qq 2>/dev/null | grep -E '^cuda(-10.2)?$' | head -1 || true)
  NOUVEAU_LOADED=$(lsmod 2>/dev/null | grep -c '^nouveau' || true)
  GPU=$(lspci -nn 2>/dev/null | grep -i 'NVIDIA.*GK107' || lspci 2>/dev/null | grep -i nvidia || true)
}

status() {
  detect
  echo "== Machine state =="
  echo "  kernel pkg     : $KERNEL_PKG ($(uname -r))"
  echo "  AUR helper     : ${HAVE_HELPER:-(MISSING!)}"
  echo "  NVIDIA GPU     : ${GPU:-(not detected)}"
  echo "  driver pkg     : ${HAVE_NVIDIA:-(none)}"
  echo "  cuda pkg       : ${HAVE_CUDA:-(none)}"
  echo "  nouveau loaded : $NOUVEAU_LOADED (refs)"
  command -v nvcc >/dev/null 2>&1 && echo "  nvcc           : $(nvcc --version | tail -1)" || echo "  nvcc           : (none)"
  command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || echo "  nvidia-smi     : (none / module not loaded)"
}

# ---- variant: 470xx driver -----------------------------------------------
install_driver() {
  detect
  C_INF "DRIVER variant: legacy NVIDIA 470xx (Kepler sm_30)"

  if [[ -z "$HAVE_HELPER" ]]; then
    C_ERR "no AUR helper found ($AUR_HELPER/yay/paru). Install yay: pacman -S --needed base-devel git && git clone ... / yay."
    exit 1
  fi

  C_WARN "This replaces nouveau with the 470xx driver and forces a REBOOT."
  C_WARN "Kernel: $(uname -r). 470xx is legacy; if DKMS does not build on this kernel,"
  C_WARN "  install linux-lts and boot from it:  sudo pacman -S linux-lts linux-lts-headers"
  C_WARN "  (470xx has better compatibility with older kernels)."
  echo; read -r -p "Continue with the driver installation? [y/N] " ans
  [[ "$ans" =~ ^[Yy]$ ]] || { C_INF "skipped."; return 0; }

  # 1) blacklist nouveau (modprobe.d)
  C_INF "blacklisting nouveau in /etc/modprobe.d/"
  if (( DRY_RUN )); then
    printf '\033[2m$ install -Dm644 /dev/stdin /etc/modprobe.d/nouveau-blacklist.conf\033[0m\n'
  else
    need_root driver
    install -Dm644 /dev/stdin /etc/modprobe.d/nouveau-blacklist.conf <<'EOF'
# Kepler CUDA: we use the legacy nvidia-470xx, disable nouveau
blacklist nouveau
options nouveau modeset=0
EOF
  fi

  # 2) DKMS package matching the kernel
  local dkms_pkg="$NVIDIA_DKMS"
  if [[ "$KERNEL_PKG" == "linux-lts" ]]; then
    dkms_pkg="nvidia-470xx-lts"   # if it exists; DKMS is universal
  fi
  # kernel headers for DKMS
  local headers_pkg="${KERNEL_PKG}-headers"
  [[ "$KERNEL_PKG" == "linux" ]] && headers_pkg="linux-headers"

  # 3) installation via AUR (yay cannot run as root -> run as a normal user)
  if (( DRY_RUN )); then
    run "$HAVE_HELPER" -S --needed "$headers_pkg" "$NVIDIA_DKMS" "$NVIDIA_UTILS"
  else
    if [[ $EUID -eq 0 ]]; then
      C_ERR "yay does not work as root. Run the AUR installation as your user:"
      C_ERR "  $HAVE_HELPER -S --needed $headers_pkg $NVIDIA_DKMS $NVIDIA_UTILS"
      C_ERR "then re-run this script (the rest of the steps as root)."
      exit 1
    fi
    run "$HAVE_HELPER" -S --needed "$headers_pkg" "$NVIDIA_DKMS" "$NVIDIA_UTILS"
  fi

  # 4) initramfs rebuild
  C_INF "rebuilding the initramfs (mkinitcpio -P)"
  if (( ! DRY_RUN )); then need_root driver; fi
  run mkinitcpio -P

  # 5) nvidia module settings at startup
  if (( ! DRY_RUN )); then
    need_root driver
    install -Dm644 /dev/stdin /etc/modules-load.d/nvidia.conf <<'EOF'
nvidia
nvidia_modeset
nvidia_uvm
nvidia_drm
EOF
  fi

  echo
  C_WARN "Driver installed. A REBOOT IS REQUIRED for nouveau to be released and nvidia loaded."
  C_INF "After the reboot check:  nvidia-smi   (it should show GT 750M, driver 470.x)"
  C_INF "If DKMS failed on kernel $(uname -r): install linux-lts + headers and boot from LTS."
}

# ---- variant: pure CUDA (toolkit 10.2) ------------------------------------
install_cuda() {
  detect
  C_INF "CUDA variant: toolkit 10.2 (nvcc + libs) — without swapping the driver"

  if [[ -z "$HAVE_HELPER" ]]; then
    C_ERR "no AUR helper ($AUR_HELPER/yay/paru)."
    exit 1
  fi

  if (( ! DRY_RUN )) && [[ $EUID -eq 0 ]]; then
    C_ERR "yay does not work as root. Run as your user:  $0 cuda"
    exit 1
  fi

  C_WARN "cuda-10.2 from AUR downloads ~2 GB and takes a lot of space; installs to ${CUDA_HOME_TARGET}."
  C_WARN "You will be able to compile right away. RUNNING on the GPU also needs the 'driver' variant."
  echo; read -r -p "Continue with the CUDA 10.2 toolkit installation? [y/N] " ans
  [[ "$ans" =~ ^[Yy]$ ]] || { C_INF "skipped."; return 0; }

  run "$HAVE_HELPER" -S --needed "$CUDA_PKG"

  # links / paths
  C_INF "Configuring the environment (CUDA_HOME + PATH)"
  if (( DRY_RUN )); then
    printf '\033[2m$ install -Dm644 /dev/stdin /etc/profile.d/cuda-10.2.sh\033[0m\n'
  else
    need_root cuda
    install -Dm644 /dev/stdin /etc/profile.d/cuda-10.2.sh <<EOF
# CUDA 10.2 for Kepler (sm_30)
export CUDA_HOME=${CUDA_HOME_TARGET}
export PATH=\$CUDA_HOME/bin:\$PATH
export LD_LIBRARY_PATH=\$CUDA_HOME/lib64:\$LD_LIBRARY_PATH
EOF
    chmod 644 /etc/profile.d/cuda-10.2.sh
  fi

  echo
  C_INF "After reloading the shell (logout/login or: source /etc/profile.d/cuda-10.2.sh):"
  C_INF "  nvcc --version   # -> release 10.2"
  C_INF "Compile with:  nvcc -gencode arch=compute_30,code=sm_30 file.cu -o file"
  if [[ -z "$HAVE_NVIDIA" ]]; then
    C_WARN "The nvidia driver is NOT installed — compiled code will not run on the GPU."
    C_WARN "Run:  sudo $0 driver   to add the legacy 470xx."
  fi
}

# ---- main -------------------------------------------------------------------
main() {
  local mode="${1:-}"
  [[ -z "$mode" ]] && { sed -n '2,20p' "$0"; exit 1; }

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dry-run) DRY_RUN=1; shift ;;
      *) break ;;
    esac
  done
  mode="${1:-$mode}"

  case "$mode" in
    driver) install_driver ;;
    cuda)   install_cuda ;;
    all)    install_driver; echo; install_cuda ;;
    status) status ;;
    -h|--help|help) sed -n '2,20p' "$0" ;;
    *) C_ERR "unknown mode: $mode"; sed -n '2,20p' "$0"; exit 1 ;;
  esac
}

main "$@"