#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# test.sh — round-trip test for install.sh, on a throwaway fake $HOME.
#
#   ./test.sh
#
# Builds tmp/install-test/{home,bin} with stub `omarchy`, `omarchy-shell` and
# `hyprctl` binaries, then checks five claims:
#   1. install is idempotent       (line count unchanged on the second run)
#   2. --check does not crash      (a grep that matches nothing must not abort)
#   3. --uninstall restores a block written by install.sh byte-for-byte
#   4. --uninstall also removes a marker-less block — one appended by hand, as
#      on a machine that predates this script
#   5. install then uninstall leaves the bindings file exactly as it started
# Your real ~/.config is never touched: HOME is redirected for every call.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/tmp/install-test"
BLOCK="$HERE/hypr/bindings-workspaces.lua"
REAL_HOME="$HOME"

rm -rf "$FIX"
mkdir -p "$FIX/home/.config/hypr" "$FIX/home/.config/omarchy/plugins" "$FIX/bin"

printf 'line one\nline two\n' > "$FIX/home/.config/hypr/bindings.lua"
cp "$FIX/home/.config/hypr/bindings.lua" "$FIX/original.lua"
printf '{ "bar": { "layout": { "left": [ { "id": "omarchy.workspaces" } ] } } }\n' \
  > "$FIX/home/.config/omarchy/shell.json"

cat > "$FIX/bin/omarchy" <<'STUB'
#!/usr/bin/env bash
if [ "$1" = plugin ] && [ "$2" = clone ]; then
  d="$HOME/.config/omarchy/plugins/$(id -un).workspaces"
  mkdir -p "$d"
  printf 'moduleName: "omarchy.workspaces"\nfunction workspaceIds() { var ids = [1,2,3,4,5] }\n' > "$d/Workspaces.qml"
  exit 0
fi
exit 0
STUB

cat > "$FIX/bin/omarchy-shell" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

# Reports only ONE bank-2 bind, so the "Move window to workspace 1x" grep
# matches nothing — the exact case that used to abort --check under pipefail.
cat > "$FIX/bin/hyprctl" <<'STUB'
#!/usr/bin/env bash
case "$1" in
  binds) printf '[{"description": "Switch to workspace 11"}]\n' ;;
esac
exit 0
STUB

chmod +x "$FIX/bin/"*
export HOME="$FIX/home"
export PATH="$FIX/bin:$PATH"

fail=0
check() { # check <label> <expected> <actual>
  if [ "$2" = "$3" ]; then
    echo "  ✓ $1"
  else
    echo "  ✗ $1 — expected '$2', got '$3'"
    fail=1
  fi
}

binds_file="$HOME/.config/hypr/bindings.lua"
widget_file="$HOME/.config/omarchy/plugins/$(id -un).workspaces/Workspaces.qml"
lines() { wc -l < "$binds_file"; }
same()  { diff -q "$FIX/original.lua" "$binds_file" >/dev/null; }

echo "round-trip on $FIX/home"

# ── scenario A: block written by install.sh (with marker) ────────────────────
"$HERE/install.sh" >/dev/null
after_first="$(lines)"
"$HERE/install.sh" >/dev/null
check "install is idempotent" "$after_first" "$(lines)"

if grep -qF -- 'for (var n = 1; n <= 20; n++)' "$widget_file" 2>/dev/null; then
  check "patched widget is installed" yes yes
else
  check "patched widget is installed" yes no
fi

set +e
"$HERE/install.sh" --check >/dev/null 2>&1
check_exit=$?
set -e
check "--check exits cleanly on a no-match grep" 0 "$check_exit"

"$HERE/install.sh" --uninstall >/dev/null
if same; then
  check "uninstall restores an install.sh-written block byte-for-byte" yes yes
else
  echo "  ✗ uninstall restores an install.sh-written block byte-for-byte — leftover:"
  diff -u "$FIX/original.lua" "$binds_file" | sed 's/^/      /' || true
  fail=1
fi

# ── scenario B: marker-less block, as appended by hand on an older machine ───
{ cat "$FIX/original.lua"; printf '\n'; cat "$BLOCK"; } > "$binds_file"
"$HERE/install.sh" --uninstall >/dev/null
if same; then
  check "uninstall restores a marker-less block byte-for-byte" yes yes
else
  echo "  ✗ uninstall restores a marker-less block byte-for-byte — leftover:"
  diff -u "$FIX/original.lua" "$binds_file" | sed 's/^/      /' || true
  fail=1
fi

# ── scenario C: a hand-added block must not make install.sh append a second ──
{ cat "$FIX/original.lua"; printf '\n'; cat "$BLOCK"; } > "$binds_file"
before="$(lines)"
"$HERE/install.sh" >/dev/null
check "install skips when the block was appended by hand" "$before" "$(lines)"

echo
if [ "$fail" -eq 0 ]; then
  echo "PASS"
else
  echo "FAIL"
fi

export HOME="$REAL_HOME"
exit "$fail"
