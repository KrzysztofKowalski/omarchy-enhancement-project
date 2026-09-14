#!/usr/bin/env bash
# Mounts the same folders from the NAS over SSHFS (encrypted SSH tunnel,
# key login — no password). Runs in user space, no sudo.
# Requires: sshfs. Key ~/.ssh/id_ed25519 (already uploaded to the server).
# Edit SERVER / Paths to match your NAS.
set -euo pipefail

SERVER="user@<nas-address>"     # <-- replace with your SSH user@host
BASE="$HOME/mnt/sshfs-nas"
IDENT="$HOME/.ssh/id_ed25519"

# SMB share on the NAS  ->  actual path on the server (reachable over SSH)
declare -A PATHS=(
  ["share1"]="Volumes/share1"
  ["share2"]="Volumes/share2"
  ["public"]="Users/public"
)

ACTION="${1:-mount}"
sanitize() { printf '%s' "$1" | tr -c 'A-Za-z0-9.-' '_' | sed 's/^_//; s/_$//'; }

case "$ACTION" in
  mount|"")
    mkdir -p "$BASE"
    for s in "${!PATHS[@]}"; do
      mp="$BASE/$(sanitize "$s")"
      remote="/${PATHS[$s]}"
      mkdir -p "$mp"
      mountpoint -q "$mp" && { echo "already mounted: $mp"; continue; }
      echo "mounting  $s  ->  $mp  (=$remote)"
      sshfs "$SERVER:$remote" "$mp" -o \
        "IdentityFile=$IDENT,reconnect,ServerAliveInterval=15,uid=1000,gid=1000,cache=yes,kernel_cache,auto_unmount" \
        && echo "  OK" || echo "  ERROR for '$s'"
    done
    ;;
  umount|-u)
    for s in "${!PATHS[@]}"; do
      mp="$BASE/$(sanitize "$s")"
      mountpoint -q "$mp" && { echo "unmounting $mp"; fusermount -u "$mp" 2>/dev/null || fusermount3 -u "$mp"; } || echo "not mounted: $mp"
    done
    ;;
  *) echo "usage: $0 [mount|umount]"; exit 1;;
esac