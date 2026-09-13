#!/bin/bash
# Legacy wrapper - kept for old habit. Backend switch is now on-demand:
#   use-aec.sh avx|webrtc  ->  start AEC with that backend
#   use-aec.sh off         ->  stop AEC
# (Same as aec.sh start <backend> / aec.sh stop.)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
case "${1:-avx}" in
    avx|webrtc) exec "$DIR/aec.sh" start "$1" ;;
    off)        exec "$DIR/aec.sh" stop ;;
    *) echo "usage: $0 [avx|webrtc|off]" >&2; exit 1 ;;
esac