#!/usr/bin/env bash
# pstate.sh — manual pstate switching of the GT 750M (nouveau, Kepler GK107)
# Written through debugfs: /sys/kernel/debug/dri/0000:01:00.0/pstate (needs root).
# THE SETTINGS ARE RUNTIME-ONLY: a reboot ALWAYS resets to the startup state.
#
# v4: integration with reclocked — `set` enables manual override (the daemon freezes auto),
#     `auto` removes the override, `status` also shows the daemon and override state.
#
# Usage:
#   ./pstate.sh status           — pstate + temp + override + daemon
#   ./pstate.sh set 0a           — set 0a (AC+DC) + enable override (freezes the daemon)
#   ./pstate.sh set ac:0a        — set 0a for AC only + override
#   ./pstate.sh auto             — remove the override (the daemon resumes auto)
#
# Pstates (core / mem, MHz):
#   07: 270-405 / 838       0a: 270-925 / 1560
#   0e: 270-925 / 4000      0f: 270-925 / 5016
# 0a/0e/0f have the SAME core range (max 925 MHz) — only the memory differs.
# ⚠️ 0e/0f = aggressive memory reclock — test carefully (history of hangs).

PSTATE=/sys/kernel/debug/dri/0000:01:00.0/pstate
VALID="07 0a 0e 0f"
OVERRIDE_DIR=/run/reclocked
OVERRIDE_FILE=/run/reclocked/override

find_hwmon() {
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nouveau" ]; then
      echo "$h"
      return 0
    fi
  done
  return 1
}

find_coretemp() {
  # CPU temp ("Package id 0") — coretemp does NOT disappear on a dGPU power-cycle
  # (unlike the nouveau hwmon). Fallback for pstate.sh when the dGPU is OFF.
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "coretemp" ] && [ -r "$h/temp1_input" ]; then
      echo "$h"
      return 0
    fi
  done
  return 1
}

usage() {
  cat <<'EOF'
Usage:
  ./pstate.sh status           — pstate + temp + override + daemon
  ./pstate.sh set 0a           — set 0a (AC+DC) + enable override (freezes the daemon)
  ./pstate.sh set ac:0a        — set 0a for AC only + override
  ./pstate.sh auto             — remove the override (the daemon resumes auto)

Pstates (core / mem, MHz):
  07: 270-405 / 838       0a: 270-925 / 1560
  0e: 270-925 / 4000      0f: 270-925 / 5016
0a/0e/0f have the SAME core range (max 925 MHz) — only the memory differs.
⚠️ 0e/0f = aggressive memory reclock — test carefully.
Settings are runtime-only: REBOOT RESETS to the startup state.
EOF
}

daemon_pid() {
  # Returns the reclocked PID or an empty string (test non-emptiness, NOT the pipe
  # exit status — `pgrep | head` has the status of `head`, always 0; see report 15).
  pgrep -x reclocked 2>/dev/null | head -1
}

daemon_status() {
  local pid
  pid=$(daemon_pid)
  if [ -n "$pid" ]; then
    echo "daemon: RUNNING (PID $pid)"
  else
    echo "daemon: not running"
  fi
}

override_status() {
  local pid content
  pid=$(daemon_pid)
  if sudo -n test -f "$OVERRIDE_FILE" 2>/dev/null; then
    content=$(sudo -n cat "$OVERRIDE_FILE" 2>/dev/null)
    if [ -n "$pid" ]; then
      echo "override: ACTIVE ($content — pstate frozen, daemon PID $pid)"
    else
      echo "override: $content (flag set, but the daemon is NOT running — the flag has no effect)"
    fi
  else
    echo "override: none (auto)"
  fi
}

# ---------------------------------------------------------------------------
# iGPU (Intel Iris Pro 5200, i915 RPS) — READ-ONLY monitoring (report 75, Option A;
# write/cap in v5.1+). We write nothing here — only sysfs reads.
# ---------------------------------------------------------------------------

IGPU_BDF="0000:00:02.0"

find_igpu_card() {
  # The cardN number changes between boots — find the card via the PCI BDF
  # (/sys/class/drm/cardN/device -> readlink gives the BDF), not a hardcoded cardN.
  local card bdf
  for card in /sys/class/drm/card*; do
    [ -e "$card" ] || continue
    bdf="$(basename "$(readlink "$card/device" 2>/dev/null)")"
    if [ "$bdf" = "$IGPU_BDF" ]; then
      printf '%s\n' "${card##*/}"
      return 0
    fi
  done
  return 1
}

igpu_line() {
  # key: value — aligned column (like the dGPU section).
  printf '  %-22s %s %s\n' "$1" "$2" "${3:-}"
}

igpu_rc6_residency() {
  # rc6_residency_ms is cumulative since boot — show mS + % from /proc/uptime.
  local d="$1" res up
  res="$(cat "$d/rc6_residency_ms" 2>/dev/null)" || res=""
  if [ -n "$res" ]; then
    up="$(awk '{print $1}' /proc/uptime 2>/dev/null)" || up=""
    if [ -n "$up" ]; then
      igpu_line "rc6_residency_ms:" "$res ms" \
        "$(awk -v r="$res" -v u="$up" 'BEGIN { if (u>0) printf "(≈%.1f%% since boot)", r/(u*1000)*100 }')"
    else
      igpu_line "rc6_residency_ms:" "$res ms" ""
    fi
  else
    igpu_line "rc6_residency_ms:" "unavailable" ""
  fi
}

igpu_rps() {
  # Common read of the RPS fields; $1 = sysfs directory, $2 = name prefix (rps_|gt_).
  local d="$1" p="$2" cur
  cur="$(cat "$d/${p}cur_freq_mhz" 2>/dev/null)" || cur="n/d"
  igpu_line "${p}cur_freq_mhz:" "$cur MHz" "* current"
  igpu_line "${p}max_freq_mhz:" "$(cat "$d/${p}max_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}min_freq_mhz:" "$(cat "$d/${p}min_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}boost_freq_mhz:" "$(cat "$d/${p}boost_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RP0_freq_mhz:" "$(cat "$d/${p}RP0_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RP1_freq_mhz:" "$(cat "$d/${p}RP1_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RPn_freq_mhz:" "$(cat "$d/${p}RPn_freq_mhz" 2>/dev/null || echo n/d) MHz"
}

igpu_status() {
  local card base d rc6
  card="$(find_igpu_card)" || {
    echo "=== iGPU (Intel Iris Pro 5200) — RPS ==="
    echo "iGPU ($IGPU_BDF): not found"
    return 0
  }
  base="/sys/class/drm/$card"
  echo "=== iGPU (Intel Iris Pro 5200) — RPS ==="
  igpu_line "card:" "$card ($IGPU_BDF)"

  if [ -d "$base/gt/gt0" ]; then
    d="$base/gt/gt0"
    igpu_line "mode:" "gt/gt0 (new interface)"
    igpu_rps "$d" "rps_"
    if [ -r "$d/rc6_enable" ]; then
      igpu_line "rc6_enable:" "$(cat "$d/rc6_enable" 2>/dev/null || echo n/d)"
      igpu_rc6_residency "$d"
    fi
  elif [ -r "$base/gt_cur_freq_mhz" ]; then
    d="$base"
    igpu_line "mode:" "gt_* (old interface)"
    igpu_rps "$d" "gt_"
    rc6="$base/power"
    if [ -r "$rc6/rc6_enable" ]; then
      igpu_line "rc6_enable:" "$(cat "$rc6/rc6_enable" 2>/dev/null || echo n/d)"
      igpu_rc6_residency "$rc6"
    fi
  else
    igpu_line "rps:" "no iGPU RPS interface"
  fi
}

fan_status() {
  # v5.1: current fan curve + RPM from the daemon (/run/reclocked/status, JSON).
  # The curve is chosen by the daemon (igd/dga/compiler/override) — read-only here.
  # Fallback without the daemon: RPM from the applesmc sysfs (curve unknown).
  local curve tmin tmax r1 r2 note
  echo "=== fans ==="
  if [ -r /run/reclocked/status ]; then
    curve=$(sed -n 's/.*"fan_curve": *"\([^"]*\)".*/\1/p' /run/reclocked/status)
    tmin=$(sed -n 's/.*"fan_tmin": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    tmax=$(sed -n 's/.*"fan_tmax": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    r1=$(sed -n 's/.*"fan_rpm1": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    r2=$(sed -n 's/.*"fan_rpm2": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    case "$curve" in
      igd)      note="iGPU-only (dGPU OFF)" ;;
      dga)      note="dGPU ON" ;;
      compiler) note="compiler boost" ;;
      override) note="override (manual)" ;;
      off)      note="fan off" ;;
      *)        note="unknown (${curve:-no data})" ;;
    esac
    printf '  %-22s %s\n' "curve:" "$curve ($tmin-$tmax°C) — $note"
    printf '  %-22s fan1=%s RPM, fan2=%s RPM\n' "RPM:" "${r1:-n/d}" "${r2:-n/d}"
  else
    local f1 f2
    f1=$(cat /sys/devices/platform/applesmc.768/fan1_input 2>/dev/null)
    f2=$(cat /sys/devices/platform/applesmc.768/fan2_input 2>/dev/null)
    printf '  %-22s %s\n' "curve:" "daemon not running — unknown"
    printf '  %-22s fan1=%s RPM, fan2=%s RPM (sysfs)\n' "RPM:" "${f1:-n/d}" "${f2:-n/d}"
  fi
}

status() {
  echo "=== pstate (debugfs) ==="
  sudo cat "$PSTATE"
  local hw ct
  if hw=$(find_hwmon); then
    # dGPU OFF (switchd v5.0): reading temp1_input fails with EINVAL — do not crash.
    if t=$(cat "$hw/temp1_input" 2>/dev/null); then
      printf "=== temperature: %.1f °C (hwmon: %s) ===\n" \
        "$(awk '{printf "%.1f", $1/1000}' <<<"$t")" "$(basename "$hw")"
    elif ct=$(find_coretemp); then
      # dGPU OFF → show the CPU temp (coretemp) — the same source that
      # reclocked uses for the fans when the dGPU is OFF (max(dGPU,coretemp)).
      printf "=== temperature: %.1f °C (dGPU OFF — CPU coretemp: %s) ===\n" \
        "$(awk '{printf "%.1f", $1/1000}' "$ct/temp1_input")" "$(basename "$ct")"
    else
      echo "=== temperature: UNAVAILABLE (dGPU OFF, no coretemp) ==="
    fi
  fi
  igpu_status
  fan_status
  echo "=== reclocked ==="
  daemon_status
  override_status
}

set_override() {
  local val="$1"
  sudo -n mkdir -p "$OVERRIDE_DIR" 2>/dev/null
  local ts
  ts=$(date '+%Y-%m-%d %H:%M:%S')
  echo "$val @ $ts" | sudo -n tee "$OVERRIDE_FILE" >/dev/null
}

set_state() {
  [ $# -ge 1 ] || { usage; return 2; }
  local arg="$1"
  local val="${arg##*:}"
  case " $VALID " in
    *" $val "*) : ;;
    *) echo "ERROR: unknown pstate '$arg'. Allowed: $VALID (optionally with the ac:/dc: prefix)" >&2
       return 2 ;;
  esac
  sudo -n test -f "$PSTATE" || { echo "ERROR: no $PSTATE — is debugfs mounted? (check: sudo cat $PSTATE)" >&2; return 1; }
  # switchd v5.0: dGPU OFF (gmux cut the power) → writing pstate = hang in nouveau
  # (nvkm_pstate_calc — the daemon hung this way in G1 live). Do not allow that.
  if [ -f /run/reclocked/status ] && grep -q '"dgpu_power": "off"' /run/reclocked/status; then
    echo "ERROR: the dGPU is OFF (switchd v5.0) — writing pstate would hang the card." >&2
    echo "      Enable the dGPU first: reclockctl dgpu-on (then reclockctl dgpu-auto)" >&2
    return 1
  fi
  case "$val" in
    0e|0f) echo "⚠️  $val = aggressive memory reclock — test carefully!" >&2 ;;
  esac
  printf '%s' "$arg" | sudo tee "$PSTATE" >/dev/null || return 1
  # Enable the manual override — the daemon freezes auto while the flag-file exists.
  set_override "$arg"
  echo "override enabled (/run/reclocked/override) — the daemon freezes auto"
  if pgrep -x reclocked >/dev/null 2>&1; then
    echo "reclocked daemon running → holding $val (auto resumes after: ./pstate.sh auto)"
  fi
  sleep 1
  echo "=== after the change ==="
  status
}

auto_override() {
  # 2026-08-29: the old `sudo -n test -f 2>/dev/null` condition lied "inactive"
  # when sudo -n had no permissions (user run 18:5x: the first command "did nothing",
  # the second passed after the credentials refreshed) — check the file directly and
  # report an rm failure honestly (exit != 0 → ✗ in the prompt).
  if [ -f "$OVERRIDE_FILE" ]; then
    if sudo -n rm -f "$OVERRIDE_FILE" 2>/dev/null; then
      echo "override removed — the daemon resumes auto"
    else
      echo "override exists, but removing it needs sudo — run: sudo ./scripts/pstate.sh auto" >&2
      status
      return 1
    fi
  else
    echo "override inactive (nothing to do)"
  fi
  status
}

case "${1:-status}" in
  status) status ;;
  set) shift; set_state "$@" ;;
  auto) auto_override ;;
  -h|--help) usage ;;
  *) usage; exit 1 ;;
esac