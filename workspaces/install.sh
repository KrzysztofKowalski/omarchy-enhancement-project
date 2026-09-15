#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# workspaces/ — 20 workspaces in the Omarchy bar, in two banks of 10
#   ./install.sh              → append the bindings + install the patched widget
#   ./install.sh --uninstall  → remove the bindings block (widget left in place)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BINDINGS="$HOME/.config/hypr/bindings.lua"
MARKER='-- om-enh:workspaces (two banks of 10)'
PLUGIN_DIR="$HOME/.config/omarchy/plugins/$(id -un).workspaces"

require_omarchy() {
  if [ ! -d "$HOME/.config/omarchy" ] || [ ! -d "$HOME/.config/hypr" ]; then
    echo "✗ ~/.config/omarchy or ~/.config/hypr not found — this does not look like Omarchy." >&2
    exit 1
  fi
}

reload_hyprland() {
  hyprctl reload >/dev/null 2>&1 || true
  hyprctl configerrors 2>/dev/null || true
}

install_bindings() {
  if grep -qF -- "$MARKER" "$BINDINGS"; then
    echo "  ~ bindings already present — skipping"
    return
  fi
  local backup="$BINDINGS.bak.$(date +%s)"
  cp "$BINDINGS" "$backup"
  { printf '\n%s\n' "$MARKER"; cat "$HERE/hypr/bindings-workspaces.lua"; } >> "$BINDINGS"
  echo "  ✓ bindings appended → $BINDINGS"
  echo "    backup: $backup"
}

uninstall_bindings() {
  if ! grep -qF -- "$MARKER" "$BINDINGS"; then
    echo "  ~ bindings block not found — nothing to remove"
    return
  fi
  local backup="$BINDINGS.bak.$(date +%s)"
  cp "$BINDINGS" "$backup"
  # Drop from the marker line through the bare `end` that closes its for-loop.
  awk -v marker="$MARKER" '
    $0 == marker { skip = 1; next }
    skip && $0 == "end" { skip = 0; next }
    !skip { print }
  ' "$backup" > "$BINDINGS"
  echo "  ✓ bindings block removed"
  echo "    backup: $backup"
}

install_widget() {
  if [ ! -d "$PLUGIN_DIR" ]; then
    omarchy plugin clone omarchy.workspaces
  fi
  cp "$HERE/omarchy-plugin/Workspaces.qml" "$PLUGIN_DIR/Workspaces.qml"
  echo "  ✓ patched widget → $PLUGIN_DIR/Workspaces.qml"
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
}

case "${1:-install}" in
  install)
    require_omarchy
    install_bindings
    install_widget
    reload_hyprland
    echo "  ✓ done — try SUPER+ALT+1 (workspace 11) and SUPER+SHIFT+1 (window → 1)"
    ;;
  --uninstall|uninstall)
    require_omarchy
    uninstall_bindings
    reload_hyprland
    echo "  ✓ done — see README.md to also revert the bar widget"
    ;;
  *)
    echo "Usage: $0 [--uninstall]" >&2
    exit 1
    ;;
esac
