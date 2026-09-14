#!/usr/bin/env bash
# Mounts all Samba shares from the NAS (CIFS/SMB3).
# Requires: cifs-utils. Mounts as root (sudo).
# Edit SERVER / SHARES / CRED to match your NAS.
set -euo pipefail

SERVER=<nas-address>            # <-- replace with your NAS address
USER="$USER"                  # <-- your SMB username
BASE=/mnt/nas
CRED="$HOME/.smbcred-nas"

# SMB share names reported by the server (smbclient -L)
SHARES=(
  "share1"
  "share2"
  "public"
)

sanitize() { printf '%s' "$1" | tr -c 'A-Za-z0-9.-' '_' | sed 's/^_//; s/_$//'; }

ACTION="${1:-mount}"

if [[ ! -f "$CRED" ]]; then
  echo "Creating credentials file $CRED"
  umask 077
  printf 'username=%s\npassword=CHANGE_ME\ndomain=WORKGROUP\n' "$USER" > "$CRED"
fi

case "$ACTION" in
  mount|"")
    sudo mkdir -p "$BASE"
    sudo chmod 755 "$BASE"
    for s in "${SHARES[@]}"; do
      mp="$BASE/$(sanitize "$s")"
      sudo mkdir -p "$mp"
      sudo chmod 755 "$mp"
      if mountpoint -q "$mp"; then
        echo "already mounted: $mp  ($s)"; continue
      fi
      echo "mounting  $s  ->  $mp"
      # the full UNC path in a single argument (spaces/brackets OK)
      sudo mount -t cifs "//${SERVER}/$s" "$mp" -o \
        "credentials=$CRED,uid=1000,gid=1000,iocharset=utf8,vers=3.0,seal,soft,actimeo=1" \
        && echo "  OK" || echo "  ERROR for '$s'"
    done
    ;;
  umount|-u)
    for s in "${SHARES[@]}"; do
      mp="$BASE/$(sanitize "$s")"
      mountpoint -q "$mp" && { echo "unmounting $mp"; sudo umount "$mp"; } || echo "not mounted: $mp"
    done
    ;;
  *) echo "usage: $0 [mount|umount]"; exit 1;;
esac