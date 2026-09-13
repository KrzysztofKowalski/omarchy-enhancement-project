#!/usr/bin/env bash
# bench-gpu.sh — glmark2 benchmark comparing the iGPU (Intel Iris Pro 5200) and
# dGPU (NVIDIA GT 750M, nouveau — Kepler GK107) on a MacBook Pro 11,3.
#
# Goal: repeatable performance comparison at the FULL panel resolution
# (default 2880x1800). Earlier tests (report 78) at 800x600 did not load the
# GPU — only the full size shows the differences between the cards.
#
# Methodology:
#   * FULL-RES (2880x1800) — the panel's native size; 800x600 is too small to
#     load the GPU (report 78).
#   * OFFSCREEN (FBO) — on-screen dGPU measurements are skewed by PRIME
#     copyback: every frame rendered in dGPU VRAM is copied to the iGPU
#     (DRI_PRIME); ceiling ~1300 FPS at 800x600, ~140 FPS at 2880x1800.
#     Offscreen = zero copyback, zero vsync.
#   * --frame-end none — glmark2 does glFinish() at the end of each frame by
#     default, which serializes the pipeline and lowers the score (2023.01 has
#     --frame-end but no --swap-interval).
#   * --swap-mode immediate — instead of vsync; on-screen without it locks the
#     compositor (report 78, repeat: 148 points vs 1000+ without the lock —
#     window focus on Xwayland changes vsync).
#   * ON-SCREEN: lock the Omarchy/Hyprland compositor before the run (e.g.
#     hyprctl dispatch lock) — screen is by design less reliable.
#   * iGPU RPS — we do NOT pin it (auto). We pin the dGPU via debugfs pstate with
#     the daemon stopped — otherwise reclocked negotiates the ceiling during the run
#     and the real pstate ≠ the set one (report 78, anomaly).
#   * Fans manual 100% — fan1_max=6156, fan2_max=5700 RPM (SEPARATELY,
#     the maxima differ; we do not write the same value).
#
# Usage:
#   sudo ./bench-gpu.sh [options]
#     -g | --gpu igd|dgpu|both          (default both)
#     -m | --mode offscreen|screen|both (default offscreen)
#     -s | --size WxH|auto              (default 2880x1800; auto = hyprctl,
#                                        fallback 2880x1800)
#     -p | --pstate 0e|0a|07|all        (default 0e; all = 07 0a 0e)
#     -f | --frame-end none|default     (default none)
#     -F | --fans 100|auto              (default 100)
#     -o | --outdir DIR                 (default tmp/bench)
#     -n | --dry-run                    (only show the commands, do not run them)
#     -h | --help
#
# Restore (trap EXIT, highest priority): fans → auto, reclocked → active,
# dGPU → Off. The script is idempotent: daemon start/dgpu-on only when needed.
set -uo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
RECLOCKCTL="${RECLOCKCTL:-$PROJ/src/reclockctl}"
APPLESMC="/sys/devices/platform/applesmc.768"
VGASW="/sys/kernel/debug/vgaswitcheroo/switch"
PSTATE_FILE="/sys/kernel/debug/dri/0000:01:00.0/pstate"
XDISPLAY=":0"                       # Xwayland — report 78 worked on DISPLAY=:0

# --- default options -----------------------------------------------------------
GPU_RAW="both"                      # igd|dgpu|both
MODE_RAW="offscreen"                # offscreen|screen|both
SIZE="2880x1800"                    # WxH or auto (hyprctl, fallback native)
PSTATE_RAW="0e"                     # 0e|0a|07|all (dGPU pstate)
FRAME_END="none"                    # none|default
FANS="100"                          # 100|auto
OUTDIR="$PROJ/tmp/bench"
DRY=0
GPU_LIST=""; MODE_LIST=""; PSTATE_LIST=""
RUN_LOGS=""                         # logs of the current run (for the summary)
ERRORS=0                            # 1 = an error happened → exit code 1
RESTORED=0

log()  { printf '[bench] %s\n' "$*"; }
fail() { ERRORS=1; printf '[bench] ERROR: %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage:
  sudo ./bench-gpu.sh [options]
    -g | --gpu igd|dgpu|both          (default both)
    -m | --mode offscreen|screen|both (default offscreen)
    -s | --size WxH|auto              (default 2880x1800; auto = hyprctl,
                                       fallback 2880x1800)
    -p | --pstate 0e|0a|07|all        (default 0e; all = 07 0a 0e)
    -f | --frame-end none|default     (default none)
    -F | --fans 100|auto              (default 100)
    -o | --outdir DIR                 (default tmp/bench)
    -n | --dry-run                    (only show the commands, do not run them)
    -h | --help
EOF
}

# --- dry-run: print instead of running ------------------------------------------
cmd() { # <command...>
  if [ "$DRY" = 1 ]; then printf 'DRY-RUN: %s\n' "$*"; else "$@"; fi
}
write_sysfs() { # <file> <value>
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: echo %s > %s\n' "$2" "$1"
  else
    printf '%s\n' "$2" >"$1" 2>/dev/null || log "ERROR: write $1 = $2"
  fi
}
sleep_if_real() { # <seconds> <description>
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: sleep %s (%s)\n' "$1" "$2"
  else
    sleep "$1"
  fi
}

# --- reclocked daemon -----------------------------------------------------------
daemon_active() { pgrep -x reclocked >/dev/null 2>&1; }

daemon_wait_active() { # <timeout-s>
  local tmo="$1" i=0
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: waiting for active reclocked (max %s s)\n' "$tmo"
    return 0
  fi
  while [ "$i" -lt "$tmo" ]; do
    daemon_active && return 0
    sleep 1
    i=$((i+1))
  done
  return 1
}

# --- vgaswitcheroo ------------------------------------------------------------
# DIS line format: 2:DIS: :Pwr:0000:01:00.0  |  2:DIS: :Off:0000:01:00.0
vgasw_line() { cat "$VGASW" 2>/dev/null | grep '^2:DIS:' | head -1; }
vgasw_is() { # <Pwr|Off>
  local line
  line="$(vgasw_line)"
  [ -n "$line" ] && case "$line" in *":$1:"*) return 0 ;; esac
  return 1
}
wait_vgasw() { # <Pwr|Off> <timeout-s>
  local want="$1" tmo="$2" i=0
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: poll vgaswitcheroo until 2:DIS: :%s (max %s s)\n' "$want" "$tmo"
    return 0
  fi
  while [ "$i" -lt "$tmo" ]; do
    vgasw_is "$want" && return 0
    sleep 1
    i=$((i+1))
  done
  return 1
}

# --- fans (applesmc) -------------------------------------------------------------
fans_manual_100() {
  local f1 f2
  f1="$(cat "$APPLESMC/fan1_max" 2>/dev/null || echo 6156)"
  f2="$(cat "$APPLESMC/fan2_max" 2>/dev/null || echo 5700)"
  log "fans manual 100% (fan1=$f1, fan2=$f2 RPM)"
  write_sysfs "$APPLESMC/fan1_manual" 1
  write_sysfs "$APPLESMC/fan1_output" "$f1"
  write_sysfs "$APPLESMC/fan2_manual" 1
  write_sysfs "$APPLESMC/fan2_output" "$f2"
}
fans_auto() {
  log "fans → auto (SMC takes over)"
  write_sysfs "$APPLESMC/fan1_manual" 0
  write_sysfs "$APPLESMC/fan2_manual" 0
}

# --- dGPU pstate (debugfs) -------------------------------------------------------
# Writing is allowed ONLY when the dGPU is in Pwr (otherwise a hang in nouveau — report 77).
# File format: "0e: core 270-925 MHz memory 4000 MHz" + "*" on the active one +
# an "AC:" line with the user-state for AC power.
pstate_mem() { # <07|0a|0e> → memory MHz (echo)
  case "$1" in 0e) echo 4000 ;; 0a) echo 1560 ;; 07) echo 838 ;; *) return 1 ;; esac
}
pin_pstate() { # <07|0a|0e>
  local ps="$1" mem cur
  mem="$(pstate_mem "$ps")" || { fail "unknown pstate '$ps'"; return 1; }
  vgasw_is Pwr || { fail "dGPU not in Pwr — NOT pinning pstate (hang!)"; return 1; }
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: echo %s > %s\n' "$ps" "$PSTATE_FILE"
    printf 'DRY-RUN: verify: grep -E "^%s:.*\\*" %s  +  grep -E "^AC:.*memory %s"\n' \
      "$ps" "$PSTATE_FILE" "$mem"
    return 0
  fi
  log "pin pstate=$ps (AC memory ${mem} MHz)"
  printf '%s\n' "$ps" >"$PSTATE_FILE" 2>/dev/null \
    || { fail "writing pstate=$ps failed"; return 1; }
  sleep 2   # time for the clocks to switch (patches 0011-0013; reclocked timeout 2 s)
  cur="$(cat "$PSTATE_FILE" 2>/dev/null)" \
    || { fail "cannot read $PSTATE_FILE"; return 1; }
  if ! grep -qE "^${ps}:.*\*" <<<"$cur"; then
    fail "pstate=$ps inactive (no '*' on the ${ps}: line)"
    return 1
  fi
  if ! grep -qE "^AC:.*memory ${mem}([^0-9]|$)" <<<"$cur"; then
    fail "AC does not hold memory ${mem} MHz — pstate=$ps unverified"
    return 1
  fi
  log "pstate=$ps confirmed (AC: core … memory ${mem} MHz)"
  return 0
}
check_pstate_still() { # <ps> — verification after the run (did the pstate change?)
  local ps="$1" cur
  [ "$DRY" = 1 ] && return 0
  cur="$(cat "$PSTATE_FILE" 2>/dev/null)" || return 0
  if ! grep -qE "^${ps}:.*\*" <<<"$cur"; then
    log "WARNING: pstate after the run ≠ $ps — the pstate changed (someone overwrote it?)"
  fi
}

# --- dGPU power (via reclockctl — flag-file executed by the daemon) ---------------
dgpu_on() {
  if vgasw_is Pwr; then
    log "dGPU already Pwr — skipping dgpu-on"
  else
    cmd "$RECLOCKCTL" dgpu-on
    wait_vgasw Pwr 30 || { fail "dGPU did not switch to Pwr after 30 s"; return 1; }
  fi
  sleep_if_real 5 "settle after power-on (PCIe link / PMU — patches 0011-0013)"
  return 0
}
dgpu_off() {
  if ! vgasw_is Pwr; then
    log "dGPU already Off — skipping dgpu-off"
  else
    cmd "$RECLOCKCTL" dgpu-off
    wait_vgasw Off 30 || fail "dGPU did not switch to Off after 30 s"
  fi
}

# --- glmark2 --------------------------------------------------------------------
glmark2_run() { # <env-prefix> <log> <args...>
  local prefix="$1" logf="$2"; shift 2
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: env %s glmark2 %s 2>&1 | tee %s\n' "$prefix" "$*" "$logf"
    return 0
  fi
  RUN_LOGS="$RUN_LOGS $logf"
  log "glmark2 → $logf"
  # shellcheck disable=SC2086  # the prefix is intentionally unsplit: DISPLAY=:0 DRI_PRIME=1
  if env $prefix glmark2 "$@" 2>&1 | tee "$logf"; then
    log "glmark2 OK → $logf"
  else
    fail "glmark2 exit code $? — $logf"
  fi
}

# --- benchmark sections ------------------------------------------------------------
bench_igd() {
  log "== iGPU — Intel Iris Pro 5200 (i915, RPS auto — not pinned) =="
  if daemon_active; then
    cmd "$RECLOCKCTL" stop || fail "reclockctl stop failed"
  else
    log "daemon inactive — skipping stop"
  fi
  [ "$FANS" = 100 ] && fans_manual_100
  for mode in $MODE_LIST; do
    local args=(--size "$SIZE" --frame-end "$FRAME_END")
    [ "$mode" = offscreen ] && args+=(--off-screen)
    args+=(--swap-mode immediate)
    glmark2_run "DISPLAY=$XDISPLAY" \
      "$OUTDIR/bench-igd-${mode}-${SIZE}-${FRAME_END}.log" "${args[@]}"
  done
}

bench_dgpu() {
  log "== dGPU — NVIDIA GT 750M (nouveau, GK107) =="
  # 1. daemon active? start + wait; then dgpu-on (flag-file) + poll Pwr
  if daemon_active; then
    log "daemon active — skipping start"
  else
    log "daemon inactive → start"
    cmd "$RECLOCKCTL" start || { fail "reclockctl start failed"; return 1; }
    daemon_wait_active 15 || { fail "daemon did not come up in 15 s"; return 1; }
  fi
  dgpu_on || return 1
  # 2. stop the daemon — it won't negotiate pstate/fans during the runs
  cmd "$RECLOCKCTL" stop || fail "reclockctl stop failed"
  # 3. fans manual 100%
  [ "$FANS" = 100 ] && fans_manual_100
  # 4. pstate × mode
  for ps in $PSTATE_LIST; do
    if ! pin_pstate "$ps"; then
      fail "pinning pstate=$ps failed — skipping this pstate"
      continue
    fi
    for mode in $MODE_LIST; do
      local args=(--size "$SIZE" --frame-end "$FRAME_END")
      [ "$mode" = offscreen ] && args+=(--off-screen)
      args+=(--swap-mode immediate)
      glmark2_run "DISPLAY=$XDISPLAY DRI_PRIME=1" \
        "$OUTDIR/bench-dgpu-${mode}-${SIZE}-${ps}-${FRAME_END}.log" "${args[@]}"
      check_pstate_still "$ps"
    done
  done
}

# --- renderer verification ------------------------------------------------------------
verify_renderers() {
  log "== RENDERER VERIFICATION =="
  local logs
  logs="$(printf '%s\n' $RUN_LOGS | grep '/bench-igd-' || true)"
  if [ -n "$logs" ]; then
    if grep -H "GL_RENDERER" $logs 2>/dev/null | grep -q "Mesa Intel.*P5200"; then
      log "iGPU: renderer OK — Mesa Intel … P5200"
    else
      fail "expected iGPU renderer missing (Mesa Intel … P5200)"
    fi
  else
    log "no iGPU runs in this session"
  fi
  logs="$(printf '%s\n' $RUN_LOGS | grep '/bench-dgpu-' || true)"
  if [ -n "$logs" ]; then
    if grep -H "GL_RENDERER" $logs 2>/dev/null | grep -q "NVE7"; then
      log "dGPU: renderer OK — NVE7 (nvc0)"
    else
      fail "expected dGPU renderer missing (NVE7 / nvc0)"
    fi
  else
    log "no dGPU runs in this session"
  fi
}

# --- restore / summary ----------------------------------------------------------------
restore() {
  [ "$RESTORED" = 1 ] && return 0
  RESTORED=1
  log "== RESTORE (highest priority) =="
  fans_auto
  if daemon_active; then
    log "daemon already active — skipping start"
  else
    log "daemon inactive → start"
    cmd "$RECLOCKCTL" start || fail "reclockctl start failed"
    daemon_wait_active 15 || fail "daemon did not come up after restore"
  fi
  dgpu_off
  print_summary
}

print_summary() {
  log "== FINAL STATE =="
  if daemon_active; then log "reclocked: active"; else log "reclocked: INACTIVE"; fi
  if vgasw_is Pwr; then log "dGPU: Pwr"; elif vgasw_is Off; then log "dGPU: Off"; else log "dGPU: unknown state"; fi
  local m1 m2
  m1="$(cat "$APPLESMC/fan1_manual" 2>/dev/null || echo '?')"
  m2="$(cat "$APPLESMC/fan2_manual" 2>/dev/null || echo '?')"
  log "fans: fan1_manual=$m1 fan2_manual=$m2 (0 = auto)"
  log "== RESULTS TABLE (Score per run) =="
  if [ -n "$RUN_LOGS" ]; then
    grep -HE "Surface Size|glmark2 Score" $RUN_LOGS 2>/dev/null \
      || log "no results in the run logs"
  else
    log "no logs (nothing was run?)"
  fi
}

# --- preflight ---------------------------------------------------------------------
preflight() {
  [ "$EUID" = 0 ] || die "run via sudo: sudo $0 [options]"
  command -v glmark2 >/dev/null 2>&1 || die "glmark2 missing from PATH"
  if [ -x "$RECLOCKCTL" ]; then
    :
  elif command -v reclockctl >/dev/null 2>&1; then
    RECLOCKCTL="$(command -v reclockctl)"
  else
    die "reclockctl missing: $RECLOCKCTL (check PATH or the project path)"
  fi
  [ -r "$VGASW" ] || die "no access to $VGASW (is debugfs mounted?)"
  [ -e "$PSTATE_FILE" ] || die "no $PSTATE_FILE (debugfs / nouveau module?)"
  for f in fan1_manual fan2_manual fan1_output fan2_output; do
    [ -w "$APPLESMC/$f" ] || die "no write access to $APPLESMC/$f"
  done
  if dmesg 2>/dev/null | grep -qi "nobody cared"; then
    log "WARNING: the kernel disabled the IRQ line (nobody cared) — dGPU after power-on"
    log "           may stall on fences; consider a reboot before the dGPU test"
  fi
}

# --- arguments -----------------------------------------------------------------------
resolve_size() { # <WxH|auto> → WxH (echo)
  local val="$1" line
  case "$val" in
    auto)
      line="$(hyprctl monitors -j 2>/dev/null | jq -r '.[] | select(.name=="eDP-1") | "\(.width)x\(.height)"' 2>/dev/null | head -1)"
      [ -n "$line" ] || line="$(hyprctl monitors -j 2>/dev/null | jq -r '.[0] | "\(.width)x\(.height)"' 2>/dev/null)"
      [ -n "$line" ] || line="2880x1800"
      log "auto size → $line (eDP-1 panel)"
      printf '%s\n' "$line"
      return 0
      ;;
    [0-9]*x[0-9]*) printf '%s\n' "$val"; return 0 ;;
    *) echo "bad size: '$val' — expected WxH (e.g. 2880x1800) or auto" >&2; return 1 ;;
  esac
}
dedup() { # <list> — remove duplicates, keep the order
  local out="" w
  for w in $1; do
    case " $out " in *" $w "*) : ;; *) out="$out $w" ;; esac
  done
  printf '%s' "${out# }"
}
parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      -g|--gpu)       [ $# -ge 2 ] || die "missing value for $1"; GPU_RAW="$2"; shift 2 ;;
      --gpu=*)        GPU_RAW="${1#*=}"; shift ;;
      -m|--mode)      [ $# -ge 2 ] || die "missing value for $1"; MODE_RAW="$2"; shift 2 ;;
      --mode=*)       MODE_RAW="${1#*=}"; shift ;;
      -s|--size)      [ $# -ge 2 ] || die "missing value for $1"; SIZE="$2"; shift 2 ;;
      --size=*)       SIZE="${1#*=}"; shift ;;
      -p|--pstate)    [ $# -ge 2 ] || die "missing value for $1"; PSTATE_RAW="$2"; shift 2 ;;
      --pstate=*)     PSTATE_RAW="${1#*=}"; shift ;;
      -f|--frame-end) [ $# -ge 2 ] || die "missing value for $1"; FRAME_END="$2"; shift 2 ;;
      --frame-end=*)  FRAME_END="${1#*=}"; shift ;;
      -F|--fans)      [ $# -ge 2 ] || die "missing value for $1"; FANS="$2"; shift 2 ;;
      --fans=*)       FANS="${1#*=}"; shift ;;
      -o|--outdir)    [ $# -ge 2 ] || die "missing value for $1"; OUTDIR="$2"; shift 2 ;;
      --outdir=*)     OUTDIR="${1#*=}"; shift ;;
      -n|--dry-run)   DRY=1; shift ;;
      -h|--help)      usage; exit 0 ;;
      *) die "unknown argument: $1 (see --help)" ;;
    esac
  done
}
validate_args() {
  local g m p list=""
  GPU_LIST=""
  for g in $GPU_RAW; do
    case "$g" in
      igd|dgpu) list="$list $g" ;;
      both)     list="$list igd dgpu" ;;
      *) die "unknown GPU: '$g' (igd|dgpu|both)" ;;
    esac
  done
  GPU_LIST="$(dedup "$list")"
  [ -n "$GPU_LIST" ] || die "no GPU for the benchmark"
  list=""; MODE_LIST=""
  for m in $MODE_RAW; do
    case "$m" in
      offscreen|screen) list="$list $m" ;;
      both)             list="$list offscreen screen" ;;
      *) die "unknown mode: '$m' (offscreen|screen|both)" ;;
    esac
  done
  MODE_LIST="$(dedup "$list")"
  [ -n "$MODE_LIST" ] || die "no mode for the benchmark"
  list=""; PSTATE_LIST=""
  for p in $PSTATE_RAW; do
    case "$p" in
      all)      list="$list 07 0a 0e" ;;
      07|0a|0e) list="$list $p" ;;
      *) die "unknown pstate: '$p' (0e|0a|07|all)" ;;
    esac
  done
  PSTATE_LIST="$(dedup "$list")"
  [ -n "$PSTATE_LIST" ] || die "no pstate for the benchmark"
  case "$FRAME_END" in none|default) ;; *) die "unknown frame-end: '$FRAME_END' (none|default)" ;; esac
  case "$FANS" in 100|auto) ;; *) die "unknown fans: '$FANS' (100|auto)" ;; esac
  SIZE="$(resolve_size "$SIZE")" || die "bad size (details above)"
}

# --- main -----------------------------------------------------------------------------
main() {
  parse_args "$@"
  validate_args
  if [ "$DRY" = 1 ]; then
    log "DRY-RUN — plan (commands will NOT be executed):"
    log "  GPU=$GPU_LIST  MODE=$MODE_LIST  SIZE=$SIZE  PSTATE=$PSTATE_LIST"
    log "  FRAME_END=$FRAME_END  FANS=$FANS  OUTDIR=$OUTDIR"
    for gpu in $GPU_LIST; do
      case "$gpu" in igd) bench_igd ;; dgpu) bench_dgpu ;; esac
    done
    exit 0
  fi
  preflight
  mkdir -p "$OUTDIR" || die "cannot create $OUTDIR"
  trap 'exit 130' INT TERM
  trap restore EXIT
  log "start: GPU=$GPU_LIST  MODE=$MODE_LIST  SIZE=$SIZE  PSTATE=$PSTATE_LIST"
  log "       FRAME_END=$FRAME_END  FANS=$FANS  OUTDIR=$OUTDIR"
  for gpu in $GPU_LIST; do
    case "$gpu" in
      igd)  bench_igd ;;
      dgpu) bench_dgpu ;;
    esac
  done
  verify_renderers
  if [ "$ERRORS" = 1 ]; then
    log "finished with errors — see above"
    exit 1
  fi
  log "end — OK (restore via trap EXIT)"
}
main "$@"