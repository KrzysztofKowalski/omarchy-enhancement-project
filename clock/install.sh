#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# clock/ — seconds in the Omarchy bar clock (+ system-locale day names)
#   ./install.sh              → clone & patch the widget, set the format, restart the shell
#   ./install.sh --check      → report the current state, change nothing
#   ./install.sh --uninstall  → restore the stock widget and format
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
USER_ID="$(id -un)"
PLUGIN_ID="$USER_ID.clock"
PLUGIN_DIR="$HOME/.config/omarchy/plugins/$PLUGIN_ID"
SHELL_JSON="$HOME/.config/omarchy/shell.json"
STOCK_DIR="/usr/share/omarchy/shell/plugins/panels/clock"

# The stock bar label format, and the one this module installs.
STOCK_FORMAT='dddd HH:mm'
WANT_FORMAT='dddd HH:mm:ss'

require_omarchy() {
  if [ ! -d "$HOME/.config/omarchy" ]; then
    echo "✗ ~/.config/omarchy not found — this does not look like Omarchy." >&2
    exit 1
  fi
  if [ ! -f "$SHELL_JSON" ]; then
    echo "✗ $SHELL_JSON not found." >&2
    exit 1
  fi
  # jq is a hard dependency of the omarchy package itself.
  command -v jq >/dev/null || { echo "✗ jq not in PATH (ships with the omarchy package)." >&2; exit 1; }
}

# Every entry in every bar section, flattened — the clock may sit in any of them.
entries() {
  jq -c --arg id "$PLUGIN_ID" '[.bar.layout[][] | select(.id == $id)]' "$SHELL_JSON"
}

entry_present() { [ "$(entries)" != "[]" ]; }

# Does the clock entry's format already carry a seconds token?
format_has_seconds() {
  jq -e --arg id "$PLUGIN_ID" \
    '[.bar.layout[][] | select(.id == $id) | .format // ""] | any(test(":ss"))' \
    "$SHELL_JSON" >/dev/null 2>&1
}

patched_precision() {
  [ -f "$PLUGIN_DIR/BarWidget.qml" ] &&
    grep -q 'precision: SystemClock.Seconds' "$PLUGIN_DIR/BarWidget.qml"
}

patched_locale() {
  [ -f "$PLUGIN_DIR/Panel.qml" ] &&
    ! grep -q 'Qt.locale("en_US")' "$PLUGIN_DIR/Panel.qml"
}

backup() {
  local f="$1"
  [ -f "$f" ] || return 0
  cp "$f" "$f.bak.$(date +%s)"
}

# Set (or add) the clock entry's format, leaving every other key alone.
set_format() {
  local fmt="$1"
  backup "$SHELL_JSON"
  if entry_present; then
    jq --arg id "$PLUGIN_ID" --arg fmt "$fmt" \
      '(.bar.layout[][] | select(.id == $id) | .format) = $fmt' \
      "$SHELL_JSON" > "$SHELL_JSON.tmp" && mv "$SHELL_JSON.tmp" "$SHELL_JSON"
    echo "  ✓ shell.json: format → $fmt"
  else
    # The clone normally creates the entry; if it is absent (widget removed from
    # the bar by hand) put it back in the center section, next to where stock has it.
    jq --arg id "$PLUGIN_ID" --arg fmt "$fmt" \
      '(.bar.layout.center) += [{"id": $id, "format": $fmt}]' \
      "$SHELL_JSON" > "$SHELL_JSON.tmp" && mv "$SHELL_JSON.tmp" "$SHELL_JSON"
    echo "  ✓ shell.json: entry $PLUGIN_ID added to the center section (format $fmt)"
  fi
}

restart_shell() {
  echo "  → omarchy restart shell"
  omarchy restart shell >/dev/null 2>&1 || true
}

install_widget() {
  if [ ! -d "$PLUGIN_DIR" ]; then
    command -v omarchy >/dev/null || { echo "✗ omarchy not in PATH — cannot clone the widget." >&2; exit 1; }
    echo "  → omarchy plugin clone omarchy.clock"
    omarchy plugin clone omarchy.clock
  fi
  local f
  for f in BarWidget.qml Panel.qml; do
    backup "$PLUGIN_DIR/$f"
    cp "$HERE/omarchy-plugin/$f" "$PLUGIN_DIR/$f"
    echo "  ✓ patched $f → $PLUGIN_DIR"
  done
}

uninstall_widget() {
  if [ ! -d "$PLUGIN_DIR" ]; then
    echo "  ~ widget clone not found — nothing to restore"
    return
  fi
  local f
  for f in BarWidget.qml Panel.qml; do
    if [ ! -f "$STOCK_DIR/$f" ]; then
      echo "  ! stock $f not found at $STOCK_DIR — leaving the clone as is" >&2
      continue
    fi
    cp "$STOCK_DIR/$f" "$PLUGIN_DIR/$f"
    echo "  ✓ $f restored to stock"
  done
}

do_check() {
  echo "state of the seconds-clock change:"
  if [ -d "$PLUGIN_DIR" ]; then
    echo "  ✓ plugin:    $PLUGIN_DIR"
  else
    echo "  - plugin:    no clone at $PLUGIN_DIR"
  fi
  if patched_precision; then
    echo "  ✓ precision: Seconds"
  else
    echo "  - precision: not Seconds — the seconds will sit still at :00"
  fi
  if patched_locale; then
    echo "  ✓ day names: system locale"
  else
    echo "  - day names: stock (hardcoded en_US)"
  fi
  if entry_present; then
    if format_has_seconds; then
      echo "  ✓ shell.json: clock entry carries ':ss'"
    else
      echo "  - shell.json: clock entry present, but the format has no ':ss'"
    fi
  else
    echo "  - shell.json: no clock entry — the bar is not showing the clone"
  fi
}

case "${1:-install}" in
  install)
    require_omarchy
    install_widget
    if format_has_seconds; then
      echo "  ~ shell.json: format already has seconds — skipping"
    else
      set_format "$WANT_FORMAT"
    fi
    # Restart, not just reload: hot reload picks up the format change, but the
    # clock's timer does not reliably re-attach to a new precision, and the
    # seconds stay at :00 until it does.
    restart_shell
    echo "  ✓ done — the bar clock should now tick every second"
    ;;

  --check|check)
    require_omarchy
    do_check
    ;;

  --uninstall|uninstall)
    require_omarchy
    uninstall_widget
    set_format "$STOCK_FORMAT"
    restart_shell
    echo "  ✓ done — back to the stock clock (minutes, English day names)"
    echo "    the now-unused clone stays in $PLUGIN_DIR"
    ;;

  *)
    echo "Usage: $0 [--check|--uninstall]" >&2
    exit 1
    ;;
esac
