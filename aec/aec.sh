#!/bin/bash
# pw-aec-avx: echo cancellation ON DEMAND - zero background cost when stopped.
# Usage: aec.sh start [avx|webrtc] | aec.sh stop | aec.sh status
#
# How it works: a tiny client process (aec-host) loads the native
# module-echo-cancel; all its audio paths are pw_streams, so they attach to
# the RUNNING PipeWire graph. No pipewire restart, instant on/off.
# stop = kill the host process -> nodes vanish, 0 CPU.
#
# monitor.mode: AEC reference = monitor of the DEFAULT sink - playback may go
# to any output device (JBL BT, built-in, ...) and is still cancelled.
set -euo pipefail
CMD="${1:-status}"
BACKEND="${2:-avx}"
DIR="$(cd "$(dirname "$0")" && pwd)"
HOST="$DIR/aec-host"
[ -x ~/.local/bin/aec-host ] && HOST=~/.local/bin/aec-host
PIDFILE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pw-aec-avx.pid"
AECARGS="tail_ms=80 delay_max_ms=600 mu=0.35"

node_present() { pactl list sources short 2>/dev/null | awk '$2=="echo_cancel_source"{print $1}'; }
host_alive() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && cat "$PIDFILE"; }

start() {
    if [ -n "$(host_alive)" ]; then
        echo "AEC already running (host pid $(host_alive)) - mic: $(pactl get-default-source)"
        return 0
    fi
    rm -f "$PIDFILE"
    case "$BACKEND" in
        avx)    LIB=aec/libspa-aec-avx ;;
        webrtc) LIB=aec/libspa-aec-webrtc; AECARGS="" ;;
        *) echo "backend must be avx|webrtc" >&2; exit 1 ;;
    esac
    export SPA_PLUGIN_DIR="$HOME/.local/lib/spa-0.2:/usr/lib/spa-0.2"
    setsid "$HOST" "$LIB" "$AECARGS" >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$(node_present)" ] && break
        sleep 0.4
    done
    if [ -z "$(node_present)" ]; then
        echo "ERROR: aec-host did not create echo_cancel_source (see: aec-host run in foreground)" >&2
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
        exit 1
    fi
    pactl set-default-source echo_cancel_source
    echo "AEC ON ($BACKEND, pid $(cat "$PIDFILE")). mic: $(pactl get-default-source), monitored output: $(pactl get-default-sink)"
}

stop() {
    HP=$(host_alive)
    if [ -z "$HP" ]; then
        echo "AEC already off - mic: $(pactl get-default-source)"
        return 0
    fi
    kill "$HP" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8; do
        [ -z "$(node_present)" ] && break
        sleep 0.3
    done
    kill -9 "$HP" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "AEC OFF (0 CPU). mic: $(pactl get-default-source)"
}

status() {
    HP=$(host_alive)
    if [ -z "$HP" ]; then
        echo "AEC OFF (0 CPU) - mic: $(pactl get-default-source)"
    else
        echo "AEC ON (host pid $HP) - mic: $(pactl get-default-source), monitored output: $(pactl get-default-sink)"
    fi
}

case "$CMD" in
    start)  start ;;
    stop)   stop ;;
    status) status ;;
    *) echo "usage: $0 start [avx|webrtc] | stop | status" >&2; exit 1 ;;
esac