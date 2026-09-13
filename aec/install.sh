#!/bin/bash
# pw-aec-avx: build, install (user-local, no sudo) and wire up PipeWire.
# AEC is NOT auto-started - zero background cost. Start with: aec.sh start
# Usage: ./install.sh
set -euo pipefail
cd "$(dirname "$0")"

echo "== building"
gcc -O3 -std=gnu11 -Wall -fPIC -shared -o libspa-aec-avx.so libspa-aec-avx.c \
    -I/usr/include/spa-0.2 -lm
gcc -O2 -std=gnu11 -Wall -o aec-host aec-host.c \
    -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 -lpipewire-0.3
./bench   # sanity: delay tracking + AVX2 speedup

echo "== installing plugin to user SPA dir (no sudo needed)"
mkdir -p ~/.local/lib/spa-0.2/aec
cp libspa-aec-avx.so ~/.local/lib/spa-0.2/aec/

echo "== systemd drop-ins: add user SPA dir to SPA_PLUGIN_DIR"
# Needed for BOTH daemons: pipewire (nodes) and pipewire-pulse (aec.sh loads
# the module there via pactl, without restarting anything).
for svc in pipewire pipewire-pulse; do
    mkdir -p ~/.config/systemd/user/$svc.service.d
    cat > ~/.config/systemd/user/$svc.service.d/spa-plugin-dir.conf <<'EOF'
# Add the user-local SPA plugin directory (pw-aec-avx lives here, no sudo
# needed for plugin installs) in front of the system one.
[Service]
Environment=SPA_PLUGIN_DIR=%h/.local/lib/spa-0.2:/usr/lib/spa-0.2
EOF
done

echo "== installing scripts to ~/.local/bin"
mkdir -p ~/.local/bin
install -m 755 aec-host aec.sh use-aec.sh ~/.local/bin/

echo "== ensuring NO auto-start of echo cancel"
rm -f ~/.config/pipewire/pipewire.conf.d/20-echo-cancel.conf
# (to go back to always-on: see echo-cancel.autostart.conf.example)

echo "== restarting pipewire + wireplumber"
systemctl --user daemon-reload
systemctl --user restart pipewire pipewire-pulse wireplumber
sleep 3

echo "== state (AEC should be OFF here):"
~/.local/bin/aec.sh status
echo "default sink: $(pactl get-default-sink)"
echo
echo "== done. Start echo cancellation before a call:   aec.sh start"
echo "                    Stop it afterwards (0 CPU):   aec.sh stop"
echo "Backend switch: use-aec.sh [avx|webrtc|off]. Tune aec.args in aec.sh."