#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# workspaces/ — 20 workspaces in the Omarchy bar, in two banks of 10
#   ./install.sh              → append the bindings + install the patched widget
#   ./install.sh --check      → report the current state, change nothing
#   ./install.sh --uninstall  → restore the stock 10-workspace layout
#   ./test.sh                 → round-trip test on a throwaway fake $HOME
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BINDINGS="$HOME/.config/hypr/bindings.lua"
PLUGIN_DIR="$HOME/.config/omarchy/plugins/$(id -un).workspaces"
SHELL_JSON="$HOME/.config/omarchy/shell.json"
STOCK_QML="/usr/share/omarchy/shell/plugins/bar/widgets/Workspaces.qml"

MARKER='-- om-enh:workspaces (two banks of 10)'
# A line from inside the block body. The block is detected by THIS, not by the
# marker: a copy appended by hand has no marker, and keying off the marker alone
# would append a second block on top of it.
BODY='Two banks of workspaces (20 instead of 10)'

require_omarchy() {
  if [ ! -d "$HOME/.config/omarchy" ] || [ ! -d "$HOME/.config/hypr" ]; then
    echo "✗ ~/.config/omarchy or ~/.config/hypr not found — this does not look like Omarchy." >&2
    exit 1
  fi
  if [ ! -f "$BINDINGS" ]; then
    echo "✗ $BINDINGS not found." >&2
    exit 1
  fi
}

reload_hyprland() {
  hyprctl reload >/dev/null 2>&1 || true
  hyprctl configerrors 2>/dev/null || true
}

bindings_installed() { grep -qF -- "$BODY" "$BINDINGS"; }
widget_patched()     { [ -f "$PLUGIN_DIR/Workspaces.qml" ] && grep -qF -- 'for (var n = 1; n <= 20; n++)' "$PLUGIN_DIR/Workspaces.qml"; }

install_bindings() {
  if bindings_installed; then
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
  if ! bindings_installed; then
    echo "  ~ bindings block not found — nothing to remove"
    return
  fi
  local backup="$BINDINGS.bak.$(date +%s)"
  cp "$BINDINGS" "$backup"
  # Locate the block by its body text, walk back to the banner comment that
  # opens it and forward to the bare `end` that closes its for-loop, and also
  # swallow the blank line separating it from the file above — so the result is
  # byte-for-byte the pre-install file, whether or not the marker is present.
  awk -v marker="$MARKER" -v body="$BODY" '
    { line[NR] = $0 }
    END {
      found = 0
      for (i = 1; i <= NR; i++) if (index(line[i], body) > 0) { found = i; break }
      if (found == 0) { for (i = 1; i <= NR; i++) print line[i]; exit }

      start = found
      for (i = found; i >= 1; i--) if (line[i] ~ /^-- =+$/) { start = i; break }

      finish = NR
      for (i = found; i <= NR; i++) if (line[i] == "end") { finish = i; break }

      from = start
      if (from > 1 && line[from - 1] == marker) from--
      if (from > 1 && line[from - 1] == "")     from--

      for (i = 1; i < from; i++) print line[i]
      for (i = finish + 1; i <= NR; i++) print line[i]
    }
  ' "$backup" > "$BINDINGS"
  echo "  ✓ bindings block removed"
  echo "    backup: $backup"
}

install_widget() {
  if [ ! -d "$PLUGIN_DIR" ]; then
    command -v omarchy >/dev/null || { echo "✗ omarchy not in PATH — cannot clone the widget." >&2; exit 1; }
    omarchy plugin clone omarchy.workspaces
  fi
  cp "$HERE/omarchy-plugin/Workspaces.qml" "$PLUGIN_DIR/Workspaces.qml"
  echo "  ✓ patched widget → $PLUGIN_DIR/Workspaces.qml"
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
}

uninstall_widget() {
  if [ ! -d "$PLUGIN_DIR" ]; then
    echo "  ~ widget clone not found — nothing to restore"
    return
  fi
  if [ ! -f "$STOCK_QML" ]; then
    echo "  ! stock widget not found at $STOCK_QML — leaving the clone as is" >&2
    return
  fi
  cp "$STOCK_QML" "$PLUGIN_DIR/Workspaces.qml"
  echo "  ✓ widget restored to stock (10 workspaces)"
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
}

do_check() {
  echo "state of the 20-workspace change:"
  if bindings_installed; then
    echo "  ✓ bindings: INSTALLED"
  else
    echo "  - bindings: not installed"
  fi
  if widget_patched; then
    echo "  ✓ widget:   PATCHED (20 slots)"
  else
    echo "  - widget:   stock (10 slots)"
  fi
  if [ -f "$SHELL_JSON" ] && grep -qF -- '"omarchy.workspaces"' "$SHELL_JSON"; then
    echo "  - bar:      still points at omarchy.workspaces (the clone is unused)"
  elif [ -f "$SHELL_JSON" ]; then
    echo "  ✓ bar:      cloned widget is wired into shell.json"
  fi
  if command -v hyprctl >/dev/null; then
    local binds sw mv
    binds="$(hyprctl binds -j 2>/dev/null || true)"
    # `|| true` is load-bearing: under `set -o pipefail` a grep that matches
    # nothing would fail the assignment and abort the script.
    sw="$(printf '%s' "$binds" | grep -oE '"description": "Switch to workspace (1[1-9]|20)"' | wc -l || true)"
    mv="$(printf '%s' "$binds" | grep -oE '"description": "Move window to workspace (1[1-9]|20)"' | wc -l || true)"
    echo "  - live binds: bank-2 switch=$sw/10  bank-2 move=$mv/10  (expect 10 and 10)"
  fi
}

case "${1:-install}" in
  install)
    require_omarchy
    install_bindings
    install_widget
    reload_hyprland
    echo "  ✓ done — try SUPER+ALT+1 (workspace 11) and SUPER+SHIFT+1 (window → 1)"
    ;;
  --check|check)
    do_check
    ;;
  --uninstall|uninstall)
    require_omarchy
    uninstall_bindings
    uninstall_widget
    reload_hyprland
    echo "  ✓ done — back to the stock 10-workspace layout"
    ;;
  *)
    echo "Usage: $0 [--check|--uninstall]" >&2
    exit 1
    ;;
esac
