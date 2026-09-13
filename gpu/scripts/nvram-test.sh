#!/usr/bin/env bash
# nvram-test.sh — combined GPU-switch tester/controller for MacBookPro11,3 (Kepler hybrid)
#
# Three switching layers:
#   1. NVRAM boot-default — gpu-power-prefs (efivarfs -> raw flash). The firmware reads
#      it at EVERY POST, before the bootloader. A change takes effect from the NEXT boot —
#      NVRAM is persistence, NOT a live path (report 70).
#   2. Runtime gmux — apple-gmux registers via the C helper gmux-io (/dev/port, indexed).
#      Live-switching the panel to the retina eDP = RISK OF BLACK SCREEN (report 74).
#   3. pstate — pstate reporting of the dGPU (managed by reclocked).
#
# Gates:
#   --apply        — allows NVRAM writes (commit on the next boot; reboot required)
#   --apply --live — additionally switches gmux NOW (live switch, black-screen risk)
#   --yes          — skips the confirmation prompt
#
# Usage:
#   ./nvram-test.sh status                     full state (read-only)
#   ./nvram-test.sh to-igd [--apply] [--live]  NVRAM -> IGD (+ optionally live gmux)
#   ./nvram-test.sh to-dis [--apply] [--live]  NVRAM -> DIS (+ optionally live gmux)
#   ./nvram-test.sh pstate                     pstate report (read-only)
#   ./nvram-test.sh --list                     list efivarfs variables
#   ./nvram-test.sh --read <name>              read one variable (hex+ascii)
#   ./nvram-test.sh --flash-gpu                gpu-power-prefs raw from flash
#   ./nvram-test.sh --gpu status               GPU switching state
#   ./nvram-test.sh --gpu igd|dis [--apply]    aliases of to-igd / to-dis
#   ./nvram-test.sh --write <var>-<guid> <hex> [--apply]  generic write
#   ./nvram-test.sh --help                     this help
#
# Recovery after a black screen (live switch / boot on IGD):
#   ssh -t user@nas-address
#   ./recover-gpu.sh --dedicated --reboot      # reverts to DIS + reboot
#   reset PRAM: Cmd+Opt+P+R at startup (clears gpu-power-prefs)

set -uo pipefail

# ---------- constants ----------
EFIVARS=/sys/firmware/efi/efivars
MTD_RO=/dev/mtd0ro
DUMP_FILE=$HOME/Projects/nv-kepler/tmp/nvram-dump/flash-8MB.bin
GMUX_IO_BIN=$HOME/Projects/nv-kepler/gmux-io
PSTATE=/sys/kernel/debug/dri/0000:01:00.0/pstate
RECLOCKD_STATUS=/run/reclocked/status
SWITCHD_DGPU=/run/switchd/dgpu

# GPU variables (report 70)
GPREFS_NAME=gpu-power-prefs
GPREFS_GUID=fa4ce28d-b62f-4c99-9cc3-6815686e30f9
GACTIVE_NAME=gpu-active
GACTIVE_GUID=fa4ce28d-b62f-4c99-9cc3-6815686e30f9
GPOLICY_NAME=gpu-policy
GPOLICY_GUID=7c436110-ab2a-4bbb-a880-fe41995c9f82
GFXRESTORE_NAME=gfx-saved-config-restore-status
GFXRESTORE_GUID=4d1ede05-38c7-4a6a-9cc6-4bcca8b38c14

# raw flash offsets of the CURRENT gpu-power-prefs generation (State 0x7F, report 70;
# offsets corrected for the report typo: 0x61615D instead of 0x61165D):
#   store1 @0x61615D (current DIS) — historical: 0x61101A (0x7C DIS), 0x616115 (0x7D IGD)
#   store2 @0x621069 (current DIS)
FLASH_GPREFS1=0x61615D
FLASH_GPREFS2=0x621069

# write attributes (uint32 LE):
#   07 00 00 00 = NV|BS|RT            (what gpu-switch writes)
#   07 00 00 80 = 0x80000007 +ENHANCED_AUTH (found in this unit's flash)
ATTRS_NVBRT="07 00 00 00"
ATTRS_NVBRT_ENH="07 00 00 80"

# ---------- global flags ----------
APPLY=0; YES=0; LIVE=0

die(){ echo "error: $*" >&2; exit 1; }

need_root(){ [ "$EUID" = 0 ] || die "root required (sudo $0 $* )"; }

need_efivars(){
  [ -d "$EFIVARS" ] || die "not an EFI system (/sys/firmware/efi missing)"
  if ! mount | grep -q "$EFIVARS"; then
    need_root
    mount -t efivarfs efivarfs "$EFIVARS" 2>/dev/null \
      || die "could not mount $EFIVARS"
  fi
}

# ---------- helper C gmux-io ----------
find_gmux_io(){
  local c
  for c in "$GMUX_IO_BIN" "$(dirname "$0")/gmux-io" "$(dirname "$0")/tmp/gmux-io"; do
    if [ -x "$c" ]; then GMUX_IO_BIN="$c"; return 0; fi
  done
  if command -v gmux-io >/dev/null 2>&1; then
    GMUX_IO_BIN="$(command -v gmux-io)"; return 0
  fi
  return 1
}

need_gmux_io(){
  find_gmux_io || die "gmux-io not found — build it: gcc -O2 -Wall -Wextra -o gmux-io gmux-io.c"
}

# run gmux-io (requires root — /dev/port)
gmux_io(){ # <args...>
  need_gmux_io
  if [ "$EUID" = 0 ]; then "$GMUX_IO_BIN" "$@"; else sudo "$GMUX_IO_BIN" "$@"; fi
}

# ---------- efivarfs ----------
resolve_var(){ # <name|name-guid> -> echo path; return 0/1
  local n="$1" f=""
  if [ -e "$EFIVARS/$n" ]; then
    f="$n"
  else
    f="$(ls "$EFIVARS" 2>/dev/null | grep -i "^$n-" | head -n1 || true)"
  fi
  if [ -n "$f" ] && [ -e "$EFIVARS/$f" ]; then
    echo "$f"; return 0
  fi
  return 1
}

dump_var(){ # <path>
  local p="$1" attrs data
  attrs="$(dd if="$p" bs=1 count=4 2>/dev/null | xxd -p)"
  data="$(dd if="$p" bs=1 skip=4 2>/dev/null | xxd -p)"
  printf 'file    %s\n' "$p"
  if [ -n "$attrs" ]; then
    printf 'attrs   %s  (LE u32)\n' "$attrs"
  else
    printf 'attrs   (read returned an error — firmware hides the data from runtime)\n'
  fi
  printf 'data    %s\n' "$data"
  printf 'ascii   %s\n' "$(dd if="$p" bs=1 skip=4 2>/dev/null | strings -n1 | tr '\n' ' ')"
}

# read gpu-power-prefs from raw SPI flash (uses the saved dump if present)
flash_read_gpu(){
  local src="$DUMP_FILE" prefix=""
  if [ ! -f "$src" ]; then
    need_root; src="$MTD_RO"; prefix="(live $MTD_RO — root)"
  fi
  echo "  $prefix"
  echo "  store1 @ $FLASH_GPREFS1 (State 0x7F, current):"
  dd if="$src" bs=1 skip=$((FLASH_GPREFS1)) count=48 2>/dev/null | xxd | sed 's/^/    /'
  echo "  store2 @ $FLASH_GPREFS2 (State 0x7F, current):"
  dd if="$src" bs=1 skip=$((FLASH_GPREFS2)) count=48 2>/dev/null | xxd | sed 's/^/    /'
}

# ---------- vgaswitcheroo ----------
vgaswitcheroo_status(){
  local out
  if [ -r /sys/kernel/debug/vgaswitcheroo/switch ]; then
    out="$(cat /sys/kernel/debug/vgaswitcheroo/switch 2>&1)"
  else
    out="$(sudo -n cat /sys/kernel/debug/vgaswitcheroo/switch 2>&1)"
  fi
  if [ -n "$out" ]; then
    echo "$out" | sed 's/^/  /'
  else
    echo "  (unavailable — debugfs mounted? root?)"
  fi
}

# ---------- pstate ----------
find_hwmon(){
  local h
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nouveau" ]; then
      echo "$h"; return 0
    fi
  done
  return 1
}

pstate_status(){
  echo "== pstate (debugfs) =="
  local out
  if [ -r "$PSTATE" ]; then
    out="$(cat "$PSTATE" 2>&1)"
  else
    out="$(sudo -n cat "$PSTATE" 2>&1)"
  fi
  if [ -n "$out" ]; then
    echo "$out" | sed 's/^/  /'
  else
    echo "  (unavailable — debugfs mounted? root? try: sudo $0 status)"
  fi
  local hw
  if hw=$(find_hwmon); then
    printf "== temperatura: %.1f °C (hwmon: %s) ==\n" \
      "$(awk '{printf "%.1f", $1/1000}' "$hw/temp1_input")" "$(basename "$hw")"
  fi
  echo "== reclocked =="
  if [ -f "$RECLOCKD_STATUS" ]; then
    cat "$RECLOCKD_STATUS" | sed 's/^/  /'
  else
    echo "  (no $RECLOCKD_STATUS — reclocked not running?)"
  fi
  if [ -f "$SWITCHD_DGPU" ]; then
    echo "== switchd dGPU =="
    cat "$SWITCHD_DGPU" | sed 's/^/  /'
  fi
}

# ---------- status ----------
cmd_status(){
  echo "== NVRAM gpu-power-prefs (efivarfs) =="
  local p
  if p=$(resolve_var "$GPREFS_NAME"); then
    dump_var "$EFIVARS/$p" | sed 's/^/  /'
  else
    echo "  (hidden from runtime — firmware does not return it via GetVariable; see raw flash)"
  fi
  echo "== NVRAM gpu-power-prefs (raw flash) =="
  flash_read_gpu
  echo "== vgaswitcheroo =="
  vgaswitcheroo_status
  echo "== gmux (helper C, indexed) =="
  if find_gmux_io; then
    local gout
    gout="$(gmux_io status 2>&1)" || echo "  (gmux-io not working — root?)"
    echo "$gout" | sed 's/^/  /'
  else
    echo "  (gmux-io not built — gcc -O2 -Wall -Wextra -o gmux-io gmux-io.c)"
  fi
  echo "== gpu-active (runtime) =="
  dump_var "$EFIVARS/$GACTIVE_NAME-$GACTIVE_GUID" 2>&1 | sed 's/^/  /' || true
  echo "== gpu-policy (runtime) =="
  dump_var "$EFIVARS/$GPOLICY_NAME-$GPOLICY_GUID" 2>&1 | sed 's/^/  /' || true
  echo "== pstate / reclocked =="
  pstate_status
}

# ---------- NVRAM write ----------
write_var(){ # <name-guid> <datahex> [attrs]
  local fname="$1" datahex="$2" attrs="${3:-$ATTRS_NVBRT}" bin target
  [ "$APPLY" = 1 ] || die "refusing write: add --apply"
  need_root
  need_efivars
  bin="$(printf '%s%s' "$attrs" "$datahex" | xxd -r -p)"
  target="$EFIVARS/$fname"
  printf 'Writing %s:\n  attrs %s  data %s\n' "$fname" "$attrs" "$datahex"
  if [ "$YES" != 1 ]; then
    read -r -p "type 'yes' to WRITE: " a
    [ "$a" = yes ] || die "aborted"
  fi
  chattr -i "$target" 2>/dev/null
  if printf '%s' "$bin" > "$target" 2>/dev/null; then
    echo "[ok] SetVariable via efivarfs succeeded"
  else
    echo "[fail] write rejected (EFI_ACCESS_DENIED / attr mismatch?)" >&2
    return 1
  fi
}

# ---------- live gmux switch ----------
cmd_live_switch(){ # <igd|dis>
  local target="$1" ddc display external
  echo
  echo "!!! LIVE SWITCH — RISK OF BLACK SCREEN !!!"
  echo "  Switching the panel mux at runtime on retina eDP is UNSUPPORTED"
  echo "  upstream (no NO_AUX_HANDSHAKE relay; reports 36/38/74)."
  echo "  Possible black screen until reboot."
  echo "  Recovery:"
  echo "    ssh -t user@nas-address"
  echo "    ./recover-gpu.sh --dedicated --reboot"
  echo "    or reset PRAM: Cmd+Opt+P+R at startup"
  echo
  if [ "$YES" != 1 ]; then
    read -r -p "type 'yes' to LIVE SWITCH gmux NOW: " a
    [ "$a" = yes ] || die "aborted"
  fi
  need_gmux_io
  if [ "$target" = igd ]; then
    ddc=1; display=2; external=2
  else
    ddc=2; display=3; external=3
  fi
  # order per gmux_write_switch_state (apple-gmux.c): DDC -> DISPLAY -> EXTERNAL
  echo "  [gmux] SWITCH_DDC 0x28 <- $ddc"
  gmux_io write 0x28 "$ddc" || return 1
  echo "  [gmux] SWITCH_DISPLAY 0x10 <- $display"
  gmux_io write 0x10 "$display" || return 1
  echo "  [gmux] SWITCH_EXTERNAL 0x40 <- $external"
  gmux_io write 0x40 "$external" || return 1
  echo "  [ok] gmux switched. Verification:"
  gmux_io status 2>&1 | sed 's/^/  /'
}

# ---------- to-igd / to-dis ----------
cmd_to_gpu(){ # <igd|dis>
  local target="$1" datahex val
  if [ "$target" = igd ]; then
    datahex="01000000"; val="IGD (iGPU)"
  else
    datahex="00000000"; val="DIS (dGPU)"
  fi
  echo "== Target: $val — boot-default via NVRAM (gpu-power-prefs) =="
  if [ "$APPLY" = 1 ]; then
    write_var "$GPREFS_NAME-$GPREFS_GUID" "$datahex" "$ATTRS_NVBRT" \
      || write_var "$GPREFS_NAME-$GPREFS_GUID" "$datahex" "$ATTRS_NVBRT_ENH" \
      || die "NVRAM write failed"
    echo "  [ok] NVRAM written — takes effect from the NEXT boot (reboot required)"
    if [ "$LIVE" = 1 ]; then
      cmd_live_switch "$target"
    else
      echo "  [i] without --live: panel mux NOT touched in this session"
      echo "  [i] live switch NOW (black-screen risk): $0 to-$target --apply --live"
    fi
  else
    echo "  [i] dry-run: without --apply nothing is written"
    echo "  [i] full write (reboot):  $0 to-$target --apply"
    echo "  [i] live switch (risk): $0 to-$target --apply --live"
  fi
}

# ---------- read-only commands ----------
cmd_list(){ ls -la "$EFIVARS" 2>/dev/null | tail -n +2; }

cmd_read(){ resolve_var "$1" >/dev/null || die "no such variable: $1"; dump_var "$EFIVARS/$(resolve_var "$1")"; }

cmd_flash_gpu(){ flash_read_gpu; }

cmd_gpu(){
  local action="$1"
  case "$action" in
    status) cmd_status;;
    ig|igd) cmd_to_gpu igd;;
    dis)    cmd_to_gpu dis;;
    test)   echo "[test] writing the current state (DIS) over DIS — state-neutral"
            cmd_to_gpu dis;;
    *) die "unknown --gpu action: $action (status|igd|dis|test)";;
  esac
}

# ---------- usage ----------
usage(){
  cat <<'EOF'
Usage: ./nvram-test.sh <command> [--apply] [--live] [--yes]

Commands:
  status                     full state (read-only): NVRAM, vgaswitcheroo, gmux, pstate
  to-igd [--apply] [--live] NVRAM -> IGD (boot-default; --live = gmux NOW)
  to-dis [--apply] [--live] NVRAM -> DIS (boot-default; --live = gmux NOW)
  pstate                     dGPU pstate report (read-only)
  --list                     list efivarfs variables
  --read <name>              read one variable (hex+ascii)
  --flash-gpu                gpu-power-prefs raw from flash
  --gpu status               GPU switching state
  --gpu igd|dis [--apply]    aliases of to-igd / to-dis
  --write <var>-<guid> <hex> [--apply]  generic write
  --help                     this help

Gates:
  --apply        allows NVRAM writes (commit on the next boot; reboot required)
  --apply --live additionally switches gmux NOW (live switch, black-screen risk)
  --yes          skips the confirmation prompt

Recovery after a black screen:
  ssh -t user@nas-address
  ./recover-gpu.sh --dedicated --reboot
  or reset PRAM: Cmd+Opt+P+R at startup (clears gpu-power-prefs)
EOF
}

# ---------- argument parsing ----------
POS=()
for a in "$@"; do
  case "$a" in
    --apply) APPLY=1;;
    --yes)   YES=1;;
    --live)  LIVE=1;;
    -h|--help) usage; exit 0;;
    *) POS+=("$a");;
  esac
done

[ ${#POS[@]} -ge 1 ] || { usage; exit 1; }

case "${POS[0]}" in
  status) cmd_status;;
  to-igd) cmd_to_gpu igd;;
  to-dis) cmd_to_gpu dis;;
  pstate) pstate_status;;
  --list) cmd_list;;
  --read) [ ${#POS[@]} -ge 2 ] || die "--read needs a variable name"; cmd_read "${POS[1]}";;
  --flash-gpu) cmd_flash_gpu;;
  --gpu)  [ ${#POS[@]} -ge 2 ] || die "--gpu needs action"; cmd_gpu "${POS[1]}";;
  --write) [ ${#POS[@]} -ge 3 ] || die "--write <var>-<guid> <hex>"; write_var "${POS[1]}" "${POS[2]}";;
  *) die "unknown command: ${POS[0]} (try --help)";;
esac
