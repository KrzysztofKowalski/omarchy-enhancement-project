#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# omarchy-enhancement-project — per-module installer
#   ./install.sh              → list modules
#   ./install.sh <module>...  → install selected
# Safe modules (theme, audio, touchpad, workspaces, clock) install themselves.
# kernel/gpu/wifi/aec/music/gaming/llm → run the scripts from their READMEs
# (boot/initramfs/reboot).
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

usage() {
  echo "Usage: $0 <module> [module...]"
  echo ""
  echo "Installable here:"
  echo "  theme     — Omarchy 4 themes (las, ogien, ciemny-las, ciemny-ogien) → ~/.config/omarchy/themes/"
  echo "  audio     — BT speaker fix (dummy AVRCP player) + audio preview fix"
  echo "  touchpad  — system-sleep hook: rebind bcm5974 after resume"
  echo "  workspaces — 20 workspaces (two banks of 10) + patched bar widget"
  echo "  clock     — seconds in the bar clock + system-locale calendar day names"
  echo ""
  echo "Manual modules (see README in each directory): kernel, gpu, wifi, aec, music, gaming, llm, mikrofon, system"
}

require_omarchy() {
  if [ ! -d "$HOME/.config/omarchy" ]; then
    echo "✗ ~/.config/omarchy not found — this does not look like Omarchy." >&2
    exit 1
  fi
}

install_theme() {
  require_omarchy
  mkdir -p "$HOME/.config/omarchy/themes"
  for t in las ogien ciemny-las ciemny-ogien; do
    local dest="$HOME/.config/omarchy/themes/$t"
    if [ -d "$dest" ]; then
      echo "  ~ $dest already exists — skipping (remove manually to overwrite)"
    else
      cp -r "$HERE/theme/$t" "$dest"
      echo "  ✓ theme $t → $dest"
    fi
  done
  echo "    activate: omarchy theme set las"
}

install_audio() {
  require_omarchy
  echo "  → running audio/fix-bt-zielony-krasnal.sh"
  bash "$HERE/audio/fix-bt-zielony-krasnal.sh"
  echo "  → running audio/fix-audio-preview.sh (may ask for sudo)"
  bash "$HERE/audio/fix-audio-preview.sh"
}

install_touchpad() {
  require_omarchy
  echo "  → running touchpad/scripts/install-sleep-hook.sh (needs sudo)"
  bash "$HERE/touchpad/scripts/install-sleep-hook.sh"
}

install_workspaces() {
  require_omarchy
  echo "  → running workspaces/install.sh"
  bash "$HERE/workspaces/install.sh"
}

install_clock() {
  require_omarchy
  echo "  → running clock/install.sh"
  bash "$HERE/clock/install.sh"
}

[ $# -eq 0 ] && { usage; exit 0; }

for mod in "$@"; do
  case "$mod" in
    theme)    install_theme ;;
    audio)    install_audio ;;
    touchpad) install_touchpad ;;
    workspaces) install_workspaces ;;
    clock)    install_clock ;;
    kernel|gpu|wifi|aec|music|gaming|llm|mikrofon|system)
      echo "  ! module '$mod' — manual install, see $mod/README.md" ;;
    *) usage; exit 1 ;;
  esac
done