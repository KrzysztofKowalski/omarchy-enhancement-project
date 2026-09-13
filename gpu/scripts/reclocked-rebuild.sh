#!/usr/bin/env bash
# Recompilation of reclocked (requirement: ZERO warnings) + systemd daemon restart + daemon status.
# Purely LOCAL (main repo). The port to official/reclocked is done separately, outside this script.
# Usage: reclocked-rebuild.sh [--reload]
#   (no arg.)  rebuild + restart + status;   --reload  SIGHUP without rebuild (change of /etc/reclocked.conf)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# After the reclockd→reclocked restructure the daemon builds in src/ (report 112:
# relative paths of scripts in scripts/ are counted from dirname $0/.. — not from CWD).
BUILD_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/src"

# Builds $1; on a compile error or any "warning:" in output → exit 1 without restart.
build_one() {
    local dir="$1" out
    echo "== build $dir =="
    if out="$(make -C "$dir" 2>&1)"; then
        echo "$out"
    else
        echo "== ERROR: build $dir failed — no restart ==" >&2
        echo "$out" >&2
        exit 1
    fi
    if grep -q "warning:" <<<"$out"; then
        echo "== ERROR: warnings in build $dir (project requirement: ZERO warnings) — no restart ==" >&2
        echo "$out" | grep "warning:" >&2
        exit 1
    fi
}

restart_daemon() {
    echo "== restart reclocked =="
    sudo systemctl restart reclocked
    sleep 3
    if ! systemctl is-active --quiet reclocked; then
        echo "== ERROR: reclocked not active after restart ==" >&2
        systemctl status reclocked --no-pager >&2 || true
        exit 1
    fi
    echo "== startup log =="
    sudo journalctl -u reclocked --since "10 sec ago" --no-pager | grep -E "start v|ERROR|error" | tail -5 || true
    echo "== status =="
    sudo cat /run/reclocked/status || true
}

case "${1:-}" in
    "")
        build_one "$BUILD_DIR"
        restart_daemon
        ;;
    --reload)
        echo "== reload reclocked (SIGHUP, no rebuild) =="
        sudo systemctl kill -s HUP reclocked
        sleep 1
        sudo journalctl -u reclocked --since "5 sec ago" --no-pager | grep -E "SIGHUP|config" | tail -3 || true
        ;;
    *)
        echo "Usage: $0 [--reload]" >&2
        exit 1
        ;;
esac

echo "== done =="
exit 0
