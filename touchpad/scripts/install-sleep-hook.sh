#!/usr/bin/env bash
# Installs an interim bcm5974 hotfix: a systemd system-sleep hook.
#   Hook: modprobe -r bcm5974 BEFORE suspend / modprobe bcm5974 AFTER resume.
#   Takes effect from the next suspend-resume cycle (no reboot needed).
#
# Usage:
#   ./scripts/install-sleep-hook.sh            # install
#   ./scripts/install-sleep-hook.sh --uninstall  # uninstall
#
# Eventually (after the -omnkp kernel with the reset_resume patches) this hook
# becomes redundant and uninstalling it is recommended — see ../om-kernel/NOTES.md.
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC_HOOK="$PROJ/scripts/sleep-bcm5974-rebind"
DEST=/usr/lib/systemd/system-sleep/010-bcm5974-rebind

if [[ "${1:-}" == "--uninstall" ]]; then
    if [[ -e "$DEST" ]]; then
        sudo rm -v "$DEST"
    else
        echo "Hook was not installed: $DEST"
    fi
    exit 0
fi

if [[ ! -s "$SRC_HOOK" ]]; then
    echo "Hook source missing: $SRC_HOOK" >&2
    exit 1
fi

sudo install -m 0755 -o root -g root "$SRC_HOOK" "$DEST"
echo "Installed: $DEST"
echo "Active at the next suspend-resume cycle. Test: sudo systemctl suspend"
echo "Uninstall: $0 --uninstall  (after the -omnkp kernel lands)"
